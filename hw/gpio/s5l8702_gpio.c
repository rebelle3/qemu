/*
 * Samsung S5L8702 GPIO block (iPod Classic 6G).
 *
 * 16 ports with PCON/PDAT/PUNA/PUNB/PUNC at a 0x20 stride, plus the
 * GPIOCMD configuration command register at +0x200.
 *
 * Two board straps matter to Rockbox on the iPod 6G:
 *  - PDAT6 bits [5:4]: LCD panel type (selects the init command set),
 *  - PDAT11 bit 1: 0 = plain PATA disk, 1 = CE-ATA disk.
 *
 * Input pin values are provided via the "in-mask"/"in-val" pairs set
 * by the board at creation time; all other registers behave like RAM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_S5L8702_GPIO "s5l8702-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702GpioState, S5L8702_GPIO)

#define GPIO_N_PORTS   16
#define GPIO_MMIO_SIZE 0x400

#define REG_GPIOCMD    0x200

struct S5L8702GpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t pcon[GPIO_N_PORTS];
    uint32_t pdat[GPIO_N_PORTS];   /* output latch */
    uint32_t pun[3][GPIO_N_PORTS];
    uint32_t gpiocmd;
    uint32_t unk380;

    /* externally driven input pins */
    uint32_t in_mask[GPIO_N_PORTS];
    uint32_t in_val[GPIO_N_PORTS];

    /* board straps */
    uint8_t lcd_type;
    bool ceata;
};

/* Allow the board (or sibling devices) to drive input pins. */
void s5l8702_gpio_set_input(DeviceState *dev, int port, uint32_t mask,
                            uint32_t val);

void s5l8702_gpio_set_input(DeviceState *dev, int port, uint32_t mask,
                            uint32_t val)
{
    S5L8702GpioState *s = S5L8702_GPIO(dev);

    s->in_mask[port] |= mask;
    s->in_val[port] = (s->in_val[port] & ~mask) | (val & mask);
}

static uint64_t s5l8702_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702GpioState *s = opaque;

    if (offset < GPIO_N_PORTS * 0x20) {
        int port = offset >> 5;

        switch (offset & 0x1f) {
        case 0x00:
            return s->pcon[port];
        case 0x04:
            return (s->pdat[port] & ~s->in_mask[port])
                   | (s->in_val[port] & s->in_mask[port]);
        case 0x08:
            return s->pun[0][port];
        case 0x0c:
            return s->pun[1][port];
        case 0x10:
            return s->pun[2][port];
        default:
            return 0;
        }
    }
    switch (offset) {
    case REG_GPIOCMD:
        return s->gpiocmd;
    case 0x380:
        return s->unk380;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8702-gpio: bad read offset 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void s5l8702_gpio_write(void *opaque, hwaddr offset,
                               uint64_t val, unsigned size)
{
    S5L8702GpioState *s = opaque;

    if (offset < GPIO_N_PORTS * 0x20) {
        int port = offset >> 5;

        switch (offset & 0x1f) {
        case 0x00:
            s->pcon[port] = val;
            return;
        case 0x04:
            s->pdat[port] = val;
            return;
        case 0x08:
            s->pun[0][port] = val;
            return;
        case 0x0c:
            s->pun[1][port] = val;
            return;
        case 0x10:
            s->pun[2][port] = val;
            return;
        default:
            return;
        }
    }
    switch (offset) {
    case REG_GPIOCMD:
        /*
         * Pin function configuration command; the relevant effects
         * (alternate function selection) have no observable behaviour
         * in this model.
         */
        s->gpiocmd = val;
        return;
    case 0x380:
        s->unk380 = val;
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8702-gpio: bad write offset 0x%" HWADDR_PRIx "\n",
                      offset);
    }
}

static const MemoryRegionOps s5l8702_gpio_ops = {
    .read = s5l8702_gpio_read,
    .write = s5l8702_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8702_gpio_reset_hold(Object *obj, ResetType type)
{
    S5L8702GpioState *s = S5L8702_GPIO(obj);

    memset(s->pcon, 0, sizeof(s->pcon));
    memset(s->pdat, 0, sizeof(s->pdat));
    memset(s->pun, 0, sizeof(s->pun));
    s->gpiocmd = 0;

    /* Board straps (constant inputs) */
    memset(s->in_mask, 0, sizeof(s->in_mask));
    memset(s->in_val, 0, sizeof(s->in_val));

    /* PDAT6[5:4]: LCD type */
    s->in_mask[6] |= 0x30;
    s->in_val[6] |= (s->lcd_type & 3) << 4;

    /* PDAT11[1]: CE-ATA strap (0 = PATA) */
    s->in_mask[11] |= 0x02;
    s->in_val[11] |= s->ceata ? 0x02 : 0x00;
}

static void s5l8702_gpio_init(Object *obj)
{
    S5L8702GpioState *s = S5L8702_GPIO(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_gpio_ops, s,
                          "s5l8702-gpio", GPIO_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property s5l8702_gpio_properties[] = {
    DEFINE_PROP_UINT8("lcd-type", S5L8702GpioState, lcd_type, 2),
    DEFINE_PROP_BOOL("ceata", S5L8702GpioState, ceata, false),
};

static const VMStateDescription vmstate_s5l8702_gpio = {
    .name = "s5l8702-gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(pcon, S5L8702GpioState, GPIO_N_PORTS),
        VMSTATE_UINT32_ARRAY(pdat, S5L8702GpioState, GPIO_N_PORTS),
        VMSTATE_UINT32_2DARRAY(pun, S5L8702GpioState, 3, GPIO_N_PORTS),
        VMSTATE_UINT32(gpiocmd, S5L8702GpioState),
        VMSTATE_UINT32(unk380, S5L8702GpioState),
        VMSTATE_UINT32_ARRAY(in_mask, S5L8702GpioState, GPIO_N_PORTS),
        VMSTATE_UINT32_ARRAY(in_val, S5L8702GpioState, GPIO_N_PORTS),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8702_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = s5l8702_gpio_reset_hold;
    dc->vmsd = &vmstate_s5l8702_gpio;
    device_class_set_props(dc, s5l8702_gpio_properties);
}

static const TypeInfo s5l8702_gpio_info = {
    .name = TYPE_S5L8702_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702GpioState),
    .instance_init = s5l8702_gpio_init,
    .class_init = s5l8702_gpio_class_init,
};

static void s5l8702_gpio_register_types(void)
{
    type_register_static(&s5l8702_gpio_info);
}

type_init(s5l8702_gpio_register_types)
