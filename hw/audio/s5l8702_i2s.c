/*
 * Samsung S5L8702 I2S controller (iPod Classic 6G) - audio output.
 *
 * Samples written to the TX data buffer (I2STXDB0, +0x10) by the PL080
 * DMA controller are forwarded to the QEMU audio subsystem when an
 * audiodev is configured (fixed 44.1 kHz stereo S16, matching what
 * Rockbox programs on this machine).  Pacing comes from the host audio
 * backend: the DMA request line is asserted while the staging ring has
 * room, and the backend's pull callback drains it.
 *
 * Without an audiodev the device falls back to discarding samples at
 * the nominal sample rate (a periodic timer grants "credits"), so the
 * guest's playback clock still advances in real time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/audio.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
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

#define SAMPLE_RATE_HZ  44100

/* fallback pacing (no audiodev) */
#define PACE_PERIOD_NS  (10 * 1000 * 1000)  /* 10 ms */
/* 16-bit writes per pace period: rate * 2 channels / 100 */
#define CREDITS_PER_PERIOD  (SAMPLE_RATE_HZ * 2 / 100)
#define CREDITS_MAX         (CREDITS_PER_PERIOD * 4)

/* staging ring towards the audio backend, in 16-bit units */
#define RING_SIZE  16384

struct S5L8702I2SState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq dreq;          /* DMA request towards the PL080 */

    uint32_t clkcon;
    uint32_t txcon;
    uint32_t txcom;
    uint32_t clkdiv;

    /* audio backend path */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    int16_t ring[RING_SIZE];
    uint32_t ring_head;     /* oldest sample */
    uint32_t ring_len;

    /* fallback path */
    int32_t credits;
    QEMUTimer pace_timer;
};

static bool s5l8702_i2s_running(S5L8702I2SState *s)
{
    return (s->txcom & TXCOM_RUN) != 0;
}

static void s5l8702_i2s_update_dreq(S5L8702I2SState *s)
{
    bool room;

    if (s->voice) {
        room = s->ring_len < RING_SIZE;
    } else {
        room = s->credits > 0;
    }
    qemu_set_irq(s->dreq, s5l8702_i2s_running(s) && room);
}

/* audio backend pulls samples */
static void s5l8702_i2s_audio_cb(void *opaque, int avail)
{
    S5L8702I2SState *s = opaque;

    while (avail > 1 && s->ring_len > 0) {
        uint32_t chunk = MIN((uint32_t)avail / 2, s->ring_len);
        size_t written;

        chunk = MIN(chunk, RING_SIZE - s->ring_head);
        written = audio_be_write(s->audio_be, s->voice,
                                 &s->ring[s->ring_head], chunk * 2);
        if (written == 0) {
            break;
        }
        s->ring_head = (s->ring_head + written / 2) % RING_SIZE;
        s->ring_len -= written / 2;
        avail -= written;
    }
    s5l8702_i2s_update_dreq(s);
}

static void s5l8702_i2s_push(S5L8702I2SState *s, uint16_t sample)
{
    if (s->voice) {
        if (s->ring_len < RING_SIZE) {
            s->ring[(s->ring_head + s->ring_len) % RING_SIZE] = sample;
            s->ring_len++;
        }
    } else if (s->credits > 0) {
        s->credits--;
    }
    s5l8702_i2s_update_dreq(s);
}

/* fallback pacing when no audio backend is configured */
static void s5l8702_i2s_pace(void *opaque)
{
    S5L8702I2SState *s = opaque;

    if (s5l8702_i2s_running(s)) {
        s->credits = MIN(s->credits + CREDITS_PER_PERIOD, CREDITS_MAX);
        s5l8702_i2s_update_dreq(s);
        timer_mod(&s->pace_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PACE_PERIOD_NS);
    }
}

static void s5l8702_i2s_set_running(S5L8702I2SState *s, bool run)
{
    if (s->voice) {
        audio_be_set_active_out(s->audio_be, s->voice, run);
    } else if (run) {
        timer_mod(&s->pace_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PACE_PERIOD_NS);
    }
    s5l8702_i2s_update_dreq(s);
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
        s5l8702_i2s_set_running(s, s5l8702_i2s_running(s));
        break;
    case REG_I2STXDB0:
        s5l8702_i2s_push(s, val & 0xffff);
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
    s->ring_head = 0;
    s->ring_len = 0;
    if (s->voice) {
        audio_be_set_active_out(s->audio_be, s->voice, false);
    }
    qemu_set_irq(s->dreq, 0);
}

static void s5l8702_i2s_realize(DeviceState *dev, Error **errp)
{
    S5L8702I2SState *s = S5L8702_I2S(dev);

    if (s->audio_be) {
        struct audsettings as = {
            .freq = SAMPLE_RATE_HZ,
            .nchannels = 2,
            .fmt = AUDIO_FORMAT_S16,
            .big_endian = false,
        };

        s->voice = audio_be_open_out(s->audio_be, s->voice, "s5l8702-i2s",
                                     s, s5l8702_i2s_audio_cb, &as);
        if (!s->voice) {
            error_setg(errp, "s5l8702-i2s: could not open audio voice");
        }
    }
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

static const Property s5l8702_i2s_properties[] = {
    DEFINE_AUDIO_PROPERTIES(S5L8702I2SState, audio_be),
};

static const VMStateDescription vmstate_s5l8702_i2s = {
    .name = "s5l8702-i2s",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(clkcon, S5L8702I2SState),
        VMSTATE_UINT32(txcon, S5L8702I2SState),
        VMSTATE_UINT32(txcom, S5L8702I2SState),
        VMSTATE_UINT32(clkdiv, S5L8702I2SState),
        VMSTATE_INT32(credits, S5L8702I2SState),
        VMSTATE_INT16_ARRAY(ring, S5L8702I2SState, RING_SIZE),
        VMSTATE_UINT32(ring_head, S5L8702I2SState),
        VMSTATE_UINT32(ring_len, S5L8702I2SState),
        VMSTATE_TIMER(pace_timer, S5L8702I2SState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8702_i2s_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = s5l8702_i2s_realize;
    rc->phases.hold = s5l8702_i2s_reset_hold;
    dc->vmsd = &vmstate_s5l8702_i2s;
    device_class_set_props(dc, s5l8702_i2s_properties);
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
