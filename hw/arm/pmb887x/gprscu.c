#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/gsm/gea.h"
#include "hw/gsm/kasumi.h"

#define TYPE_PMB887X_GPRSCU "pmb887x-gprscu"
#define PMB887X_GPRSCU(obj) OBJECT_CHECK(pmb887x_gprscu_t, (obj), TYPE_PMB887X_GPRSCU)

#define GPRSCU_FIFO_SIZE 32
#define GPRSCU_SEGMENT_LIST_SIZE 32
#define GPRSCU_ID_BASE 0xF003C000
#define GPRSCU_SEGMENT_SRC 1
#define GPRSCU_LLC_FCS_POLYNOMIAL 0xAD85DD
#define GPRSCU_CRC_MASK 0xFFFFFF
#define GPRSCU_FCS_CRC_SHIFT 8
#define GPRSCU_FCS_BYTE_MASK 0xFF

#define GPRSCU_CON_CONFIGURATION (GPRSCU_CON_DIRECTION | GPRSCU_CON_CRC_CTRL | GPRSCU_CON_CIPH_CTRL | \
	GPRSCU_CON_GEA2 | GPRSCU_CON_SEGMENT_MODE | GPRSCU_CON_GEA3)
#define GPRSCU_CON_COMMANDS (GPRSCU_CON_INIT | GPRSCU_CON_SEGMENT_START | GPRSCU_CON_SEGMENT_RESET)
#define GPRSCU_SEGMENT_CONFIGURATION (GPRSCU_SEGMENT_LENGTH | GPRSCU_SEGMENT_CRC_CTRL | GPRSCU_SEGMENT_CIPH_CTRL)

typedef struct pmb887x_gprscu_t pmb887x_gprscu_t;

struct pmb887x_gprscu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	DebugChannel dbg;
	uint32_t revision;

	qemu_irq irq[2];
	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src[2];

	uint32_t con;
	uint32_t segment;
	uint32_t input[2];
	uint32_t key[4];
	uint32_t crc;
	uint8_t fcs_byte;
	uint32_t polynom;

	uint32_t segments[GPRSCU_SEGMENT_LIST_SIZE];
	size_t segment_count;
	size_t segment_index;
	uint32_t segment_received;
	uint32_t segment_output_count;
	bool segment_start_pending;

	uint32_t fifo[GPRSCU_FIFO_SIZE];
	size_t fifo_head;
	size_t fifo_count;
	uint32_t data_latch;
	bool output_underrun;

	bool gea3;
	GEAKeystream gea;
	KGCore kgcore;
};

static void gprscu_update_crc(pmb887x_gprscu_t *p, uint8_t value) {
	p->crc ^= value;
	for (size_t bit = 0; bit < 8; bit++)
		p->crc = (p->crc >> 1) ^ ((p->crc & 1) != 0 ? GPRSCU_LLC_FCS_POLYNOMIAL : 0);
}

static void gprscu_update_fcs(pmb887x_gprscu_t *p, uint8_t value) {
	gprscu_update_crc(p, value);
	p->fcs_byte = value;
}

static void gprscu_start_keystream(pmb887x_gprscu_t *p) {
	p->gea3 = (p->con & GPRSCU_CON_GEA3) != 0;
	if (p->gea3) {
		uint8_t key[KASUMI_KEY_BYTES];

		for (size_t i = 0; i < KASUMI_KEY_BYTES; i++)
			key[i] = p->key[ARRAY_SIZE(p->key) - 1 - i / 4] >> (24 - 8 * (i % 4));
		kgcore_start(&p->kgcore, ((uint64_t) p->input[0] << 32) | p->input[1], key);
	} else {
		GEAAlgorithm algorithm = (p->con & GPRSCU_CON_GEA2) != 0 ? GEA_ALGORITHM_GEA2 : GEA_ALGORITHM_GEA1;
		uint64_t kc = bswap64(((uint64_t) p->key[1] << 32) | p->key[0]);

		gea_start(&p->gea, algorithm, p->input[0], (p->con & GPRSCU_CON_DIRECTION) != 0, kc);
	}
}

static uint8_t gprscu_process_byte(pmb887x_gprscu_t *p, uint8_t input, bool crc, bool cipher) {
	uint8_t output = input;

	if (cipher)
		output ^= p->gea3 ? kgcore_next_byte(&p->kgcore) : gea_next_byte(&p->gea);
	if (crc)
		gprscu_update_fcs(p, (p->con & GPRSCU_CON_DIRECTION) != 0 ? output : input);
	return output;
}

static uint8_t gprscu_receive_segment_byte(pmb887x_gprscu_t *p, uint8_t value) {
	uint32_t segment = p->segments[p->segment_index];
	uint8_t output = gprscu_process_byte(p, value, (segment & GPRSCU_SEGMENT_CRC_CTRL) != 0,
		(segment & GPRSCU_SEGMENT_CIPH_CTRL) != 0);

	p->segment_received++;
	if (p->segment_received != (segment & GPRSCU_SEGMENT_LENGTH))
		return output;

	if (p->segment_index + 1 < p->segment_count) {
		p->segment_index++;
		p->segment_received = 0;
	} else if (p->segment_start_pending) {
		p->segment_start_pending = false;
		pmb887x_src_update(&p->src[GPRSCU_SEGMENT_SRC], 0, MOD_SRC_SETR);
	}
	return output;
}

static void gprscu_start_segment(pmb887x_gprscu_t *p) {
	p->con &= ~GPRSCU_CON_SEGMENT_START;
	if (p->fifo_count != 0) {
		pmb887x_src_update(&p->src[GPRSCU_SEGMENT_SRC], 0, MOD_SRC_SETR);
	} else {
		p->segment_start_pending = true;
	}
}

static void gprscu_reset_segments(pmb887x_gprscu_t *p) {
	bool active = false;
	uint32_t active_segment = 0;
	if (p->segment_count != 0) {
		active_segment = p->segments[p->segment_index];
		active = p->segment_received != 0 && p->segment_received < (active_segment & GPRSCU_SEGMENT_LENGTH);
	}

	p->segment = 0;
	p->segment_index = 0;
	p->segment_output_count = 0;
	if (active) {
		p->segments[0] = active_segment;
		p->segment_count = 1;
	} else {
		p->segment_count = 0;
		p->segment_received = 0;
	}
	p->con &= ~GPRSCU_CON_SEGMENT_RESET;
}

static uint32_t gprscu_status(pmb887x_gprscu_t *p) {
	uint32_t value = p->fifo_count << GPRSCU_STAT_OUTPUT_COUNT_SHIFT;
	value |= (GPRSCU_FIFO_SIZE - p->fifo_count) << GPRSCU_STAT_INPUT_FREE_SHIFT;
	if (p->output_underrun)
		value |= GPRSCU_STAT_OUTPUT_UNDERRUN;
	return value;
}

static uint64_t gprscu_read_data(pmb887x_gprscu_t *p, unsigned int size) {
	if (p->fifo_count == 0) {
		p->output_underrun = true;
		return p->data_latch & MAKE_64BIT_MASK(0, size * 8);
	}

	p->data_latch = p->fifo[p->fifo_head];
	p->fifo_head = (p->fifo_head + 1) % GPRSCU_FIFO_SIZE;
	p->fifo_count--;

	if ((p->con & GPRSCU_CON_SEGMENT_MODE) != 0 && p->segment_count != 0) {
		p->segment_output_count++;
		if (p->segment_output_count == GPRSCU_FIFO_SIZE) {
			p->segment_output_count = 0;
			pmb887x_src_update(&p->src[GPRSCU_SEGMENT_SRC], 0, MOD_SRC_SETR);
		}
	}
	return p->data_latch & MAKE_64BIT_MASK(0, size * 8);
}

static void gprscu_write_data(pmb887x_gprscu_t *p, uint64_t value, unsigned int size) {
	if (p->fifo_count == GPRSCU_FIFO_SIZE)
		return;

	bool control_mode = (p->con & GPRSCU_CON_SEGMENT_MODE) == 0;
	bool control_crc = control_mode && (p->con & GPRSCU_CON_CRC_CTRL) != 0;
	bool control_cipher = control_mode && (p->con & GPRSCU_CON_CIPH_CTRL) != 0;
	uint32_t output = 0;

	for (unsigned int index = 0; index < size; index++) {
		uint8_t byte = value >> (index * 8);

		if (p->segment_index < p->segment_count) {
			byte = gprscu_receive_segment_byte(p, byte);
		} else {
			byte = gprscu_process_byte(p, byte, control_crc, control_cipher);
		}
		output |= (uint32_t) byte << (index * 8);
	}

	p->fifo[(p->fifo_head + p->fifo_count) % GPRSCU_FIFO_SIZE] = output;
	p->fifo_count++;
}

static void gprscu_initialize(pmb887x_gprscu_t *p) {
	p->segment_index = 0;
	p->segment_received = 0;
	p->segment_output_count = 0;
	gprscu_start_keystream(p);
	p->con &= ~(GPRSCU_CON_INIT | GPRSCU_CON_BUSY);
}

static uint64_t gprscu_io_read(void *opaque, hwaddr haddr, unsigned int size) {
	pmb887x_gprscu_t *p = opaque;
	uint64_t value = 0;

	switch (haddr) {
		case GPRSCU_CLC:
			value = pmb887x_clc_get(&p->clc);
			break;

		case GPRSCU_ID:
			value = GPRSCU_ID_BASE | p->revision;
			break;

		case GPRSCU_CON:
			value = p->con;
			break;

		case GPRSCU_DATA:
			value = gprscu_read_data(p, size);
			break;

		case GPRSCU_STAT:
			value = gprscu_status(p);
			break;

		case GPRSCU_SEGMENT:
			value = p->segment;
			break;

		case GPRSCU_INPUT0 ... GPRSCU_INPUT1:
			value = p->input[(haddr - GPRSCU_INPUT0) / sizeof(uint32_t)];
			break;

		case GPRSCU_KEY0 ... GPRSCU_KEY3:
			value = p->key[(haddr - GPRSCU_KEY0) / sizeof(uint32_t)];
			break;

		case GPRSCU_FCS:
			value = (p->crc << GPRSCU_FCS_CRC_SHIFT) | p->fcs_byte;
			break;

		case GPRSCU_POLYNOM:
			value = p->polynom;
			break;

		case GPRSCU_SRC0 ... GPRSCU_SRC1:
			value = pmb887x_src_get(&p->src[(haddr - GPRSCU_SRC0) / sizeof(uint32_t)]);
			break;

		default:
			DEBUG_GUEST_ERROR(&p->dbg, "unknown reg read: %02"PRIX64"\n", haddr);
			break;
	}

	return value;
}

static void gprscu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned int size) {
	pmb887x_gprscu_t *p = opaque;

	switch (haddr) {
		case GPRSCU_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case GPRSCU_CON:
			p->con = value & (GPRSCU_CON_CONFIGURATION | GPRSCU_CON_COMMANDS);
			if ((p->con & GPRSCU_CON_INIT) != 0)
				gprscu_initialize(p);
			if ((p->con & GPRSCU_CON_SEGMENT_RESET) != 0)
				gprscu_reset_segments(p);
			if ((p->con & GPRSCU_CON_SEGMENT_START) != 0)
				gprscu_start_segment(p);
			break;

		case GPRSCU_DATA:
			gprscu_write_data(p, value, size);
			break;

		case GPRSCU_STAT:
			if ((value & GPRSCU_STAT_OUTPUT_UNDERRUN) != 0)
				p->output_underrun = false;
			break;

		case GPRSCU_SEGMENT:
			p->segment = value & GPRSCU_SEGMENT_CONFIGURATION;
			if (p->segment_count < GPRSCU_SEGMENT_LIST_SIZE)
				p->segments[p->segment_count++] = p->segment;
			break;

		case GPRSCU_INPUT0 ... GPRSCU_INPUT1:
			p->input[(haddr - GPRSCU_INPUT0) / sizeof(uint32_t)] = value;
			break;

		case GPRSCU_KEY0 ... GPRSCU_KEY3:
			p->key[(haddr - GPRSCU_KEY0) / sizeof(uint32_t)] = value;
			break;

		case GPRSCU_FCS:
			p->crc = (value >> GPRSCU_FCS_CRC_SHIFT) & GPRSCU_CRC_MASK;
			p->fcs_byte = value & GPRSCU_FCS_BYTE_MASK;
			break;

		case GPRSCU_POLYNOM:
			p->polynom = value;
			break;

		case GPRSCU_SRC0 ... GPRSCU_SRC1:
			pmb887x_src_set(&p->src[(haddr - GPRSCU_SRC0) / sizeof(uint32_t)], value);
			break;

		default:
			DEBUG_GUEST_ERROR(&p->dbg, "unknown reg write: %02"PRIX64" = %08"PRIX64"\n", haddr, value);
			break;
	}
}

static const MemoryRegionOps gprscu_io_ops = {
	.read = gprscu_io_read,
	.write = gprscu_io_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 1,
		.max_access_size = 4,
	},
};

static void gprscu_init(Object *obj) {
	pmb887x_gprscu_t *p = PMB887X_GPRSCU(obj);
	pmb887x_clc_init(&p->clc, DEVICE(obj));

	memory_region_init_io(&p->mmio, obj, &gprscu_io_ops, p, TYPE_PMB887X_GPRSCU, GPRSCU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	for (size_t index = 0; index < ARRAY_SIZE(p->irq); index++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[index]);
}

static void gprscu_reset(DeviceState *dev) {
	pmb887x_gprscu_t *p = PMB887X_GPRSCU(dev);

	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
	for (size_t index = 0; index < ARRAY_SIZE(p->src); index++)
		pmb887x_src_reset(&p->src[index]);

	p->con = 0;
	p->segment = 0;
	memset(p->input, 0, sizeof(p->input));
	memset(p->key, 0, sizeof(p->key));
	p->crc = 0;
	p->fcs_byte = 0;
	p->polynom = 0;
	memset(p->segments, 0, sizeof(p->segments));
	p->segment_count = 0;
	p->segment_index = 0;
	p->segment_received = 0;
	p->segment_output_count = 0;
	p->segment_start_pending = false;
	p->fifo_head = 0;
	p->fifo_count = 0;
	p->data_latch = 0;
	p->output_underrun = false;
}

static void gprscu_realize(DeviceState *dev, Error **errp) {
	pmb887x_gprscu_t *p = PMB887X_GPRSCU(dev);
	pmb887x_mod_debug_init(&p->dbg, dev, &p->mmio, &pmb887x_gprscu_regs);

	for (size_t index = 0; index < ARRAY_SIZE(p->src); index++)
		pmb887x_src_init(&p->src[index], p->irq[index]);
	gprscu_reset(dev);
}

static const Property gprscu_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_gprscu_t, revision, 0),
};

static void gprscu_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);

	device_class_set_props(dc, gprscu_properties);
	device_class_set_legacy_reset(dc, gprscu_reset);
	dc->realize = gprscu_realize;
}

static const TypeInfo gprscu_info = {
	.name = TYPE_PMB887X_GPRSCU,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(pmb887x_gprscu_t),
	.instance_init = gprscu_init,
	.class_init = gprscu_class_init,
};

static void gprscu_register_types(void) {
	type_register_static(&gprscu_info);
}

type_init(gprscu_register_types)
