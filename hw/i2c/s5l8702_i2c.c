/*
 * Samsung S5L8702 I2C controller.
 *
 * Register interface resembling the S3C24xx IIC block with some
 * S5L-specific quirks Rockbox relies on:
 *  - +0x10 ("IICUNK10") is polled until it reads 0 before every access,
 *  - +0x20 ("IICSTA2") holds byte-complete (bit 8) and stop-detected
 *    (bit 13) flags, cleared by writing 1s,
 *  - IICSTAT bit 5 reads as "bus busy" while a transfer is in progress,
 *    bit 0 reads as 1 when the last byte was NACKed,
 *  - writing IICCON clocks the next byte in (rx) or out (tx).
 *
 * Transfers complete instantly in this model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_S5L8702_I2C "s5l8702-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702I2CState, S5L8702_I2C)

#define REG_IICCON   0x00
#define REG_IICSTAT  0x04
#define REG_IICADD   0x08
#define REG_IICDS    0x0c
#define REG_IICUNK10 0x10
#define REG_IICSTA2  0x20

/* IICSTAT bits */
#define IICSTAT_START   0x20    /* write: generate START/output enable */
#define IICSTAT_BUSY    0x20    /* read: bus busy */
#define IICSTAT_NACK    0x01

/* IICSTA2 bits */
#define IICSTA2_BYTE    (1 << 8)
#define IICSTA2_STOP    (1 << 13)

struct S5L8702I2CState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;

    uint32_t iiccon;
    uint32_t iicstat_mode;  /* bits 6-7 written by guest */
    uint32_t iicadd;
    uint32_t iicds;
    uint32_t iicsta2;
    bool busy;              /* transfer in progress */
    bool nack;
    bool recv;              /* current transfer is a read */
    bool ds_loaded;         /* IICDS written since last byte sent */
};

static uint64_t s5l8702_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702I2CState *s = opaque;

    switch (offset) {
    case REG_IICCON:
        return s->iiccon;
    case REG_IICSTAT:
        return s->iicstat_mode
               | (s->busy ? IICSTAT_BUSY : 0)
               | (s->nack ? IICSTAT_NACK : 0);
    case REG_IICADD:
        return s->iicadd;
    case REG_IICDS:
        return s->iicds;
    case REG_IICUNK10:
        return 0;   /* always ready */
    case REG_IICSTA2:
        return s->iicsta2;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8702-i2c: bad read offset 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void s5l8702_i2c_write(void *opaque, hwaddr offset,
                              uint64_t val, unsigned size)
{
    S5L8702I2CState *s = opaque;

    switch (offset) {
    case REG_IICCON:
        s->iiccon = val;
        if (s->busy) {
            if (s->recv) {
                /* clock in the next byte */
                s->iicds = i2c_recv(s->bus);
                s->iicsta2 |= IICSTA2_BYTE;
            } else if (s->ds_loaded) {
                s->nack = i2c_send(s->bus, s->iicds) != 0;
                s->ds_loaded = false;
                s->iicsta2 |= IICSTA2_BYTE;
            }
        }
        break;
    case REG_IICSTAT:
        s->iicstat_mode = val & 0xc0;
        if (val & IICSTAT_START) {
            /* START: IICDS holds address byte (bit 0 = read) */
            uint8_t addr = s->iicds;

            s->recv = addr & 1;
            s->nack = i2c_start_transfer(s->bus, addr >> 1, s->recv) != 0;
            s->busy = true;
            s->ds_loaded = false;
            s->iicsta2 |= IICSTA2_BYTE;
        } else if (s->busy) {
            /* START bit cleared: STOP condition */
            i2c_end_transfer(s->bus);
            s->busy = false;
            s->iicsta2 |= IICSTA2_STOP;
        }
        break;
    case REG_IICADD:
        s->iicadd = val;
        break;
    case REG_IICDS:
        s->iicds = val & 0xff;
        s->ds_loaded = true;
        break;
    case REG_IICSTA2:
        /* write-1-to-clear */
        s->iicsta2 &= ~val;
        break;
    case REG_IICUNK10:
    case 0x14:
    case 0x18:
    case 0x1c:
        /* clock/timing scratch registers */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8702-i2c: bad write offset 0x%" HWADDR_PRIx "\n",
                      offset);
    }
}

static const MemoryRegionOps s5l8702_i2c_ops = {
    .read = s5l8702_i2c_read,
    .write = s5l8702_i2c_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8702_i2c_reset_hold(Object *obj, ResetType type)
{
    S5L8702I2CState *s = S5L8702_I2C(obj);

    s->iiccon = 0;
    s->iicstat_mode = 0;
    s->iicadd = 0;
    s->iicds = 0;
    s->iicsta2 = 0;
    s->busy = false;
    s->nack = false;
    s->recv = false;
    s->ds_loaded = false;
}

static void s5l8702_i2c_init(Object *obj)
{
    S5L8702I2CState *s = S5L8702_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_i2c_ops, s,
                          "s5l8702-i2c", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static const VMStateDescription vmstate_s5l8702_i2c = {
    .name = "s5l8702-i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(iiccon, S5L8702I2CState),
        VMSTATE_UINT32(iicstat_mode, S5L8702I2CState),
        VMSTATE_UINT32(iicadd, S5L8702I2CState),
        VMSTATE_UINT32(iicds, S5L8702I2CState),
        VMSTATE_UINT32(iicsta2, S5L8702I2CState),
        VMSTATE_BOOL(busy, S5L8702I2CState),
        VMSTATE_BOOL(nack, S5L8702I2CState),
        VMSTATE_BOOL(recv, S5L8702I2CState),
        VMSTATE_BOOL(ds_loaded, S5L8702I2CState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8702_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = s5l8702_i2c_reset_hold;
    dc->vmsd = &vmstate_s5l8702_i2c;
}

static const TypeInfo s5l8702_i2c_info = {
    .name = TYPE_S5L8702_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702I2CState),
    .instance_init = s5l8702_i2c_init,
    .class_init = s5l8702_i2c_class_init,
};

static void s5l8702_i2c_register_types(void)
{
    type_register_static(&s5l8702_i2c_info);
}

type_init(s5l8702_i2c_register_types)
