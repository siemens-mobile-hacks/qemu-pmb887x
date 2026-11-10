/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>

#include "qemu/atomic.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#include "system/debug-channel.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/three-wire-serial.h"
#include "hw/gsm/gsm-air.h"
#include "regs.h"

#define TYPE_HD155153NP "hd155153np"

OBJECT_DECLARE_SIMPLE_TYPE(HD155153NPState, HD155153NP)

#define HD155153NP_REGISTER_ADDRESS_MASK        0x7
#define HD155153NP_REGISTER_ADDRESS_SHIFT       0U
#define HD155153NP_REGISTER_VALUE_SHIFT         3U
#define HD155153NP_BITS_TO_BYTES_SHIFT          3U
#define HD155153NP_TELEGRAM_BITS                24U
#define HD155153NP_TELEGRAM_MASK                MAKE_64BIT_MASK(0, HD155153NP_TELEGRAM_BITS)
#define HD155153NP_REFERENCE_FREQUENCY          26000000U
#define HD155153NP_CHANNEL_RASTER_DIVIDER       130U
#define HD155153NP_PLL_DIVIDER_MASK             0xFFFF
#define HD155153NP_PLL_BAND_MASK                0x3
#define HD155153NP_PLL_BAND_GSM850              0U
#define HD155153NP_PLL_BAND_GSM900              1U
#define HD155153NP_PLL_BAND_DCS1800             2U
#define HD155153NP_PLL_BAND_PCS1900             3U
#define HD155153NP_PLL_LOW_BAND_DIVIDER_SHIFT   3U
#define HD155153NP_PLL_HIGH_BAND_DIVIDER_SHIFT  2U
#define HD155153NP_RECEIVER_BASE_GAIN_DB        25
#define HD155153NP_PGA_STEP_DB                  2
#define HD155153NP_MIXER1_LOW_GAIN_REDUCTION_DB 36
#define HD155153NP_MIXER2_LOW_GAIN_REDUCTION_DB 17
#define HD155153NP_FAKE_SIGNAL_LEVEL_DBM        (-60)
#define HD155153NP_ADC_AMPLITUDE_OFFSET_DB      60
#define HD155153NP_ADC_MAX_AMPLITUDE            16000

struct HD155153NPState {
    SysBusDevice parent_obj;
    DebugChannel dbg;
    ThreeWireSerialTargetPort serial_port;
    IQSourcePort iq_port;
    GSMAir air;
    uint32_t telegram;
    uint32_t frequency;
    int32_t amplitude;
    uint64_t shift_register;
    uint8_t telegram_bits;
    uint8_t shift_bits;
    bool strobe_asserted;
    bool shift_overflow;
};

static void hd155153np_set_gain(HD155153NPState *p, uint32_t value) {
    uint32_t step = (value & HD155153NP_GAIN_CONTROL_PGA_GAIN) >> HD155153NP_GAIN_CONTROL_PGA_GAIN_SHIFT;
    int32_t gain_db = HD155153NP_RECEIVER_BASE_GAIN_DB + (int32_t) step * HD155153NP_PGA_STEP_DB;

    if ((value & HD155153NP_GAIN_CONTROL_MIXER1_LOW_GAIN) != 0)
        gain_db -= HD155153NP_MIXER1_LOW_GAIN_REDUCTION_DB;
    if ((value & HD155153NP_GAIN_CONTROL_MIXER2_LOW_GAIN) != 0)
        gain_db -= HD155153NP_MIXER2_LOW_GAIN_REDUCTION_DB;

    /* Firmware AGC calibration describes 2 dB PGA steps and 36/17 dB mixer range reductions. */
    int32_t adc_level_db = HD155153NP_FAKE_SIGNAL_LEVEL_DBM + HD155153NP_ADC_AMPLITUDE_OFFSET_DB + gain_db;
    int32_t amplitude = (int32_t) lround(pow(10.0, adc_level_db / 20.0));

    qatomic_set(&p->amplitude, MIN(amplitude, HD155153NP_ADC_MAX_AMPLITUDE));
}

static void hd155153np_set_channel(HD155153NPState *p, uint32_t value) {
    uint32_t synthesizer = value & HD155153NP_CHANNEL_PLL_SYNTHESIZER;
    uint32_t mode = (value & HD155153NP_CHANNEL_PLL_MODE) >> HD155153NP_CHANNEL_PLL_MODE_SHIFT;
    uint32_t band_code = synthesizer & HD155153NP_PLL_BAND_MASK;
    const char *band;
    uint8_t divider_shift;

    switch (band_code) {
        case HD155153NP_PLL_BAND_GSM850:
            band = "GSM850";
            divider_shift = HD155153NP_PLL_LOW_BAND_DIVIDER_SHIFT;
            break;
        case HD155153NP_PLL_BAND_GSM900:
            band = "GSM900";
            divider_shift = HD155153NP_PLL_LOW_BAND_DIVIDER_SHIFT;
            break;
        case HD155153NP_PLL_BAND_DCS1800:
            band = "DCS1800";
            divider_shift = HD155153NP_PLL_HIGH_BAND_DIVIDER_SHIFT;
            break;
        case HD155153NP_PLL_BAND_PCS1900:
            band = "PCS1900";
            divider_shift = HD155153NP_PLL_HIGH_BAND_DIVIDER_SHIFT;
            break;
    }

    uint32_t divider = (synthesizer & HD155153NP_PLL_DIVIDER_MASK) >> divider_shift;

    if (divider == 0) {
        qatomic_set(&p->frequency, 0);
        return;
    }

    uint32_t channel_raster = HD155153NP_REFERENCE_FREQUENCY / HD155153NP_CHANNEL_RASTER_DIVIDER;
    uint32_t frequency = divider * channel_raster;

    qatomic_set(&p->frequency, frequency);
    DEBUG_LOG(&p->dbg, "tuned: frequency=%u Hz band=%s mode=%u\n", frequency, band, mode);
}

static void hd155153np_write_telegram(HD155153NPState *p, uint32_t telegram, uint8_t bits) {
    uint8_t address = (telegram & HD155153NP_REGISTER_ADDRESS_MASK) >> HD155153NP_REGISTER_ADDRESS_SHIFT;
    uint32_t value = telegram >> HD155153NP_REGISTER_VALUE_SHIFT;

    p->telegram = telegram;
    p->telegram_bits = bits;

    switch (address) {
        case HD155153NP_CHANNEL_PLL:
            hd155153np_set_channel(p, value);
            break;

        case HD155153NP_GAIN_CONTROL:
            hd155153np_set_gain(p, value);
            break;
    }

    DEBUG_IO_WRITE(&p->dbg, address, bits >> HD155153NP_BITS_TO_BYTES_SHIFT, value);
}

static void hd155153np_set_strobe(ThreeWireSerialTarget *target, bool asserted) {
    HD155153NPState *p = HD155153NP(target);

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
        hd155153np_write_telegram(p, p->shift_register & HD155153NP_TELEGRAM_MASK, p->shift_bits);
}

static void hd155153np_write(ThreeWireSerialTarget *target, uint32_t value, unsigned int bits) {
    HD155153NPState *p = HD155153NP(target);

    if (!p->strobe_asserted || p->shift_overflow)
        return;
    if (bits > HD155153NP_TELEGRAM_BITS - p->shift_bits) {
        p->shift_overflow = true;
        return;
    }

    p->shift_register = (p->shift_register << bits) | (value & MAKE_64BIT_MASK(0, bits));
    p->shift_bits += bits;
}

static IQSample hd155153np_read_iq(IQSource *source, int64_t timestamp, uint64_t _sample_index) {
    HD155153NPState *p = HD155153NP(source);
    uint32_t frequency = qatomic_read(&p->frequency);
    int32_t amplitude = qatomic_read(&p->amplitude);

    return gsm_air_read_iq(&p->air, frequency, timestamp, amplitude);
}

static void hd155153np_reset(DeviceState *device) {
    HD155153NPState *p = HD155153NP(device);

    p->telegram = 0;
    qatomic_set(&p->frequency, 0);
    qatomic_set(&p->amplitude, 0);
    p->shift_register = 0;
    p->telegram_bits = 0;
    p->shift_bits = 0;
    p->strobe_asserted = false;
    p->shift_overflow = false;
}

static void hd155153np_init(Object *obj) {
    HD155153NPState *p = HD155153NP(obj);

    three_wire_serial_target_port_init(&p->serial_port, DEVICE(obj), "RFSSC", THREE_WIRE_SERIAL_TARGET(obj));
    iq_source_port_init(&p->iq_port, DEVICE(obj), "IQ_TX", IQ_SOURCE(obj));
}

static void hd155153np_realize(DeviceState *dev, Error **_errp) {
    HD155153NPState *p = HD155153NP(dev);
    debug_channel_init_device_regs(&p->dbg, dev, &hd155153np_debug_regs);
}

static const Property hd155153np_properties[] = {
    DEFINE_PROP_LINK("gsm-network", HD155153NPState, air.network, TYPE_GSM_NETWORK, GSMNetwork *),
};

static void hd155153np_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *device_class = DEVICE_CLASS(klass);
    ThreeWireSerialTargetClass *serial_class = THREE_WIRE_SERIAL_TARGET_CLASS(klass);
    IQSourceClass *iq_class = IQ_SOURCE_CLASS(klass);

    serial_class->set_strobe = hd155153np_set_strobe;
    serial_class->write = hd155153np_write;
    iq_class->read = hd155153np_read_iq;
    device_class_set_legacy_reset(device_class, hd155153np_reset);
    device_class->realize = hd155153np_realize;
    device_class_set_props(device_class, hd155153np_properties);
}

static const TypeInfo hd155153np_info = {
    .name = TYPE_HD155153NP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(HD155153NPState),
    .instance_init = hd155153np_init,
    .class_init = hd155153np_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_THREE_WIRE_SERIAL_TARGET },
        { TYPE_IQ_SOURCE },
        { NULL },
    },
};

static void hd155153np_register_types(void) {
    type_register_static(&hd155153np_info);
}

type_init(hd155153np_register_types)
