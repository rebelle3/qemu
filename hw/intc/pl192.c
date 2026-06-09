/*
 * ARM PL192 Vectored Interrupt Controller
 *
 * Unlike the PL190, the PL192 has 32 individually programmable vector
 * slots (one per interrupt source) with 16 software-programmable
 * priority levels, and the vector address register lives at offset
 * 0xF00.  This model implements the register interface and the
 * priority-stack semantics of VICADDRESS reads/writes (read = enter
 * service of the highest-priority pending interrupt, write = end of
 * interrupt).
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/intc/pl192.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define PL192_PRIO_LEVELS    16
/* Priority value meaning "nothing being serviced" */
#define PL192_PRIO_NONE      PL192_PRIO_LEVELS

static const uint8_t pl192_id[] = {
    /* Peripheral ID */
    0x92, 0x11, 0x04, 0x00,
    /* PrimeCell ID */
    0x0d, 0xf0, 0x05, 0xb1
};

/* Interrupts pending after enable/select masking (IRQ only) */
static inline uint32_t pl192_irq_pending(PL192State *s)
{
    return (s->level | s->soft_int) & s->irq_enable & ~s->fiq_select;
}

static inline uint32_t pl192_fiq_pending(PL192State *s)
{
    return (s->level | s->soft_int) & s->irq_enable & s->fiq_select;
}

/* Priority currently being serviced (lower value = higher priority) */
static inline int pl192_active_priority(PL192State *s)
{
    return s->stack_idx > 0 ? s->prio_stack[s->stack_idx - 1]
                            : PL192_PRIO_NONE;
}

/*
 * Find the pending interrupt with the highest priority that is
 * allowed by the software priority mask and is strictly higher
 * priority than whatever is currently being serviced.
 * Returns the interrupt number or -1.
 */
static int pl192_resolve(PL192State *s)
{
    uint32_t pending = pl192_irq_pending(s);
    int active = pl192_active_priority(s);
    int best = -1;
    int best_prio = PL192_PRIO_NONE;
    int i;

    for (i = 0; i < PL192_NUM_IRQS; i++) {
        int prio;

        if (!(pending & (1u << i))) {
            continue;
        }
        prio = s->vect_priority[i];
        if (!(s->sw_priority_mask & (1u << prio))) {
            continue;
        }
        if (prio < best_prio) {
            best_prio = prio;
            best = i;
        }
    }
    if (best >= 0 && best_prio < active) {
        return best;
    }
    return -1;
}

static void pl192_update(PL192State *s)
{
    qemu_set_irq(s->irq, pl192_resolve(s) >= 0);
    qemu_set_irq(s->fiq, pl192_fiq_pending(s) != 0);
}

static void pl192_set_irq(void *opaque, int irq, int level)
{
    PL192State *s = (PL192State *)opaque;

    if (level) {
        s->level |= 1u << irq;
    } else {
        s->level &= ~(1u << irq);
    }
    pl192_update(s);
}

static uint64_t pl192_read(void *opaque, hwaddr offset, unsigned size)
{
    PL192State *s = (PL192State *)opaque;

    if (offset >= 0xfe0 && offset < 0x1000) {
        return pl192_id[(offset - 0xfe0) >> 2];
    }
    if (offset >= 0x100 && offset < 0x100 + 4 * PL192_NUM_IRQS) {
        return s->vect_addr[(offset - 0x100) >> 2];
    }
    if (offset >= 0x200 && offset < 0x200 + 4 * PL192_NUM_IRQS) {
        return s->vect_priority[(offset - 0x200) >> 2];
    }
    switch (offset) {
    case 0x000: /* IRQSTATUS */
        return pl192_irq_pending(s);
    case 0x004: /* FIQSTATUS */
        return pl192_fiq_pending(s);
    case 0x008: /* RAWINTR */
        return s->level | s->soft_int;
    case 0x00c: /* INTSELECT */
        return s->fiq_select;
    case 0x010: /* INTENABLE */
        return s->irq_enable;
    case 0x018: /* SOFTINT */
        return s->soft_int;
    case 0x020: /* PROTECTION */
        return s->protection;
    case 0x024: /* SWPRIORITYMASK */
        return s->sw_priority_mask;
    case 0x028: /* VECTPRIORITYDAISY */
        return s->daisy_priority;
    case 0xf00: /* ADDRESS */
    {
        int irq = pl192_resolve(s);

        if (irq >= 0) {
            /* Enter service: push priority, mask equal/lower ones. */
            s->address = s->vect_addr[irq];
            if (s->stack_idx < PL192_STACK_SIZE) {
                s->prio_stack[s->stack_idx++] = s->vect_priority[irq];
            }
            pl192_update(s);
        }
        return s->address;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl192_read: Bad offset 0x%" HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void pl192_write(void *opaque, hwaddr offset,
                        uint64_t val, unsigned size)
{
    PL192State *s = (PL192State *)opaque;

    if (offset >= 0x100 && offset < 0x100 + 4 * PL192_NUM_IRQS) {
        s->vect_addr[(offset - 0x100) >> 2] = val;
        pl192_update(s);
        return;
    }
    if (offset >= 0x200 && offset < 0x200 + 4 * PL192_NUM_IRQS) {
        s->vect_priority[(offset - 0x200) >> 2] = val & 0xf;
        pl192_update(s);
        return;
    }
    switch (offset) {
    case 0x00c: /* INTSELECT */
        s->fiq_select = val;
        break;
    case 0x010: /* INTENABLE */
        s->irq_enable |= val;
        break;
    case 0x014: /* INTENCLEAR */
        s->irq_enable &= ~val;
        break;
    case 0x018: /* SOFTINT */
        s->soft_int |= val;
        break;
    case 0x01c: /* SOFTINTCLEAR */
        s->soft_int &= ~val;
        break;
    case 0x020: /* PROTECTION */
        s->protection = val & 1;
        break;
    case 0x024: /* SWPRIORITYMASK */
        s->sw_priority_mask = val & 0xffff;
        break;
    case 0x028: /* VECTPRIORITYDAISY */
        s->daisy_priority = val & 0xf;
        break;
    case 0xf00: /* ADDRESS: end of interrupt */
        if (s->stack_idx > 0) {
            s->stack_idx--;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl192_write: Bad offset 0x%" HWADDR_PRIx "\n", offset);
        return;
    }
    pl192_update(s);
}

static const MemoryRegionOps pl192_ops = {
    .read = pl192_read,
    .write = pl192_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void pl192_reset_hold(Object *obj, ResetType type)
{
    PL192State *s = PL192(obj);
    int i;

    s->soft_int = 0;
    s->irq_enable = 0;
    s->fiq_select = 0;
    s->protection = 0;
    s->sw_priority_mask = 0xffff;
    s->daisy_priority = 0xf;
    s->address = 0;
    s->stack_idx = 0;
    for (i = 0; i < PL192_NUM_IRQS; i++) {
        s->vect_addr[i] = 0;
        s->vect_priority[i] = 0xf;
    }
    pl192_update(s);
}

static void pl192_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PL192State *s = PL192(obj);

    memory_region_init_io(&s->iomem, obj, &pl192_ops, s, "pl192", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_in(DEVICE(obj), pl192_set_irq, PL192_NUM_IRQS);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->fiq);
}

static const VMStateDescription vmstate_pl192 = {
    .name = "pl192",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(level, PL192State),
        VMSTATE_UINT32(soft_int, PL192State),
        VMSTATE_UINT32(irq_enable, PL192State),
        VMSTATE_UINT32(fiq_select, PL192State),
        VMSTATE_UINT32(protection, PL192State),
        VMSTATE_UINT32(sw_priority_mask, PL192State),
        VMSTATE_UINT32(daisy_priority, PL192State),
        VMSTATE_UINT32_ARRAY(vect_addr, PL192State, PL192_NUM_IRQS),
        VMSTATE_UINT32_ARRAY(vect_priority, PL192State, PL192_NUM_IRQS),
        VMSTATE_UINT32(address, PL192State),
        VMSTATE_UINT8_ARRAY(prio_stack, PL192State, PL192_STACK_SIZE),
        VMSTATE_INT32(stack_idx, PL192State),
        VMSTATE_END_OF_LIST()
    }
};

static void pl192_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = pl192_reset_hold;
    dc->vmsd = &vmstate_pl192;
}

static const TypeInfo pl192_info = {
    .name = TYPE_PL192,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PL192State),
    .instance_init = pl192_init,
    .class_init = pl192_class_init,
};

static void pl192_register_types(void)
{
    type_register_static(&pl192_info);
}

type_init(pl192_register_types)
