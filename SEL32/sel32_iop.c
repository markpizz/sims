/* sel32_iop.c: SEL-32 Model 8000/8001/8002 IOP processor controller

   Copyright (c) 2018-2020, James C. Bevier

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   JAMES C. BEVIER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   This channel is the interrupt fielder for all of the IOP sub channels.  It's
   channel address is 7E00.  This code handles the INCH command for the IOP
   devices and controls the status FIFO for the iop devices on interrupts and
   TIO instructions..

   Possible devices:
   The f8iop communication controller (TY7EA0), (TY7EB0), (TY7EC0) 
   The ctiop console communications controller (CT7EFC & CT7EFD)
   The lpiop line printer controller (LP7EF8), (LP7EF9)

*/

#include "sel32_defs.h"

#ifdef NUM_DEVS_IOP

extern  t_stat  set_dev_addr(UNIT *uptr, int32 val, CONST char *cptr, void *desc);
extern  t_stat  show_dev_addr(FILE *st, UNIT * uptr, int32 v, CONST void *desc);
extern  void    chan_end(uint16 chan, uint8 flags);
extern  int     chan_read_byte(uint16 chan, uint8 *data);
extern  int     chan_write_byte(uint16 chan, uint8 *data);
extern  void    set_devattn(uint16 addr, uint8 flags);
extern  void    post_extirq(void);
extern  uint32  attention_trap;             /* set when trap is requested */
extern  void    set_devwake(uint16 addr, uint8 flags);
extern  t_stat  set_inch(UNIT *uptr, uint32 inch_addr); /* set channel inch address */
extern  CHANP  *find_chanp_ptr(uint16 chsa);             /* find chanp pointer */

/* forward definitions */
uint8   iop_startcmd(UNIT *uptr, uint16 chan, uint8 cmd);
void    iop_ini(UNIT *uptr, t_bool f);
t_stat  iop_srv(UNIT *uptr);
t_stat  iop_reset(DEVICE *dptr);
t_stat  iop_help(FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char  *iop_desc(DEVICE *dptr);

/* Held in u3 is the device command and status */
#define IOP_INCH    0x00    /* Initialize channel command */
#define IOP_INCH2   0xf0    /* Initialize channel command after start */
#define IOP_NOP     0x03    /* NOP command */
#define IOP_MSK     0xff    /* Command mask */

/* Status held in u3 */
/* controller/unit address in upper 16 bits */
#define CON_INPUT   0x100   /* Input ready for unit */
#define CON_CR      0x200   /* Output at beginning of line */
#define CON_REQ     0x400   /* Request key pressed */
#define CON_EKO     0x800   /* Echo input character */
#define CON_OUTPUT  0x1000  /* Output ready for unit */
#define CON_READ    0x2000  /* Read mode selected */

/* not used u4 */

/* in u5 packs sense byte 0,1 and 3 */
/* Sense byte 0 */
#define SNS_CMDREJ  0x80000000    /* Command reject */
#define SNS_INTVENT 0x40000000    /* Unit intervention required */
/* sense byte 3 */
#define SNS_RDY     0x80        /* device ready */
#define SNS_ONLN    0x40        /* device online */

/* std devices. data structures

    iop_dev     Console device descriptor
    iop_unit    Console unit descriptor
    iop_reg     Console register list
    iop_mod     Console modifiers list
*/

struct _iop_data
{
    uint8       ibuff[145];         /* Input line buffer */
    uint8       incnt;              /* char count */
}
iop_data[NUM_UNITS_IOP];

/* channel program information */
CHANP           iop_chp[NUM_UNITS_IOP] = {0};

MTAB            iop_mod[] = {
    {MTAB_XTD | MTAB_VUN | MTAB_VALR, 0, "DEV", "DEV",
        &set_dev_addr, &show_dev_addr, NULL, "Device address"},
    {0}
};

UNIT            iop_unit[] = {
    {UDATA(&iop_srv, UNIT_IDLE, 0), 0, UNIT_ADDR(0x7E00)},       /* Channel controller */
};

//DIB iop_dib = {NULL, iop_startcmd, NULL, NULL, NULL, iop_ini, iop_unit, iop_chp, NUM_UNITS_IOP, 0xff, 0x7e00,0,0,0};
DIB             iop_dib = {
    NULL,           /* uint8 (*pre_io)(UNIT *uptr, uint16 chan)*/       /* Start I/O */
    iop_startcmd,   /* uint8 (*start_cmd)(UNIT *uptr, uint16 chan, uint8 cmd)*/ /* Start a command SIO */
    NULL,           /* uint8 (*halt_io)(UNIT *uptr) */          /* Stop I/O HIO */
    NULL,           /* uint8 (*test_io)(UNIT *uptr) */          /* Test I/O TIO */
    NULL,           /* uint8 (*post_io)(UNIT *uptr) */          /* Post I/O */
    iop_ini,        /* void  (*dev_ini)(UNIT *, t_bool) */      /* init function */
    iop_unit,       /* UNIT* units */                           /* Pointer to units structure */
    iop_chp,        /* CHANP* chan_prg */                       /* Pointer to chan_prg structure */
    NUM_UNITS_IOP,   /* uint8 numunits */                        /* number of units defined */
    0xff,           /* uint8 mask */                            /* 16 devices - device mask */
    0x7e00,         /* uint16 chan_addr */                      /* parent channel address */
    0,              /* uint32 chan_fifo_in */                   /* fifo input index */
    0,              /* uint32 chan_fifo_out */                  /* fifo output index */
    {0}             /* uint32 chan_fifo[FIFO_SIZE] */           /* interrupt status fifo for channel */
};

DEVICE          iop_dev = {
    "IOP", iop_unit, NULL, iop_mod,
    NUM_UNITS_IOP, 8, 15, 1, 8, 8,
    NULL, NULL, &iop_reset,         /* examine, deposit, reset */
    NULL, NULL, NULL,               /* boot, attach, detach */
    &iop_dib, DEV_UADDR|DEV_DISABLE|DEV_DEBUG, 0, dev_debug,  /* dib, dev flags, debug flags, debug */
//  NULL, NULL, &iop_help,          /* ?, ?, help */
//  NULL, NULL, &iop_desc           /* ?, ?, description */
};

/* IOP controller routines */
/* initialize the console chan/unit */
void iop_ini(UNIT *uptr, t_bool f)
{
    int     unit = (uptr - iop_unit);               /* unit 0 */
    DEVICE *dptr = &iop_dev;                        /* one and only dummy device */

    sim_debug(DEBUG_CMD, &iop_dev,
        "IOP init device %s controller/device %04x\n",
        dptr->name, GET_UADDR(uptr->u3));
    iop_data[unit].incnt = 0;                       /* no input data */
    uptr->u5 = SNS_RDY|SNS_ONLN;                    /* status is online & ready */
}

/* start an I/O operation */
uint8  iop_startcmd(UNIT *uptr, uint16 chan, uint8 cmd)
{
    sim_debug(DEBUG_CMD, &iop_dev,
        "IOP startcmd %02x controller/device %04x\n",
        cmd, GET_UADDR(uptr->u3));
    if ((uptr->u3 & IOP_MSK) != 0)                  /* is unit busy */
        return SNS_BSY;                             /* yes, return busy */

    /* process the commands */
    switch (cmd & 0xFF) {
    /* UTX uses the INCH cmd to detect the IOP or MFP */
    /* IOP has INCH cmd of 0, while MFP uses 0x80 */
    case IOP_INCH:                                  /* INCH command */
        uptr->u5 = SNS_RDY|SNS_ONLN;                /* status is online & ready */
        uptr->u3 &= LMASK;                          /* leave only chsa */
        sim_debug(DEBUG_CMD, &iop_dev,
            "iop_startcmd %04x: Cmd INCH iptr %06x INCHa %06x\n",
            chan, iop_chp[0].ccw_addr,              /* set inch buffer addr */
            iop_chp[0].chan_inch_addr);             /* set inch buffer addr */

        iop_chp[0].chan_inch_addr = iop_chp[0].ccw_addr;   /* set inch buffer addr */
//      set_inch(uptr, iop_chp[0].ccw_addr);        /* new address */

        uptr->u3 |= IOP_INCH2;                      /* save INCH command as 0xf0 */
        sim_activate(uptr, 20);                     /* go on */
        return 0;                                   /* no status change */
        break;

    case IOP_NOP:                                   /* NOP command */
        sim_debug(DEBUG_CMD, &iop_dev, "iop_startcmd %04x: Cmd NOP\n", chan);
        uptr->u5 = SNS_RDY|SNS_ONLN;                /* status is online & ready */
        uptr->u3 &= LMASK;                          /* leave only chsa */
        uptr->u3 |= (cmd & IOP_MSK);                /* save NOP command */
        sim_activate(uptr, 20);                     /* TRY 07-13-19 */
        return 0;                                   /* no status change */
        break;

    default:                                        /* invalid command */
        uptr->u5 |= SNS_CMDREJ;                     /* command rejected */
        sim_debug(DEBUG_CMD, &iop_dev, "iop_startcmd %04x: Cmd Invalid %02x status %02x\n",
            chan, cmd, uptr->u5);
        uptr->u3 &= LMASK;                          /* leave only chsa */
        uptr->u3 |= (cmd & IOP_MSK);                /* save command */
        sim_activate(uptr, 20);                     /* force interrupt */
        return 0;                                   /* no status change */
        break;
    }

    return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;       /* not reachable for now */
}

/* Handle transfers for other sub-channels on IOP */
t_stat iop_srv(UNIT *uptr)
{
    uint16  chsa = GET_UADDR(uptr->u3);
    int     cmd = uptr->u3 & IOP_MSK;
//  CHANP   *chp = find_chanp_ptr(chsa);            /* find the chanp pointer */
    CHANP   *chp = &iop_chp[0];                     /* find the chanp pointer */
//  int     i;
//  int     len = chp->ccw_count;                   /* INCH command count */
    uint32  mema = chp->ccw_addr;                   /* get inch or buffer addr */

    /* test for NOP or INCH cmds */
    if ((cmd != IOP_NOP) && (cmd != IOP_INCH2)) {   /* NOP or INCH */
        uptr->u3 &= LMASK;                          /* nothing left, command complete */
        sim_debug(DEBUG_CMD, &iop_dev,
            "iop_srv Unknown cmd %02x chan %02x: chnend|devend|unitexp\n", cmd, chsa);
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITEXP);  /* done */
        return SCPE_OK;
    } else

    if (cmd == IOP_NOP) {                           /* NOP do nothing */
        uptr->u3 &= LMASK;                          /* nothing left, command complete */
        sim_debug(DEBUG_CMD, &iop_dev, "iop_srv INCH/NOP chan %02x: chnend|devend\n", chsa);
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND);      /* done */
        return SCPE_OK;
    } else

    /* test for INCH cmd */
    if (cmd == IOP_INCH2) {                         /* INCH */
        sim_debug(DEBUG_CMD, &iop_dev,
            "iop_srv starting INCH %06x cmd, chsa %04x MemBuf %06x cnt %04x\n",
            mema, chsa, chp->ccw_addr, chp->ccw_count);

        /* the chp->ccw_addr location contains the inch address */
        /* call set_inch() to setup inch buffer */
//      i = set_inch(uptr, mema);                   /* new address */
        set_inch(uptr, mema);                       /* new address */
//      chp->chan_inch_addr = mema;                 /* set inch buffer addr */
        uptr->u3 &= LMASK;                          /* clear the cmd */
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND);      /* we are done dev|chan end */
//      chan_end(chsa, SNS_CHNEND);                 /* we are done dev|chan end */
    }
    return SCPE_OK;
}

t_stat iop_reset(DEVICE *dptr)
{
    /* add reset code here */
    return SCPE_OK;
}

/* sho help iop */
t_stat iop_help(FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, CONST char *cptr)
{
    fprintf(st, "SEL-32 IOP Model 8000 Channel Controller at 0x7E00\r\n");
    fprintf(st, "The IOP fields all interrupts and status posting\r\n");
    fprintf(st, "for each of the controllers on the system.\r\n");
    fprintf(st, "Nothing can be configured for this Channel.\r\n");
//    fprint_set_help(st, dptr);
//    fprint_show_help(st, dptr);
    return SCPE_OK;
}

const char *iop_desc(DEVICE *dptr)
{
    return("SEL-32 IOP Model 8000 Channel Controller @ 0x7E00");
}

#endif

