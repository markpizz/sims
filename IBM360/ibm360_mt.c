/* ibm360_mt.c: IBM 360 2400 Magnetic tape controller

   Copyright (c) 2017, Richard Cornwell

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
   RICHARD CORNWELL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Magnetic tapes are represented as a series of variable records
   of the form:

        32b byte count
        byte 0
        byte 1
        :
        byte n-2
        byte n-1
        32b byte count

   If the byte count is odd, the record is padded with an extra byte
   of junk.  File marks are represented by a byte count of 0.
*/

#include "ibm360_defs.h"
#include "sim_tape.h"

#ifdef NUM_DEVS_MT
#define BUFFSIZE       (64 * 1024)
#define MTUF_9TR       (1 << MTUF_V_UF)
#define DEV_BUF_NUM(x)  (((x) & 07) << DEV_V_UF)
#define GET_DEV_BUF(x)  (((x) >> DEV_V_UF) & 07)
#define UNIT_MT(x)     UNIT_ATTABLE | UNIT_DISABLE | UNIT_ROABLE | MTUF_9TR | \
                          DEV_BUF_NUM(x)



#define MT_WRITE            0x01       /* Write command */
#define MT_READ             0x02       /* Read command */
#define MT_RDBK             0x0c       /* Read Backward */
#define MT_SENSE            0x04       /* Sense command */
#define MT_REW              0x07       /* Rewind command */
#define MT_RUN              0x0f       /* Rewind and unload */
#define MT_ERG              0x17       /* Erase Gap */
#define MT_WTM              0x1f       /* Write Tape Mark */
#define MT_BSR              0x27       /* Back space record */
#define MT_BSF              0x2f       /* Back space file */
#define MT_FSR              0x37       /* Forward space record */
#define MT_FSF              0x3f       /* Forward space file */
#define MT_MODE             0x03       /* Mode command */
#define MT_MODEMSK          0x07       /* Mode Mask */

#define MT_MDEN_200         0x00       /* 200 BPI mode 7 track only */
#define MT_MDEN_556         0x40       /* 556 BPI mode 7 track only */
#define MT_MDEN_800         0x80       /* 800 BPI mode 7 track only */
#define MT_MDEN_1600        0xc0       /* 1600 BPI mode 9 track only */
#define MT_MDEN_MSK         0xc0       /* Density mask */

#define MT_CTL_MSK          0x38       /* Mask for control flags */
#define MT_CTL_NOP          0x00       /* Nop control mode */
#define MT_CTL_NRZI         0x08       /* 9 track 800 bpi mode */
#define MT_CTL_RST          0x10       /* Set density, odd, convert on, trans off */
#define MT_CTL_NOP2         0x18       /* 9 track 1600 NRZI mode */
#define MT_CTL_MD0          0x20       /* Set density, even, convert off, trans off */
#define MT_CTL_MD1          0x28       /* Set density, even, convert off, trans on */
#define MT_CTL_MD2          0x30       /* Set density, odd, convert off, trans off */
#define MT_CTL_MD3          0x38       /* Set density, odd, convert off, trans on */

/* in u3 is device command code and status */
#define MT_CMDMSK            0x003f       /* Command being run */
#define MT_READDONE          0x0400       /* Read finished, end channel */
#define MT_MARK              0x0800       /* Sensed tape mark in move command */
#define MT_ODD               0x1000       /* Odd parity */
#define MT_TRANS             0x2000       /* Translation turned on ignored 9 track  */
#define MT_CONV              0x4000       /* Data converter on ignored 9 track  */
#define MT_BUSY              0x8000       /* Flag to send a CUE */

/* in u4 is current buffer position */

/* in u5 packs sense byte 0,1 and 3 */
/* Sense byte 0 */
#define SNS_CMDREJ       0x80       /* Command reject */
#define SNS_INTVENT      0x40       /* Unit intervention required */
#define SNS_BUSCHK       0x20       /* Parity error on bus */
#define SNS_EQUCHK       0x10       /* Equipment check */
#define SNS_DATCHK       0x08       /* Data Check */
#define SNS_OVRRUN       0x04       /* Data overrun */
#define SNS_WCZERO       0x02       /* Write with no data */
#define SNS_CVTCHK       0x01       /* Data conversion error */

/* Sense byte 1 */
#define SNS_NOISE        0x80       /* Noise record */
#define SNS_TUASTA       0x40       /* Selected and ready */
#define SNS_TUBSTA       0x20       /* Not ready, rewinding. */
#define SNS_7TRACK       0x10       /* Seven track unit */
#define SNS_LOAD         0x08       /* Load Point */
#define SNS_WR           0x04       /* Unit write */
#define SNS_WRP          0x02       /* No write ring */
#define SNS_DENS         0x01       /* Density error 9tr only */

/* Sense byte 2 */
#define SNS_BYTE2        0xc0       /* Not supported feature */

/* Sense byte 3 */
#define SNS_VRC          0x80       /* Veritical parity error */
#define SNS_LRCR         0x40       /* Logituntial parity error */
#define SNS_SKEW         0x20       /* Skew */
#define SNS_CRC          0x10       /* CRC error. 9t only */
#define SNS_SKEWVRC      0x08       /* VRC Skew */
#define SNS_PE           0x04       /* Phase encoding */
#define SNS_BACK         0x01       /* tape in backward status */

#define SNS_BYTE4        0x00       /* Hardware errors not supported */
#define SNS_BYTE5        0x00       /* Hardware errors not supported */

#define MT_CONV1         0x40
#define MT_CONV2         0x80
#define MT_CONV3         0xc0

/* u6 holds the packed characters and unpack counter */
#define BUF_EMPTY(u)  (u->hwmark == 0xFFFFFFFF)
#define CLR_BUF(u)     u->hwmark =  0xFFFFFFFF

uint8               mt_startcmd(UNIT *uptr, uint16 chan,  uint8 cmd) ;
t_stat              mt_srv(UNIT *);
t_stat              mt_boot(int32, DEVICE *);
void                mt_ini(UNIT *, t_bool);
t_stat              mt_reset(DEVICE *);
t_stat              mt_attach(UNIT *, CONST char *);
t_stat              mt_detach(UNIT *);
t_stat              mt_boot(int32, DEVICE *);


/* One buffer per channel */
uint8               mt_buffer[NUM_DEVS_MT][BUFFSIZE];
uint8               mt_busy[NUM_DEVS_MT];

MTAB                mt_mod[] = {
    {MTUF_WLK, 0, "write enabled", "WRITEENABLED", NULL},
    {MTUF_WLK, MTUF_WLK, "write locked", "LOCKED", NULL},
    {MTUF_9TR, 0, "7 track", "7T", NULL},
    {MTUF_9TR, MTUF_9TR, "9 track", "9T", NULL},
    {MTAB_XTD | MTAB_VUN, 0, "FORMAT", "FORMAT",
     &sim_tape_set_fmt, &sim_tape_show_fmt, NULL},
    {MTAB_XTD | MTAB_VUN | MTAB_VALR, 0, "DEV", "DEV", &set_dev_addr,
        &show_dev_addr, NULL},
    {0}
};

UNIT                mta_unit[] = {
    {UDATA(&mt_srv, UNIT_MT(0), 0), 0, UNIT_ADDR(0x180)},       /* 0 */
    {UDATA(&mt_srv, UNIT_MT(0), 0), 0, UNIT_ADDR(0x181)},       /* 1 */
    {UDATA(&mt_srv, UNIT_MT(0), 0), 0, UNIT_ADDR(0x182)},       /* 2 */
    {UDATA(&mt_srv, UNIT_MT(0), 0), 0, UNIT_ADDR(0x183)},       /* 3 */
    {UDATA(&mt_srv, UNIT_MT(0), 0), 0, UNIT_ADDR(0x184)},       /* 4 */
    {UDATA(&mt_srv, UNIT_MT(0), 0), 0, UNIT_ADDR(0x185)},       /* 5 */
    {UDATA(&mt_srv, UNIT_MT(0), 0), 0, UNIT_ADDR(0x186)},       /* 6 */
    {UDATA(&mt_srv, UNIT_MT(0), 0), 0, UNIT_ADDR(0x187)},       /* 7 */
};

struct dib mta_dib = { 0xF8, NUM_UNITS_MT, NULL, mt_startcmd, NULL, mta_unit, mt_ini};

DEVICE              mta_dev = {
    "MTA", mta_unit, NULL, mt_mod,
    NUM_UNITS_MT, 8, 15, 1, 8, 8,
    NULL, NULL, &mt_reset, &mt_boot, &mt_attach, &mt_detach,
    &mta_dib, DEV_BUF_NUM(0) | DEV_DISABLE | DEV_DEBUG | DEV_TAPE, 0, dev_debug
};

#if NUM_DEVS_MT > 1
UNIT                mtb_unit[] = {
    {UDATA(&mt_srv, UNIT_MT(1), 0), 0, UNIT_ADDR(0x280)},       /* 0 */
    {UDATA(&mt_srv, UNIT_MT(1), 0), 0, UNIT_ADDR(0x281)},       /* 1 */
    {UDATA(&mt_srv, UNIT_MT(1), 0), 0, UNIT_ADDR(0x282)},       /* 2 */
    {UDATA(&mt_srv, UNIT_MT(1), 0), 0, UNIT_ADDR(0x283)},       /* 3 */
    {UDATA(&mt_srv, UNIT_MT(1), 0), 0, UNIT_ADDR(0x284)},       /* 4 */
    {UDATA(&mt_srv, UNIT_MT(1), 0), 0, UNIT_ADDR(0x285)},       /* 5 */
    {UDATA(&mt_srv, UNIT_MT(1), 0), 0, UNIT_ADDR(0x286)},       /* 6 */
    {UDATA(&mt_srv, UNIT_MT(1), 0), 0, UNIT_ADDR(0x287)},       /* 7 */
};

struct dib mtb_dib = { 0xF8, NUM_UNITS_MT, NULL, mt_startcmd, NULL, mtb_unit, mt_ini};

DEVICE              mtb_dev = {
    "MTB", mtb_unit, NULL, mt_mod,
    NUM_UNITS_MT, 8, 15, 1, 8, 8,
    NULL, NULL, &mt_reset, &mt_boot, &mt_attach, &mt_detach,
    &mtb_dib, DEV_BUF_NUM(1) | DEV_DISABLE | DEV_DEBUG | DEV_TAPE, 0, dev_debug
};
#endif

uint8               parity_table[64] = {
    /* 0    1    2    3    4    5    6    7 */
    0000, 0100, 0100, 0000, 0100, 0000, 0000, 0100,
    0100, 0000, 0000, 0100, 0000, 0100, 0100, 0000,
    0100, 0000, 0000, 0100, 0000, 0100, 0100, 0000,
    0000, 0100, 0100, 0000, 0100, 0000, 0000, 0100,
    0100, 0000, 0000, 0100, 0000, 0100, 0100, 0000,
    0000, 0100, 0100, 0000, 0100, 0000, 0000, 0100,
    0000, 0100, 0100, 0000, 0100, 0000, 0000, 0100,
    0100, 0000, 0000, 0100, 0000, 0100, 0100, 0000
};

uint8                  bcd_to_ebcdic[64] = {
     0x40, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
     0xf8, 0xf9, 0xf0, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
     0x7a, 0x61, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
     0xe8, 0xe9, 0xe0, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
     0x60, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
     0xd8, 0xd9, 0xd0, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
     0x50, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
     0xc8, 0xc9, 0xc0, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f};


uint8  mt_startcmd(UNIT *uptr, uint16 chan,  uint8 cmd) {
    uint16         addr = GET_UADDR(uptr->u3);
    DEVICE         *dptr = find_dev_from_unit(uptr);
    int            unit = (uptr - dptr->units);

    if (mt_busy[GET_DEV_BUF(dptr->flags)] != 0 || (uptr->u3 & MT_CMDMSK) != 0) {
        sim_debug(DEBUG_CMD, dptr, "CMD busy unit=%d %x\n", unit, cmd);
        uptr->flags |= MT_BUSY;   /* Flag we need to send CUE */
        return SNS_BSY;
    }

    sim_debug(DEBUG_CMD, dptr, "CMD unit=%d %x\n", unit, cmd);

    switch (cmd & 0xF) {
    case 0x7:              /* Tape motion */
    case 0xf:              /* Tape motion */
    case 0x1:              /* Write command */
    case 0x2:              /* Read command */
    case 0xc:              /* Read backward */
         uptr->u5 = 0;
         uptr->u5 |= SNS_TUASTA << 8;
         if ((uptr->flags & MTUF_9TR) == 0)
             uptr->u5 |= (SNS_7TRACK << 8);
         if (sim_tape_wrp(uptr))
             uptr->u5 |= (SNS_WRP << 8);
         if (sim_tape_bot(uptr))
             uptr->u5 |= (SNS_LOAD << 8);
          /* Fall through */

    case 0x4:              /* Sense */
         uptr->u3 &= ~(MT_CMDMSK);
         uptr->u3 |= cmd & MT_CMDMSK;
         sim_activate(uptr, 1000);       /* Start unit off */
         CLR_BUF(uptr);
         uptr->u4 = 0;
         uptr->u6 = 0;
         mt_busy[GET_DEV_BUF(dptr->flags)] = 1;
         if ((cmd & 0x7) == 0x7)           /* Quick end channel on control */
             return SNS_CHNEND;
         return 0;

    case 0x3:              /* Control */
    case 0xb:              /* Control */
         if ((uptr->flags & UNIT_ATT) == 0) {
             uptr->u5 |= SNS_INTVENT;
             return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;
         }
         if ((uptr->flags & MTUF_9TR) == 0)  {
             uptr->u5 |= (SNS_7TRACK << 8);
             if ((cmd & 0xc0) == 0xc0) {
                 uptr->u5 |= SNS_CMDREJ;
                 return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;
             }
             switch((cmd >> 3) & 07) {
             case 0:      /* NOP */
             case 1:      /* Diagnostics */
             case 3:
                  return SNS_CHNEND|SNS_DEVEND ;
             case 2:      /* Reset condition */
                  uptr->u3 &= ~(MT_ODD|MT_TRANS|MT_CONV|MT_MDEN_MSK);
                  uptr->u3 |= (cmd & MT_MDEN_MSK) | MT_ODD | MT_CONV;
                  break;
             case 4:
                  uptr->u3 &= ~(MT_ODD|MT_TRANS|MT_CONV|MT_MDEN_MSK);
                  uptr->u3 |= (cmd & MT_MDEN_MSK);
                  break;
             case 5:
                  uptr->u3 &= ~(MT_ODD|MT_TRANS|MT_CONV|MT_MDEN_MSK);
                  uptr->u3 |= (cmd & MT_MDEN_MSK) | MT_TRANS;
                  break;
             case 6:
                  uptr->u3 &= ~(MT_ODD|MT_TRANS|MT_CONV|MT_MDEN_MSK);
                  uptr->u3 |= (cmd & MT_MDEN_MSK) | MT_ODD;
                  break;
             case 7:
                  uptr->u3 &= ~(MT_ODD|MT_TRANS|MT_CONV|MT_MDEN_MSK);
                  uptr->u3 |= (cmd & MT_MDEN_MSK) | MT_ODD | MT_TRANS;
                  break;
             }
         } else {
             uptr->u3 &= ~MT_MDEN_MSK;
             if (cmd & 0x8)
                 uptr->u3 |= MT_MDEN_800;
             else
                 uptr->u3 |= MT_MDEN_1600;
         }
         uptr->u5 = 0;
         break;

    case 0x0:               /* Status */
         break;

    default:                /* invalid command */
         uptr->u5 |= SNS_CMDREJ;
         break;
    }
    if (uptr->u5 & 0xff)
        return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;
    return SNS_CHNEND|SNS_DEVEND;
}

/* Map simH errors into machine errors */
t_stat mt_error(UNIT * uptr, uint16 addr, t_stat r, DEVICE * dptr)
{
    mt_busy[GET_DEV_BUF(dptr->flags)] &= ~1;
    switch (r) {
    case MTSE_OK:              /* no error */
       break;

    case MTSE_TMK:              /* tape mark */
       sim_debug(DEBUG_EXP, dptr, "MARK ");
       chan_end(addr, SNS_CHNEND|SNS_DEVEND|SNS_UNITEXP);
       return SCPE_OK;

    case MTSE_WRP:              /* write protected */
    case MTSE_UNATT:              /* unattached */
       sim_debug(DEBUG_EXP, dptr, "ATTENTION %d ", r);
       break;

    case MTSE_IOERR:              /* IO error */
    case MTSE_FMT:              /* invalid format */
    case MTSE_RECE:              /* error in record */
       sim_debug(DEBUG_EXP, dptr, "ERROR %d ", r);
       break;
    case MTSE_BOT:              /* beginning of tape */
       sim_debug(DEBUG_EXP, dptr, "BOT ");
       break;
    case MTSE_INVRL:              /* invalid rec lnt */
       break;
    case MTSE_EOM:              /* end of medium */
       sim_debug(DEBUG_EXP, dptr, "EOT ");
       uptr->u5 = SNS_EQUCHK;
       chan_end(addr, SNS_CHNEND|SNS_DEVEND|SNS_UNITEXP);
       return SCPE_OK;
    }
    chan_end(addr, SNS_CHNEND|SNS_DEVEND);
    return SCPE_OK;
}

/* Handle processing of tape requests. */
t_stat mt_srv(UNIT * uptr)
{
    uint16              addr = GET_UADDR(uptr->u3);
    DEVICE             *dptr = find_dev_from_unit(uptr);
    int                 unit = (uptr - dptr->units);
    int                 cmd = uptr->u3 & MT_CMDMSK;
    int                 bufnum = GET_DEV_BUF(dptr->flags);
    t_mtrlnt            reclen;
    t_stat              r = SCPE_ARG;       /* Force error if not set */
    uint8               ch;
    int                 mode = 0;

    if ((uptr->flags & UNIT_ATT) == 0) {
        uptr->u5 |= SNS_INTVENT;
        if (cmd != MT_SENSE) {
            uptr->u3 &= ~(MT_CMDMSK);
            mt_busy[bufnum] &= ~1;
            chan_end(addr, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
            return SCPE_OK;
        }
    }

    switch (cmd & 0xf) {
    case 0:                               /* No command, stop tape */
         sim_debug(DEBUG_DETAIL, dptr, "Idle unit=%d\n", unit);
         break;

    case MT_SENSE:
         ch = uptr->u5 & 0xff;
         sim_debug(DEBUG_DETAIL, dptr, "sense unit=%d 1 %x\n", unit, ch);
         chan_write_byte(addr, &ch) ;
         ch = (uptr->u5 >> 8) & 0xff;
         sim_debug(DEBUG_DETAIL, dptr, "sense unit=%d 2 %x\n", unit, ch);
         chan_write_byte(addr, &ch) ;
         ch = 0xc0;
         sim_debug(DEBUG_DETAIL, dptr, "sense unit=%d 3 %x\n", unit, ch);
         chan_write_byte(addr, &ch) ;
         ch = (uptr->u5 >> 16) & 0xff;
         sim_debug(DEBUG_DETAIL, dptr, "sense unit=%d 4 %x\n", unit, ch);
         chan_write_byte(addr, &ch) ;
         ch = 0;
         chan_write_byte(addr, &ch) ;
         chan_write_byte(addr, &ch);
         uptr->u3 &= ~MT_CMDMSK;
         mt_busy[bufnum] &= ~1;
         chan_end(addr, SNS_CHNEND|SNS_DEVEND);
         break;

    case MT_READ:

         if (uptr->u3 & MT_READDONE) {
            uptr->u3 &= ~(MT_CMDMSK|MT_READDONE);
            mt_busy[bufnum] &= ~1;
            chan_end(addr, SNS_CHNEND|SNS_DEVEND);
            break;
         }

         /* If empty buffer, fill */
         if (BUF_EMPTY(uptr)) {
             sim_debug(DEBUG_DETAIL, dptr, "Read unit=%d ", unit);
             if ((r = sim_tape_rdrecf(uptr, &mt_buffer[bufnum][0], &reclen,
                              BUFFSIZE)) != MTSE_OK) {
                 sim_debug(DEBUG_DETAIL, dptr, " error %d\n", r);
                 uptr->u3 &= ~(MT_CMDMSK|MT_READDONE);
                 return mt_error(uptr, addr, r, dptr);
             }
             uptr->u4 = 0;
             uptr->hwmark = reclen;
             sim_debug(DEBUG_DETAIL, dptr, "Block %d chars\n", reclen);
         }

         ch = mt_buffer[bufnum][uptr->u4++];
         /* if we are a 7track tape, handle conversion */
         if ((uptr->flags & MTUF_9TR) == 0) {
             mode = (uptr->u3 & MT_ODD) ? 0 : 0100;
             if ((parity_table[ch & 077] ^ (ch & 0100) ^ mode) == 0) {
                 sim_debug(DEBUG_DETAIL, dptr, "Parity error unit=%d %d %03o\n",
                       unit, uptr->u4-1, ch);
                 uptr->u5 |= (SNS_VRC << 16) | SNS_DATCHK;
             }
             ch &= 077;
             if (uptr->u3 & MT_TRANS)
                 ch = bcd_to_ebcdic[ch];
             if (uptr->u3 & MT_CONV) {
                 sim_debug(DEBUG_DATA, dptr, "Read raw data unit=%d %d %02x %02x\n",
                       unit, uptr->u4, ch, uptr->u6);
                 if (uptr->u6 == 0 && (t_addr)uptr->u4 < uptr->hwmark) {
                     uptr->u6 = MT_CONV1 | ch;
                     sim_activate(uptr, 20);
                     return SCPE_OK;
                 } else if ((uptr->u6 & 0xc0) == MT_CONV1) {
                     int t = uptr->u6 & 0x3F;
                     uptr->u6 = MT_CONV2 | ch;
                     ch = (t << 2) | ((ch >> 4) & 03);
                 } else if ((uptr->u6 & 0xc0) == MT_CONV2) {
                     int  t = uptr->u6 & 0xf;
                     uptr->u6 = MT_CONV3 | ch;
                     ch = (t << 4) | ((ch >> 2) & 0xf);
                 } else if ((uptr->u6 & 0xc0) == MT_CONV3) {
                     ch |= ((uptr->u6 & 0x3) << 6);
                     uptr->u6 = 0;
                 }
             }
         }

         /* Send character over to channel */
         if (chan_write_byte(addr, &ch)) {
             sim_debug(DEBUG_DATA, dptr, "Read unit=%d EOR\n\r", unit);
             /* If not read whole record, skip till end */
             if ((t_addr)uptr->u4 < uptr->hwmark) {
                 /* Send dummy character to force SLI */
                 chan_write_byte(addr, &ch);
                 sim_activate(uptr, (uptr->hwmark-uptr->u4) * 20);
                 uptr->u3 |= MT_READDONE;
                 break;
             }
             uptr->u3 &= ~MT_CMDMSK;
             mt_busy[bufnum] &= ~1;
             chan_end(addr, SNS_DEVEND);
         } else {
              sim_debug(DEBUG_DATA, dptr, "Read data unit=%d %d %02x\n\r",
                       unit, uptr->u4, ch);
              if ((t_addr)uptr->u4 >= uptr->hwmark) {       /* In IRG */
                  /* Handle end of record */
                  uptr->u3 &= ~MT_CMDMSK;
                  mt_busy[bufnum] &= ~1;
                  chan_end(addr, SNS_CHNEND|SNS_DEVEND);
             } else
                  sim_activate(uptr, 20);
         }
         break;


    case MT_WRITE:
         /* Check if write protected */
         if (sim_tape_wrp(uptr)) {
             uptr->u5 |= SNS_CMDREJ;
             uptr->u3 &= ~MT_CMDMSK;
             mt_busy[bufnum] &= ~1;
             chan_end(addr, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
             break;
         }

         /* Grab data until channel has no more */
         if (chan_read_byte(addr, &ch)) {
             if (uptr->u4 > 0) {                      /* Only if data in record */
                 reclen = uptr->hwmark;
                 sim_debug(DEBUG_DETAIL, dptr, "Write unit=%d Block %d chars\n",
                          unit, reclen);
                 r = sim_tape_wrrecf(uptr, &mt_buffer[bufnum][0], reclen);
                 uptr->u4 = 0;
                 uptr->u3 &= ~MT_CMDMSK;
                 mt_error(uptr, addr, r, dptr);       /* Record errors */
             } else {
                 uptr->u5 |= SNS_WCZERO;              /* Write with no data */
             }
         } else {
             if ((uptr->flags & MTUF_9TR) == 0) {
                 mode = (uptr->u3 & MT_ODD) ? 0 : 0100;
                 if (uptr->u3 & MT_TRANS)
                     ch = (ch & 0xf) | ((ch & 0x30) ^ 0x30);
                 if (uptr->u3 & MT_CONV) {
                     if (uptr->u6 == 0) {
                         uptr->u6 = MT_CONV1 | (ch & 0x3);
                         ch >>= 2;
                     } else if ((uptr->u6 & 0xc0) == MT_CONV1) {
                         int t = uptr->u6 & 0x3;
                         uptr->u6 = MT_CONV2 | (ch & 0xf);
                         ch = (t << 4) | ((ch >> 4) & 0xf);
                    } else if ((uptr->u6 & 0xc0) == MT_CONV2) {
                         int  t = uptr->u6 & 0xf;
                         ch = (t << 2) | ((ch >> 6) & 0x3);
                         ch ^= parity_table[ch & 077] ^ mode;
                         mt_buffer[bufnum][uptr->u4++] = ch;
                         uptr->u6 = 0;
                    }
                }
                ch &= 077;
                ch |= parity_table[ch] ^ mode;
             }
             mt_buffer[bufnum][uptr->u4++] = ch;
             sim_debug(DEBUG_DATA, dptr, "Write data unit=%d %d %02o\n\r",
                      unit, uptr->u4, ch);
             uptr->hwmark = uptr->u4;
             break;
         }
         sim_activate(uptr, 20);
         break;

    case MT_RDBK:
         if (uptr->u3 & MT_READDONE) {
            uptr->u3 &= ~(MT_CMDMSK|MT_READDONE);
            mt_busy[bufnum] &= ~1;
            chan_end(addr, SNS_CHNEND|SNS_DEVEND);
            return SCPE_OK;
         }

         /* If at end of record, fill buffer */
         if (BUF_EMPTY(uptr)) {
              if (sim_tape_bot(uptr)) {
                  uptr->u3 &= ~MT_CMDMSK;
                  mt_busy[GET_DEV_BUF(dptr->flags)] &= ~1;
                  chan_end(addr, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
                  return SCPE_OK;
              }
             sim_debug(DEBUG_DETAIL, dptr, "Read backward unit=%d ", unit);
             if ((r = sim_tape_rdrecr(uptr, &mt_buffer[bufnum][0], &reclen,
                                BUFFSIZE)) != MTSE_OK) {
                  uptr->u3 &= ~(MT_CMDMSK|MT_READDONE);
                  return mt_error(uptr, addr, r, dptr);
             }
             uptr->u4 = reclen;
             uptr->hwmark = reclen;
             sim_debug(DEBUG_DETAIL, dptr, "Binary Block %d chars\n", reclen);
         }

         ch = mt_buffer[bufnum][--uptr->u4];
         if ((uptr->flags & MTUF_9TR) == 0) {
             mode = (uptr->u3 & MT_ODD) ? 0 : 0100;
             ch &= 077;
             if ((parity_table[ch & 077] ^ (ch & 0100) ^ mode) == 0) {
                 uptr->u5 |= (SNS_VRC << 16) | SNS_DATCHK;
             }
             if (uptr->u3 & MT_TRANS)
                 ch = bcd_to_ebcdic[ch];
             if (uptr->u3 & MT_CONV) {
                 if (uptr->u6 == 0 && (t_addr)uptr->u4 < uptr->hwmark) {
                     uptr->u6 = MT_CONV1 | ch;
                     sim_activate(uptr, 20);
                     return SCPE_OK;
                 } else if ((uptr->u6 & 0xc0) == MT_CONV1) {
                     int t = uptr->u6 & 0x3F;
                     uptr->u6 = MT_CONV2 | ch;
                     ch = t | ((ch << 6) & 0xc0);
                 } else if ((uptr->u6 & 0xc0) == MT_CONV2) {
                     int  t = uptr->u6 & 0x3C;
                     uptr->u6 = MT_CONV3 | ch;
                     ch = (t >> 2) | ((ch << 4) & 0xf0);
                 } else if ((uptr->u6 & 0xc0) == MT_CONV3) {
                     ch |= ((uptr->u6 & 0x30) >> 4);
                     uptr->u6 = 0;
                 }
             }
         }

         if (chan_write_byte(addr, &ch)) {
                   sim_debug(DEBUG_DATA, dptr, "Read unit=%d EOR\n\r", unit);
              /* If not read whole record, skip till end */
              if (uptr->u4 >= 0) {
                  sim_activate(uptr, (uptr->u4) * 20);
                  uptr->u3 |= MT_READDONE;
                  return SCPE_OK;
              }
              uptr->u3 &= ~MT_CMDMSK;
              mt_busy[bufnum] &= ~1;
              chan_end(addr, SNS_CHNEND|SNS_DEVEND);
         } else {
              sim_debug(DEBUG_DATA, dptr, "Read data unit=%d %d %02o\n\r",
                            unit, uptr->u4, ch);
              if (uptr->u4 == 0) {      /* In IRG */
                  uptr->u3 &= ~MT_CMDMSK;
                  mt_busy[bufnum] &= ~1;
                  chan_end(addr, SNS_CHNEND|SNS_DEVEND);
               } else
                  sim_activate(uptr, 20);
         }
         break;
    case 0x7:
    case 0xf:
         switch (cmd) {
         case MT_WTM:
              if (uptr->u4 == 0) {
                 if (sim_tape_wrp(uptr)) {
                     uptr->u5 |= SNS_CMDREJ;
                     uptr->u3 &= ~MT_CMDMSK;
                     mt_busy[GET_DEV_BUF(dptr->flags)] &= ~1;
                     set_devattn(addr, SNS_DEVEND|SNS_UNITCHK);
                     return SCPE_OK;
                 }
                 uptr->u4 ++;
                 sim_activate(uptr, 500);
              } else {
                 sim_debug(DEBUG_DETAIL, dptr, "Write Mark unit=%d\n", unit);
                 uptr->u3 &= ~(MT_CMDMSK);
                 r = sim_tape_wrtmk(uptr);
                 set_devattn(addr, SNS_DEVEND);
                 mt_busy[bufnum] &= ~1;
              }
              break;

         case MT_BSR:
              switch (uptr->u4 ) {
              case 0:
                   if (sim_tape_bot(uptr)) {
                       uptr->u3 &= ~MT_CMDMSK;
                       mt_busy[GET_DEV_BUF(dptr->flags)] &= ~1;
                       set_devattn(addr, SNS_DEVEND|SNS_UNITCHK);
                       return SCPE_OK;
                   }
                   uptr->u4 ++;
                   sim_activate(uptr, 500);
                   break;
              case 1:
                   uptr->u4++;
                   sim_debug(DEBUG_DETAIL, dptr, "Backspace rec unit=%d ", unit);
                   r = sim_tape_sprecr(uptr, &reclen);
                   /* We don't set EOF on BSR */
                   if (r == MTSE_TMK) {
                       uptr->u4++;
                       sim_debug(DEBUG_DETAIL, dptr, "MARK\n");
                       sim_activate(uptr, 50);
                   } else {
                       sim_debug(DEBUG_DETAIL, dptr, "%d \n", reclen);
                       sim_activate(uptr, 10 + (10 * reclen));
                   }
                   break;
              case 2:
                   uptr->u3 &= ~(MT_CMDMSK);
                   set_devattn(addr, SNS_DEVEND);
                   mt_busy[bufnum] &= ~1;
                   break;
              case 3:
                   uptr->u3 &= ~(MT_CMDMSK);
                   set_devattn(addr, SNS_DEVEND|SNS_UNITEXP);
                   mt_busy[bufnum] &= ~1;
                   break;
              }
              break;

         case MT_BSF:
              switch(uptr->u4) {
              case 0:
                   if (sim_tape_bot(uptr)) {
                       uptr->u3 &= ~MT_CMDMSK;
                       mt_busy[bufnum] &= ~1;
                       set_devattn(addr, SNS_DEVEND|SNS_UNITCHK);
                       break;
                    }
                    uptr->u4 ++;
                    sim_activate(uptr, 500);
                    break;
              case 1:
                   sim_debug(DEBUG_DETAIL, dptr, "Backspace file unit=%d\n", unit);
                   r = sim_tape_sprecr(uptr, &reclen);
                   if (r == MTSE_TMK) {
                       uptr->u4++;
                       sim_debug(DEBUG_DETAIL, dptr, "MARK\n");
                       sim_activate(uptr, 50);
                    } else if (r == MTSE_BOT) {
                       uptr->u4+= 2;
                       sim_activate(uptr, 50);
                    } else {
                       sim_activate(uptr, 10 + (10 * reclen));
                    }
                    break;
              case 2:
                   uptr->u3 &= ~(MT_CMDMSK);
                   set_devattn(addr, SNS_DEVEND|SNS_UNITEXP);
                   mt_busy[bufnum] &= ~1;
                   break;
              case 3:
                   uptr->u3 &= ~(MT_CMDMSK);
                   set_devattn(addr, SNS_DEVEND|SNS_UNITCHK);
                   mt_busy[bufnum] &= ~1;
                   break;
              }
              break;

         case MT_FSR:
              switch(uptr->u4) {
              case 0:
                   uptr->u4 ++;
                   sim_activate(uptr, 500);
                   break;
              case 1:
                   uptr->u4++;
                   sim_debug(DEBUG_DETAIL, dptr, "Skip rec unit=%d ", unit);
                   r = sim_tape_sprecf(uptr, &reclen);
                   if (r == MTSE_TMK) {
                       uptr->u4 = 3;
                       sim_debug(DEBUG_DETAIL, dptr, "MARK\n");
                       sim_activate(uptr, 50);
                   } else if (r == MTSE_EOM) {
                       uptr->u4 = 4;
                       sim_activate(uptr, 50);
                   } else {
                       sim_debug(DEBUG_DETAIL, dptr, "%d\n", reclen);
                       sim_activate(uptr, 10 + (10 * reclen));
                   }
                   break;
              case 2:
                   uptr->u3 &= ~(MT_CMDMSK);
                   set_devattn(addr, SNS_DEVEND);
                   mt_busy[bufnum] &= ~1;
                   break;
              case 3:
                   uptr->u3 &= ~(MT_CMDMSK);
                   set_devattn(addr, SNS_DEVEND|SNS_UNITEXP);
                   mt_busy[bufnum] &= ~1;
                   break;
              case 4:
                   uptr->u3 &= ~(MT_CMDMSK);
                   set_devattn(addr, SNS_DEVEND|SNS_UNITCHK);
                   mt_busy[bufnum] &= ~1;
                   break;
              }
              break;

         case MT_FSF:
              switch(uptr->u4) {
              case 0:
                   uptr->u4 ++;
                   sim_activate(uptr, 500);
                   break;
              case 1:
                   sim_debug(DEBUG_DETAIL, dptr, "Skip rec unit=%d ", unit);
                   r = sim_tape_sprecf(uptr, &reclen);
                   if (r == MTSE_TMK) {
                       uptr->u4++;
                       sim_debug(DEBUG_DETAIL, dptr, "MARK\n");
                       sim_activate(uptr, 50);
                   } else if (r == MTSE_EOM) {
                       uptr->u4+= 2;
                       sim_activate(uptr, 50);
                   } else {
                       sim_debug(DEBUG_DETAIL, dptr, "%d\n", reclen);
                       sim_activate(uptr, 10 + (10 * reclen));
                   }
                   break;
              case 2:
                   uptr->u3 &= ~(MT_CMDMSK);
                   set_devattn(addr, SNS_DEVEND);
                   mt_busy[bufnum] &= ~1;
                   sim_debug(DEBUG_DETAIL, dptr, "Skip done unit=%d\n", unit);
                   break;
              case 3:
                   uptr->u3 &= ~(MT_CMDMSK);
                   set_devattn(addr, SNS_DEVEND|SNS_UNITCHK);
                   mt_busy[bufnum] &= ~1;
                   break;
              }
              break;

         case MT_ERG:
              switch (uptr->u4) {
              case 0:
                   if (sim_tape_wrp(uptr)) {
                       uptr->u5 |= SNS_CMDREJ;
                       uptr->u3 &= ~MT_CMDMSK;
                       mt_busy[bufnum] &= ~1;
                       set_devattn(addr, SNS_DEVEND|SNS_UNITCHK);
                   } else {
                       uptr->u4 ++;
                       sim_activate(uptr, 500);
                   }
                   break;
              case 1:
                   sim_debug(DEBUG_DETAIL, dptr, "Erase unit=%d\n", unit);
                   r = sim_tape_wrgap(uptr, 35);
                   sim_activate(uptr, 5000);
                   uptr->u4++;
                   break;
              case 2:
                   uptr->u3 &= ~(MT_CMDMSK);
                   set_devattn(addr, SNS_DEVEND);
                   mt_busy[bufnum] &= ~1;
              }
              break;

         case MT_REW:
              if (uptr->u4 == 0) {
                  uptr->u4 ++;
                  sim_activate(uptr, 30000);
                  mt_busy[bufnum] &= ~1;
              } else {
                  sim_debug(DEBUG_DETAIL, dptr, "Rewind unit=%d\n", unit);
                  uptr->u3 &= ~(MT_CMDMSK);
                  r = sim_tape_rewind(uptr);
                  set_devattn(addr, SNS_DEVEND);
              }
              break;

         case MT_RUN:
              if (uptr->u4 == 0) {
                  uptr->u4 ++;
                  mt_busy[bufnum] &= ~1;
                  sim_activate(uptr, 30000);
              } else {
                  sim_debug(DEBUG_DETAIL, dptr, "Unload unit=%d\n", unit);
                  uptr->u3 &= ~(MT_CMDMSK);
                  r = sim_tape_detach(uptr);
                  set_devattn(addr, SNS_DEVEND);
              }
              break;
         }
    }
    return SCPE_OK;
}

void
mt_ini(UNIT * uptr, t_bool f)
{
    DEVICE             *dptr = find_dev_from_unit(uptr);

    uptr->u3 &= UNIT_ADDR_MASK;
    if ((uptr->flags & MTUF_9TR) == 0)
        uptr->u3 |= MT_ODD|MT_CONV|MT_MDEN_800;
    mt_busy[GET_DEV_BUF(dptr->flags)] = 0;
}

t_stat
mt_reset(DEVICE * dptr)
{
    return SCPE_OK;
}

t_stat
mt_attach(UNIT * uptr, CONST char *file)
{
    uint16              addr = GET_UADDR(uptr->u3);
    t_stat              r;

    if ((r = sim_tape_attach_ex(uptr, file, 0, 0)) != SCPE_OK)
       return r;
    set_devattn(addr, SNS_DEVEND);
    uptr->u3 &= UNIT_ADDR_MASK;
    uptr->u4 = 0;
    uptr->u5 = 0;
    return SCPE_OK;
}

t_stat
mt_detach(UNIT * uptr)
{
    uptr->u3 &= UNIT_ADDR_MASK;
    uptr->u4 = 0;
    uptr->u5 = 0;
    return sim_tape_detach(uptr);
}

t_stat
mt_boot(int32 unit_num, DEVICE * dptr)
{
    UNIT               *uptr = &dptr->units[unit_num];

    if ((uptr->flags & UNIT_ATT) == 0)
        return SCPE_UNATT;      /* attached? */
    if ((uptr->flags & MTUF_9TR) == 0)  {
        uptr->u3 &= UNIT_ADDR_MASK;
        uptr->u3 |= MT_ODD|MT_CONV|MT_MDEN_800;
    }
    return chan_boot(GET_UADDR(uptr->u3), dptr);
}

#endif
