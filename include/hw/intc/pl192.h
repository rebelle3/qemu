/*
 * ARM PL192 Vectored Interrupt Controller
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_PL192_H
#define HW_INTC_PL192_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PL192 "pl192"
OBJECT_DECLARE_SIMPLE_TYPE(PL192State, PL192)

#define PL192_NUM_IRQS    32
/* 32 vectors + one slot for "no active interrupt" */
#define PL192_STACK_SIZE  (PL192_NUM_IRQS + 1)

struct PL192State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t level;             /* raw input line state */
    uint32_t soft_int;
    uint32_t irq_enable;
    uint32_t fiq_select;
    uint32_t protection;
    uint32_t sw_priority_mask;
    uint32_t daisy_priority;
    uint32_t vect_addr[PL192_NUM_IRQS];
    uint32_t vect_priority[PL192_NUM_IRQS];
    uint32_t address;           /* last value returned by VICADDRESS */

    /* stack of priorities currently being serviced */
    uint8_t prio_stack[PL192_STACK_SIZE];
    int stack_idx;

    qemu_irq irq;
    qemu_irq fiq;
};

#endif
