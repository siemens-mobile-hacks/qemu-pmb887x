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

#define TYPE_PMB627X "pmb627x"
#define TYPE_PMB6270 "pmb6270"
#define TYPE_PMB6272 "pmb6272"

OBJECT_DECLARE_TYPE(PMB627XState, PMB627XClass, PMB627X)

#define PMB627X_REGISTER_ADDRESS_MASK   0xF
#define PMB627X_REGISTER_ADDRESS_SHIFT  0U
#define PMB627X_BITS_TO_BYTES_SHIFT     3U
#define PMB627X_REFERENCE_FREQUENCY     26000000U
#define PMB627X_TELEGRAM_BITS           24U
#define PMB627X_TELEGRAM_MASK           MAKE_64BIT_MASK(0, PMB627X_TELEGRAM_BITS)
#define PMB627X_REGISTER_COUNT          16U
#define PMB627X_CHANNEL_FRACTION_BITS   23U
#define PMB627X_LNA_LOW_GAIN_DB         23
#define PMB627X_LNA_MEDIUM_GAIN_DB      43
#define PMB627X_LNA_HIGH_GAIN_DB        57
#define PMB627X_RXGS0_GAIN_DB           12
#define PMB627X_RXGS1_GAIN_DB           6
#define PMB627X_RXGS2_GAIN_DB           (-6)
#define PMB627X_RXGS3_GAIN_DB           (-3)
#define PMB627X_FAKE_SIGNAL_LEVEL_DBM   (-60)
#define PMB627X_APPROX_ADC_OFFSET_DB    75
#define PMB627X_ADC_MAX_AMPLITUDE       16000

struct PMB627XState {
    SysBusDevice parent_obj;
    DebugChannel dbg;
    ThreeWireSerialTargetPort serial_port;
    IQSourcePort iq_port;
    GSMAir air;
    uint32_t registers[PMB627X_REGISTER_COUNT];
    uint32_t telegram;
    uint32_t channel_fraction;
    uint32_t frequency;
    int32_t amplitude;
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

struct PMB627XClass {
    SysBusDeviceClass parent_class;
    bool mirrors_spectrum;
};

static void pmb627x_update_air(PMB627XState *p, int64_t timestamp) {
    if (p->power_mode != PMB627X_CHANNEL2_POWER_MODE_ALL_ON) {
        gsm_air_tune(&p->air, GSM_AIR_PATH_OFF, 0, 0, timestamp);
    } else if (p->transmit) {
        gsm_air_tune(&p->air, GSM_AIR_PATH_TRANSMIT, p->frequency, 0, timestamp);
    } else {
        gsm_air_tune(&p->air, GSM_AIR_PATH_RECEIVE, p->frequency, p->amplitude, timestamp);
    }
}

static int32_t pmb627x_lna_gain_db(uint32_t rxtx) {
    switch (rxtx & PMB627X_RXTX_RXGAIN) {
        case PMB627X_RXTX_RXGAIN_HIGH:
            return PMB627X_LNA_HIGH_GAIN_DB;

        case PMB627X_RXTX_RXGAIN_MEDIUM:
            return PMB627X_LNA_MEDIUM_GAIN_DB;
    }
    return PMB627X_LNA_LOW_GAIN_DB;
}

static void pmb627x_set_gain(PMB627XState *p, uint32_t rxtx, int64_t timestamp) {
    int32_t gain_db = pmb627x_lna_gain_db(rxtx);

    if ((rxtx & PMB627X_RXTX_RXGS0) != 0)
        gain_db += PMB627X_RXGS0_GAIN_DB;
    if ((rxtx & PMB627X_RXTX_RXGS1) != 0)
        gain_db += PMB627X_RXGS1_GAIN_DB;
    if ((rxtx & PMB627X_RXTX_RXGS2) != 0)
        gain_db += PMB627X_RXGS2_GAIN_DB;
    if ((rxtx & PMB627X_RXTX_RXGS3) != 0)
        gain_db += PMB627X_RXGS3_GAIN_DB;

    int32_t adc_level_db = PMB627X_FAKE_SIGNAL_LEVEL_DBM + PMB627X_APPROX_ADC_OFFSET_DB + gain_db;
    int32_t amplitude = (int32_t) lround(pow(10.0, adc_level_db / 20.0));

    p->amplitude = MIN(amplitude, PMB627X_ADC_MAX_AMPLITUDE);
    pmb627x_update_air(p, timestamp);
}

static void pmb627x_write_telegram(PMB627XState *p, uint32_t telegram, uint8_t bits, int64_t timestamp) {
    uint8_t address = (telegram & PMB627X_REGISTER_ADDRESS_MASK) >> PMB627X_REGISTER_ADDRESS_SHIFT;

    p->telegram = telegram;
    p->telegram_bits = bits;
    p->registers[address] = telegram;

    switch (address) {
        case PMB627X_CHANNEL1:
            p->channel_fraction = deposit32(p->channel_fraction, 0, 20,
                (telegram & PMB627X_CHANNEL1_FRACTION) >> PMB627X_CHANNEL1_FRACTION_SHIFT);
            break;

        case PMB627X_CHANNEL2: {
            p->channel_fraction = deposit32(p->channel_fraction, 20, 3,
                (telegram & PMB627X_CHANNEL2_FRACTION) >> PMB627X_CHANNEL2_FRACTION_SHIFT);
            p->channel_integer = (telegram & PMB627X_CHANNEL2_INTEGER) >> PMB627X_CHANNEL2_INTEGER_SHIFT;
            p->power_mode = telegram & PMB627X_CHANNEL2_POWER_MODE;
            p->transmit = (telegram & PMB627X_CHANNEL2_TRX) != 0;
            p->band = (telegram & PMB627X_CHANNEL2_BAND) >> PMB627X_CHANNEL2_BAND_SHIFT;

            uint64_t synthesizer = ((uint64_t) p->channel_integer << PMB627X_CHANNEL_FRACTION_BITS) |
                p->channel_fraction;
            uint32_t divisor = p->band < 2 ? 4 : 2;
            uint32_t frequency = muldiv64(synthesizer, PMB627X_REFERENCE_FREQUENCY,
                (uint64_t) 1 << PMB627X_CHANNEL_FRACTION_BITS) / divisor;

            p->frequency = frequency;
            pmb627x_update_air(p, timestamp);
            DEBUG_LOG(&p->dbg, "tuned to %u Hz: band=%u path=%s\n", frequency, p->band,
                p->transmit ? "TX" : "RX");
            break;
        }

        case PMB627X_RXTX:
            pmb627x_set_gain(p, telegram, timestamp);
            break;
    }

    DEBUG_IO_WRITE(&p->dbg, address, bits >> PMB627X_BITS_TO_BYTES_SHIFT, telegram);
}

static void pmb627x_set_strobe(ThreeWireSerialTarget *target, bool asserted, int64_t timestamp) {
    PMB627XState *p = PMB627X(target);

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
        pmb627x_write_telegram(p, p->shift_register & PMB627X_TELEGRAM_MASK, p->shift_bits, timestamp);
}

static void pmb627x_write(ThreeWireSerialTarget *target, uint32_t value, unsigned int bits) {
    PMB627XState *p = PMB627X(target);

    if (!p->strobe_asserted || p->shift_overflow)
        return;
    if (bits > PMB627X_TELEGRAM_BITS - p->shift_bits) {
        p->shift_overflow = true;
        return;
    }

    p->shift_register = (p->shift_register << bits) | (value & MAKE_64BIT_MASK(0, bits));
    p->shift_bits += bits;
}

static IQSample pmb627x_read_iq(IQSource *source, int64_t timestamp, uint64_t _sample_index) {
    IQSample sample = gsm_air_read_iq(&PMB627X(source)->air, timestamp);

    if (PMB627X_GET_CLASS(source)->mirrors_spectrum)
        sample.q = -sample.q;
    return sample;
}

static void pmb627x_transmit(IQSource *source, int64_t timestamp, bool symbol) {
    gsm_air_transmit(&PMB627X(source)->air, timestamp, symbol);
}

static void pmb627x_transmit_until(IQSource *source, int64_t timestamp) {
    gsm_air_transmit_until(&PMB627X(source)->air, timestamp);
}

static void pmb627x_reset(DeviceState *device) {
    PMB627XState *p = PMB627X(device);

    memset(p->registers, 0, sizeof(p->registers));
    p->telegram = 0;
    p->channel_fraction = 0;
    p->frequency = 0;
    p->amplitude = 0;
    p->shift_register = 0;
    p->telegram_bits = 0;
    p->shift_bits = 0;
    p->channel_integer = 0;
    p->band = 0;
    p->power_mode = PMB627X_CHANNEL2_POWER_MODE_ALL_OFF;
    p->transmit = false;
    p->strobe_asserted = false;
    p->shift_overflow = false;
    gsm_air_reset(&p->air);
}

static void pmb627x_init(Object *obj) {
    PMB627XState *p = PMB627X(obj);

    three_wire_serial_target_port_init(&p->serial_port, DEVICE(obj), "RFSSC", THREE_WIRE_SERIAL_TARGET(obj));
    iq_source_port_init(&p->iq_port, DEVICE(obj), "IQ_TX", IQ_SOURCE(obj));
    gsm_air_init_uplink(&p->air);
}

static void pmb627x_finalize(Object *obj) {
    PMB627XState *p = PMB627X(obj);
    gsm_air_cleanup_uplink(&p->air);
}

static void pmb627x_realize(DeviceState *dev, Error **_errp) {
    PMB627XState *p = PMB627X(dev);
    debug_channel_init_device_regs(&p->dbg, dev, &pmb627x_debug_regs);
}

static const Property pmb627x_properties[] = {
    DEFINE_PROP_LINK("gsm-network", PMB627XState, air.network, TYPE_GSM_NETWORK, GSMNetwork *),
};

static void pmb627x_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *device_class = DEVICE_CLASS(klass);
    ThreeWireSerialTargetClass *serial_class = THREE_WIRE_SERIAL_TARGET_CLASS(klass);
    IQSourceClass *iq_class = IQ_SOURCE_CLASS(klass);

    serial_class->set_strobe = pmb627x_set_strobe;
    serial_class->write = pmb627x_write;
    iq_class->read = pmb627x_read_iq;
    iq_class->transmit = pmb627x_transmit;
    iq_class->transmit_until = pmb627x_transmit_until;
    device_class_set_legacy_reset(device_class, pmb627x_reset);
    device_class->realize = pmb627x_realize;
    device_class_set_props(device_class, pmb627x_properties);
}

/*
 * Firmware for PMB6272 disables the DSP I/Q swap (IQ_SWAP_1/2 = 0) that firmware for
 * PMB6270 and other transceivers enables, on boards with straight I/Q wiring.
 */
static void pmb6272_class_init(ObjectClass *klass, const void *_data) {
    PMB627X_CLASS(klass)->mirrors_spectrum = true;
}

static const TypeInfo pmb627x_types[] = {
    {
        .name = TYPE_PMB627X,
        .parent = TYPE_SYS_BUS_DEVICE,
        .abstract = true,
        .instance_size = sizeof(PMB627XState),
        .instance_init = pmb627x_init,
        .instance_finalize = pmb627x_finalize,
        .class_size = sizeof(PMB627XClass),
        .class_init = pmb627x_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_THREE_WIRE_SERIAL_TARGET },
            { TYPE_IQ_SOURCE },
            { NULL },
        },
    },
    {
        .name = TYPE_PMB6270,
        .parent = TYPE_PMB627X,
    },
    {
        .name = TYPE_PMB6272,
        .parent = TYPE_PMB627X,
        .class_init = pmb6272_class_init,
    },
};

DEFINE_TYPES(pmb627x_types)
