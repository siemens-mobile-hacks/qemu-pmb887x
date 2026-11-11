/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#include "system/debug-channel.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/three-wire-serial.h"
#include "hw/gsm/gsm-air.h"
#include "regs.h"

#define TYPE_PMB6272 "pmb6272"

OBJECT_DECLARE_SIMPLE_TYPE(PMB6272State, PMB6272)

#define PMB6272_REGISTER_ADDRESS_MASK   0xF
#define PMB6272_REGISTER_ADDRESS_SHIFT  0U
#define PMB6272_BITS_TO_BYTES_SHIFT     3U
#define PMB6272_REFERENCE_FREQUENCY     26000000U
#define PMB6272_TELEGRAM_BITS           24U
#define PMB6272_TELEGRAM_MASK           MAKE_64BIT_MASK(0, PMB6272_TELEGRAM_BITS)
#define PMB6272_REGISTER_COUNT          16U
#define PMB6272_CHANNEL_FRACTION_BITS   23U
#define PMB6272_SIGNAL_AMPLITUDE        4000

struct PMB6272State {
    SysBusDevice parent_obj;
    DebugChannel dbg;
    ThreeWireSerialTargetPort serial_port;
    IQSourcePort iq_port;
    GSMAir air;
    uint32_t registers[PMB6272_REGISTER_COUNT];
    uint32_t telegram;
    uint32_t channel_fraction;
    uint32_t frequency;
    uint64_t shift_register;
    uint8_t telegram_bits;
    uint8_t shift_bits;
    uint8_t channel_integer;
    uint8_t band;
    uint32_t power_mode;
    bool transmit;
    bool strobe_asserted;
    bool shift_overflow;
};

static void pmb6272_update_air(PMB6272State *p) {
    if (p->power_mode != PMB6272_CHANNEL2_POWER_MODE_ALL_ON) {
        gsm_air_tune(&p->air, GSM_AIR_PATH_OFF, 0, 0);
    } else if (p->transmit) {
        gsm_air_tune(&p->air, GSM_AIR_PATH_TRANSMIT, p->frequency, 0);
    } else {
        gsm_air_tune(&p->air, GSM_AIR_PATH_RECEIVE, p->frequency, PMB6272_SIGNAL_AMPLITUDE);
    }
}

static void pmb6272_write_telegram(PMB6272State *p, uint32_t telegram, uint8_t bits) {
    static const char *band_names[] = { "GSM850", "GSM900", "DCS1800", "PCS1900" };
    uint8_t address = (telegram & PMB6272_REGISTER_ADDRESS_MASK) >> PMB6272_REGISTER_ADDRESS_SHIFT;

    p->telegram = telegram;
    p->telegram_bits = bits;
    p->registers[address] = telegram;

    switch (address) {
        case PMB6272_CHANNEL1:
            p->channel_fraction = deposit32(p->channel_fraction, 0, 20,
                (telegram & PMB6272_CHANNEL1_FRACTION) >> PMB6272_CHANNEL1_FRACTION_SHIFT);
            break;

        case PMB6272_CHANNEL2: {
            p->channel_fraction = deposit32(p->channel_fraction, 20, 3,
                (telegram & PMB6272_CHANNEL2_FRACTION) >> PMB6272_CHANNEL2_FRACTION_SHIFT);
            p->channel_integer = (telegram & PMB6272_CHANNEL2_INTEGER) >> PMB6272_CHANNEL2_INTEGER_SHIFT;
            p->power_mode = telegram & PMB6272_CHANNEL2_POWER_MODE;
            p->transmit = (telegram & PMB6272_CHANNEL2_TRX) != 0;
            p->band = (telegram & PMB6272_CHANNEL2_BAND) >> PMB6272_CHANNEL2_BAND_SHIFT;

            uint64_t synthesizer = ((uint64_t) p->channel_integer << PMB6272_CHANNEL_FRACTION_BITS) |
                p->channel_fraction;
            uint32_t divisor = p->band < 2 ? 4 : 2;
            uint32_t frequency = muldiv64(synthesizer, PMB6272_REFERENCE_FREQUENCY,
                (uint64_t) 1 << PMB6272_CHANNEL_FRACTION_BITS) / divisor;

            p->frequency = frequency;
            pmb6272_update_air(p);
            DEBUG_LOG(&p->dbg, "tuned to %u Hz: band=%s path=%s\n", frequency, band_names[p->band],
                p->transmit ? "TX" : "RX");
            break;
        }
    }

    DEBUG_IO_WRITE(&p->dbg, address, bits >> PMB6272_BITS_TO_BYTES_SHIFT, telegram);
}

static void pmb6272_set_strobe(ThreeWireSerialTarget *target, bool asserted) {
    PMB6272State *p = PMB6272(target);

    if (asserted) {
        p->shift_register = 0;
        p->shift_bits = 0;
        p->shift_overflow = false;
        p->strobe_asserted = true;
        return;
    }

    if (!p->strobe_asserted)
        return;

    p->strobe_asserted = false;
    if (!p->shift_overflow && p->shift_bits != 0)
        pmb6272_write_telegram(p, p->shift_register & PMB6272_TELEGRAM_MASK, p->shift_bits);
}

static void pmb6272_write(ThreeWireSerialTarget *target, uint32_t value, unsigned int bits) {
    PMB6272State *p = PMB6272(target);

    if (!p->strobe_asserted || p->shift_overflow)
        return;
    if (bits > PMB6272_TELEGRAM_BITS - p->shift_bits) {
        p->shift_overflow = true;
        return;
    }

    p->shift_register = (p->shift_register << bits) | (value & MAKE_64BIT_MASK(0, bits));
    p->shift_bits += bits;
}

static IQSample pmb6272_read_iq(IQSource *source, int64_t timestamp, uint64_t _sample_index) {
    IQSample sample = gsm_air_read_iq(&PMB6272(source)->air, timestamp);

    /* Firmware for this transceiver finds FCCH but decodes SCH only from the mirrored spectrum. */
    sample.q = -sample.q;
    return sample;
}

static void pmb6272_transmit(IQSource *source, int64_t timestamp, bool symbol) {
    gsm_air_transmit(&PMB6272(source)->air, timestamp, symbol);
}

static void pmb6272_transmit_until(IQSource *source, int64_t timestamp) {
    gsm_air_transmit_until(&PMB6272(source)->air, timestamp);
}

static void pmb6272_reset(DeviceState *device) {
    PMB6272State *p = PMB6272(device);

    memset(p->registers, 0, sizeof(p->registers));
    p->telegram = 0;
    p->channel_fraction = 0;
    p->frequency = 0;
    p->shift_register = 0;
    p->telegram_bits = 0;
    p->shift_bits = 0;
    p->channel_integer = 0;
    p->band = 0;
    p->power_mode = PMB6272_CHANNEL2_POWER_MODE_ALL_OFF;
    p->transmit = false;
    p->strobe_asserted = false;
    p->shift_overflow = false;
    gsm_air_reset(&p->air);
}

static void pmb6272_init(Object *obj) {
    PMB6272State *p = PMB6272(obj);

    three_wire_serial_target_port_init(&p->serial_port, DEVICE(obj), "RFSSC", THREE_WIRE_SERIAL_TARGET(obj));
    iq_source_port_init(&p->iq_port, DEVICE(obj), "IQ_TX", IQ_SOURCE(obj));
    gsm_air_init_uplink(&p->air);
}

static void pmb6272_finalize(Object *obj) {
    PMB6272State *p = PMB6272(obj);
    gsm_air_cleanup_uplink(&p->air);
}

static void pmb6272_realize(DeviceState *dev, Error **_errp) {
    PMB6272State *p = PMB6272(dev);
    debug_channel_init_device_regs(&p->dbg, dev, &pmb6272_debug_regs);
}

static const Property pmb6272_properties[] = {
    DEFINE_PROP_LINK("gsm-network", PMB6272State, air.network, TYPE_GSM_NETWORK, GSMNetwork *),
};

static void pmb6272_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *device_class = DEVICE_CLASS(klass);
    ThreeWireSerialTargetClass *serial_class = THREE_WIRE_SERIAL_TARGET_CLASS(klass);
    IQSourceClass *iq_class = IQ_SOURCE_CLASS(klass);

    serial_class->set_strobe = pmb6272_set_strobe;
    serial_class->write = pmb6272_write;
    iq_class->read = pmb6272_read_iq;
    iq_class->transmit = pmb6272_transmit;
    iq_class->transmit_until = pmb6272_transmit_until;
    device_class_set_legacy_reset(device_class, pmb6272_reset);
    device_class->realize = pmb6272_realize;
    device_class_set_props(device_class, pmb6272_properties);
}

static const TypeInfo pmb6272_info = {
    .name = TYPE_PMB6272,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PMB6272State),
    .instance_init = pmb6272_init,
    .instance_finalize = pmb6272_finalize,
    .class_init = pmb6272_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_THREE_WIRE_SERIAL_TARGET },
        { TYPE_IQ_SOURCE },
        { NULL },
    },
};

static void pmb6272_register_types(void) {
    type_register_static(&pmb6272_info);
}

type_init(pmb6272_register_types)
