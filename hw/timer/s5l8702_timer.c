/*
 * Samsung S5L8702 timer block (as used by the iPod Classic 6G).
 *
 * The block contains four 16-bit timers (A-D) and four 32-bit timers
 * (E-H).  Each timer has CON/CMD/DATA0/DATA1/PRE/CNT registers; the
 * interrupt status bits of the 32-bit timers live in the shared TSTAT
 * register (write-1-to-clear).  16-bit timer interrupt status lives in
 * bits [14:12] of the timer's own CON register.
 *
 * Rockbox uses timer E as a free-running 1 MHz microsecond counter
 * (ECLK 12 MHz with prescaler 11) and timer F as the kernel tick
 * (interval mode with INT0 enabled).  This model implements counting
 * and interval interrupts for the 32-bit timers and treats the 16-bit
 * timers as dormant register storage.
 *
 * 32-bit timer interrupts are delivered on the "timer32" IRQ output
 * (VIC0 line 7 on the S5L8702); 16-bit timers would use the "timer"
 * output (line 8).
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

#define TYPE_S5L8702_TIMER "s5l8702-timer"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702TimerState, S5L8702_TIMER)

#define S5L8702_ECLK_HZ  12000000

/* Register offsets within one 32-bit timer bank */
#define TxCON   0x00
#define TxCMD   0x04
#define TxDATA0 0x08
#define TxDATA1 0x0c
#define TxPRE   0x10
#define TxCNT   0x14

#define REG_TSTAT 0x118

/* TxCMD bits */
#define TCMD_EN   (1 << 0)
#define TCMD_CLR  (1 << 1)

/* Timers A-D are 16 bit wide, E-H are 32 bit wide */
#define N_TIMERS 8
#define N_T16    4

/* TSTAT bit positions for INT0 of timers E,F,G,H */
static const int t32_stat_shift[N_TIMERS - N_T16] = { 24, 16, 8, 0 };
/* MMIO base offsets of timers A,B,C,D,E,F,G,H */
static const hwaddr timer_base[N_TIMERS] = {
    0x00, 0x20, 0x40, 0x60, 0xa0, 0xc0, 0xe0, 0x100
};

typedef struct S5L8702Timer {
    uint32_t con;
    uint32_t data0;
    uint32_t data1;
    uint32_t pre;
    /*
     * 16-bit timers: interrupt status flags (INT0/INT1/OVF), exposed
     * in CON bits [18:16] and cleared by writing them back as ones.
     * 32-bit timers keep their flags in the shared TSTAT register.
     */
    uint32_t status;
    bool enabled;
    /* QEMU clock time (ns) corresponding to counter value 0 */
    int64_t epoch_ns;
    QEMUTimer qtimer;
    void *parent;
    int index;
} S5L8702Timer;

typedef S5L8702Timer S5L8702Timer32;

struct S5L8702TimerState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq32;     /* 32-bit timers: VIC0 line 7 */
    qemu_irq irq16;     /* 16-bit timers: VIC0 line 8 */

    S5L8702Timer timers[N_TIMERS];
    uint32_t tstat;
};

static inline bool timer_is_16bit(const S5L8702Timer *t)
{
    return t->index < N_T16;
}

/* Effective counting frequency of a timer in Hz */
static uint64_t t32_freq(S5L8702Timer *t)
{
    /*
     * TxCON[10:8] select clock divider, TxCON[6] selects ECLK.
     * Dividers: 0 -> /2, 1 -> /4, 2 -> /16, 3 -> /64, 4+ -> /1.
     * (Rockbox: /1 for the usec timer E, /16 for the tick timer B.)
     */
    static const int divs[8] = { 2, 4, 16, 64, 1, 1, 1, 1 };
    int cs = (t->con >> 8) & 7;
    uint64_t base = S5L8702_ECLK_HZ; /* PCLK modelled at ECLK rate too */

    return base / divs[cs] / (t->pre + 1);
}

static uint32_t t32_count(S5L8702Timer *t)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ticks;
    uint64_t period = (uint64_t)t->data0 + 1;

    if (!t->enabled) {
        return 0;
    }
    ticks = muldiv64(now - t->epoch_ns, t32_freq(t), NANOSECONDS_PER_SECOND);
    if (t->data0 == 0) {
        return 0;
    }
    return ticks % period;
}

static void s5l8702_timer_update_irq(S5L8702TimerState *s)
{
    uint32_t active32 = 0;
    uint32_t active16 = 0;
    int i;

    for (i = 0; i < N_TIMERS; i++) {
        /* TxCON[14:12] = interrupt enables (INT0/INT1/OVF) */
        uint32_t en = (s->timers[i].con >> 12) & 7;

        if (timer_is_16bit(&s->timers[i])) {
            active16 |= en & s->timers[i].status;
        } else {
            uint32_t flags = (s->tstat >> t32_stat_shift[i - N_T16]) & 7;
            active32 |= en & flags;
        }
    }
    qemu_set_irq(s->irq32, active32 != 0);
    qemu_set_irq(s->irq16, active16 != 0);
}

static void t32_rearm(S5L8702Timer *t)
{
    S5L8702TimerState *s = t->parent;
    uint64_t period_ticks = (uint64_t)t->data0 + 1;
    int64_t now;

    timer_del(&t->qtimer);
    if (!t->enabled) {
        return;
    }
    /* Only schedule callbacks when INT0 is enabled (interval mode) */
    if (!((t->con >> 12) & 1)) {
        return;
    }
    if (t->data0 == 0 || t->data0 == UINT32_MAX) {
        /* free-running counter, no practical interval interrupt */
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t period_ns = muldiv64(period_ticks, NANOSECONDS_PER_SECOND,
                                  t32_freq(t));
    uint64_t elapsed = now - t->epoch_ns;
    uint64_t next = period_ns - (elapsed % period_ns);

    if (period_ns == 0) {
        return;
    }
    timer_mod(&t->qtimer, now + next);
    (void)s;
}

static void t32_tick(void *opaque)
{
    S5L8702Timer *t = opaque;
    S5L8702TimerState *s = t->parent;

    /* INT0: interval interrupt */
    if (timer_is_16bit(t)) {
        t->status |= 1;
    } else {
        s->tstat |= 1u << t32_stat_shift[t->index - N_T16];
    }
    s5l8702_timer_update_irq(s);
    t32_rearm(t);
}

static uint64_t s5l8702_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702TimerState *s = opaque;
    int i;

    if (offset == REG_TSTAT) {
        return s->tstat;
    }
    for (i = 0; i < N_TIMERS; i++) {
        if (offset >= timer_base[i] && offset < timer_base[i] + 0x18) {
            S5L8702Timer *t = &s->timers[i];

            switch (offset - timer_base[i]) {
            case TxCON:
                if (timer_is_16bit(t)) {
                    return t->con | (t->status << 16);
                }
                return t->con;
            case TxCMD:
                return t->enabled ? TCMD_EN : 0;
            case TxDATA0:
                return t->data0;
            case TxDATA1:
                return t->data1;
            case TxPRE:
                return t->pre;
            case TxCNT:
                return t32_count(t);
            }
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "s5l8702-timer: bad read offset 0x%" HWADDR_PRIx "\n",
                  offset);
    return 0;
}

static void s5l8702_timer_write(void *opaque, hwaddr offset,
                                uint64_t val, unsigned size)
{
    S5L8702TimerState *s = opaque;
    int i;

    if (offset == REG_TSTAT) {
        /* write-1-to-clear */
        s->tstat &= ~val;
        s5l8702_timer_update_irq(s);
        return;
    }
    for (i = 0; i < N_TIMERS; i++) {
        if (offset >= timer_base[i] && offset < timer_base[i] + 0x18) {
            S5L8702Timer *t = &s->timers[i];

            switch (offset - timer_base[i]) {
            case TxCON:
                if (timer_is_16bit(t)) {
                    /*
                     * Bits [18:16] are the interrupt flags; writing
                     * them back as ones clears them ("TBCON = TBCON").
                     */
                    t->status &= ~((val >> 16) & 7);
                    t->con = val & 0xffff;
                } else {
                    t->con = val;
                }
                s5l8702_timer_update_irq(s);
                t32_rearm(t);
                return;
            case TxCMD:
                if (val & TCMD_CLR) {
                    t->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                }
                t->enabled = (val & TCMD_EN) != 0;
                t32_rearm(t);
                return;
            case TxDATA0:
                t->data0 = val;
                t32_rearm(t);
                return;
            case TxDATA1:
                t->data1 = val;
                return;
            case TxPRE:
                t->pre = val;
                t32_rearm(t);
                return;
            }
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "s5l8702-timer: bad write offset 0x%" HWADDR_PRIx "\n",
                  offset);
}

static const MemoryRegionOps s5l8702_timer_ops = {
    .read = s5l8702_timer_read,
    .write = s5l8702_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8702_timer_reset_hold(Object *obj, ResetType type)
{
    S5L8702TimerState *s = S5L8702_TIMER(obj);
    int i;

    for (i = 0; i < N_TIMERS; i++) {
        S5L8702Timer *t = &s->timers[i];

        timer_del(&t->qtimer);
        t->con = 0;
        t->data0 = 0;
        t->data1 = 0;
        t->pre = 0;
        t->status = 0;
        t->enabled = false;
        t->epoch_ns = 0;
    }
    s->tstat = 0;
    qemu_set_irq(s->irq32, 0);
    qemu_set_irq(s->irq16, 0);
}

static void s5l8702_timer_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    S5L8702TimerState *s = S5L8702_TIMER(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &s5l8702_timer_ops, s,
                          "s5l8702-timer", 0x200);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq32);
    sysbus_init_irq(sbd, &s->irq16);

    for (i = 0; i < N_TIMERS; i++) {
        s->timers[i].parent = s;
        s->timers[i].index = i;
        timer_init_ns(&s->timers[i].qtimer, QEMU_CLOCK_VIRTUAL,
                      t32_tick, &s->timers[i]);
    }
}

static const VMStateDescription vmstate_s5l8702_timer_one = {
    .name = "s5l8702-timer/timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(con, S5L8702Timer),
        VMSTATE_UINT32(data0, S5L8702Timer),
        VMSTATE_UINT32(data1, S5L8702Timer),
        VMSTATE_UINT32(pre, S5L8702Timer),
        VMSTATE_UINT32(status, S5L8702Timer),
        VMSTATE_BOOL(enabled, S5L8702Timer),
        VMSTATE_INT64(epoch_ns, S5L8702Timer),
        VMSTATE_TIMER(qtimer, S5L8702Timer),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_s5l8702_timer = {
    .name = "s5l8702-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(timers, S5L8702TimerState, N_TIMERS, 1,
                             vmstate_s5l8702_timer_one, S5L8702Timer),
        VMSTATE_UINT32(tstat, S5L8702TimerState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8702_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = s5l8702_timer_reset_hold;
    dc->vmsd = &vmstate_s5l8702_timer;
}

static const TypeInfo s5l8702_timer_info = {
    .name = TYPE_S5L8702_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702TimerState),
    .instance_init = s5l8702_timer_init,
    .class_init = s5l8702_timer_class_init,
};

static void s5l8702_timer_register_types(void)
{
    type_register_static(&s5l8702_timer_info);
}

type_init(s5l8702_timer_register_types)
