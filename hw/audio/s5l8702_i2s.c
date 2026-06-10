/*
 * Samsung S5L8702 I2S controller (iPod Classic 6G) - audio sink.
 *
 * Minimal model: registers behave like RAM, and the TX data buffer
 * (I2STXDB0, +0x10) consumes samples delivered by the PL080 DMA
 * controller.  Consumption is paced at the configured sample rate via
 * a DREQ credit scheme so that Rockbox's playback clock advances in
 * real time: a periodic timer grants "sample credits"; while credits
 * are available the IIS0_TX DMA request line is asserted, and each
 * FIFO write spends one credit.  Samples themselves are discarded
 * (no host audio output).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_S5L8702_I2S "s5l8702-i2s"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702I2SState, S5L8702_I2S)

#define REG_I2SCLKCON  0x00
#define REG_I2STXCON   0x04
#define REG_I2STXCOM   0x08
#define REG_I2STXDB0   0x10
#define REG_I2SRXCON   0x30
#define REG_I2SRXCOM   0x34
#define REG_I2SRXDB    0x38
#define REG_I2SSTATUS  0x3c
#define REG_I2SCLKDIV  0x40

/* I2STXCOM: 0xe = run (transmit enabled), 0xa = stop */
#define TXCOM_RUN  (1 << 2)

#define PACE_PERIOD_NS  (10 * 1000 * 1000)  /* 10 ms */
#define SAMPLE_RATE_HZ  44100
/* 16-bit writes per pace period: rate * 2 channels / 100 */
#define CREDITS_PER_PERIOD  (SAMPLE_RATE_HZ * 2 / 100)
#define CREDITS_MAX         (CREDITS_PER_PERIOD * 4)

struct S5L8702I2SState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq dreq;          /* DMA request towards the PL080 */

    uint32_t clkcon;
    uint32_t txcon;
    uint32_t txcom;
    uint32_t clkdiv;

    int32_t credits;
    QEMUTimer pace_timer;
};

static void s5l8702_i2s_update_dreq(S5L8702I2SState *s)
{
    bool running = (s->txcom & TXCOM_RUN) != 0;

    qemu_set_irq(s->dreq, running && s->credits > 0);
}

static void s5l8702_i2s_pace(void *opaque)
{
    S5L8702I2SState *s = opaque;

    if (s->txcom & TXCOM_RUN) {
        s->credits = MIN(s->credits + CREDITS_PER_PERIOD, CREDITS_MAX);
        s5l8702_i2s_update_dreq(s);
        timer_mod(&s->pace_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PACE_PERIOD_NS);
    }
}

static uint64_t s5l8702_i2s_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702I2SState *s = opaque;

    switch (offset) {
    case REG_I2SCLKCON:
        return s->clkcon;
    case REG_I2STXCON:
        return s->txcon;
    case REG_I2STXCOM:
        return s->txcom;
    case REG_I2SSTATUS:
        /* TX FIFO empty-ish status; nothing polls this critically */
        return 0;
    case REG_I2SCLKDIV:
        return s->clkdiv;
    default:
        return 0;
    }
}

static void s5l8702_i2s_write(void *opaque, hwaddr offset,
                              uint64_t val, unsigned size)
{
    S5L8702I2SState *s = opaque;

    switch (offset) {
    case REG_I2SCLKCON:
        s->clkcon = val;
        break;
    case REG_I2STXCON:
        s->txcon = val;
        break;
    case REG_I2STXCOM:
        s->txcom = val;
        if (val & TXCOM_RUN) {
            timer_mod(&s->pace_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PACE_PERIOD_NS);
        }
        s5l8702_i2s_update_dreq(s);
        break;
    case REG_I2STXDB0:
        /* sample consumed and discarded */
        if (s->credits > 0) {
            s->credits--;
        }
        s5l8702_i2s_update_dreq(s);
        break;
    case REG_I2SCLKDIV:
        s->clkdiv = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps s5l8702_i2s_ops = {
    .read = s5l8702_i2s_read,
    .write = s5l8702_i2s_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void s5l8702_i2s_reset_hold(Object *obj, ResetType type)
{
    S5L8702I2SState *s = S5L8702_I2S(obj);

    timer_del(&s->pace_timer);
    s->clkcon = 0;
    s->txcon = 0;
    s->txcom = 0;
    s->clkdiv = 0;
    s->credits = 0;
    qemu_set_irq(s->dreq, 0);
}

static void s5l8702_i2s_init(Object *obj)
{
    S5L8702I2SState *s = S5L8702_I2S(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_i2s_ops, s,
                          "s5l8702-i2s", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), &s->dreq, "dreq", 1);
    timer_init_ns(&s->pace_timer, QEMU_CLOCK_VIRTUAL, s5l8702_i2s_pace, s);
}

static const VMStateDescription vmstate_s5l8702_i2s = {
    .name = "s5l8702-i2s",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(clkcon, S5L8702I2SState),
        VMSTATE_UINT32(txcon, S5L8702I2SState),
        VMSTATE_UINT32(txcom, S5L8702I2SState),
        VMSTATE_UINT32(clkdiv, S5L8702I2SState),
        VMSTATE_INT32(credits, S5L8702I2SState),
        VMSTATE_TIMER(pace_timer, S5L8702I2SState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8702_i2s_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = s5l8702_i2s_reset_hold;
    dc->vmsd = &vmstate_s5l8702_i2s;
}

static const TypeInfo s5l8702_i2s_info = {
    .name = TYPE_S5L8702_I2S,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702I2SState),
    .instance_init = s5l8702_i2s_init,
    .class_init = s5l8702_i2s_class_init,
};

static void s5l8702_i2s_register_types(void)
{
    type_register_static(&s5l8702_i2s_info);
}

type_init(s5l8702_i2s_register_types)
