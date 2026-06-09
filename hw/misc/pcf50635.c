/*
 * NXP PCF50635 PMU (power management unit), as found in the iPod
 * Classic 6G, plus a tiny ACK-everything I2C stub used for the
 * CS42L55 audio codec.
 *
 * Only the behaviour Rockbox depends on is modelled:
 *  - OOCSHDWN reports COLDBOOT (so the bootloader does not think the
 *    device is waking from hibernation),
 *  - GPIOSTAT reports GPIO2 high (hold switch off),
 *  - the ADC state machine completes instantly and reports a healthy
 *    battery voltage (~4.0 V) on the BATSNS channels,
 *  - everything else behaves like RAM (registers read back writes).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_PCF50635 "pcf50635"
OBJECT_DECLARE_SIMPLE_TYPE(PCF50635State, PCF50635)

#define PCF_NUM_REGS 0x100

/* Register addresses */
#define REG_INT1     0x02
#define REG_INT5     0x06
#define REG_OOCSHDWN 0x0c
#define REG_OOCSTAT  0x12
#define REG_GPIO3CFG 0x16
#define REG_MBCS1    0x4b
#define REG_ADCC3    0x52
#define REG_ADCC2    0x53
#define REG_ADCC1    0x54
#define REG_ADCS1    0x55
#define REG_ADCS2    0x56
#define REG_ADCS3    0x57
/* PCF50635 extensions */
#define REG_INT6     0x85
#define REG_INT6M    0x86
#define REG_GPIOSTAT 0x87

/* Bits */
#define OOCSHDWN_COLDBOOT  0x08
#define GPIOSTAT_GPIO2     0x02
#define ADCS3_ADCRDY       0x80
#define ADCC1_ADCSTART     0x01
#define ADCC1_MUX_MASK     0xf0
#define ADCC1_MUX_BATSNS_RES    0x00
#define ADCC1_MUX_BATSNS_SUBTR  0x10
#define ADCC1_RES_MASK     0x02 /* 0 = 8 bit, 2 = 10 bit (RES_10BIT=0x02) */

/*
 * Battery voltage to report, in raw ADC counts.
 * Rockbox converts BATSNS_SUBTR as: mV = raw10 * 2000 / 1023 + 2250.
 * raw10 = 895 -> ~4000 mV.
 */
#define ADC_BATT_RAW10  895

struct PCF50635State {
    I2CSlave parent_obj;

    uint8_t regs[PCF_NUM_REGS];
    uint8_t ptr;        /* register pointer */
    bool ptr_latched;   /* first byte of a write selects the register */
    uint16_t adc_raw;
};

static void pcf50635_adc_start(PCF50635State *s)
{
    uint8_t mux = s->regs[REG_ADCC1] & ADCC1_MUX_MASK;

    switch (mux) {
    case ADCC1_MUX_BATSNS_RES:
    case ADCC1_MUX_BATSNS_SUBTR:
        s->adc_raw = ADC_BATT_RAW10;
        break;
    default:
        s->adc_raw = 0;
        break;
    }
    s->regs[REG_ADCS1] = s->adc_raw >> 2;
    s->regs[REG_ADCS2] = 0;
    s->regs[REG_ADCS3] = ADCS3_ADCRDY | (s->adc_raw & 3);
}

static uint8_t pcf50635_read_reg(PCF50635State *s, uint8_t reg)
{
    switch (reg) {
    case REG_OOCSHDWN:
        /* device cold-booted from NoPower; not hibernated */
        return s->regs[reg] | OOCSHDWN_COLDBOOT;
    case REG_GPIOSTAT:
        /* GPIO2 high = hold switch released */
        return s->regs[reg] | GPIOSTAT_GPIO2;
    case REG_INT1 ... REG_INT5:
    case REG_INT6:
        /* no interrupts pending; reading clears */
        return 0;
    case REG_OOCSTAT:
        /*
         * No ONKEY pressed, no USB VBUS (EXTON2 clear).  EXTON3 is
         * active-low accessory detect: keep it high = no accessory
         * (otherwise Rockbox starts the iAP serial protocol).
         */
        return 0x08;
    default:
        return s->regs[reg];
    }
}

static int pcf50635_event(I2CSlave *i2c, enum i2c_event event)
{
    PCF50635State *s = PCF50635(i2c);

    if (event == I2C_START_SEND) {
        s->ptr_latched = false;
    }
    return 0;
}

static int pcf50635_tx(I2CSlave *i2c, uint8_t data)
{
    PCF50635State *s = PCF50635(i2c);

    if (!s->ptr_latched) {
        s->ptr = data;
        s->ptr_latched = true;
        return 0;
    }
    s->regs[s->ptr] = data;
    if (s->ptr == REG_ADCC1 && (data & ADCC1_ADCSTART)) {
        pcf50635_adc_start(s);
    }
    s->ptr++;
    return 0;
}

static uint8_t pcf50635_rx(I2CSlave *i2c)
{
    PCF50635State *s = PCF50635(i2c);

    return pcf50635_read_reg(s, s->ptr++);
}

static void pcf50635_reset_hold(Object *obj, ResetType type)
{
    PCF50635State *s = PCF50635(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->ptr = 0;
    s->ptr_latched = false;
    /* GPIO3CFG non-zero: a hibernating OF would have cleared it */
    s->regs[REG_GPIO3CFG] = 0x07;
}

static const VMStateDescription vmstate_pcf50635 = {
    .name = "pcf50635",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PCF50635State),
        VMSTATE_UINT8_ARRAY(regs, PCF50635State, PCF_NUM_REGS),
        VMSTATE_UINT8(ptr, PCF50635State),
        VMSTATE_BOOL(ptr_latched, PCF50635State),
        VMSTATE_UINT16(adc_raw, PCF50635State),
        VMSTATE_END_OF_LIST()
    }
};

static void pcf50635_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->event = pcf50635_event;
    k->send = pcf50635_tx;
    k->recv = pcf50635_rx;
    rc->phases.hold = pcf50635_reset_hold;
    dc->vmsd = &vmstate_pcf50635;
}

/* ------------------------------------------------------------------ */
/* Generic ACK-everything I2C slave (CS42L55 codec placeholder).       */

#define TYPE_I2C_ACK_STUB "i2c-ack-stub"
OBJECT_DECLARE_SIMPLE_TYPE(I2CAckStubState, I2C_ACK_STUB)

struct I2CAckStubState {
    I2CSlave parent_obj;
    uint8_t regs[0x100];
    uint8_t ptr;
    bool ptr_latched;
};

static int i2c_ack_stub_event(I2CSlave *i2c, enum i2c_event event)
{
    I2CAckStubState *s = I2C_ACK_STUB(i2c);

    if (event == I2C_START_SEND) {
        s->ptr_latched = false;
    }
    return 0;
}

static int i2c_ack_stub_tx(I2CSlave *i2c, uint8_t data)
{
    I2CAckStubState *s = I2C_ACK_STUB(i2c);

    if (!s->ptr_latched) {
        s->ptr = data;
        s->ptr_latched = true;
    } else {
        s->regs[s->ptr++] = data;
    }
    return 0;
}

static uint8_t i2c_ack_stub_rx(I2CSlave *i2c)
{
    I2CAckStubState *s = I2C_ACK_STUB(i2c);

    return s->regs[s->ptr++];
}

static void i2c_ack_stub_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = i2c_ack_stub_event;
    k->send = i2c_ack_stub_tx;
    k->recv = i2c_ack_stub_rx;
}

static const TypeInfo pcf50635_types[] = {
    {
        .name = TYPE_PCF50635,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(PCF50635State),
        .class_init = pcf50635_class_init,
    },
    {
        .name = TYPE_I2C_ACK_STUB,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(I2CAckStubState),
        .class_init = i2c_ack_stub_class_init,
    },
};

DEFINE_TYPES(pcf50635_types)
