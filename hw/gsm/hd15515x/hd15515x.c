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

#define TYPE_HD15515X       "hd15515x"
#define TYPE_HD155153NP     "hd155153np"
#define TYPE_HD155155NPEB   "hd155155npeb"

OBJECT_DECLARE_TYPE(HD15515XState, HD15515XClass, HD15515X)

#define HD15515X_REGISTER_ADDRESS_MASK          0x7
#define HD15515X_REGISTER_ADDRESS_SHIFT         0U
#define HD15515X_REGISTER_VALUE_SHIFT           3U
#define HD15515X_BITS_TO_BYTES_SHIFT            3U
#define HD15515X_TELEGRAM_BITS                  24U
#define HD15515X_TELEGRAM_MASK                  MAKE_64BIT_MASK(0, HD15515X_TELEGRAM_BITS)
#define HD15515X_REFERENCE_FREQUENCY            26000000U
#define HD15515X_CHANNEL_RASTER_DIVIDER         130U
#define HD15515X_LAST_LOW_BAND                  1U
#define HD15515X_TRANSMIT_OFFSET_HZ             80000000U
#define HD15515X_TRANSMIT_OFFSET_82MHZ_HZ       82000000U
#define HD15515X_PGA_STEP_DB                    2
#define HD15515X_FRONT_END_LOW_BAND             BIT(0)
#define HD15515X_FRONT_END_HIGH_BAND            BIT(1)
#define HD15515X_FAKE_SIGNAL_LEVEL_DBM          (-60)
#define HD15515X_ADC_AMPLITUDE_OFFSET_DB        60
#define HD15515X_ADC_MAX_AMPLITUDE              16000

#define HD155153NP_RECEIVER_BASE_GAIN_DB        25
#define HD155153NP_MIXER1_LOW_GAIN              BIT(1)
#define HD155153NP_MIXER2_LOW_GAIN              BIT(0)
#define HD155153NP_MIXER1_LOW_GAIN_REDUCTION_DB 36
#define HD155153NP_MIXER2_LOW_GAIN_REDUCTION_DB 17

#define HD155155NPEB_APPROX_BASE_GAIN_DB        23
#define HD155155NPEB_PGA_OFFSET_DB              (-36)
#define HD155155NPEB_FRONT_END_LOW_BAND_DB      38
#define HD155155NPEB_FRONT_END_HIGH_BAND_DB     29

struct HD15515XState {
    SysBusDevice parent_obj;
    DebugChannel dbg;
    ThreeWireSerialTargetPort serial_port;
    IQSourcePort iq_port;
    GSMAir air;
    uint32_t telegram;
    uint32_t frequency;
    uint32_t transmit_offset;
    uint32_t band;
    int32_t amplitude;
    uint64_t shift_register;
    uint8_t telegram_bits;
    uint8_t shift_bits;
    bool strobe_asserted;
    bool shift_overflow;
    bool transmitting;
};

struct HD15515XClass {
    SysBusDeviceClass parent_class;
    int32_t (*receiver_gain_db)(HD15515XState *p, uint32_t pga_gain, uint32_t front_end);
};

static void hd15515x_update_air(HD15515XState *p) {
    if (p->frequency == 0) {
        gsm_air_tune(&p->air, GSM_AIR_PATH_OFF, 0, 0);
    } else if (p->transmitting) {
        gsm_air_tune(&p->air, GSM_AIR_PATH_TRANSMIT, p->frequency - p->transmit_offset, 0);
    } else {
        gsm_air_tune(&p->air, GSM_AIR_PATH_RECEIVE, p->frequency, p->amplitude);
    }
}

static int32_t hd155153np_receiver_gain_db(HD15515XState *_p, uint32_t pga_gain, uint32_t front_end) {
    int32_t gain_db = HD155153NP_RECEIVER_BASE_GAIN_DB + (int32_t) pga_gain * HD15515X_PGA_STEP_DB;

    if ((front_end & HD155153NP_MIXER1_LOW_GAIN) != 0)
        gain_db -= HD155153NP_MIXER1_LOW_GAIN_REDUCTION_DB;
    if ((front_end & HD155153NP_MIXER2_LOW_GAIN) != 0)
        gain_db -= HD155153NP_MIXER2_LOW_GAIN_REDUCTION_DB;
    return gain_db;
}

static int32_t hd155155npeb_receiver_gain_db(HD15515XState *p, uint32_t pga_gain, uint32_t front_end) {
    bool low_band = p->band <= HD15515X_LAST_LOW_BAND;
    uint32_t band_stage = low_band ? HD15515X_FRONT_END_LOW_BAND : HD15515X_FRONT_END_HIGH_BAND;
    int32_t gain_db = HD155155NPEB_APPROX_BASE_GAIN_DB + HD155155NPEB_PGA_OFFSET_DB +
        (int32_t) pga_gain * HD15515X_PGA_STEP_DB;

    if ((front_end & band_stage) == 0)
        return gain_db;
    if (low_band)
        return gain_db + HD155155NPEB_FRONT_END_LOW_BAND_DB;
    return gain_db + HD155155NPEB_FRONT_END_HIGH_BAND_DB;
}

static void hd15515x_set_gain(HD15515XState *p, uint32_t value) {
    HD15515XClass *klass = HD15515X_GET_CLASS(p);
    uint32_t pga_gain = (value & HD15515X_GAIN_CONTROL_PGA_GAIN) >> HD15515X_GAIN_CONTROL_PGA_GAIN_SHIFT;
    uint32_t front_end = (value & HD15515X_GAIN_CONTROL_FRONT_END) >> HD15515X_GAIN_CONTROL_FRONT_END_SHIFT;
    int32_t gain_db = klass->receiver_gain_db(p, pga_gain, front_end);

    int32_t adc_level_db = HD15515X_FAKE_SIGNAL_LEVEL_DBM + HD15515X_ADC_AMPLITUDE_OFFSET_DB + gain_db;
    int32_t amplitude = (int32_t) lround(pow(10.0, adc_level_db / 20.0));

    p->amplitude = MIN(amplitude, HD15515X_ADC_MAX_AMPLITUDE);
    hd15515x_update_air(p);
}

static void hd15515x_set_channel(HD15515XState *p, uint32_t value) {
    uint32_t band = (value & HD15515X_CHANNEL_PLL_BAND) >> HD15515X_CHANNEL_PLL_BAND_SHIFT;
    uint32_t divider = (value & HD15515X_CHANNEL_PLL_DIVIDER) >> HD15515X_CHANNEL_PLL_DIVIDER_SHIFT;
    uint32_t mode = (value & HD15515X_CHANNEL_PLL_MODE) >> HD15515X_CHANNEL_PLL_MODE_SHIFT;

    if (band <= HD15515X_LAST_LOW_BAND)
        divider >>= 1;

    p->band = band;
    if (divider == 0) {
        p->frequency = 0;
        hd15515x_update_air(p);
        return;
    }

    uint32_t channel_raster = HD15515X_REFERENCE_FREQUENCY / HD15515X_CHANNEL_RASTER_DIVIDER;
    bool offset_82mhz = (value & HD15515X_CHANNEL_PLL_TX_OFFSET) == HD15515X_CHANNEL_PLL_TX_OFFSET_OFFSET_82MHZ;

    p->transmitting = (value & HD15515X_CHANNEL_PLL_RECEIVE) == 0;
    p->transmit_offset = offset_82mhz ? HD15515X_TRANSMIT_OFFSET_82MHZ_HZ : HD15515X_TRANSMIT_OFFSET_HZ;
    p->frequency = divider * channel_raster;
    hd15515x_update_air(p);
    DEBUG_LOG(&p->dbg, "tuned: frequency=%u Hz band=%u mode=%u\n", p->frequency, band, mode);
}

static void hd15515x_write_telegram(HD15515XState *p, uint32_t telegram, uint8_t bits) {
    uint8_t address = (telegram & HD15515X_REGISTER_ADDRESS_MASK) >> HD15515X_REGISTER_ADDRESS_SHIFT;
    uint32_t value = telegram >> HD15515X_REGISTER_VALUE_SHIFT;

    p->telegram = telegram;
    p->telegram_bits = bits;

    switch (address) {
        case HD15515X_CHANNEL_PLL:
            hd15515x_set_channel(p, value);
            break;

        case HD15515X_GAIN_CONTROL:
            hd15515x_set_gain(p, value);
            break;
    }

    DEBUG_IO_WRITE(&p->dbg, address, bits >> HD15515X_BITS_TO_BYTES_SHIFT, value);
}

static void hd15515x_set_strobe(ThreeWireSerialTarget *target, bool asserted) {
    HD15515XState *p = HD15515X(target);

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
        hd15515x_write_telegram(p, p->shift_register & HD15515X_TELEGRAM_MASK, p->shift_bits);
}

static void hd15515x_write(ThreeWireSerialTarget *target, uint32_t value, unsigned int bits) {
    HD15515XState *p = HD15515X(target);

    if (!p->strobe_asserted || p->shift_overflow)
        return;
    if (bits > HD15515X_TELEGRAM_BITS - p->shift_bits) {
        p->shift_overflow = true;
        return;
    }

    p->shift_register = (p->shift_register << bits) | (value & MAKE_64BIT_MASK(0, bits));
    p->shift_bits += bits;
}

static IQSample hd15515x_read_iq(IQSource *source, int64_t timestamp, uint64_t _sample_index) {
    return gsm_air_read_iq(&HD15515X(source)->air, timestamp);
}

static void hd15515x_transmit(IQSource *source, int64_t timestamp, bool symbol) {
    gsm_air_transmit(&HD15515X(source)->air, timestamp, symbol);
}

static void hd15515x_transmit_until(IQSource *source, int64_t timestamp) {
    gsm_air_transmit_until(&HD15515X(source)->air, timestamp);
}

static void hd15515x_reset(DeviceState *device) {
    HD15515XState *p = HD15515X(device);

    p->telegram = 0;
    p->frequency = 0;
    p->transmit_offset = HD15515X_TRANSMIT_OFFSET_HZ;
    p->band = 0;
    p->amplitude = 0;
    p->shift_register = 0;
    p->telegram_bits = 0;
    p->shift_bits = 0;
    p->strobe_asserted = false;
    p->shift_overflow = false;
    p->transmitting = false;
    gsm_air_reset(&p->air);
}

static void hd15515x_init(Object *obj) {
    HD15515XState *p = HD15515X(obj);

    three_wire_serial_target_port_init(&p->serial_port, DEVICE(obj), "RFSSC", THREE_WIRE_SERIAL_TARGET(obj));
    iq_source_port_init(&p->iq_port, DEVICE(obj), "IQ_TX", IQ_SOURCE(obj));
    gsm_air_init_uplink(&p->air);
}

static void hd15515x_finalize(Object *obj) {
    HD15515XState *p = HD15515X(obj);
    gsm_air_cleanup_uplink(&p->air);
}

static void hd15515x_realize(DeviceState *dev, Error **_errp) {
    HD15515XState *p = HD15515X(dev);
    debug_channel_init_device_regs(&p->dbg, dev, &hd15515x_debug_regs);
}

static const Property hd15515x_properties[] = {
    DEFINE_PROP_LINK("gsm-network", HD15515XState, air.network, TYPE_GSM_NETWORK, GSMNetwork *),
};

static void hd15515x_class_init(ObjectClass *klass, const void *_data) {
    DeviceClass *device_class = DEVICE_CLASS(klass);
    ThreeWireSerialTargetClass *serial_class = THREE_WIRE_SERIAL_TARGET_CLASS(klass);
    IQSourceClass *iq_class = IQ_SOURCE_CLASS(klass);

    serial_class->set_strobe = hd15515x_set_strobe;
    serial_class->write = hd15515x_write;
    iq_class->read = hd15515x_read_iq;
    iq_class->transmit = hd15515x_transmit;
    iq_class->transmit_until = hd15515x_transmit_until;
    device_class_set_legacy_reset(device_class, hd15515x_reset);
    device_class->realize = hd15515x_realize;
    device_class_set_props(device_class, hd15515x_properties);
}

static void hd155153np_class_init(ObjectClass *klass, const void *_data) {
    HD15515X_CLASS(klass)->receiver_gain_db = hd155153np_receiver_gain_db;
}

static void hd155155npeb_class_init(ObjectClass *klass, const void *_data) {
    HD15515X_CLASS(klass)->receiver_gain_db = hd155155npeb_receiver_gain_db;
}

static const TypeInfo hd15515x_types[] = {
    {
        .name = TYPE_HD15515X,
        .parent = TYPE_SYS_BUS_DEVICE,
        .abstract = true,
        .instance_size = sizeof(HD15515XState),
        .instance_init = hd15515x_init,
        .instance_finalize = hd15515x_finalize,
        .class_size = sizeof(HD15515XClass),
        .class_init = hd15515x_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_THREE_WIRE_SERIAL_TARGET },
            { TYPE_IQ_SOURCE },
            { NULL },
        },
    },
    {
        .name = TYPE_HD155153NP,
        .parent = TYPE_HD15515X,
        .class_init = hd155153np_class_init,
    },
    {
        .name = TYPE_HD155155NPEB,
        .parent = TYPE_HD15515X,
        .class_init = hd155155npeb_class_init,
    },
};

DEFINE_TYPES(hd15515x_types)
