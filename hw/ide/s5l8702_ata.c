/*
 * Samsung S5L8702 ATA host controller (iPod Classic 6G), PATA mode.
 *
 * This is a self-contained model backed by a BlockBackend.  The
 * controller is nothing like a standard PCI/MMIO IDE adapter: the ATA
 * taskfile registers are accessed indirectly through a ready/handshake
 * protocol (ATA_PIO_READY, ATA_PIO_RDATA) and block transfers are
 * driven by an integrated DMA engine that copies directly between the
 * disk and guest memory (ATA_TBUF_START / ATA_SBUF_START).
 *
 * Only the small command set the Rockbox driver issues is implemented:
 * IDENTIFY DEVICE, READ/WRITE DMA (+EXT), SET FEATURES, FLUSH CACHE,
 * SET MULTIPLE, STANDBY IMMEDIATE and SLEEP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/block-backend.h"
#include "system/dma.h"
#include "exec/cpu-common.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_S5L8702_ATA "s5l8702-ata"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702AtaState, S5L8702_ATA)

/* Register offsets */
#define REG_CONTROL    0x00
#define REG_STATUS     0x04
#define REG_COMMAND    0x08
#define REG_SWRST      0x0c
#define REG_IRQ        0x10
#define REG_IRQ_MASK   0x14
#define REG_CFG        0x18
#define REG_MDMA_TIME  0x28
#define REG_PIO_TIME   0x2c
#define REG_UDMA_TIME  0x30
#define REG_XFR_NUM    0x34
#define REG_XFR_CNT    0x38
#define REG_TBUF_START 0x3c
#define REG_TBUF_SIZE  0x40
#define REG_SBUF_START 0x44
#define REG_SBUF_SIZE  0x48
#define REG_CADR_TBUF  0x4c
#define REG_CADR_SBUF  0x50
#define REG_PIO_DTR    0x54
#define REG_PIO_FED    0x58
#define REG_PIO_SCR    0x5c
#define REG_PIO_LLR    0x60
#define REG_PIO_LMR    0x64
#define REG_PIO_LHR    0x68
#define REG_PIO_DVR    0x6c
#define REG_PIO_CSD    0x70
#define REG_PIO_DAD    0x74
#define REG_PIO_READY  0x78
#define REG_PIO_RDATA  0x7c

/* CFG bits */
#define CFG_DIR_WRITE  (1 << 4)

/* ATA status bits */
#define ATA_BSY  0x80
#define ATA_DRDY 0x40
#define ATA_DRQ  0x08
#define ATA_ERR  0x01

/* Commands */
#define CMD_READ_SECTORS    0x20
#define CMD_READ_SECT_EXT   0x24
#define CMD_READ_DMA_EXT    0x25
#define CMD_READ_MULTIPLE   0xc4
#define CMD_READ_DMA        0xc8
#define CMD_WRITE_SECTORS   0x30
#define CMD_WRITE_SECT_EXT  0x34
#define CMD_WRITE_DMA_EXT   0x35
#define CMD_WRITE_MULTIPLE  0xc5
#define CMD_WRITE_DMA       0xca
#define CMD_IDENTIFY        0xec
#define CMD_SET_FEATURES    0xef
#define CMD_SET_MULTIPLE    0xc6
#define CMD_FLUSH_CACHE     0xe7
#define CMD_FLUSH_CACHE_EXT 0xea
#define CMD_STANDBY_IMM     0xe0
#define CMD_SLEEP           0xe6

#define SECTOR_SIZE 512
#define IDENTIFY_WORDS 256

struct S5L8702AtaState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    BlockBackend *blk;
    uint64_t nb_sectors;

    uint32_t control;
    uint32_t cfg;
    uint32_t irq_stat;
    uint32_t irq_mask;
    uint32_t xfr_num;
    uint32_t tbuf_start, tbuf_size;
    uint32_t sbuf_start, sbuf_size;

    /* taskfile shadow registers with 2-deep HOB FIFOs */
    uint8_t scr_cur, scr_prev;
    uint8_t llr_cur, llr_prev;
    uint8_t lmr_cur, lmr_prev;
    uint8_t lhr_cur, lhr_prev;
    uint8_t dvr;
    uint8_t fed;
    uint8_t command;
    uint8_t status;

    uint32_t rdata;     /* last latched read value */

    /* PIO data buffer (IDENTIFY) */
    uint16_t data_buf[IDENTIFY_WORDS];
    int data_pos;
    int data_len;
    bool lba48_cmd;
};

static void s5l8702_ata_update_irq(S5L8702AtaState *s)
{
    qemu_set_irq(s->irq, (s->irq_stat & s->irq_mask) != 0);
}

static void put_le16_str(uint16_t *buf, int word, int nwords, const char *str)
{
    int i;

    for (i = 0; i < nwords; i++) {
        char a = *str ? *str++ : ' ';
        char b = *str ? *str++ : ' ';
        /* identify strings are byte-swapped within each word */
        buf[word + i] = (a << 8) | b;
    }
}

static void s5l8702_ata_build_identify(S5L8702AtaState *s)
{
    uint16_t *id = s->data_buf;
    uint64_t sectors = s->nb_sectors;
    uint32_t lba28 = sectors >= 0x0fffffff ? 0x0fffffff : sectors;

    memset(id, 0, sizeof(s->data_buf));
    id[0] = 0x0040;                 /* fixed device */
    id[1] = 16383;                  /* logical cylinders */
    id[3] = 16;                     /* heads */
    id[6] = 63;                     /* sectors per track */
    put_le16_str(id, 10, 10, "RB00000000000000000000");  /* serial */
    put_le16_str(id, 23, 4, "1.00");                     /* firmware */
    put_le16_str(id, 27, 20, "iPod6G QEMU disk");        /* model */
    id[47] = 0x8010;                /* max sectors per READ/WRITE MULTIPLE */
    id[49] = 0x0300;                /* LBA + DMA supported */
    id[53] = 0x0007;                /* words 64-70, 88 valid */
    id[60] = lba28 & 0xffff;        /* LBA28 capacity */
    id[61] = (lba28 >> 16) & 0xffff;
    id[63] = 0x0007;                /* MWDMA 0-2 supported */
    id[64] = 0x0003;                /* PIO modes 3,4 */
    id[80] = 0x00f0;                /* supports ATA-4..7 */
    id[82] = 0x0060;                /* write cache + read lookahead */
    id[83] = 0x6400;                /* LBA48 (bit10) + FLUSH EXT (bit13) */
    id[84] = 0x4000;
    id[85] = 0x0060;
    id[86] = 0x6400;
    id[87] = 0x4000;
    id[88] = 0x203f;                /* UDMA 0-5, mode 5 selected */
    id[93] = 0x4001;               /* PATA: w93 bit13 clear, bit0 set */
    id[100] = sectors & 0xffff;     /* LBA48 capacity */
    id[101] = (sectors >> 16) & 0xffff;
    id[102] = (sectors >> 32) & 0xffff;
    id[103] = (sectors >> 48) & 0xffff;
    id[106] = 0x4000;               /* 512-byte logical sectors */
}

static uint64_t s5l8702_ata_lba(S5L8702AtaState *s, uint32_t *count)
{
    if (s->lba48_cmd) {
        *count = ((uint32_t)s->scr_prev << 8) | s->scr_cur;
        if (*count == 0) {
            *count = 65536;
        }
        return ((uint64_t)s->lhr_prev << 40) | ((uint64_t)s->lmr_prev << 32) |
               ((uint64_t)s->llr_prev << 24) | ((uint64_t)s->lhr_cur << 16) |
               ((uint64_t)s->lmr_cur << 8) | s->llr_cur;
    }
    *count = s->scr_cur ? s->scr_cur : 256;
    return (((uint32_t)s->dvr & 0xf) << 24) | ((uint32_t)s->lhr_cur << 16) |
           ((uint32_t)s->lmr_cur << 8) | s->llr_cur;
}

/* Execute the DMA transfer triggered by ATA_COMMAND bit 0 */
static void s5l8702_ata_run_dma(S5L8702AtaState *s)
{
    uint32_t bytes = s->xfr_num + 1;
    bool write = (s->cfg & CFG_DIR_WRITE) != 0;
    uint32_t addr = write ? s->sbuf_start : s->tbuf_start;
    uint32_t count;
    uint64_t lba = s5l8702_ata_lba(s, &count);
    g_autofree uint8_t *buf = g_malloc(bytes);

    if (!s->blk) {
        s->status = ATA_DRDY | ATA_ERR;
        goto done;
    }
    if (write) {
        cpu_physical_memory_read(addr, buf, bytes);
        blk_pwrite(s->blk, lba * SECTOR_SIZE, bytes, buf, 0);
    } else {
        blk_pread(s->blk, lba * SECTOR_SIZE, bytes, buf, 0);
        cpu_physical_memory_write(addr, buf, bytes);
    }
    s->status = ATA_DRDY;   /* DRQ cleared: end of transfer */

done:
    s->irq_stat |= 1;       /* transfer-done interrupt */
    s5l8702_ata_update_irq(s);
}

static void s5l8702_ata_command(S5L8702AtaState *s, uint8_t cmd)
{
    s->command = cmd;
    s->status = ATA_DRDY;

    switch (cmd) {
    case CMD_IDENTIFY:
        s5l8702_ata_build_identify(s);
        s->data_pos = 0;
        s->data_len = IDENTIFY_WORDS;
        s->status = ATA_DRDY | ATA_DRQ;     /* start of transfer */
        break;

    case CMD_READ_DMA_EXT:
    case CMD_WRITE_DMA_EXT:
    case CMD_READ_SECT_EXT:
    case CMD_WRITE_SECT_EXT:
        s->lba48_cmd = true;
        s->status = ATA_DRDY | ATA_DRQ;     /* ready for DMA engine */
        break;

    case CMD_READ_DMA:
    case CMD_WRITE_DMA:
    case CMD_READ_SECTORS:
    case CMD_WRITE_SECTORS:
    case CMD_READ_MULTIPLE:
    case CMD_WRITE_MULTIPLE:
        s->lba48_cmd = false;
        s->status = ATA_DRDY | ATA_DRQ;
        break;

    case CMD_SET_FEATURES:
    case CMD_SET_MULTIPLE:
    case CMD_FLUSH_CACHE:
    case CMD_FLUSH_CACHE_EXT:
    case CMD_STANDBY_IMM:
    case CMD_SLEEP:
        s->status = ATA_DRDY;               /* immediate completion */
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8702-ata: unknown command 0x%02x\n", cmd);
        s->status = ATA_DRDY | ATA_ERR;
        break;
    }
}

/* Latch a taskfile register value for the read handshake */
static uint32_t s5l8702_ata_taskfile_read(S5L8702AtaState *s, hwaddr offset)
{
    switch (offset) {
    case REG_PIO_DTR:
        if (s->data_pos < s->data_len) {
            return s->data_buf[s->data_pos++];
        }
        return 0;
    case REG_PIO_FED:
        return s->fed;
    case REG_PIO_SCR:
        return s->scr_cur;
    case REG_PIO_LLR:
        return s->llr_cur;
    case REG_PIO_LMR:
        return s->lmr_cur;
    case REG_PIO_LHR:
        return s->lhr_cur;
    case REG_PIO_DVR:
        return s->dvr;
    case REG_PIO_CSD:
    case REG_PIO_DAD:
        return s->status;
    default:
        return 0;
    }
}

static uint64_t s5l8702_ata_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702AtaState *s = opaque;

    if (offset >= REG_PIO_DTR && offset <= REG_PIO_DAD) {
        /* taskfile access also latches the value for ATA_PIO_RDATA */
        s->rdata = s5l8702_ata_taskfile_read(s, offset);
        return s->rdata;
    }

    switch (offset) {
    case REG_CONTROL:
        /* bit1: "clock down" acknowledged when disabled (bit0 == 0) */
        return s->control | ((s->control & 1) ? 0 : 2);
    case REG_STATUS:
        return 0;
    case REG_COMMAND:
        return 0;
    case REG_IRQ:
        return s->irq_stat;
    case REG_IRQ_MASK:
        return s->irq_mask;
    case REG_CFG:
        return s->cfg;
    case REG_XFR_NUM:
        return s->xfr_num;
    case REG_XFR_CNT:
        return s->xfr_num;       /* whole transfer consumed */
    case REG_TBUF_START:
        return s->tbuf_start;
    case REG_TBUF_SIZE:
        return s->tbuf_size;
    case REG_SBUF_START:
        return s->sbuf_start;
    case REG_SBUF_SIZE:
        return s->sbuf_size;
    case REG_CADR_TBUF:
        return s->tbuf_start + s->xfr_num + 1;
    case REG_CADR_SBUF:
        return s->sbuf_start + s->xfr_num + 1;
    case REG_PIO_READY:
        return 0x3;              /* always ready to access / data valid */
    case REG_PIO_RDATA:
        return s->rdata;
    default:
        return 0;
    }
}

static void s5l8702_ata_write(void *opaque, hwaddr offset,
                              uint64_t val, unsigned size)
{
    S5L8702AtaState *s = opaque;

    switch (offset) {
    case REG_CONTROL:
        s->control = val;
        return;
    case REG_SWRST:
        if (val & 1) {
            s->status = ATA_DRDY;
            s->irq_stat = 0;
        }
        return;
    case REG_COMMAND:
        if (val & 1) {
            /* start the DMA engine for the pending command */
            s5l8702_ata_run_dma(s);
        }
        /* bit1 = acknowledge / stop: nothing to do */
        return;
    case REG_IRQ:
        s->irq_stat &= ~val;     /* write-1-to-clear */
        s5l8702_ata_update_irq(s);
        return;
    case REG_IRQ_MASK:
        s->irq_mask = val;
        s5l8702_ata_update_irq(s);
        return;
    case REG_CFG:
        s->cfg = val;
        return;
    case REG_XFR_NUM:
        s->xfr_num = val;
        return;
    case REG_TBUF_START:
        s->tbuf_start = val;
        return;
    case REG_TBUF_SIZE:
        s->tbuf_size = val;
        return;
    case REG_SBUF_START:
        s->sbuf_start = val;
        return;
    case REG_SBUF_SIZE:
        s->sbuf_size = val;
        return;
    case REG_MDMA_TIME:
    case REG_PIO_TIME:
    case REG_UDMA_TIME:
        return;

    /* taskfile writes (HOB FIFOs) */
    case REG_PIO_DTR:
        return;     /* PIO writes not used (DMA path only) */
    case REG_PIO_FED:
        s->fed = val;
        return;
    case REG_PIO_SCR:
        s->scr_prev = s->scr_cur;
        s->scr_cur = val;
        return;
    case REG_PIO_LLR:
        s->llr_prev = s->llr_cur;
        s->llr_cur = val;
        return;
    case REG_PIO_LMR:
        s->lmr_prev = s->lmr_cur;
        s->lmr_cur = val;
        return;
    case REG_PIO_LHR:
        s->lhr_prev = s->lhr_cur;
        s->lhr_cur = val;
        return;
    case REG_PIO_DVR:
        s->dvr = val;
        return;
    case REG_PIO_CSD:
        s5l8702_ata_command(s, val);
        return;
    case REG_PIO_DAD:
        return;     /* device control */
    default:
        return;
    }
}

static const MemoryRegionOps s5l8702_ata_ops = {
    .read = s5l8702_ata_read,
    .write = s5l8702_ata_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void s5l8702_ata_reset_hold(Object *obj, ResetType type)
{
    S5L8702AtaState *s = S5L8702_ATA(obj);

    s->control = 0;
    s->cfg = 0;
    s->irq_stat = 0;
    s->irq_mask = 0;
    s->xfr_num = 0;
    s->status = ATA_DRDY;
    s->data_pos = 0;
    s->data_len = 0;
    s->lba48_cmd = false;
    qemu_set_irq(s->irq, 0);
}

static void s5l8702_ata_realize(DeviceState *dev, Error **errp)
{
    S5L8702AtaState *s = S5L8702_ATA(dev);

    if (s->blk) {
        uint64_t bytes = blk_getlength(s->blk);

        s->nb_sectors = bytes / SECTOR_SIZE;
        blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_ALL, &error_abort);
    }
}

static void s5l8702_ata_init(Object *obj)
{
    S5L8702AtaState *s = S5L8702_ATA(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_ata_ops, s,
                          "s5l8702-ata", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const Property s5l8702_ata_properties[] = {
    DEFINE_PROP_DRIVE("drive", S5L8702AtaState, blk),
};

static const VMStateDescription vmstate_s5l8702_ata = {
    .name = "s5l8702-ata",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, S5L8702AtaState),
        VMSTATE_UINT32(cfg, S5L8702AtaState),
        VMSTATE_UINT32(irq_stat, S5L8702AtaState),
        VMSTATE_UINT32(irq_mask, S5L8702AtaState),
        VMSTATE_UINT32(xfr_num, S5L8702AtaState),
        VMSTATE_UINT32(tbuf_start, S5L8702AtaState),
        VMSTATE_UINT32(sbuf_start, S5L8702AtaState),
        VMSTATE_UINT8(scr_cur, S5L8702AtaState),
        VMSTATE_UINT8(scr_prev, S5L8702AtaState),
        VMSTATE_UINT8(llr_cur, S5L8702AtaState),
        VMSTATE_UINT8(lmr_cur, S5L8702AtaState),
        VMSTATE_UINT8(lhr_cur, S5L8702AtaState),
        VMSTATE_UINT8(dvr, S5L8702AtaState),
        VMSTATE_UINT8(command, S5L8702AtaState),
        VMSTATE_UINT8(status, S5L8702AtaState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8702_ata_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = s5l8702_ata_realize;
    rc->phases.hold = s5l8702_ata_reset_hold;
    dc->vmsd = &vmstate_s5l8702_ata;
    device_class_set_props(dc, s5l8702_ata_properties);
}

static const TypeInfo s5l8702_ata_info = {
    .name = TYPE_S5L8702_ATA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702AtaState),
    .instance_init = s5l8702_ata_init,
    .class_init = s5l8702_ata_class_init,
};

static void s5l8702_ata_register_types(void)
{
    type_register_static(&s5l8702_ata_info);
}

type_init(s5l8702_ata_register_types)
