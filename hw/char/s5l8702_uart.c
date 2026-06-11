/*
 * Samsung S5L8702 UART block (4 ports, S3C-style registers).
 *
 * Minimal model with correct idle status semantics: the receiver never
 * has data, the transmitter is always empty, and transmitted bytes are
 * discarded.  This matters because Rockbox's iPod Accessory Protocol
 * polls the serial port for remote-control commands: a RAM-backed stub
 * whose status registers echo write-one-to-clear flags back makes it
 * decode an endless stream of phantom button presses.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_S5L8702_UART "s5l8702-uart"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702UartState, S5L8702_UART)

#define UART_N_PORTS    4
#define UART_PORT_SIZE  0x4000
#define UART_PORT_REGS  (0x40 / 4)

#define REG_UTRSTAT  0x10
#define REG_UERSTAT  0x14
#define REG_UFSTAT   0x18
#define REG_UTXH     0x20
#define REG_URXH     0x24
#define REG_UABRSTAT 0x30

/* UTRSTAT: TX buffer empty | TX shifter empty; RX ready never set */
#define UTRSTAT_IDLE 0x6

struct S5L8702UartState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    CharFrontend chr;       /* TX of all ports tee'd here (debug console) */
    uint32_t regs[UART_N_PORTS][UART_PORT_REGS];
};

static uint64_t s5l8702_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702UartState *s = opaque;
    int port = offset / UART_PORT_SIZE;
    hwaddr reg = offset % UART_PORT_SIZE;

    if (port >= UART_N_PORTS || reg >= 0x40) {
        return 0;
    }
    switch (reg) {
    case REG_UTRSTAT:
        return UTRSTAT_IDLE;
    case REG_UERSTAT:
    case REG_UFSTAT:
    case REG_URXH:
    case REG_UABRSTAT:
        return 0;
    default:
        return s->regs[port][reg >> 2];
    }
}

static void s5l8702_uart_write(void *opaque, hwaddr offset,
                               uint64_t val, unsigned size)
{
    S5L8702UartState *s = opaque;
    int port = offset / UART_PORT_SIZE;
    hwaddr reg = offset % UART_PORT_SIZE;

    if (port >= UART_N_PORTS || reg >= 0x40) {
        return;
    }
    switch (reg) {
    case REG_UTXH: {
        /* Tee transmitted bytes to the host chardev (osos/Rockbox debug
         * console).  RX stays idle so the IAP remote stays silent. */
        uint8_t ch = val;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        return;
    }
    case REG_UTRSTAT:
        /* status flags are discarded */
        return;
    default:
        s->regs[port][reg >> 2] = val;
    }
}

static const MemoryRegionOps s5l8702_uart_ops = {
    .read = s5l8702_uart_read,
    .write = s5l8702_uart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8702_uart_reset_hold(Object *obj, ResetType type)
{
    S5L8702UartState *s = S5L8702_UART(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void s5l8702_uart_init(Object *obj)
{
    S5L8702UartState *s = S5L8702_UART(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_uart_ops, s,
                          "s5l8702-uart", UART_N_PORTS * UART_PORT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_s5l8702_uart = {
    .name = "s5l8702-uart",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_2DARRAY(regs, S5L8702UartState,
                               UART_N_PORTS, UART_PORT_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l8702_uart_props[] = {
    DEFINE_PROP_CHR("chardev", S5L8702UartState, chr),
};

static void s5l8702_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = s5l8702_uart_reset_hold;
    dc->vmsd = &vmstate_s5l8702_uart;
    device_class_set_props(dc, s5l8702_uart_props);
}

static const TypeInfo s5l8702_uart_info = {
    .name = TYPE_S5L8702_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702UartState),
    .instance_init = s5l8702_uart_init,
    .class_init = s5l8702_uart_class_init,
};

static void s5l8702_uart_register_types(void)
{
    type_register_static(&s5l8702_uart_info);
}

type_init(s5l8702_uart_register_types)
