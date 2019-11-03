/* sel32_hsdp.c: SEL-32 8064 High Speed Disk Processor

   Copyright (c) 2018-2019, James C. Bevier
   Portions provided by Richard Cornwell and other SIMH contributers

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
*/

#include "sel32_defs.h"

extern t_stat set_dev_addr(UNIT * uptr, int32 val, CONST char *cptr, void *desc);
extern t_stat show_dev_addr(FILE * st, UNIT * uptr, int32 v, CONST void *desc);
extern void chan_end(uint16 chan, uint8 flags);
extern int  chan_read_byte(uint16 chsa, uint8 *data);
extern int  chan_write_byte(uint16 chsa, uint8 *data);
extern void set_devattn(uint16 addr, uint8 flags);
extern t_stat chan_boot(uint16 addr, DEVICE *dptr);
extern int test_write_byte_end(uint16 chsa);

extern uint32   M[];                            /* our memory */
extern uint32   SPAD[];                         /* cpu SPAD memory */

#ifdef NUM_DEVS_HSDP
#define UNIT_V_TYPE        (UNIT_V_UF + 0)
#define UNIT_TYPE          (0xf << UNIT_V_TYPE)

#define GET_TYPE(x)        ((UNIT_TYPE & (x)) >> UNIT_V_TYPE)
#define SET_TYPE(x)        (UNIT_TYPE & ((x) << UNIT_V_TYPE))
#define UNIT_HSDP          UNIT_ATTABLE | UNIT_IDLE

/* INCH command information */
/*
WD 0 - Data address
WD 1 - Flags - 0 -36 byte count

Data - 224 word INCH buffer address
WD 1 Drive 0 Attribute register
WD 2 Drive 1 Attribute register
WD 3 Drive 2 Attribute register
WD 4 Drive 3 Attribute register
WD 5 Drive 4 Attribute register
WD 6 Drive 5 Attribute register
WD 7 Drive 6 Attribute register
WD 8 Drive 7 Attribute register

Memory attribute register layout
bits 0-7 - Flags
        bits 0&1 - 00=Reserved, 01=MHD, 10=FHD, 11=MHD with FHD option
        bit  2   - 1=Cartridge module drive
        bit  3   - 0=Reserved
        bit  4   - 1=Drive not present
        bit  5   - 1=Dual Port
        bit  6   - 0=Reserved
        bit  7   - 0=Reserved
bits 8-15 - sector count (sectors per track)(F16=16, F20=20)
bits 16-23 - MHD Head count (number of heads on MHD)
bits 24-31 - FHD head count (number of heads on FHD or number head on FHD option of
  mini-module)
*/


/* 224 word INCH Buffer layout */
/* 128 word subchannel status storage (SST) */
/*  66 words of program status queue (PSQ) */
/*  26 words of scratchpad */
/*   4 words of label buffer registers */

#define CMD        u3
/* u3 */
/* in u3 is device command code and status */
#define DSK_CMDMSK       0x00ff                 /* Command being run */
#define DSK_STAR         0x0100                 /* STAR value in u4 */
#define DSK_NU2          0x0200                 /*                    */
#define DSK_READDONE     0x0400                 /* Read finished, end channel */
#define DSK_ENDDSK       0x0800                 /* Sensed end of disk */
#define DSK_SEEKING      0x1000                 /* Disk is currently seeking */
#define DSK_READING      0x2000                 /* Disk is reading data */
#define DSK_WRITING      0x4000                 /* Disk is writing data */
#define DSK_BUSY         0x8000                 /* Flag to send a CUE */
/* commands */
#define DSK_INCH           0x00                 /* Initialize channel */
#define DSK_WD             0x01                 /* Write data */
#define DSK_RD             0x02                 /* Read data */
#define DSK_NOP            0x03                 /* No operation */
#define DSK_SNS            0x04                 /* Sense */
#define DSK_SCK            0x07                 /* Seek cylinder, track, sector */
#define DSK_TIC            0x08                 /* Transfer in channel */
#define DSK_FNSK           0x0B                 /* Format for no skip */
#define DSK_LPL            0x13                 /* Lock protected label */
#define DSK_LMR            0x1F                 /* Load mode register */
#define DSK_RES            0x23                 /* Reserve */
#define DSK_WSL            0x31                 /* Write sector label */
#define DSK_RSL            0x32                 /* Read sector label */
#define DSK_REL            0x33                 /* Release */
#define DSK_XEZ            0x37                 /* Rezero */
#define DSK_POR            0x43                 /* Priority Override */
#define DSK_IHA            0x47                 /* Increment head address */
#define DSK_SRM            0x4F                 /* Set reserve track mode */
#define DSK_WTL            0x51                 /* Write track label */
#define DSK_RTL            0x52                 /* Read track label */
#define DSK_XRM            0x5F                 /* Reset reserve track mode */
#define DSK_RAP            0xA2                 /* Read angular positions */
#define DSK_TESS           0xAB                 /* Test STAR (subchannel target address register) */
#define DSK_ICH            0xFF                 /* Initialize Controller */

#define STAR      u4
/* u4 - sector target address register (STAR) */
/* Holds the current cylinder, head(track), sector */
#define DISK_CYL     0xFFFF0000                 /* cylinder mask */
#define DISK_TRACK   0x0000FF00                 /* track mask */
#define DISK_SECTOR  0x000000ff                 /* sector mask */

#define SNS       u5
/* u5 */
/* Sense byte 0  - mode register */
#define SNS_DROFF      0x80000000               /* Drive Carriage will be offset */
#define SNS_TRKOFF     0x40000000               /* Track offset: 0=positive, 1=negative */
#define SNS_RDTMOFF    0x20000000               /* Read timing offset = 1 */
#define SNS_RDSTRBT    0x10000000               /* Read strobe timing: 1=positive, 0=negative */
#define SNS_DIAGMOD    0x08000000               /* Diagnostic Mode ECC Code generation and checking */
#define SNS_RSVTRK     0x04000000               /* Reserve Track mode: 1=OK to write, 0=read only */
#define SNS_FHDOPT     0x02000000               /* FHD or FHD option = 1 */
#define SNS_RESERV     0x01000000               /* Reserved */

/* Sense byte 1 */
#define SNS_CMDREJ     0x800000                 /* Command reject */
#define SNS_INTVENT    0x400000                 /* Unit intervention required */
#define SNS_SPARE1     0x200000                 /* Spare */
#define SNS_EQUCHK     0x100000                 /* Equipment check */
#define SNS_DATCHK     0x080000                 /* Data Check */
#define SNS_OVRRUN     0x040000                 /* Data overrun/underrun */
#define SNS_DSKFERR    0x020000                 /* Disk format error */
#define SNS_DEFTRK     0x010000                 /* Defective track encountered */

/* Sense byte 2 */
#define SNS_LAST       0x8000                   /* Last track flag encountered */
#define SNS_AATT       0x4000                   /* At Alternate track */
#define SNS_WPER       0x2000                   /* Write protection error */
#define SNS_WRL        0x1000                   /* Write lock error */
#define SNS_MOCK       0x0800                   /* Mode check */
#define SNS_INAD       0x0400                   /* Invalid memory address */
#define SNS_RELF       0x0200                   /* Release fault */
#define SNS_CHER       0x0100                   /* Chaining error */

/* Sense byte 3 */
#define SNS_REVL       0x80                     /* Revolution lost */
#define SNS_DADE       0x40                     /* Disc addressing or seek error */
#define SNS_BUCK       0x20                     /* Buffer check */
#define SNS_ECCS       0x10                     /* ECC error in sector label */
#define SNS_ECCD       0x08                     /* ECC error iin data */
#define SNS_ECCT       0x04                     /* ECC error in track label */
#define SNS_RTAE       0x02                     /* Reserve track access error */
#define SNS_UESS       0x01                     /* Uncorrectable ECC error */

#define ATTR      u6
/* u6 holds drive attribute entry */
/* provided by inch command for controller */
/*
bits 0-7 - Flags
        bits 0&1 - 00=Reserved, 01=MHD, 10=FHD, 11=MHD with FHD option
        bit  2   - 1=Cartridge module drive
        bit  3   - 0=Reserved
        bit  4   - 1=Drive not present
        bit  5   - 1=Dual Port
        bit  6   - 0=Reserved
        bit  7   - 0=Reserved
bits 8-15 - sector count (sectors per track)(F16=16, F20=20)
bits 16-23 - MHD Head count (number of heads on MHD)
bits 24-31 - FHD head count (number of heads on FHD or number head on FHD option of
 mini-module)
*/

#define DDATA     up7
/* Pointer held in up7 */
/* sects/cylinder = sects/track * numhds */
/* allocated during attach command for each unit defined */
struct ddata_t
{
    uint16      cyl;                            /* Cylinder head at */
    uint16      tpos;                           /* Track position */
    uint16      spos;                           /* Sector position */
};

/* disk definition structure */
struct hsdp_t
{
    const char  *name;                          /* Device ID Name */
    uint32      taus;                           /* total allocation units */
    uint16      bms;                            /* bit map size */
    uint16      nhds;                           /* Number of heads */
    uint16      ssiz;                           /* sector size in words */
    uint16      spt;                            /* # sectors per track(cylinder) */
    uint8       spau;                           /* # sectors per allocation unit */
    uint8       spb;                            /* # sectors per block (256 WDS)*/
    uint32      cyl;                            /* Number of cylinders */
    uint8       type;                           /* Device type code */
}
hsdp_type[] =
{
    /* Class F Disc Devices */                                 /*XX CYL SIZE  */
    {"MH040",  20000,  625,   5, 256, 16, 2, 1,  400, 0x40},   /*0  411  40 M */
    {"MH080",  40000, 1250,   5, 256, 16, 2, 1,  800, 0x40},   /*1  823  80 M */
    {"MH160",  80000, 1250,  10, 256, 16, 4, 1, 1600, 0x40},   /*2  823 160 M */
    {"MH300",  76000, 2375,  19, 256, 16, 4, 1,  800, 0x40},   /*3  823 300 M */
    {"MH340",  76000, 2375,  24, 256, 16, 4, 1,  800, 0x40},   /*4  711 340 M */
    {"FH005",   5120,  184,   4, 256, 16, 1, 1,   64, 0x80},   /*5   64   5 M */
    {"CD032",   8000,  250,   1, 256, 16, 2, 1,  800, 0x60},   /*6  823  32 M */
    {"CD032",   8000,  250,   1, 256, 16, 2, 1,  800, 0x60},   /*7  823  32 M */
    {"CD064",   8000,  250,   1, 256, 16, 2, 1,  800, 0x60},   /*8  823  64 M */
    {"CD064",  24000,  750,   3, 256, 16, 2, 1,  800, 0x60},   /*9  823  64 M */
    {"CD096",   8000,  250,   1, 256, 16, 2, 1,  800, 0x60},   /*10 823  96 M */
    {"CD096",  40000, 1250,   5, 256, 16, 2, 1,  800, 0x60},   /*11 823  96 M */
    {"MH600",  80000, 2500,  40, 256, 16, 8, 1,  800, 0x40},   /*12 843 600 M */
    {"FM600",  80000, 2500,  40, 256, 16, 8, 1,  800, 0x40},   /*13 843 600 M */
    {"FM600",   1600,   50,  40, 256, 16, 1, 1,    2, 0x80},   /*14  10 600 M */
    {NULL, 0}
};

#if 0
*****************************************************************
*                  DEVICE ID TABLE
*****************************************************************
         SPACE
         BOUND     1W
DID.TBL  EQU       $    MPX1.X
*
*DEVICE ID NAME..................................................
*TOTAL ALLOC. UNITS.....................................        :
*BIT MAP SIZE      ..............................      :        :
*NO. OF HEADS      ........................     :      :        :
*SECTOR SIZE       ...................    :     :      :        :
*SECTORS/TRACK     ..............    :    :     :      :        :
*SECTORS/ALOC. UNIT..........   :    :    :     :      :        :
*SECTORS/BLOCK     .......  :   :    :    :     :      :        :
*OLD DEVICE ID NAME....  :  :   :    :    :     :      :        :
*                     :  :  :   :    :    :     :      :        :
*               ......:..:..:...:....:....:.....:......:........:
DID      FORM        32, 8, 8,  8,   8,  16,   16,    32,      64
         SPACE
*        CLASS 'F' EXTENDED I/O DISC DEVICES (MPX 1.X)
         DID    C'DF01', 3, 3, 26,  64,   2,     ,  1334, C'FL001'
         DID    C'DF02', 1, 2, 20, 192,   5,  625, 20000, C'MH040'
         DID    C'DF03', 1, 2, 20, 192,   5, 1250, 40000, C'MH080'
         DID    C'DF04', 1, 4, 20, 192,  19, 2375, 76000, C'MH300'
         DID    C'DF05', 1, 1, 20, 192,   4,  184,  5120, C'FH005'
         DID    C'DF06', 1, 2, 20, 192,   1,  250,  8000, C'CD032'
         DID    C'DF06', 1, 2, 20, 192,   1,  250,  8000, C'CD032'
         DID    C'DF07', 1, 2, 20, 192,   1,  250,  8000, C'CD064'
         DID    C'DF07', 1, 2, 20, 192,   3,  750, 24000, C'CD064'
         DID    C'DF08', 1, 2, 20, 192,   1,  250,  8000, C'CD096'
         DID    C'DF08', 1, 2, 20, 192,   5, 1250, 40000, C'CD096'
         DID    C'DF09', 1, 8, 20, 192,  40, 2500, 80000, C'MH600'
         DID    C'DF0A', 1, 8, 20, 192,  40, 2500, 80000, C'FM600'
         DID    C'DF0A', 1, 1, 20, 192,  40,   50,  1600, C'FM600'
*
         BOUND     1W
DID.TBL  EQU       $    MPX3.X
*
*DEVICE ID NAME..................................................
*TOTAL SECTORS .........................................        :
*BIT MAP SIZE      ..............................      :        :
*NO. OF HEADS      ........................     :      :        :
*SECTOR SIZE       ...................    :     :      :        :
*SECTORS/TRACK     ..............    :    :     :      :        :
*SECTORS/ALOC. UNIT..........   :    :    :     :      :        :
*SECTORS/BLOCK     .......  :   :    :    :     :      :        :
*OLD DEVICE ID NAME....  :  :   :    :    :     :      :        :
*                     :  :  :   :    :    :     :      :        :
*               ......:..:..:...:....:....:.....:......:........:
DID      FORM        32, 8, 8,  8,   8,  16,   16,    32,      64
         SPACE
*        CLASS 'F' EXTENDED I/O DISC DEVICES MPX3.X
         DID    C'DF01', 3, 1, 26,  64,   2,     ,  4004, C'FL001   '
         DID    C'DF02', 1, 2, 20, 192,   5,  642, 41100, C'MH040   '
         DID    C'DF03', 1, 2, 20, 192,   5, 1286, 82300, C'MH080   '
         DID    C'DF04', 1, 4, 20, 192,  19, 2444,312740, C'MH300   '
         DID    C'DF05', 1, 1, 20, 192,   4,  160,  5120, C'FH005   '
         DID    C'DF06', 1, 2, 20, 192,   1,  258, 16460, C'CD032   '
         DID    C'DF06', 1, 2, 20, 192,   1,  258, 16460, C'CD032   '
         DID    C'DF07', 1, 2, 20, 192,   1,  258, 16460, C'CD064   '
         DID    C'DF07', 1, 2, 20, 192,   3,  772, 49380, C'CD064   '
         DID    C'DF08', 1, 2, 20, 192,   1,  258, 16460, C'CD096   '
         DID    C'DF08', 1, 2, 20, 192,   5, 1286, 82300, C'CD096   '
         DID    C'DF09', 1,10, 20, 192,  40, 2635,674400, C'MH600   '
         DID    C'DF0A', 1,10, 20, 192,  40, 2635,674400, C'FM600   '
         DID    C'DF0A', 1, 1, 20, 192,  96,   60,  1920, C'FM600   '
         DID    C'DF0B', 1, 4, 20, 192,  10, 1286,164600, C'MH160   '
         DID    C'DF0C', 1, 2, 20, 192,   5, 1286,    00, C'ANY     '
         DID    C'DF0D', 1, 4, 20, 192,  24, 2670,341280, C'MH340   '
*
#endif

uint8   hsdp_preio(UNIT *uptr, uint16 chan) ;
uint8   hsdp_startcmd(UNIT *uptr, uint16 chan,  uint8 cmd) ;
uint8   hsdp_haltio(uint16 addr);
t_stat  hsdp_srv(UNIT *);
t_stat  hsdp_boot(int32, DEVICE *);
void    hsdp_ini(UNIT *, t_bool);
t_stat  hsdp_reset(DEVICE *);
t_stat  hsdp_attach(UNIT *, CONST char *);
t_stat  hsdp_detach(UNIT *);
t_stat  hsdp_set_type(UNIT * uptr, int32 val, CONST char *cptr, void *desc);
t_stat  hsdp_get_type(FILE * st, UNIT * uptr, int32 v, CONST void *desc);
t_stat  hsdp_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char  *hsdp_description (DEVICE *dptr);

/* channel program information */
CHANP           dpa_chp[NUM_UNITS_HSDP] = {0};

MTAB            hsdp_mod[] = {
    {MTAB_XTD | MTAB_VUN | MTAB_VALR, 0, "TYPE", "TYPE",
    &hsdp_set_type, &hsdp_get_type, NULL, "Type of disk"},
    {MTAB_XTD | MTAB_VUN | MTAB_VALR, 0, "DEV", "DEV", &set_dev_addr,
        &show_dev_addr, NULL, "Device channel address"},
    {0}
};

UNIT            dpa_unit[] = {
/* SET_TYPE(3) DM300 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0xC00)},       /* 0 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0xC02)},       /* 1 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0xC04)},       /* 2 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0xC06)},       /* 3 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0xC08)},       /* 4 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0xC0A)},       /* 5 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0xC0C)},       /* 6 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0xC0E)},       /* 7 */
};

DIB             dpa_dib = {
    hsdp_preio,                                 /* Pre start I/O */
    hsdp_startcmd,                              /* Start a command */
    NULL,                                       /* Stop I/O */
    NULL,                                       /* Test I/O */
    NULL,                                       /* Post I/O */
    hsdp_ini,                                   /* init function */
    dpa_unit,                                   /* Pointer to units structure */
    dpa_chp,                                    /* Pointer to chan_prg structure */
    NUM_UNITS_HSDP,                             /* number of units defined */
    0x0F,                                       /* 16 devices - device mask */
    0x0C00,                                     /* parent channel address */
    0,                                          /* fifo input index */
    0,                                          /* fifo output index */
    0,                                          /* interrupt status fifo for channel */
};

DEVICE          dpa_dev = {
    "DPA", dpa_unit, NULL, hsdp_mod,
    NUM_UNITS_HSDP, 16, 24, 4, 16, 32,
    NULL, NULL, &hsdp_reset, &hsdp_boot, &hsdp_attach, &hsdp_detach,
    &dpa_dib, DEV_DISABLE|DEV_DEBUG, 0, dev_debug,
    NULL, NULL, &hsdp_help, NULL, NULL, &hsdp_description
};

#if NUM_DEVS_HSDP > 1
/* channel program information */
CHANP           dpb_chp[NUM_UNITS_HSDP] = {0};

UNIT            dpb_unit[] = {
/* SET_TYPE(3) DM300 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0x800)},       /* 0 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0x802)},       /* 1 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0x804)},       /* 2 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0x806)},       /* 3 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0x808)},       /* 4 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0x80A)},       /* 5 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0x80C)},       /* 6 */
    {UDATA(&hsdp_srv, UNIT_HSDP|SET_TYPE(3), 0), 0, UNIT_ADDR(0x80E)},       /* 7 */
};


DIB             dpb_dib = {
    hsdp_preio,                                 /* Pre Start I/O */
    hsdp_startcmd,                              /* Start a command SIO */
    NULL,                                       /* Stop I/O HIO */
    NULL,                                       /* Test I/O TIO */
    NULL,                                       /* Post I/O */
    hsdp_ini,                                   /* init function */
    dpb_unit,                                   /* Pointer to units structure */
    dpb_chp,                                    /* Pointer to chan_prg structure */
    NUM_UNITS_HSDP,                             /* number of units defined */
    0x0F,                                       /* 8 devices - device mask */
    0x0800,                                     /* parent channel address */
    0,                                          /* fifo input index */
    0,                                          /* fifo output index */
    0,                                          /* interrupt status fifo for channel */
};

DEVICE          dpb_dev = {
    "DPB", dpb_unit, NULL, hsdp_mod,
    NUM_UNITS_HSDP, 16, 24, 4, 16, 32,
    NULL, NULL, &hsdp_reset, &hsdp_boot, &hsdp_attach, &hsdp_detach,
    &dpb_dib, DEV_DISABLE|DEV_DEBUG, 0, dev_debug,
    NULL, NULL, &hsdp_help, NULL, NULL, &hsdp_description
};
#endif

/* start a disk operation */
uint8  hsdp_preio(UNIT *uptr, uint16 chan)
{
    DEVICE         *dptr = find_dev_from_unit(uptr);
    int            unit = (uptr - dptr->units);

    if ((uptr->CMD & 0xff00) != 0) {            /* just return if busy */
        return SNS_BSY;
    }

    sim_debug(DEBUG_CMD, dptr, "hsdp_preio unit=%02x OK\n", unit);
    return 0;                                   /* good to go */
}

uint8  hsdp_startcmd(UNIT *uptr, uint16 chan,  uint8 cmd)
{
    uint16      addr = GET_UADDR(uptr->CMD);
    DEVICE      *dptr = find_dev_from_unit(uptr);
    int         unit = (uptr - dptr->units);
    uint8       ch;

    sim_debug(DEBUG_CMD, dptr, "hsdp_startcmd unit %02x cmd %02x CMD %08x\n", unit, cmd, uptr->CMD);
    if ((uptr->flags & UNIT_ATT) == 0) {        /* unit attached status */
        uptr->SNS |= SNS_INTVENT;               /* unit intervention required */
        if (cmd != DSK_SNS)                     /* we are completed with unit check status */
            return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;
    }

    if ((uptr->CMD & DSK_CMDMSK) != 0) {
        uptr->CMD |= DSK_BUSY;                  /* Flag we we are busy */
        return SNS_BSY;
    }
    if ((uptr->CMD & 0xff00) != 0) {            /* if any status info, we are busy */
        return SNS_BSY;
    }
    sim_debug(DEBUG_CMD, dptr, "hsdp_startcmd CMD continue unit=%02x cmd %02x\n", unit, cmd);

    if ((uptr->flags & UNIT_ATT) == 0) {        /* see if unit is attached */
        if (cmd == DSK_SNS) {                   /* not attached, is cmd Sense 0x04 */
            sim_debug(DEBUG_CMD, dptr, "hsdp_startcmd CMD sense\n");
            /* bytes 0,1 - Cyl entry from STAR reg in STAR */
            ch = (uptr->STAR >> 24) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "hsdp_startcmd sense STAR b0 unit=%02x 1 %02x\n",
                unit, ch);
            chan_write_byte(addr, &ch) ;
            ch = (uptr->STAR >> 16) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "hsdp_startcmd sense STAR b1 unit=%02x 1 %02x\n",
                unit, ch);
            chan_write_byte(addr, &ch) ;
            /* byte 2 - Track entry from STAR reg in STAR */
            ch = (uptr->STAR >> 8) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "hsdp_startcmd sense STAR b2 unit=%02x 1 %02x\n",
                unit, ch);
            chan_write_byte(addr, &ch) ;
            /* byte 3 - Sector entry from STAR reg in STAR */
            ch = (uptr->STAR) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "hsdp_startcmd sense STAR b3 unit=%02x 1 %02x\n",
                unit, ch);
            chan_write_byte(addr, &ch) ;
            /* bytes 4 - mode reg, byte 0 of SNS */
            ch = (uptr->SNS >> 24) & 0xff;      /* return the sense data for device */
            sim_debug(DEBUG_DETAIL, dptr, "hsdp_startcmd sense unit=%02x 1 %02x\n",
                unit, ch);
            chan_write_byte(addr, &ch) ;
            /* bytes 5-7 - status bytes, bytes 1-3 of SNS */
            ch = (uptr->SNS >> 16) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "hsdp_startcmd sense unit=%02x %02x\n",
                unit, ch);
            chan_write_byte(addr, &ch) ;
            ch = (uptr->SNS >> 8) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "hsdp_startcmd sense unit=%02x 3 %02x\n",
                unit, ch);
            chan_write_byte(addr, &ch) ;
            ch = (uptr->SNS) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "hsdp_startcmd sense unit=%02x 4 %02x\n",
                unit, ch);
            chan_write_byte(addr, &ch) ;
            /* bytes 8-11 - drive attribute register (DATR) entries from uptr->ATTR via
             * INCH cmd */
            ch = (uptr->ATTR >> 24) & 0xff;
            chan_write_byte(addr, &ch) ;
            ch = (uptr->ATTR >> 16) & 0xff;
            chan_write_byte(addr, &ch) ;
            ch = (uptr->ATTR >> 8 ) & 0xff;
            chan_write_byte(addr, &ch) ;
            ch = (uptr->ATTR >> 0) & 0xff;
            chan_write_byte(addr, &ch) ;
            /* bytes 12 & 13 contain drive related status */
            ch = 0;                             /* zero for now */
            chan_write_byte(addr, &ch) ;
            chan_write_byte(addr, &ch) ;

            uptr->SNS &= 0xff000000;            /* clear status bytes, but leave mode data */
            return SNS_CHNEND|SNS_DEVEND;
        }
        if (cmd == 0x0)                         /* INCH cmd gives unit check here */
           return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;

        uptr->SNS |= (SNS_INTVENT|SNS_CMDREJ);  /* set new error status */
        return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;   /* we done */
    }

    /* Unit is online, so process a command */
    switch (cmd) {

    case DSK_INCH:              /* INCH 0x00 */
    {
        uint32  mema;                           /* memory address */
        uint32  i;
        UNIT    *up = dptr->units;              /* first unit for this device */
        sim_debug(DEBUG_CMD, dptr, "hsdp_startcmd starting inch cmd addr %04x STAR %08x\n",
                   addr, uptr->STAR);
        /* STAR (u4) has IOCD word 1 contents.  For the disk processor it contains */
        /* a pointer to the INCH buffer followed by 8 drive attribute words that */
        /* contains the flags, sector count, MHD head count, and FHD count */
        /* us9 has the byte count from IOCD wd2 and should be 0x24 (36) */
        /* the INCH buffer address must be returned in STAR and us9 left non-zero */
        /* just return OK and channel software will use up8 as status buffer */
        mema = (uint32)uptr->STAR;              /* get memory address of buffer */
        uptr->STAR = M[mema>>2];                /* get status buffer address for XIO return status */
        sim_debug(DEBUG_CMD, dptr,
             "hsdp_startcmd starting inch cmd addr %04x STAR %08x mema %08x units %02x\n",
            addr, uptr->STAR, mema, dptr->numunits);
        /* the next 8 words have drive data for each unit */
        /* WARNING 8 drives must be defined for this controller */
        /* so we will not have a map fault */
        for (i=0; i<dptr->numunits && i<8; i++) {   /* process all drives */
            up->ATTR = M[(mema>>2)+i+1];        /* save each unit's drive data */
            sim_debug(DEBUG_CMD, dptr,
                "hsdp_startcmd ATTR data %08x unit %02x flags %02x sec %02x MHD %02x FHD %02x\n",
                up->ATTR, i, (up->ATTR >> 24)&0xff, (up->ATTR >> 16)&0xff, (up->ATTR >> 8)&0xff, (up->ATTR&0xff));
            up++;                               /* next unit for this device */
        }
        sim_debug(DEBUG_CMD, dptr, "hsdp_startcmd done inch cmd addr %04x\n", addr);
        uptr->CMD |= DSK_CMDMSK;                /* use 0xff for inch, just need int */
        sim_activate(uptr, 20);                 /* start things off */
        return 0;
    }

    case DSK_SCK:                               /* Seek command 0x07 */
    case DSK_XEZ:                               /* Rezero & Read IPL record 0x1f */
        uptr->CMD &= ~(DSK_STAR);               /* show we do not have seek STAR in STAR */
    case DSK_WD:                                /* Write command 0x01 */
    case DSK_RD:                                /* Read command 0x02 */
    case DSK_LMR:                               /* read mode register */

        uptr->CMD |= cmd;                       /* save cmd */
        sim_debug(DEBUG_CMD, dptr,
             "hsdp_startcmd starting disk seek r/w cmd %02x addr %04x\n", cmd, addr);
        sim_activate(uptr, 20);                 /* start things off */
        return 0;

    case DSK_NOP:                               /* NOP 0x03 */
        uptr->CMD |= cmd;                       /* save cmd */
        sim_activate(uptr, 20);                 /* start things off */
        return 0;

    case DSK_SNS:                               /* Sense 0x04 */
        uptr->CMD |= cmd;                       /* save cmd */
        sim_activate(uptr, 20);                 /* start things off */
        break;
    }
    sim_debug(DEBUG_CMD, dptr,
        "hsdp_startcmd done with hsdp_startcmd %02x addr %04x SNS %08x\n",
              cmd, addr, uptr->SNS);
    if (uptr->SNS & 0xff)                       /* any other cmd is error */
        return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;
    sim_activate(uptr, 20);                     /* start things off */
    return SNS_CHNEND|SNS_DEVEND;
}

/* Handle processing of disk requests. */
t_stat hsdp_srv(UNIT * uptr)
{
    uint16          chsa = GET_UADDR(uptr->CMD);
    DEVICE          *dptr = find_dev_from_unit(uptr);
    DIB             *dibp = (DIB *)dptr->ctxt;
    /* get pointer to Dev Info Blk for this device */
    CHANP           *chp = (CHANP *)dibp->chan_prg; /* get pointer to channel program */
    struct ddata_t  *data = (struct ddata_t *)(uptr->DDATA);
    int             cmd = uptr->CMD & DSK_CMDMSK;
    int             type = GET_TYPE(uptr->flags);
    uint32          trk, cyl;
    int             unit = (uptr - dptr->units);
    int             len;
    int             i;
    uint8           ch;
    int             tsize = hsdp_type[type].spt * hsdp_type[type].ssiz * 4; /* get track size in bytes */
    int             ssize = hsdp_type[type].ssiz * 4;   /* disk sector size in bytes */
    int             tstart;
    uint8           buf2[1024];
    uint8           buf[1024];

    sim_debug(DEBUG_DETAIL, &dda_dev,
              "hsdp_srv entry unit %02x cmd %02x chsa %04x chan %04x count %04x\n",
             unit, cmd, chsa, chsa>>8, chp->ccw_count);

    if ((uptr->flags & UNIT_ATT) == 0) {        /* unit attached status */
        uptr->SNS |= SNS_INTVENT;               /* unit intervention required */
        if (cmd != DSK_SNS)                     /* we are completed with unit check status */
            return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;
    }

    sim_debug(DEBUG_CMD, dptr, "hsdp_srv cmd=%02x chsa %04x count %04x\n",
              cmd, chsa, chp->ccw_count);
    switch (cmd) {
    case 0:                                     /* No command, stop disk */
        break;

    case DSK_CMDMSK:        /* use 0xff for inch, just need int */
        uptr->CMD &= ~(0xffff);                 /* remove old cmd */
        sim_debug(DEBUG_CMD, dptr, "hsdp_srv cmd=%02x chsa %04x count %04x completed\n",
                 cmd, chsa, chp->ccw_count);
#ifdef FIX4MPX
        chan_end(chsa, SNS_CHNEND);             /* return just channel end OK */
#else
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND);  /* return OK */
#endif
        break;

    case DSK_NOP:           /* NOP 0x03 */
        uptr->CMD &= ~(0xffff);                 /* remove old cmd */
        sim_debug(DEBUG_CMD, dptr, "hsdp_srv cmd NOP chsa %04x count %04x completed\n",
            chsa, chp->ccw_count);
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND);  /* return OK */
        break;

    case DSK_SNS: /* 0x4 */
        ch = uptr->SNS & 0xff;
        sim_debug(DEBUG_DETAIL, dptr, "dsk_srv sense unit=%02x 1 %02x\n", unit, ch);
        chan_write_byte(chsa, &ch) ;
        ch = (uptr->SNS >> 8) & 0xff;
        sim_debug(DEBUG_DETAIL, dptr, "dsk_srv sense unit=%02x 2 %02x\n", unit, ch);
        chan_write_byte(chsa, &ch) ;
        ch = 0;
        sim_debug(DEBUG_DETAIL, dptr, "dsk_srv sense unit=%02x 3 %02x\n", unit, ch);
        chan_write_byte(chsa, &ch) ;
        ch = unit;
        sim_debug(DEBUG_DETAIL, dptr, "dsk_srv sense unit=%02x 4 %02x\n", unit, ch);
        chan_write_byte(chsa, &ch) ;
        ch = 4;
        uptr->CMD &= ~(0xff00);
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND);
        break;

    case DSK_SCK:            /* Seek cylinder, track, sector 0x07 */

        /* If we are waiting on seek to finish, check if there yet. */
        if (uptr->CMD & DSK_SEEKING) {
            /* see if on cylinder yet */
            if ((uptr->STAR >> 16) == data->cyl) {
                /* we are on cylinder, seek is done */
                sim_debug(DEBUG_CMD, dptr, "dsk_srv seek on cylinder unit=%02x %02x %04x\n",
                    unit, uptr->STAR >> 16, data->cyl);
                uptr->CMD &= ~(0xffff);         /* remove old status bits & cmd */
                set_devattn(chsa, SNS_DEVEND);  /* start the operation */
                sim_debug(DEBUG_DETAIL, dptr, "dsk_srv seek end unit=%02x %02x %04x\n",
                    unit, uptr->STAR >> 16, data->cyl);
                sim_activate(uptr, 20);
                break;
            } else {
                /* Compute delay based of difference. */
                /* Set next state = index */
                i = (uptr->STAR >> 16) - data->cyl;
                sim_debug(DEBUG_CMD, dptr, "dsk_srv seek unit=%02x %02x %04x\n", unit,
                        uptr->STAR >> 16, i);
                if (i > 0 ) {
                    if (i > 50) {
                        data->cyl += 50;        /* seek 50 cyl */
                        sim_activate(uptr, 800);
                    } else
                    if (i > 20) {
                        data->cyl += 20;        /* seek 20 cyl */
                        sim_activate(uptr, 400);
                    } else {
                        data->cyl++;            /* Seek 1 cyl */
                        sim_activate(uptr, 200);
                    }
                    if (data->cyl >= (int)hsdp_type[type].cyl)   /* test for over max */
                        data->cyl = hsdp_type[type].cyl-1;  /* make max */
                } else {
                    if (i < -50) {
                        data->cyl -= 50;        /* seek 50 cyl */
                        sim_activate(uptr, 800);
                    } else
                    if (i < -20) {
                        data->cyl -= 20;        /* seek 20 cyl */
                        sim_activate(uptr, 400);
                    } else {
                        data->cyl--;            /* seek 1 cyl */
                        sim_activate(uptr, 200);
                    }
                    if (data->cyl < 0)          /* test for less than zero */
                        data->cyl = 0;          /* make zero */
                }
                sim_debug(DEBUG_DETAIL, dptr, "dsk_srv seek next unit=%02x %02x %04x\n",
                        unit, uptr->STAR >> 16, data->cyl);
                sim_activate(uptr, 2);
                break;
            }
        }

        /* not seeking, so start a new seek */
        /* Read in 4 character seek code */
        for (i = 0; i < 4; i++) {
            if (chan_read_byte(chsa, &buf[i])) {
                /* we have error, bail out */
                uptr->CMD &= ~(0xffff);         /* remove old status bits & cmd */
                uptr->SNS |= SNS_CMDREJ|SNS_EQUCHK;
                chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
                break;
            }
        }
rezero:
        sim_debug(DEBUG_DETAIL, dptr, "dsk_srv seek unit=%02x star %02x %02x %02x %02x\n",
            unit, buf[0], buf[1], buf[2], buf[3]);
        /* save STAR (target sector) data in STAR */
        uptr->STAR = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | (buf[3]);
        cyl = uptr->STAR >> 16;                 /* get the cylinder */
        trk = buf[2];                           /* get the track */
        sim_debug(DEBUG_DETAIL, dptr,
           "dsk_srv SEEK cyl %04x trk %02x sec %02x unit=%02x\n", cyl&0xffff, trk, buf[3], unit);

        /* FIXME do something with FHD here */
        /* Check if seek valid */
        if (cyl > hsdp_type[type].cyl ||
            trk >= hsdp_type[type].nhds ||
            buf[3] > hsdp_type[type].spt)  {
            sim_debug(DEBUG_CMD, dptr,
                   "dsk_srv seek ERROR cyl %04x trk %02x sec %02x unit=%02x\n",
                            cyl, trk, buf[3], unit);
            uptr->CMD &= ~(0xffff);             /* remove old status bits & cmd */
            uptr->SNS |= SNS_CMDREJ|SNS_EQUCHK;  /* set error status */

            /* we have an error, tell user */
            chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);  /* end command */
            break;
        }

        uptr->CMD |= DSK_STAR;                  /* show we have seek STAR in STAR */
        /* calc the sector address of data */
        /* calculate file position in bytes of requested sector */
        tstart = (cyl * hsdp_type[type].nhds * tsize) + (trk * tsize) + (buf[3] * 1024);
        data->tpos = trk;                       /* save the track/head number */
        data->spos = buf[3];                    /* save the sector number */
        sim_debug(DEBUG_DETAIL, dptr, "dsk_srv seek start %04x trk %02x sec %02x\n",
                       tstart, trk, buf[3]);
        if ((sim_fseek(uptr->fileref, tstart, SEEK_SET)) != 0) {  /* seek home */
            sim_debug(DEBUG_DETAIL, dptr, "dsk_srv Error on seek to %04x\n", tstart);
        }

        /* Check if already on correct cylinder */
        if (trk != data->cyl) {
            /* Do seek */
            uptr->CMD |= DSK_SEEKING;           /* show we are seeking */
            sim_debug(DEBUG_DETAIL, dptr, "dsk_srv seek unit=%02x trk %02x cyl %02x\n",
                             unit, trk, data->cyl);
            sim_activate(uptr, 20);
            chan_end(chsa, SNS_CHNEND);
        } else {
            sim_debug(DEBUG_DETAIL, dptr,
                "dsk_srv calc sect addr seek start %04x trk %02x sec %02x\n",
                      tstart, trk, buf[3]);
            uptr->CMD &= ~(0xffff);             /* remove old status bits & cmd */
            sim_activate(uptr, 20);
            chan_end(chsa, SNS_DEVEND|SNS_CHNEND);
        }
        return SCPE_OK;

    case DSK_XEZ:          /* Rezero & Read IPL record */

        sim_debug(DEBUG_CMD, dptr, "RD REZERO IPL unit=%02x seek 0\n", unit);
        /* Do a seek to 0 */
        uptr->STAR = 0;                         /* set STAR to 0, 0, 0 */
        uptr->CMD &= ~(0xffff);                 /* remove old cmd */
        uptr->CMD |= DSK_SCK;                   /* show as seek command */
        tstart = 0;                             /* byte offset is 0 */
        /* Read in 1 dummy character for length to inhibit SLI posting */
        if (chan_read_byte(chsa, &buf[0])) {
            /* we have error, bail out */
            uptr->CMD &= ~(0xffff);             /* remove old status bits & cmd */
            uptr->SNS |= SNS_CMDREJ|SNS_EQUCHK;
            chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
            break;
        }
        /* zero stuff */
        buf[0] = buf[1] = buf[2] = buf[3] = 0;
        goto rezero;                            /* murge with seek code */
        break;

    case DSK_LMR:
        sim_debug(DEBUG_CMD, dptr, "Load Mode Reg unit=%02x\n", unit);
        /* Read in 1 character of mode data */
        if (chan_read_byte(chsa, &buf[0])) {
            /* we have error, bail out */
            uptr->CMD &= ~(0xffff);             /* remove old status bits & cmd */
            uptr->SNS |= SNS_CMDREJ|SNS_EQUCHK;
            chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
            break;
        }
        uptr->CMD &= ~(0xffff);                 /* remove old cmd */
        uptr->SNS &= 0x00ffffff;                /* clear old mode data */
        uptr->SNS |= (buf[0] << 24);            /* save mode value */
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND);
        break;

    case DSK_RD:            /* Read Data */
        if ((uptr->CMD & DSK_READING) == 0) {   /* see if we are reading data */
            uptr->CMD |= DSK_READING;           /* read from disk starting */
            sim_debug(DEBUG_CMD, dptr,
                "DISK READ starting unit=%02x CMD %02x\n", unit, uptr->CMD);
        }

        if (uptr->CMD & DSK_READING) {          /* see if we are reading data */
            /* read in a sector of data from disk */
            if ((len=sim_fread(buf, 1, ssize, uptr->fileref)) != ssize) {
                sim_debug(DEBUG_CMD, dptr,
                    "Error %08x on read %04x of diskfile cyl %04x hds %02x sec %02x\n",
                     len, ssize, data->cyl, data->tpos, data->spos);
                uptr->CMD &= ~(0xffff);         /* remove old status bits & cmd */
                chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
                break;
            }

            sim_debug(DEBUG_CMD, dptr,
                  "hsdp_srv after READ chsa %04x count %04x\n", chsa, chp->ccw_count);
            /* process the next sector of data */
            for (i=0; i<len; i++) {
                ch = buf[i];                    /* get a char from buffer */
                if (chan_write_byte(chsa, &ch)) {   /* put a byte to memory */
                    sim_debug(DEBUG_DATAIO, dptr,
                      "DISK Read %04x bytes from dskfile cyl %04x hds %02x sec %02x\n",
                       i, data->cyl, data->tpos, data->spos);
                    uptr->CMD &= ~(0xffff);     /* remove old status bits & cmd */
                    chan_end(chsa, SNS_CHNEND|SNS_DEVEND);
                    goto rddone;
                }
            }

            sim_debug(DEBUG_CMD, dptr,
                "DISK READ from sec end bytes end %04x from diskfile cyl %04x hds %02x sec %02x\n",
                ssize, data->cyl, data->tpos, data->spos);
            data->spos++;
            /* set sector to read next one */
            if (data->spos >= (hsdp_type[type].spt)) {
                data->spos = 0;                 /* number of sectors per track */
                data->tpos++;                   /* track position */
                if (data->tpos >= (hsdp_type[type].nhds)) {
                    data->tpos = 0;             /* number of tracks per cylinder */
                    data->cyl++;                /* cylinder position */
                    if (data->cyl >= (int)(hsdp_type[type].cyl)) {
                        /* EOM reached, abort */
                        uptr->CMD &= ~(0xffff); /* remove old status bits & cmd */
                        chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
                        break;
                    }
                }
            }
            /* see if we are done reading data */
            if (test_write_byte_end(chsa)) {
                sim_debug(DEBUG_DATAIO, dptr,
                    "DISK Read complete Read bytes from diskfile cyl %04x hds %02x sec %02x\n",
                    data->cyl, data->tpos, data->spos);
                uptr->CMD &= ~(0xffff);         /* remove old status bits & cmd */
                chan_end(chsa, SNS_CHNEND|SNS_DEVEND);
            }
rddone:
            sim_activate(uptr, 10);             /* wait to read next sector */
            break;
        }
        break;

    case DSK_WD:            /* Write Data */
        if ((uptr->CMD & DSK_WRITING) == 0) {   /* see if we are writing data */
            uptr->CMD |= DSK_WRITING;           /* write to disk starting */
            sim_debug(DEBUG_CMD, dptr,
                "DISK WRITE starting unit=%02x CMD %02x\n", unit, uptr->CMD);
        }
        if (uptr->CMD & DSK_WRITING) {          /* see if we are writing data */
            /* process the next sector of data */
            len = 0;                            /* used here as a flag for short read */
            for (i=0; i<ssize; i++) {
                if (chan_read_byte(chsa, &ch)) {    /* get a byte from memory */
                    /* if error on reading 1st byte, we are done writing */
                    if (i == 0) {
                        uptr->CMD &= ~(0xffff);  /* remove old status bits & cmd */
                        sim_debug(DEBUG_CMD, dptr,
                            "DISK Wrote %04x bytes to diskfile cyl %04x hds %02x sec %02x\n",
                            ssize, data->cyl, data->tpos, data->spos);
                        chan_end(chsa, SNS_CHNEND|SNS_DEVEND);
                        goto wrdone;
                    }
                    ch = 0;                     /* finish out the sector with zero */
                    len++;                      /* show we have no more data to write */
                }
                buf2[i] = ch;                   /* save the char */
            }
            /* write the sector to disk */
            if ((i=sim_fwrite(buf2, 1, ssize, uptr->fileref)) != ssize) {
                sim_debug(DEBUG_CMD, dptr,
                    "Error %08x on write %04x bytes to diskfile cyl %04x hds %02x sec %02x\n",
                    i, ssize, data->cyl, data->tpos, data->spos);
                uptr->CMD &= ~(0xffff);         /* remove old status bits & cmd */
                chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
                break;
            }
            if (len != 0) {                     /* see if done with write command */
                sim_debug(DEBUG_DATAIO, dptr,
                    "DISK WroteB %04x bytes to diskfile cyl %04x hds %02x sec %02x\n",
                     ssize, data->cyl, data->tpos, data->spos);
                uptr->CMD &= ~(0xffff);         /* remove old status bits & cmd */
                chan_end(chsa, SNS_CHNEND|SNS_DEVEND);  /* we done */
                break;
            }
            sim_debug(DEBUG_CMD, dptr,
                "DISK WR to sec end %0x4x bytes end %04x to diskfile cyl %04x hds %02x sec %02x\n",
                len, ssize, data->cyl, data->tpos, data->spos);
            data->spos++;
            if (data->spos >= (hsdp_type[type].spt)) {
                data->spos = 0;                 /* number of sectors per track */
                data->tpos++;                   /* track position */
                if (data->tpos >= (hsdp_type[type].nhds)) {
                    data->tpos = 0;             /* number of tracks per cylinder */
                    data->cyl++;                /* cylinder position */
                    if (data->cyl >= (int)(hsdp_type[type].cyl)) {
                        /* EOM reached, abort */
                        sim_debug(DEBUG_DETAIL, dptr,
                            "Error %08x on write %04x to diskfile cyl %04x hds %02x sec %02x\n",
                            i, ssize, data->cyl, data->tpos, data->spos);
                        uptr->CMD &= ~(0xffff);  /* remove old status bits & cmd */
                        chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
                        break;
                    }
                }
            }
wrdone:
            sim_activate(uptr, 10);
            break;
         }
         break;

    default:
        sim_debug(DEBUG_DETAIL, dptr, "invalid command %02x unit %02x\n", cmd, unit);
        uptr->SNS |= SNS_CMDREJ;
        uptr->CMD &= ~(0xffff);                 /* remove old status bits & cmd */
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
        break;
    }
    sim_debug(DEBUG_DETAIL, dptr, "hsdp_srv done cmd=%02x chsa %04x count %04x\n",
        cmd, chsa, chp->ccw_count);
    return SCPE_OK;
}

/* initialize the disk */
void hsdp_ini(UNIT *uptr, t_bool f)
{
    DEVICE  *dptr = find_dev_from_unit(uptr);
    int     i = GET_TYPE(uptr->flags);

    uptr->CMD &= ~0xffff;                       /* clear out the flags but leave ch/sa */
    /* capacity is total allocation units time sectors per allocation unit */
    /* total sectors on disk */
    uptr->capac  = hsdp_type[i].taus * hsdp_type[i].spau;

    sim_debug(DEBUG_EXP, &dda_dev, "DPA init device %s on unit DPA%.1x cap %x\n",
        dptr->name, GET_UADDR(uptr->CMD), uptr->capac);
}

t_stat hsdp_reset(DEVICE * dptr)
{
    /* add reset code here */
    return SCPE_OK;
}

/* attach the selected file to the disk */
t_stat hsdp_attach(UNIT *uptr, CONST char *file)
{
    uint16          addr = GET_UADDR(uptr->CMD);
    int             type = GET_TYPE(uptr->flags);
    DEVICE          *dptr = find_dev_from_unit(uptr);
    t_stat          r;
    uint16          tsize;                      /* track size in bytes */
    uint16          ssize;                      /* sector size in bytes */
    struct ddata_t  *data;
//  uint8           buff[1024];

    /* have simulator attach the file to the unit */
    if ((r = attach_unit(uptr, file)) != SCPE_OK)
        return r;

    if (hsdp_type[type].name == 0) {            /* does the assigned disk have a name */
        detach_unit(uptr);                      /* no, reject */
        return SCPE_FMT;                        /* error */
    }

    /* get a buffer to hold hsdp_t structure */
    /* extended data structure per unit */
    if ((data = (struct ddata_t *)calloc(1, sizeof(struct ddata_t))) == 0) {
        detach_unit(uptr);
        return SCPE_FMT;
    }

    uptr->DDATA = (void *)data;                 /* save pointer to structure in DDATA */
    /* track size in bytes is sectors/track times words/sector time 4 bytse/word */
    tsize = hsdp_type[type].spt * hsdp_type[type].ssiz * 4; /* get track size in bytes */
    uptr->capac = hsdp_type[type].taus * hsdp_type[type].spau;
                                                /* disk capacity in sectors */
    ssize = hsdp_type[type].ssiz * 4;           /* disk sector size in bytes */
    uptr->capac *= ssize;                       /* disk capacity in bytes */

    sim_debug(DEBUG_CMD, dptr, "Disk taus %d spau %d ssiz %d cap %d\n",
        hsdp_type[type].taus, hsdp_type[type].spau, hsdp_type[type].ssiz * 4,
        uptr->capac);                           /* disk capacity */

    if ((sim_fseek(uptr->fileref, 0, SEEK_SET)) != 0) { /* seek home */
        detach_unit(uptr);                      /* if no space, error */
        return SCPE_FMT;                        /* error */
    }

    data->tpos = 0;                             /* current track position */
    data->spos = 0;                             /* current sector position */

    set_devattn(addr, SNS_DEVEND);
    return SCPE_OK;
}

/* detach a disk device */
t_stat hsdp_detach(UNIT * uptr) {
    struct ddata_t       *data = (struct ddata_t *)uptr->DDATA;

    if (data != 0) {
        free(data);                             /* free disk data structure */
    }
    uptr->DDATA = 0;                            /* no pointer to disk data */
    uptr->CMD &= ~0xffff;                       /* no cmd and flags */
    return detach_unit(uptr);                   /* tell simh we are done with disk */
}

/* boot from the specified disk unit */
t_stat hsdp_boot(int32 unit_num, DEVICE * dptr) {
    UNIT    *uptr = &dptr->units[unit_num];     /* find disk unit number */

    sim_debug(DEBUG_CMD, &dda_dev, "Disk Boot dev/unit %x\n", GET_UADDR(uptr->CMD));
    SPAD[0xf4] = GET_UADDR(uptr->CMD);          /* put boot device chan/sa into spad */
    SPAD[0xf8] = 0xF000;                        /* show as F class device */
    if ((uptr->flags & UNIT_ATT) == 0)
        return SCPE_UNATT;                      /* attached? */
    return chan_boot(GET_UADDR(uptr->CMD), dptr); /* boot the ch/sa */
}

/* Disk option setting commands */
t_stat hsdp_set_type(UNIT * uptr, int32 val, CONST char *cptr, void *desc)
{
    int     i;

    if (cptr == NULL)
        return SCPE_ARG;
    if (uptr == NULL)
        return SCPE_IERR;
    if (uptr->flags & UNIT_ATT)
        return SCPE_ALATT;
    for (i = 0; hsdp_type[i].name != 0; i++) {
        if (strcmp(hsdp_type[i].name, cptr) == 0) {
            uptr->flags &= ~UNIT_TYPE;
            uptr->flags |= SET_TYPE(i);
            uptr->capac  = hsdp_type[i].taus * hsdp_type[i].spau;
            return SCPE_OK;
        }
    }
    return SCPE_ARG;
}

t_stat hsdp_get_type(FILE * st, UNIT * uptr, int32 v, CONST void *desc)
{
    if (uptr == NULL)
        return SCPE_IERR;
    fputs("TYPE=", st);
    fputs(hsdp_type[GET_TYPE(uptr->flags)].name, st);
    return SCPE_OK;
}

/* help information for disk */
t_stat hsdp_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag,
    const char *cptr)
{
    int i;
    fprintf (st, "SEL 8064 High Speed Disk Processor\r\n");
    fprintf (st, "Use:\r\n");
    fprintf (st, "    sim> SET %sn TYPE=type\r\n", dptr->name);
    fprintf (st, "Type can be: ");
    for (i = 0; hsdp_type[i].name != 0; i++) {
        fprintf(st, "%s", hsdp_type[i].name);
        if (hsdp_type[i+1].name != 0)
        fprintf(st, ", ");
    }
    fprintf (st, ".\nEach drive has the following storage capacity:\r\n");
    for (i = 0; hsdp_type[i].name != 0; i++) {
        /* disk capacity in sectors */
        int32 capac = hsdp_type[i].taus * hsdp_type[i].spau;
        int32 ssize = hsdp_type[i].ssiz * 4;    /* disk sector size in bytes */
        int32 size = capac * ssize;             /* disk capacity in bytes */
        size /= 1024;                           /* make KB */
        size = (10 * size) / 1024;              /* size in MB * 10 */
        fprintf(st, "      %-8s %4d.%1d MB\r\n", hsdp_type[i].name, size/10, size%10);
    }
    fprint_set_help(st, dptr);
    fprint_show_help(st, dptr);
    return SCPE_OK;
}

const char *hsdp_description (DEVICE *dptr)
{
    return "SEL 8064 High Speed Disk Processor";
}

#endif