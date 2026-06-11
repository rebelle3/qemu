/*
 * Samsung S5L8702 click wheel controller (iPod Classic 6G).
 *
 * The wheel controller delivers 32-bit packets in WHEELRX and raises
 * an interrupt; the driver acknowledges by writing the interrupt bits
 * back to WHEELINT and reads the packet.
 *
 * Packet formats consumed by Rockbox (button-clickwheel.c):
 *  - init ack:  (pkt & 0x8000FFFF) == 0x8000023A, buttons in bits 16-20.
 *    Sent in response to the driver writing 0x8000023A to WHEELTX;
 *    releases Rockbox's button_init_wakeup semaphore.
 *  - status:    (pkt & 0x800000FF) == 0x8000001A, buttons in bits 8-12
 *    (SELECT/RIGHT/LEFT/PLAY/MENU), bit 30 = wheel touched,
 *    bits 16-22 = wheel position (0..95, clockwise).
 *
 * Host input mapping:
 *    Up        -> MENU            Down      -> PLAY
 *    Left      -> LEFT            Right     -> RIGHT
 *    Enter/KP enter -> SELECT
 *    PgUp / ]  -> scroll forward (clockwise)
 *    PgDn / [  -> scroll backward
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
#include "ui/input.h"

#define TYPE_S5L8702_WHEEL "s5l8702-wheel"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702WheelState, S5L8702_WHEEL)

#define REG_WHEEL00  0x00
#define REG_WHEEL04  0x04
#define REG_WHEEL08  0x08
#define REG_WHEEL0C  0x0c
#define REG_WHEEL10  0x10
#define REG_WHEELINT 0x14
#define REG_WHEELRX  0x18
#define REG_WHEELTX  0x1c

#define WHEEL_INT_RX 0x2

/* status packet button bits */
#define PKT_BTN_SELECT  0x00000100
#define PKT_BTN_RIGHT   0x00000200
#define PKT_BTN_LEFT    0x00000400
#define PKT_BTN_PLAY    0x00000800
#define PKT_BTN_MENU    0x00001000
#define PKT_TOUCHED     0x40000000

/* init ack packet button bits live at bits 16..20 in the same order */

#define WHEEL_POSITIONS    96
#define SCROLL_STEP        4    /* wheel clicks per key press */
/*
 * Clicks are streamed one per interval, not in an instant burst:
 * Rockbox's wheel driver derives scroll speed (and acceleration) from
 * the time between position packets, so a realistic ~20 ms spacing
 * keeps velocity low and lands exactly one list item per key press
 * instead of overshooting.
 */
#define SCROLL_INTERVAL_NS 20000000

#define FIFO_SIZE 16

struct S5L8702WheelState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t reg00;
    uint32_t reg04;
    uint32_t reg08;
    uint32_t reg10;
    uint32_t wheelint;
    uint32_t rx;
    uint32_t tx;

    uint32_t fifo[FIFO_SIZE];
    uint32_t fifo_head, fifo_len;

    uint32_t buttons;       /* current PKT_BTN_* state */
    uint32_t wheel_pos;     /* 0..95 */
    bool touched;

    QEMUTimer *scroll_timer;
    int scroll_pending;     /* signed wheel clicks left to stream */
};

static void s5l8702_wheel_update_irq(S5L8702WheelState *s)
{
    /* IRQ asserted while any int bit set and interrupts enabled */
    qemu_set_irq(s->irq, (s->reg04 & 1) && s->wheelint);
}

static void s5l8702_wheel_push(S5L8702WheelState *s, uint32_t pkt)
{
    if (!(s->reg00 & 0x380000)) {
        /* controller not started */
        return;
    }
    if (getenv("IPOD6G_DEBUG")) {
        fprintf(stderr, "PKTDBG push %08x\n", pkt);
    }
    if (s->wheelint == 0 && s->fifo_len == 0) {
        s->rx = pkt;
        s->wheelint |= WHEEL_INT_RX;
        s5l8702_wheel_update_irq(s);
        return;
    }
    if (s->fifo_len < FIFO_SIZE) {
        s->fifo[(s->fifo_head + s->fifo_len) % FIFO_SIZE] = pkt;
        s->fifo_len++;
    } else {
        /*
         * Overflow (the guest stalled mid-gesture): keep the newest
         * state. Positions are absolute, so losing an intermediate
         * move costs nothing, while losing the final position or the
         * untouch packet loses the whole gesture.
         */
        s->fifo[(s->fifo_head + FIFO_SIZE - 1) % FIFO_SIZE] = pkt;
    }
}

static void s5l8702_wheel_pop(S5L8702WheelState *s)
{
    if (s->fifo_len > 0) {
        s->rx = s->fifo[s->fifo_head];
        s->fifo_head = (s->fifo_head + 1) % FIFO_SIZE;
        s->fifo_len--;
        s->wheelint |= WHEEL_INT_RX;
    }
    s5l8702_wheel_update_irq(s);
}

static uint32_t s5l8702_wheel_status_pkt(S5L8702WheelState *s)
{
    uint32_t pkt = 0x8000001a | s->buttons;

    if (s->touched) {
        pkt |= PKT_TOUCHED | ((s->wheel_pos & 0x7f) << 16);
    }
    return pkt;
}

static void s5l8702_wheel_key(DeviceState *dev, QemuConsole *src,
                              InputEvent *evt)
{
    S5L8702WheelState *s = S5L8702_WHEEL(dev);
    InputKeyEvent *key = evt->u.key.data;
    int qcode = qemu_input_key_value_to_qcode(key->key);
    bool down = key->down;
    uint32_t btn = 0;
    int scroll = 0;

    switch (qcode) {
    case Q_KEY_CODE_UP:
        btn = PKT_BTN_MENU;
        break;
    case Q_KEY_CODE_DOWN:
        btn = PKT_BTN_PLAY;
        break;
    case Q_KEY_CODE_LEFT:
        btn = PKT_BTN_LEFT;
        break;
    case Q_KEY_CODE_RIGHT:
        btn = PKT_BTN_RIGHT;
        break;
    case Q_KEY_CODE_RET:
    case Q_KEY_CODE_KP_ENTER:
        btn = PKT_BTN_SELECT;
        break;
    case Q_KEY_CODE_PGUP:
    case Q_KEY_CODE_BRACKET_RIGHT:
        scroll = SCROLL_STEP;
        break;
    case Q_KEY_CODE_PGDN:
    case Q_KEY_CODE_BRACKET_LEFT:
        scroll = -SCROLL_STEP;
        break;
    default:
        return;
    }

    if (btn) {
        if (down) {
            s->buttons |= btn;
        } else {
            s->buttons &= ~btn;
        }
        s5l8702_wheel_push(s, s5l8702_wheel_status_pkt(s));
    } else if (scroll && down) {
        /*
         * Stream the clicks through the pacing timer instead of an
         * instant burst.  Plant the finger now (anchor packet); the
         * timer walks the position one click at a time and lifts the
         * finger when the queue drains.
         */
        if (s->scroll_pending == 0 && !s->touched) {
            s->touched = true;
            s5l8702_wheel_push(s, s5l8702_wheel_status_pkt(s));
        }
        s->scroll_pending += scroll;
        timer_mod(s->scroll_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCROLL_INTERVAL_NS);
    }
}

static void s5l8702_wheel_scroll_tick(void *opaque)
{
    S5L8702WheelState *s = opaque;

    if (s->scroll_pending != 0) {
        int dir = s->scroll_pending > 0 ? 1 : -1;

        s->wheel_pos = (s->wheel_pos + WHEEL_POSITIONS + dir) % WHEEL_POSITIONS;
        s5l8702_wheel_push(s, s5l8702_wheel_status_pkt(s));
        s->scroll_pending -= dir;
        timer_mod(s->scroll_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCROLL_INTERVAL_NS);
    } else if (s->touched) {
        /* queue drained: lift the finger */
        s->touched = false;
        s5l8702_wheel_push(s, s5l8702_wheel_status_pkt(s));
    }
}

static void s5l8702_wheel_touch_gpio(void *opaque, int line, int level)
{
    S5L8702WheelState *s = opaque;
    bool touched = level != 0;

    if (s->touched != touched) {
        s->touched = touched;
        s5l8702_wheel_push(s, s5l8702_wheel_status_pkt(s));
    }
}

static void s5l8702_wheel_pos_gpio(void *opaque, int line, int level)
{
    S5L8702WheelState *s = opaque;
    uint32_t pos = (uint32_t)level % WHEEL_POSITIONS;

    if (s->wheel_pos != pos) {
        s->wheel_pos = pos;
        if (s->touched) {
            s5l8702_wheel_push(s, s5l8702_wheel_status_pkt(s));
        }
    }
}

static const QemuInputHandler s5l8702_wheel_handler = {
    .name = "iPod click wheel",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = s5l8702_wheel_key,
};

static uint64_t s5l8702_wheel_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702WheelState *s = opaque;

    switch (offset) {
    case REG_WHEEL00:
        return s->reg00;
    case REG_WHEEL04:
        return s->reg04;
    case REG_WHEEL08:
        return s->reg08;
    case REG_WHEEL10:
        return s->reg10;
    case REG_WHEELINT:
        return s->wheelint;
    case REG_WHEELRX:
        return s->rx;
    case REG_WHEELTX:
        return s->tx;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8702-wheel: bad read offset 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void s5l8702_wheel_write(void *opaque, hwaddr offset,
                                uint64_t val, unsigned size)
{
    S5L8702WheelState *s = opaque;

    switch (offset) {
    case REG_WHEEL00:
        s->reg00 = val;
        break;
    case REG_WHEEL04:
        s->reg04 = val;
        s5l8702_wheel_update_irq(s);
        break;
    case REG_WHEEL08:
        s->reg08 = val;
        break;
    case REG_WHEEL10:
        s->reg10 = val;
        break;
    case REG_WHEELINT:
        /* write-1-to-clear; deliver next queued packet if any */
        s->wheelint &= ~val;
        if (s->wheelint == 0) {
            s5l8702_wheel_pop(s);
        } else {
            s5l8702_wheel_update_irq(s);
        }
        break;
    case REG_WHEELTX:
        s->tx = val;
        if (val == 0x8000023a) {
            /* init / keep-alive request: respond with init ack */
            uint32_t ack = 0x8000023a;

            /* mirror current button state into bits 16..20 */
            ack |= (s->buttons & 0x1f00) << 8;
            s5l8702_wheel_push(s, ack);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8702-wheel: bad write offset 0x%" HWADDR_PRIx "\n",
                      offset);
    }
}

static const MemoryRegionOps s5l8702_wheel_ops = {
    .read = s5l8702_wheel_read,
    .write = s5l8702_wheel_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8702_wheel_reset_hold(Object *obj, ResetType type)
{
    S5L8702WheelState *s = S5L8702_WHEEL(obj);

    s->reg00 = 0;
    s->reg04 = 0;
    s->reg08 = 0;
    s->reg10 = 0;
    s->wheelint = 0;
    s->rx = 0;
    s->tx = 0;
    s->fifo_head = 0;
    s->fifo_len = 0;
    s->buttons = 0;
    s->wheel_pos = 0;
    s->touched = false;
    s->scroll_pending = 0;
    if (s->scroll_timer) {
        timer_del(s->scroll_timer);
    }
}

static void s5l8702_wheel_realize(DeviceState *dev, Error **errp)
{
    qemu_input_handler_register(dev, &s5l8702_wheel_handler);
}

static void s5l8702_wheel_init(Object *obj)
{
    S5L8702WheelState *s = S5L8702_WHEEL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_wheel_ops, s,
                          "s5l8702-wheel", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), s5l8702_wheel_touch_gpio,
                            "wheel-touch", 1);
    qdev_init_gpio_in_named(DEVICE(obj), s5l8702_wheel_pos_gpio,
                            "wheel-pos", 1);
    s->scroll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   s5l8702_wheel_scroll_tick, s);
}

static const VMStateDescription vmstate_s5l8702_wheel = {
    .name = "s5l8702-wheel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(reg00, S5L8702WheelState),
        VMSTATE_UINT32(reg04, S5L8702WheelState),
        VMSTATE_UINT32(reg08, S5L8702WheelState),
        VMSTATE_UINT32(reg10, S5L8702WheelState),
        VMSTATE_UINT32(wheelint, S5L8702WheelState),
        VMSTATE_UINT32(rx, S5L8702WheelState),
        VMSTATE_UINT32(tx, S5L8702WheelState),
        VMSTATE_UINT32_ARRAY(fifo, S5L8702WheelState, FIFO_SIZE),
        VMSTATE_UINT32(fifo_head, S5L8702WheelState),
        VMSTATE_UINT32(fifo_len, S5L8702WheelState),
        VMSTATE_UINT32(buttons, S5L8702WheelState),
        VMSTATE_UINT32(wheel_pos, S5L8702WheelState),
        VMSTATE_BOOL(touched, S5L8702WheelState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8702_wheel_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = s5l8702_wheel_realize;
    rc->phases.hold = s5l8702_wheel_reset_hold;
    dc->vmsd = &vmstate_s5l8702_wheel;
}

static const TypeInfo s5l8702_wheel_info = {
    .name = TYPE_S5L8702_WHEEL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702WheelState),
    .instance_init = s5l8702_wheel_init,
    .class_init = s5l8702_wheel_class_init,
};

static void s5l8702_wheel_register_types(void)
{
    type_register_static(&s5l8702_wheel_info);
}

type_init(s5l8702_wheel_register_types)
