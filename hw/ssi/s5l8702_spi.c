/*
 * Samsung S5L8702 SPI controller with the iPod Classic 6G boot NOR.
 *
 * Rockbox talks to the 1 MB serial boot flash through SPI port 0
 * (spi-s5l8702.c / norboot-s5l8702.c).  The driver bit-bangs the chip
 * select through a GPIO and frames standard SST25-style commands over
 * the controller's TX/RX data registers:
 *
 *   - 0x05 RDSR : read status register (WIP polled in bit 0)
 *   - 0x03 READ : 3-byte big-endian address, then stream bytes out
 *   - 0x06/0x04 WREN/WRDI, 0x50 EWSR, 0x01 WRSR, 0x20 erase, 0xAD AAI
 *     (write path; modelled only far enough not to desync reads)
 *
 * Only the data path matters for emulation, so the model decodes the
 * command stream directly from TXDATA writes (which are only ever
 * opcode/address bytes) and serves READ data from RXDATA.  The status
 * register always reports "transfer complete / FIFO ready" so the
 * driver's wait loops terminate immediately:
 *
 *   spi_write():  while ((STATUS & 0x1f0) == 0x100);  -> need != 0x100
 *                 while (!(STATUS & 0x3e00));          -> need a bit set
 *   spi_read():   while (!(STATUS & 0x3e00));          (per byte)
 *
 * The NOR contents come from a -drive if=mtd image; with no drive the
 * flash reads as erased (0xff), which makes SysCfg degrade gracefully
 * instead of hanging.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/block-backend.h"
#include "qapi/error.h"

#define TYPE_S5L8702_SPI "s5l8702-spi"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702SpiState, S5L8702_SPI)

/* register offsets */
#define REG_CTRL     0x00
#define REG_SETUP    0x04
#define REG_STATUS   0x08
#define REG_PIN      0x0c
#define REG_TXDATA   0x10
#define REG_RXDATA   0x20
#define REG_CLKDIV   0x30
#define REG_RXLIMIT  0x34
#define REG_DD       0x38

/* status bits the driver polls (see spi-s5l8702.c) */
#define STATUS_READY 0x3e00      /* RX/transfer ready: any of these set */
/* the 0x1f0 TX-space field must never read back as exactly 0x100 */

/* serial-flash commands */
#define CMD_WRSR     0x01
#define CMD_READ     0x03
#define CMD_WRDI     0x04
#define CMD_RDSR     0x05
#define CMD_WREN     0x06
#define CMD_ERASE    0x20
#define CMD_EWSR     0x50
#define CMD_AAI      0xad

/* command FSM phase */
enum {
    PHASE_CMD = 0,   /* expecting an opcode */
    PHASE_ADDR,      /* collecting big-endian address bytes */
    PHASE_STATUS,    /* RDSR: next transfer clocks out the status byte */
    PHASE_SWALLOW,   /* WRSR: next transfer is the status value, ignored */
    PHASE_READ,      /* positioned; RXDATA streams flash bytes */
};

struct S5L8702SpiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    BlockBackend *blk;

    uint8_t *nor;
    uint32_t nor_size;

    /* shadowed registers */
    uint32_t ctrl, setup, pin, clkdiv, rxlimit;

    /* command stream decoder */
    uint8_t phase;
    uint8_t cmd;
    uint8_t addr_cnt;
    uint32_t addr;
    uint32_t read_ptr;
    uint8_t rx_byte;     /* full-duplex byte returned by a TXDATA transfer */
};

static void s5l8702_spi_tx(S5L8702SpiState *s, uint8_t b)
{
    switch (s->phase) {
    case PHASE_ADDR:
        s->addr = (s->addr << 8) | b;
        s->rx_byte = 0xff;
        if (--s->addr_cnt == 0) {
            if (s->cmd == CMD_READ) {
                s->read_ptr = s->addr % s->nor_size;
                s->phase = PHASE_READ;
            } else {
                s->phase = PHASE_CMD;   /* erase: address noted, no-op */
            }
        }
        return;

    case PHASE_STATUS:
        /* status register: WIP (bit 0) clear -> always ready */
        s->rx_byte = 0x00;
        s->phase = PHASE_CMD;
        return;

    case PHASE_SWALLOW:
        s->rx_byte = 0xff;
        s->phase = PHASE_CMD;
        return;

    default:
        break;      /* PHASE_CMD / PHASE_READ: a write starts a new frame */
    }

    /* decode a fresh opcode */
    s->cmd = b;
    s->rx_byte = 0xff;
    switch (b) {
    case CMD_READ:
        s->addr = 0;
        s->addr_cnt = 3;
        s->phase = PHASE_ADDR;
        break;
    case CMD_ERASE:
        s->addr = 0;
        s->addr_cnt = 3;
        s->phase = PHASE_ADDR;
        break;
    case CMD_RDSR:
        s->phase = PHASE_STATUS;
        break;
    case CMD_WRSR:
        s->phase = PHASE_SWALLOW;
        break;
    case CMD_WREN:
    case CMD_WRDI:
    case CMD_EWSR:
    default:
        /* standalone, no argument bytes; writes are not persisted */
        s->phase = PHASE_CMD;
        break;
    }
}

static uint8_t s5l8702_spi_rx(S5L8702SpiState *s)
{
    if (s->phase == PHASE_READ && (s->setup & 1)) {
        uint8_t v = s->nor[s->read_ptr % s->nor_size];
        s->read_ptr++;
        return v;
    }
    return s->rx_byte;
}

static uint64_t s5l8702_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702SpiState *s = opaque;

    switch (offset) {
    case REG_CTRL:    return s->ctrl;
    case REG_SETUP:   return s->setup;
    case REG_STATUS:  return STATUS_READY;   /* always ready, TX field 0 */
    case REG_PIN:     return s->pin;
    case REG_RXDATA:  return s5l8702_spi_rx(s);
    case REG_CLKDIV:  return s->clkdiv;
    case REG_RXLIMIT: return s->rxlimit;
    default:          return 0;
    }
}

static void s5l8702_spi_write(void *opaque, hwaddr offset, uint64_t val,
                              unsigned size)
{
    S5L8702SpiState *s = opaque;

    switch (offset) {
    case REG_CTRL:    s->ctrl = val; break;
    case REG_SETUP:   s->setup = val; break;
    case REG_STATUS:  break;                 /* write-1-to-clear, no-op */
    case REG_PIN:     s->pin = val; break;
    case REG_TXDATA:  s5l8702_spi_tx(s, val & 0xff); break;
    case REG_CLKDIV:  s->clkdiv = val; break;
    case REG_RXLIMIT: s->rxlimit = val; break;
    default:          break;
    }
}

static const MemoryRegionOps s5l8702_spi_ops = {
    .read = s5l8702_spi_read,
    .write = s5l8702_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8702_spi_reset_hold(Object *obj, ResetType type)
{
    S5L8702SpiState *s = S5L8702_SPI(obj);

    s->ctrl = s->setup = s->pin = s->rxlimit = 0;
    s->clkdiv = 4;
    s->phase = PHASE_CMD;
    s->cmd = 0;
    s->addr = s->addr_cnt = 0;
    s->read_ptr = 0;
    s->rx_byte = 0xff;
}

static void s5l8702_spi_realize(DeviceState *dev, Error **errp)
{
    S5L8702SpiState *s = S5L8702_SPI(dev);

    s->nor_size = 0x100000;       /* 1 MB iPod Classic boot NOR */
    if (s->blk) {
        uint64_t len = blk_getlength(s->blk);
        if (len > 0) {
            s->nor_size = len;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
    }
    s->nor = g_malloc(s->nor_size);
    memset(s->nor, 0xff, s->nor_size);
    if (s->blk) {
        if (blk_pread(s->blk, 0, s->nor_size, s->nor, 0) < 0) {
            error_setg(errp, "s5l8702-spi: cannot read NOR image");
            return;
        }
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &s5l8702_spi_ops, s,
                          TYPE_S5L8702_SPI, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_s5l8702_spi = {
    .name = TYPE_S5L8702_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, S5L8702SpiState),
        VMSTATE_UINT32(setup, S5L8702SpiState),
        VMSTATE_UINT32(pin, S5L8702SpiState),
        VMSTATE_UINT32(clkdiv, S5L8702SpiState),
        VMSTATE_UINT32(rxlimit, S5L8702SpiState),
        VMSTATE_UINT8(phase, S5L8702SpiState),
        VMSTATE_UINT8(cmd, S5L8702SpiState),
        VMSTATE_UINT8(addr_cnt, S5L8702SpiState),
        VMSTATE_UINT32(addr, S5L8702SpiState),
        VMSTATE_UINT32(read_ptr, S5L8702SpiState),
        VMSTATE_UINT8(rx_byte, S5L8702SpiState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property s5l8702_spi_props[] = {
    DEFINE_PROP_DRIVE("drive", S5L8702SpiState, blk),
};

static void s5l8702_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = s5l8702_spi_realize;
    rc->phases.hold = s5l8702_spi_reset_hold;
    dc->vmsd = &vmstate_s5l8702_spi;
    device_class_set_props(dc, s5l8702_spi_props);
}

static const TypeInfo s5l8702_spi_types[] = {
    {
        .name = TYPE_S5L8702_SPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8702SpiState),
        .class_init = s5l8702_spi_class_init,
    },
};

DEFINE_TYPES(s5l8702_spi_types)
