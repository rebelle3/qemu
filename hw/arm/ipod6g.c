/*
 * Apple iPod Classic 6G (Samsung S5L8702 SoC) machine model.
 *
 * The S5L8702 is an ARM926EJ-S SoC with 64 MB of SDRAM at 0x08000000
 * and 256 KB of IRAM at 0x22000000 (remappable to address 0).  This
 * board is sufficient to run the Rockbox bootloader and firmware:
 *
 *   qemu-system-arm -M ipod6g -kernel bootloader.bin \
 *       -drive file=disk.img,format=raw,if=ide
 *
 * The -kernel image may be either the raw Rockbox bootloader binary
 * (loaded at the IRAM base, exactly where the NOR boot ROM would put
 * it) or a scrambled rockbox.ipod (detected by its "ip6g" model magic
 * and loaded directly at the DRAM base).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/blockdev.h"
#include "system/reset.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "system/block-backend.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/or-irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "target/arm/cpu.h"

/* Memory map */
#define IPOD6G_DRAM_BASE     0x08000000
#define IPOD6G_IRAM_BASE     0x22000000
#define IPOD6G_IRAM_SIZE     (256 * KiB)

#define IPOD6G_SHA_BASE      0x38000000
#define IPOD6G_MIU_BASE      0x38100000
#define IPOD6G_DMAC0_BASE    0x38200000
#define IPOD6G_LCD_BASE      0x38300000
#define IPOD6G_USBHC_BASE    0x38600000
#define IPOD6G_ATA_BASE      0x38700000
#define IPOD6G_USBOTG_BASE   0x38800000
#define IPOD6G_SDCI_BASE     0x38b00000
#define IPOD6G_AES_BASE      0x38c00000
#define IPOD6G_VIC0_BASE     0x38e00000
#define IPOD6G_VIC1_BASE     0x38e01000
#define IPOD6G_VICEDGE_BASE  0x38e02000
#define IPOD6G_DMAC1_BASE    0x39900000
#define IPOD6G_EIC_BASE      0x39a00000
#define IPOD6G_WHEEL_BASE    0x3c200000
#define IPOD6G_SPI0_BASE     0x3c300000
#define IPOD6G_USBPHY_BASE   0x3c400000
#define IPOD6G_CLK_BASE      0x3c500000
#define IPOD6G_I2C0_BASE     0x3c600000
#define IPOD6G_TIMER_BASE    0x3c700000
#define IPOD6G_WDT_BASE      0x3c800000
#define IPOD6G_I2C1_BASE     0x3c900000
#define IPOD6G_I2S0_BASE     0x3ca00000
#define IPOD6G_UART_BASE     0x3cc00000
#define IPOD6G_ADC_BASE      0x3ce00000
#define IPOD6G_GPIO_BASE     0x3cf00000
#define IPOD6G_SPI2_BASE     0x3d200000

/* VIC0 interrupt lines */
#define IRQ_TIMER32  7
#define IRQ_TIMER    8
#define IRQ_LCD      14
#define IRQ_DMAC0    16
#define IRQ_DMAC1    17
#define IRQ_I2C0     21
#define IRQ_I2C1     22
#define IRQ_WHEEL    23
#define IRQ_ATA      29

#define TYPE_IPOD6G_MACHINE MACHINE_TYPE_NAME("ipod6g")
OBJECT_DECLARE_SIMPLE_TYPE(Ipod6gMachineState, IPOD6G_MACHINE)

struct Ipod6gMachineState {
    MachineState parent_obj;

    MemoryRegion iram;
    MemoryRegion iram_alias;
    ARMCPU *cpu;
    uint32_t entry;
};

static void ipod6g_cpu_reset(void *opaque)
{
    Ipod6gMachineState *s = opaque;

    cpu_reset(CPU(s->cpu));
    cpu_set_pc(CPU(s->cpu), s->entry);
}

static DeviceState *ipod6g_stub(const char *name, hwaddr base, uint32_t size)
{
    DeviceState *dev = qdev_new("s5l8702-ramregs");

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint32(dev, "size", size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return dev;
}

static DeviceState *ipod6g_dmac(hwaddr base, qemu_irq irq)
{
    DeviceState *dev = qdev_new("pl080");

    object_property_set_link(OBJECT(dev), "downstream",
                             OBJECT(get_system_memory()), &error_fatal);
    /* emulate peripheral FIFO drain so chain appends win the race */
    qdev_prop_set_uint32(dev, "tc-delay-ns", 2000000);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    return dev;
}

static void ipod6g_load_kernel(Ipod6gMachineState *s, const char *filename)
{
    gsize len;
    gchar *data;
    GError *gerr = NULL;

    if (!g_file_get_contents(filename, &data, &len, &gerr)) {
        error_report("ipod6g: cannot load '%s': %s", filename, gerr->message);
        exit(1);
    }
    if (len > 8 && memcmp(data + 4, "ip6g", 4) == 0) {
        /* scrambled rockbox.ipod: strip header, load at DRAM base */
        rom_add_blob_fixed("rockbox", data + 8, len - 8, IPOD6G_DRAM_BASE);
        s->entry = IPOD6G_DRAM_BASE;
    } else {
        /* raw bootloader image: load at IRAM base */
        rom_add_blob_fixed("bootloader", data, len, IPOD6G_IRAM_BASE);
        s->entry = IPOD6G_IRAM_BASE;
    }
    g_free(data);
}

static void ipod6g_init(MachineState *machine)
{
    Ipod6gMachineState *s = IPOD6G_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *vic[2], *cpu_irq_or, *cpu_fiq_or, *dev;
    Object *cpuobj;
    I2CBus *i2c0;
    int i;

    /* CPU */
    cpuobj = object_new(machine->cpu_type);
    object_property_set_bool(cpuobj, "realized", true, &error_fatal);
    s->cpu = ARM_CPU(cpuobj);

    /* Memory: DRAM, IRAM, and the boot alias of IRAM at 0x0 */
    memory_region_add_subregion(sysmem, IPOD6G_DRAM_BASE, machine->ram);
    memory_region_init_ram(&s->iram, NULL, "ipod6g.iram", IPOD6G_IRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, IPOD6G_IRAM_BASE, &s->iram);
    memory_region_init_alias(&s->iram_alias, NULL, "ipod6g.iram.alias",
                             &s->iram, 0, IPOD6G_IRAM_SIZE);
    memory_region_add_subregion(sysmem, 0, &s->iram_alias);

    /* Interrupt controllers: two PL192s ORed into the CPU IRQ/FIQ */
    cpu_irq_or = qdev_new(TYPE_OR_IRQ);
    qdev_prop_set_uint16(cpu_irq_or, "num-lines", 2);
    qdev_realize_and_unref(cpu_irq_or, NULL, &error_fatal);
    qdev_connect_gpio_out(cpu_irq_or, 0,
                          qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));

    cpu_fiq_or = qdev_new(TYPE_OR_IRQ);
    qdev_prop_set_uint16(cpu_fiq_or, "num-lines", 2);
    qdev_realize_and_unref(cpu_fiq_or, NULL, &error_fatal);
    qdev_connect_gpio_out(cpu_fiq_or, 0,
                          qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));

    for (i = 0; i < 2; i++) {
        SysBusDevice *sbd;

        vic[i] = qdev_new("pl192");
        sbd = SYS_BUS_DEVICE(vic[i]);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0,
                        i ? IPOD6G_VIC1_BASE : IPOD6G_VIC0_BASE);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(cpu_irq_or, i));
        sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(cpu_fiq_or, i));
    }

    /* Timers */
    dev = qdev_new("s5l8702-timer");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, IPOD6G_TIMER_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(vic[0], IRQ_TIMER32));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1,
                       qdev_get_gpio_in(vic[0], IRQ_TIMER));

    /* Clock / system controller */
    dev = qdev_new("s5l8702-clk");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, IPOD6G_CLK_BASE);

    /* GPIO with iPod 6G board straps (LCD type 2, PATA disk) */
    dev = qdev_new("s5l8702-gpio");
    qdev_prop_set_uint8(dev, "lcd-type", 2);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, IPOD6G_GPIO_BASE);

    /* I2C buses; PMU and audio codec on bus 0 */
    dev = qdev_new("s5l8702-i2c");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, IPOD6G_I2C0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(vic[0], IRQ_I2C0));
    i2c0 = I2C_BUS(qdev_get_child_bus(dev, "i2c"));
    i2c_slave_create_simple(i2c0, "pcf50635", 0xe6 >> 1);
    i2c_slave_create_simple(i2c0, "i2c-ack-stub", 0x94 >> 1);

    dev = qdev_new("s5l8702-i2c");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, IPOD6G_I2C1_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(vic[0], IRQ_I2C1));

    /* Click wheel */
    dev = qdev_new("s5l8702-wheel");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, IPOD6G_WHEEL_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(vic[0], IRQ_WHEEL));

    /* LCD */
    dev = qdev_new("s5l8702-lcd");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, IPOD6G_LCD_BASE);

    /* ATA disk controller */
    {
        DriveInfo *dinfo = drive_get(IF_IDE, 0, 0);

        dev = qdev_new("s5l8702-ata");
        if (dinfo) {
            qdev_prop_set_drive_err(dev, "drive",
                                    blk_by_legacy_dinfo(dinfo), &error_fatal);
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, IPOD6G_ATA_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                           qdev_get_gpio_in(vic[0], IRQ_ATA));
    }

    /* DMA controllers (PL080) */
    DeviceState *dmac0 = ipod6g_dmac(IPOD6G_DMAC0_BASE,
                                     qdev_get_gpio_in(vic[0], IRQ_DMAC0));
    /* The LCD write FIFO (DMAC0 request 3) is always ready */
    qemu_irq_raise(qdev_get_gpio_in_named(dmac0, "dreq", 3));
    ipod6g_dmac(IPOD6G_DMAC1_BASE, qdev_get_gpio_in(vic[0], IRQ_DMAC1));

    /*
     * RAM-like stubs for blocks that are configured but whose
     * behaviour is irrelevant for running Rockbox.
     */
    ipod6g_stub("s5l8702.sha", IPOD6G_SHA_BASE, 0x1000);
    ipod6g_stub("s5l8702.miu", IPOD6G_MIU_BASE, 0x1000);
    ipod6g_stub("s5l8702.usbhc", IPOD6G_USBHC_BASE, 0x1000);
    ipod6g_stub("s5l8702.usbotg", IPOD6G_USBOTG_BASE, 0x40000);
    ipod6g_stub("s5l8702.sdci", IPOD6G_SDCI_BASE, 0x1000);
    ipod6g_stub("s5l8702.aes", IPOD6G_AES_BASE, 0x1000);
    ipod6g_stub("s5l8702.vicedge", IPOD6G_VICEDGE_BASE, 0x1000);
    ipod6g_stub("s5l8702.eic", IPOD6G_EIC_BASE, 0x1000);
    ipod6g_stub("s5l8702.usbphy", IPOD6G_USBPHY_BASE, 0x1000);
    ipod6g_stub("s5l8702.wdt", IPOD6G_WDT_BASE, 0x1000);
    /* I2S0: audio output (pacing the playback DMA) */
    {
        DeviceState *i2s = qdev_new("s5l8702-i2s");

        if (machine->audiodev) {
            qdev_prop_set_string(i2s, "audiodev", machine->audiodev);
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(i2s), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(i2s), 0, IPOD6G_I2S0_BASE);
        /* IIS0_TX is DMAC0 peripheral request line 0xA */
        qdev_connect_gpio_out_named(i2s, "dreq", 0,
                                    qdev_get_gpio_in_named(dmac0, "dreq", 0xa));
    }
    {
        DeviceState *uart = qdev_new("s5l8702-uart");

        sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, IPOD6G_UART_BASE);
    }
    ipod6g_stub("s5l8702.adc", IPOD6G_ADC_BASE, 0x1000);
    ipod6g_stub("s5l8702.spi0", IPOD6G_SPI0_BASE, 0x1000);
    ipod6g_stub("s5l8702.spi2", IPOD6G_SPI2_BASE, 0x1000);

    /* Boot */
    if (machine->kernel_filename) {
        ipod6g_load_kernel(s, machine->kernel_filename);
    } else {
        s->entry = IPOD6G_IRAM_BASE;
    }
    qemu_register_reset(ipod6g_cpu_reset, s);
}

static void ipod6g_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Apple iPod Classic 6G (Samsung S5L8702)";
    mc->init = ipod6g_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    mc->default_ram_size = 64 * MiB;
    mc->default_ram_id = "ipod6g.ram";
    machine_add_audiodev_property(mc);
    mc->ignore_memory_transaction_failures = true;
}

static const TypeInfo ipod6g_machine_types[] = {
    {
        .name = TYPE_IPOD6G_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(Ipod6gMachineState),
        .class_init = ipod6g_machine_class_init,
        .interfaces = arm_machine_interfaces,
    },
};

DEFINE_TYPES(ipod6g_machine_types)
