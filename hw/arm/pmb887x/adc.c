/*
 * ADC
 * */
#define PMB887X_TRACE_ID		ADC
#define PMB887X_TRACE_PREFIX	"pmb887x-adc"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/ptimer.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"

#include "hw/arm/pmb887x/adc.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_ADC	"pmb887x-adc"
#define PMB887X_ADC(obj)	OBJECT_CHECK(pmb887x_adc_t, (obj), TYPE_PMB887X_ADC)

#define CPU_VCC_VOLTAGE		2650
#define ADC_REF_VOLTAGE		1000
#define ADC_MAX_VALUE		0x7FF
#define ADC_ZERO_VALUE		0x800

// Virtual ADC channels
enum {
	ADC_CH_M0			= 0x01,
	ADC_CH_M1			= 0x02,
	ADC_CH_M2			= 0x03,
	ADC_CH_M7			= 0x08,
	ADC_CH_M8			= 0x09,
	ADC_CH_M9			= 0x10,
	ADC_CH_M10			= 0x11,
	ADC_CH_M0_M9_A		= 0x0C,
	ADC_CH_M0_M9_B		= 0x18,
};

enum {
	ADC_MEASURE_MODE_NONE,
	ADC_MEASURE_MODE_SIGNLE,
	ADC_MEASURE_MODE_TRIG,
	ADC_MEASURE_MODE_CONTINUOUS,
};

enum {
	ADC_CH_TYPE_NORMAL,
	ADC_CH_TYPE_DIFFERENTIAL,
};

typedef struct {
	const char *name;
	uint8_t gain; // 100 - 1.0
	int8_t polarity_a;
	int8_t polarity_b;
	uint8_t type;
	uint8_t input_a;
	uint8_t input_b;
} pmb887x_adc_ch_cfg_t;

typedef struct {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	int measure_mode;
	
	qemu_irq irq[2];
	
	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src[2];
	
	uint32_t pllcon;
	uint32_t con0;
	uint32_t con1;
	
	pmb887x_adc_input_t inputs[PMB887X_ADC_MAX_INPUTS];
	struct pmb887x_pll_t *pll;
} pmb887x_adc_t;

// Known channels with known parameters
static const pmb887x_adc_ch_cfg_t channels_cfg[0xFF] = {
	[ADC_CH_M0]			= { "+M0",			50,		1,	1,		ADC_CH_TYPE_NORMAL,			PMB887X_ADC_INPUT_M0,	PMB887X_ADC_INPUT_M0 },
	[ADC_CH_M1]			= { "+M1",			44,		1,	1,		ADC_CH_TYPE_NORMAL,			PMB887X_ADC_INPUT_M1,	PMB887X_ADC_INPUT_M1 },
	[ADC_CH_M2]			= { "+M2",			50,		1,	1,		ADC_CH_TYPE_NORMAL,			PMB887X_ADC_INPUT_M2,	PMB887X_ADC_INPUT_M2 },
	[ADC_CH_M7]			= { "-M7",			100,	-1,	-1,		ADC_CH_TYPE_NORMAL,			PMB887X_ADC_INPUT_M7,	PMB887X_ADC_INPUT_M7 },
	[ADC_CH_M8]			= { "-M8",			50,		-1,	-1,		ADC_CH_TYPE_NORMAL,			PMB887X_ADC_INPUT_M8,	PMB887X_ADC_INPUT_M8 },
	[ADC_CH_M9]			= { "-M9",			50,		-1,	-1,		ADC_CH_TYPE_NORMAL,			PMB887X_ADC_INPUT_M9,	PMB887X_ADC_INPUT_M9 },
	[ADC_CH_M10]		= { "-M10",			50,		1,	1,		ADC_CH_TYPE_NORMAL,			PMB887X_ADC_INPUT_M10,	PMB887X_ADC_INPUT_M10 },
	[ADC_CH_M0_M9_A]	= { "+M0,-M9(A)",	50,		1,	-1,		ADC_CH_TYPE_DIFFERENTIAL,	PMB887X_ADC_INPUT_M0,	PMB887X_ADC_INPUT_M9 },
	[ADC_CH_M0_M9_B]	= { "+M0,-M9(B)",	100,	1,	-1,		ADC_CH_TYPE_DIFFERENTIAL,	PMB887X_ADC_INPUT_M0,	PMB887X_ADC_INPUT_M9 },
};

static int32_t adc_get_input_voltage(pmb887x_adc_t *p, uint8_t input_n, uint8_t current) {
	const pmb887x_adc_input_t *input = &p->inputs[input_n];
	
	switch (input->type) {
		case PMB887X_ADC_INPUT_RESISTOR_DIV:
			// INPUT -[ R1 ]- ADC_INPUT -[ R2 ]- GND
			return DIV_ROUND_UP(input->value * input->r2, (input->r1 + input->r2));
		break;
		
		case PMB887X_ADC_INPUT_RESISTOR:
			// ADC_INPUT -[ R1 ]- GND
			return current ? DIV_ROUND_UP(current * input->r1, 1000) : 0;
		break;
		
		case PMB887X_ADC_INPUT_VOLTAGE:
			return input->value;
		break;
		
		case PMB887X_ADC_INPUT_NONE:
			// Not connected
		break;
		
		default:
			EPRINTF("Unknown adc input type: %d\n", input->type);
		break;
	}
	
	return current > 0 ? CPU_VCC_VOLTAGE : 0;
}

static uint16_t adc_get_ch_current(pmb887x_adc_t *p) {
	switch ((p->con1 & ADC_CON1_MODE)) {
		case ADC_CON1_MODE_I_30:		return 30;
		case ADC_CON1_MODE_I_60:		return 60;
		case ADC_CON1_MODE_I_90:		return 90;
		case ADC_CON1_MODE_I_120:		return 120;
		case ADC_CON1_MODE_I_150:		return 150;
		case ADC_CON1_MODE_I_180:		return 180;
		case ADC_CON1_MODE_I_210:		return 210;
	}
	return 0;
}

static uint16_t adc_read_channel(pmb887x_adc_t *p) {
	uint8_t ch = (p->con1 & ADC_CON1_CH) >> ADC_CON1_CH_SHIFT;
	uint16_t current_a = adc_get_ch_current(p);
	uint16_t current_b = adc_get_ch_current(p);
	const pmb887x_adc_ch_cfg_t *cfg = &channels_cfg[ch];
	
	// On real hardware M9 has only 60 uA mode...
	if (current_a > 0 && cfg->input_a == PMB887X_ADC_INPUT_M9)
		current_a = 60;
	if (current_b > 0 && cfg->input_b == PMB887X_ADC_INPUT_M9)
		current_b = 60;
	
	int32_t input;
	if (cfg->type == ADC_CH_TYPE_DIFFERENTIAL) {
		int32_t input_a = adc_get_input_voltage(p, cfg->input_a, current_a) * cfg->polarity_a;
		int32_t input_b = adc_get_input_voltage(p, cfg->input_b, current_b) * cfg->polarity_b;
		input = input_a + input_b;
	} else {
		input = adc_get_input_voltage(p, cfg->input_a, current_a) * cfg->polarity_a;
	}
	
	// Apply preamp gain
	if (cfg->gain != 100)
		input = DIV_ROUND_UP(input * cfg->gain, 100);
	
	// Clip value to [-1V, 1V]
	input = MAX(-ADC_REF_VOLTAGE, MIN(ADC_REF_VOLTAGE, input));
	
	// Invert signal polarity
	input = input * (p->con1 & ADC_CON1_PREAMP_INV ? -1 : 1);
	
	// [-1V, 1V] -> [0V, 2V] -> [0, 0xFFF]
	uint16_t adc_value = DIV_ROUND_UP(((input + ADC_REF_VOLTAGE) * 0xFFF), (ADC_REF_VOLTAGE * 2));
	
	if (current_a > 0) {
		if (cfg->type == ADC_CH_TYPE_DIFFERENTIAL) {
			DPRINTF("ADC %02X (%s): input=%d mV, value=%03X%s (%duA, %duA)\n", ch, cfg->name, input, adc_value, (p->con1 & ADC_CON1_PREAMP_INV ? " [INV]" : ""), current_a, current_b);
		} else {
			DPRINTF("ADC %02X (%s): input=%d mV, value=%03X%s (%duA)\n", ch, cfg->name, input, adc_value, (p->con1 & ADC_CON1_PREAMP_INV ? " [INV]" : ""), current_a);
		}
	} else {
		DPRINTF("ADC %02X (%s): input=%d mV, value=%03X%s (voltage)\n", ch, cfg->name, input, adc_value, (p->con1 & ADC_CON1_PREAMP_INV ? " [INV]" : ""));
	}
	
	return adc_value;
}

static void adc_update_state(pmb887x_adc_t *p) {
	uint32_t div = pmb887x_clc_get_rmc(&p->clc);
	uint32_t fadc = div > 0 ? pmb887x_pll_get_fsys(p->pll) / div : 0;
	bool is_enabled = fadc > 0 && pmb887x_clc_is_enabled(&p->clc);
	
	if ((p->con1 & ADC_CON1_TRIG)) {
		p->measure_mode = ADC_MEASURE_MODE_TRIG;
	} else if ((p->con1 & ADC_CON1_SINGLE)) {
		p->measure_mode = ADC_MEASURE_MODE_SIGNLE;
	} else if ((p->con1 & ADC_CON1_FREQ)) {
		p->measure_mode = ADC_MEASURE_MODE_CONTINUOUS;
	} else {
		p->measure_mode = ADC_MEASURE_MODE_NONE;
	}
	
	DPRINTF("is_enabled=%d, fADC=%d, measure_mode=%d, start=%d\n", is_enabled, fadc, p->measure_mode, p->con1 & ADC_CON1_START ? 1 : 0);
}

static uint64_t adc_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_adc_t *p = (pmb887x_adc_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case ADC_CLC:
			value = pmb887x_clc_get(&p->clc);
		break;
		
		case ADC_ID:
			value = 0x0000C011;
		break;
		
		case ADC_STAT:
			value = ADC_STAT_READY;
		break;
		
		case ADC_FIFO0 ... ADC_FIFO7:
			value = adc_read_channel(p);
		break;
		
		case ADC_PLLCON:
			value = p->pllcon;
		break;
		
		case ADC_CON0:
			value = p->con0;
		break;
		
		case ADC_CON1:
			value = p->con1;
		break;
		
		case ADC_SRC0:
			value = pmb887x_src_get(&p->src[0]);
		break;
		
		case ADC_SRC1:
			value = pmb887x_src_get(&p->src[1]);
		break;
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
		break;
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void adc_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_adc_t *p = (pmb887x_adc_t *) opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case ADC_CLC:
			pmb887x_clc_set(&p->clc, value);
		break;
		
		case ADC_PLLCON:
			p->pllcon = value;
		break;
		
		case ADC_CON0:
			p->con0 = value;
		break;
		
		case ADC_CON1:
			p->con1 = value;
		break;
		
		case ADC_SRC0:
			pmb887x_src_set(&p->src[0], value);
		break;
		
		case ADC_SRC1:
			pmb887x_src_set(&p->src[1], value);
		break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
		break;
	}
	
	adc_update_state(p);
}

void pmb887x_adc_set_input(DeviceState *dev, uint32_t n, const pmb887x_adc_input_t *input) {
	pmb887x_adc_t *p = PMB887X_ADC(dev);
	memcpy(&p->inputs[n], input, sizeof(pmb887x_adc_input_t));
}

static const MemoryRegionOps io_ops = {
	.read			= adc_io_read,
	.write			= adc_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void adc_init(Object *obj) {
	pmb887x_adc_t *p = PMB887X_ADC(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-adc", ADC_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void adc_realize(DeviceState *dev, Error **errp) {
	pmb887x_adc_t *p = PMB887X_ADC(dev);
	
	pmb887x_clc_init(&p->clc);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->irq); i++)
		pmb887x_src_init(&p->src[i], p->irq[i]);
	
	adc_update_state(p);
}

static const Property adc_properties[] = {
	DEFINE_PROP_LINK("pll", pmb887x_adc_t, pll, "pmb887x-pll", struct pmb887x_pll_t *),
};

static void adc_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, adc_properties);
	dc->realize = adc_realize;
}

static const TypeInfo adc_info = {
    .name          	= TYPE_PMB887X_ADC,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_adc_t),
    .instance_init 	= adc_init,
    .class_init    	= adc_class_init,
};

static void adc_register_types(void) {
	type_register_static(&adc_info);
}
type_init(adc_register_types)
