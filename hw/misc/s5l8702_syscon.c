/*
 * Samsung S5L8702 system controller (clock generation) model, plus a
 * generic RAM-backed register-block stub used for the SoC blocks that
 * only need write/readback behaviour (MIU SDRAM controller, watchdog,
 * AES/SHA engines, ...).
 *
 * The guest (Rockbox) requires:
 *  - CG16_xx clock-gate registers to read back exactly what was
 *    written (cg16_config() polls for this),
 *  - PLLLOCK to report all PLLs locked,
 *  - everything else in the block to behave like RAM.
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

/* ------------------------------------------------------------------ */
/* Clock controller at 0x3C500000                                      */

#define TYPE_S5L8702_CLK "s5l8702-clk"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702ClkState, S5L8702_CLK)

#define CLK_REGS_SIZE  0x1000

#define REG_PLLLOCK    0x40

struct S5L8702ClkState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    /*
     * Byte-granular storage: the CG16 clock-gate registers are 16 bit
     * wide and accessed as halfwords; cg16_config() polls until a
     * register reads back exactly the written value.
     */
    uint8_t regs[CLK_REGS_SIZE];
};

static uint64_t s5l8702_clk_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702ClkState *s = opaque;
    uint64_t val = 0;

    if (offset >= REG_PLLLOCK && offset < REG_PLLLOCK + 4) {
        /* All PLLs always report locked (DM and MM lock bits). */
        return MAKE_64BIT_MASK(0, size * 8);
    }
    memcpy(&val, &s->regs[offset], size);
    return le64_to_cpu(val);
}

static void s5l8702_clk_write(void *opaque, hwaddr offset,
                              uint64_t val, unsigned size)
{
    S5L8702ClkState *s = opaque;
    uint64_t le = cpu_to_le64(val);

    memcpy(&s->regs[offset], &le, size);
}

static const MemoryRegionOps s5l8702_clk_ops = {
    .read = s5l8702_clk_read,
    .write = s5l8702_clk_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void s5l8702_clk_reset_hold(Object *obj, ResetType type)
{
    S5L8702ClkState *s = S5L8702_CLK(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void s5l8702_clk_init(Object *obj)
{
    S5L8702ClkState *s = S5L8702_CLK(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_clk_ops, s,
                          "s5l8702-clk", CLK_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_s5l8702_clk = {
    .name = "s5l8702-clk",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, S5L8702ClkState, CLK_REGS_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8702_clk_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = s5l8702_clk_reset_hold;
    dc->vmsd = &vmstate_s5l8702_clk;
}

/* ------------------------------------------------------------------ */
/* Generic RAM-backed register block stub                              */

#define TYPE_S5L8702_RAMREGS "s5l8702-ramregs"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702RamRegsState, S5L8702_RAMREGS)

struct S5L8702RamRegsState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint8_t *regs;
    uint32_t size;
    char *name;
};

static uint64_t s5l8702_ramregs_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    S5L8702RamRegsState *s = opaque;
    uint64_t val = 0;

    memcpy(&val, &s->regs[offset], size);
    return le64_to_cpu(val);
}

static void s5l8702_ramregs_write(void *opaque, hwaddr offset,
                                  uint64_t val, unsigned size)
{
    S5L8702RamRegsState *s = opaque;
    uint64_t le = cpu_to_le64(val);

    memcpy(&s->regs[offset], &le, size);
}

static const MemoryRegionOps s5l8702_ramregs_ops = {
    .read = s5l8702_ramregs_read,
    .write = s5l8702_ramregs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void s5l8702_ramregs_realize(DeviceState *dev, Error **errp)
{
    S5L8702RamRegsState *s = S5L8702_RAMREGS(dev);

    if (s->size == 0) {
        s->size = 0x1000;
    }
    s->regs = g_malloc0(s->size);
    memory_region_init_io(&s->iomem, OBJECT(dev), &s5l8702_ramregs_ops, s,
                          s->name ? s->name : "s5l8702-ramregs", s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void s5l8702_ramregs_reset_hold(Object *obj, ResetType type)
{
    S5L8702RamRegsState *s = S5L8702_RAMREGS(obj);

    if (s->regs) {
        memset(s->regs, 0, s->size);
    }
}

static const Property s5l8702_ramregs_properties[] = {
    DEFINE_PROP_UINT32("size", S5L8702RamRegsState, size, 0x1000),
    DEFINE_PROP_STRING("name", S5L8702RamRegsState, name),
};

static void s5l8702_ramregs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = s5l8702_ramregs_realize;
    rc->phases.hold = s5l8702_ramregs_reset_hold;
    device_class_set_props(dc, s5l8702_ramregs_properties);
    /* state is scratch; no vmstate needed for stub */
}

static const TypeInfo s5l8702_syscon_types[] = {
    {
        .name = TYPE_S5L8702_CLK,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8702ClkState),
        .instance_init = s5l8702_clk_init,
        .class_init = s5l8702_clk_class_init,
    },
    {
        .name = TYPE_S5L8702_RAMREGS,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8702RamRegsState),
        .class_init = s5l8702_ramregs_class_init,
    },
};

DEFINE_TYPES(s5l8702_syscon_types)
