/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>

#include "qemu/bitops.h"
#include "qemu/module.h"

#include "system/debug-channel.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/three-wire-serial.h"
#include "hw/gsm/gsm-air.h"
#include "regs.h"

#define TYPE_BRIGHT_6PLA "bright-6pla"

OBJECT_DECLARE_SIMPLE_TYPE(Bright6plaState, BRIGHT_6PLA)

#define BRIGHT_6PLA_REGISTER_BITS               4U
#define BRIGHT_6PLA_MAX_TELEGRAM_BITS           24U
#define BRIGHT_6PLA_BITS_TO_BYTES_SHIFT         3U
#define BRIGHT_6PLA_LAST_LOW_BAND               1U
#define BRIGHT_6PLA_LOW_BAND_STEP_HZ            100000U
#define BRIGHT_6PLA_HIGH_BAND_STEP_HZ           200000U
#define BRIGHT_6PLA_PGA_STEP_DB                 2
#define BRIGHT_6PLA_LNA_NORMAL                  0U
#define BRIGHT_6PLA_LNA_REDUCED                 2U
#define BRIGHT_6PLA_APPROX_BASE_GAIN_DB         20
#define BRIGHT_6PLA_APPROX_LNA_REDUCED_DB       18
#define BRIGHT_6PLA_APPROX_LNA_MINIMUM_DB       36
#define BRIGHT_6PLA_FAKE_SIGNAL_LEVEL_DBM       (-60)
#define BRIGHT_6PLA_ADC_AMPLITUDE_OFFSET_DB     60
#define BRIGHT_6PLA_ADC_MAX_AMPLITUDE           16000

struct Bright6plaState {
    SysBusDevice parent_obj;
    DebugChannel dbg;
    ThreeWireSerialTargetPort serial_port;
    IQSourcePort iq_port;
    GSMAir air;
    uint32_t frequency;
    int32_t amplitude;
    uint32_t shift_register;
    uint8_t shift_bits;
    bool strobe_asserted;
    bool shift_overflow;
    bool transmitting;
};

static void bright_6pla_update_air(Bright6plaState *p, int64_t timestamp) {
    if (p->frequency == 0) {
        gsm_air_tune(&p->air, GSM_AIR_PATH_OFF, 0, 0, timestamp);
    } else if (p->transmitting) {
        gsm_air_tune(&p->air, GSM_AIR_PATH_TRANSMIT, p->frequency, 0, timestamp);
    } else {
        gsm_air_tune(&p->air, GSM_AIR_PATH_RECEIVE, p->frequency, p->amplitude, timestamp);
    }
}

static int32_t bright_6pla_lna_gain_db(uint32_t lna) {
    if (lna == BRIGHT_6PLA_LNA_NORMAL)
        return 0;
    if (lna == BRIGHT_6PLA_LNA_REDUCED)
        return -BRIGHT_6PLA_APPROX_LNA_REDUCED_DB;
    return -BRIGHT_6PLA_APPROX_LNA_MINIMUM_DB;
}

static void bright_6pla_set_gain(Bright6plaState *p, uint32_t value, int64_t timestamp) {
    uint32_t lna = (value & BRIGHT_6PLA_GAIN_LNA) >> BRIGHT_6PLA_GAIN_LNA_SHIFT;
    uint32_t pga = (value & BRIGHT_6PLA_GAIN_PGA) >> BRIGHT_6PLA_GAIN_PGA_SHIFT;
    int32_t gain_db = BRIGHT_6PLA_APPROX_BASE_GAIN_DB + bright_6pla_lna_gain_db(lna) +
        (int32_t) pga * BRIGHT_6PLA_PGA_STEP_DB;

    int32_t adc_level_db = BRIGHT_6PLA_FAKE_SIGNAL_LEVEL_DBM + BRIGHT_6PLA_ADC_AMPLITUDE_OFFSET_DB + gain_db;
    int32_t amplitude = (int32_t) lround(pow(10.0, adc_level_db / 20.0));

    p->amplitude = MIN(amplitude, BRIGHT_6PLA_ADC_MAX_AMPLITUDE);
    bright_6pla_update_air(p, timestamp);
}

static void bright_6pla_set_channel(Bright6plaState *p, uint32_t value, int64_t timestamp) {
    uint32_t band = (value & BRIGHT_6PLA_CHANNEL_BAND) >> BRIGHT_6PLA_CHANNEL_BAND_SHIFT;
    uint32_t channel = (value & BRIGHT_6PLA_CHANNEL_FREQUENCY) >> BRIGHT_6PLA_CHANNEL_FREQUENCY_SHIFT;
    uint32_t step_hz = band <= BRIGHT_6PLA_LAST_LOW_BAND ? BRIGHT_6PLA_LOW_BAND_STEP_HZ : BRIGHT_6PLA_HIGH_BAND_STEP_HZ;

    p->transmitting = (value & BRIGHT_6PLA_CHANNEL_RECEIVE) == 0;
    p->frequency = channel * step_hz;
    bright_6pla_update_air(p, timestamp);
    DEBUG_LOG(&p->dbg, "tuned: frequency=%u Hz band=%u %s\n", p->frequency, band, p->transmitting ? "TX" : "RX");
}

static void bright_6pla_write_telegram(Bright6plaState *p, uint32_t telegram, uint8_t bits, int64_t timestamp) {
    uint8_t value_bits = bits - BRIGHT_6PLA_REGISTER_BITS;
    uint8_t address = telegram >> value_bits;
    uint32_t value = telegram & MAKE_64BIT_MASK(0, value_bits);

    switch (address) {
        case BRIGHT_6PLA_CHANNEL:
            bright_6pla_set_channel(p, value, timestamp);
            break;

        case BRIGHT_6PLA_GAIN:
            bright_6pla_set_gain(p, value, timestamp);
            break;
    }

    DEBUG_IO_WRITE(&p->dbg, address, bits >> BRIGHT_6PLA_BITS_TO_BYTES_SHIFT, value);
}

static void bright_6pla_set_strobe(ThreeWireSerialTarget *target, bool asserted, int64_t timestamp) {
    Bright6plaState *p = BRIGHT_6PLA(target);

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
    if (!p->shift_overflow && p->shift_bits > BRIGHT_6PLA_REGISTER_BITS)
        bright_6pla_write_telegram(p, p->shift_register, p->shift_bits, timestamp);
}

static void bright_6pla_write(ThreeWireSerialTarget *target, uint32_t value, unsigned int bits) {
    Bright6plaState *p = BRIGHT_6PLA(target);

    if (!p->strobe_asserted || p->shift_overflow)
        return;
    if (bits > BRIGHT_6PLA_MAX_TELEGRAM_BITS - p->shift_bits) {
        p->shift_overflow = true;
        return;
    }

    p->shift_register = (p->shift_register << bits) | (value & MAKE_64BIT_MASK(0, bits));
    p->shift_bits += bits;
}

static IQSample bright_6pla_read_iq(IQSource *source, int64_t timestamp, uint64_t _sample_index) {
    return gsm_air_read_iq(&BRIGHT_6PLA(source)->air, timestamp);
}

static void bright_6pla_transmit(IQSource *source, int64_t timestamp, bool symbol) {
    gsm_air_transmit(&BRIGHT_6PLA(source)->air, timestamp, symbol);
}

static void bright_6pla_transmit_until(IQSource *source, int64_t timestamp) {
    gsm_air_transmit_until(&BRIGHT_6PLA(source)->air, timestamp);
}

static void bright_6pla_reset(DeviceState *device) {
    Bright6plaState *p = BRIGHT_6PLA(device);

    p->frequency = 0;
    p->amplitude = 0;
    p->shift_register = 0;
    p->shift_bits = 0;
    p->strobe_asserted = false;
    p->shift_overflow = false;
    p->transmitting = false;
    gsm_air_reset(&p->air);
}

static void bright_6pla_init(Object *obj) {
    Bright6plaState *p = BRIGHT_6PLA(obj);

    three_wire_serial_target_port_init(&p->serial_port, DEVICE(obj), "RFSSC", THREE_WIRE_SERIAL_TARGET(obj));
    iq_source_port_init(&p->iq_port, DEVICE(obj), "IQ_TX", IQ_SOURCE(obj));
    gsm_air_init_uplink(&p->air);
}

static void bright_6pla_finalize(Object *obj) {
    Bright6plaState *p = BRIGHT_6PLA(obj);
    gsm_air_cleanup_uplink(&p->air);
}

static void bright_6pla_realize(DeviceState *dev, Error **_errp) {
    Bright6plaState *p = BRIGHT_6PLA(dev);
    debug_channel_init_device_regs(&p->dbg, dev, &bright_6pla_debug_regs);
}

static const Property bright_6pla_properties[] = {
    DEFINE_PROP_LINK("gsm-network", Bright6plaState, air.network, TYPE_GSM_NETWORK, GSMNetwork *),
};

static void bright_6pla_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *device_class = DEVICE_CLASS(klass);
    ThreeWireSerialTargetClass *serial_class = THREE_WIRE_SERIAL_TARGET_CLASS(klass);
    IQSourceClass *iq_class = IQ_SOURCE_CLASS(klass);

    serial_class->set_strobe = bright_6pla_set_strobe;
    serial_class->write = bright_6pla_write;
    iq_class->read = bright_6pla_read_iq;
    iq_class->transmit = bright_6pla_transmit;
    iq_class->transmit_until = bright_6pla_transmit_until;
    device_class_set_legacy_reset(device_class, bright_6pla_reset);
    device_class->realize = bright_6pla_realize;
    device_class_set_props(device_class, bright_6pla_properties);
}

static const TypeInfo bright_6pla_types[] = {
    {
        .name = TYPE_BRIGHT_6PLA,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Bright6plaState),
        .instance_init = bright_6pla_init,
        .instance_finalize = bright_6pla_finalize,
        .class_init = bright_6pla_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_THREE_WIRE_SERIAL_TARGET },
            { TYPE_IQ_SOURCE },
            { NULL },
        },
    },
};

DEFINE_TYPES(bright_6pla_types)
