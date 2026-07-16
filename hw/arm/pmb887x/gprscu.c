/*
 * GPRS Ciphering Unit
 */
#define PMB887X_TRACE_ID GPRSCU
#define PMB887X_TRACE_PREFIX "pmb887x-gprscu"

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"

#include "hw/arm/pmb887x/fifo.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_GPRSCU "pmb887x-gprscu"
#define PMB887X_GPRSCU(obj) OBJECT_CHECK(pmb887x_gprscu_t, (obj), TYPE_PMB887X_GPRSCU)

#define GPRSCU_FIFO_SIZE 32
#define GPRSCU_ID_BASE 0xF003C000

#define GPRSCU_CON_CONFIGURATION (GPRSCU_CON_DIRECTION | GPRSCU_CON_CRC_CTRL | GPRSCU_CON_CIPH_CTRL | \
	GPRSCU_CON_GEA2 | GPRSCU_CON_SEGMENT_MODE | GPRSCU_CON_GEA3)
#define GPRSCU_CON_COMMANDS (GPRSCU_CON_INIT | GPRSCU_CON_SEGMENT_START | GPRSCU_CON_SEGMENT_RESET)
#define GPRSCU_SEGMENT_CONFIGURATION (GPRSCU_SEGMENT_LENGTH | GPRSCU_SEGMENT_CRC_CTRL | GPRSCU_SEGMENT_CIPH_CTRL)

typedef struct pmb887x_gprscu_t pmb887x_gprscu_t;

struct pmb887x_gprscu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	uint32_t revision;

	qemu_irq irq[2];
	pmb887x_clc_reg_t clc;
	pmb887x_src_reg_t src[2];

	uint32_t con;
	uint32_t segment;
	uint32_t input[2];
	uint32_t key[4];
	uint32_t fcs;
	uint32_t polynom;
	bool output_underrun;

	pmb887x_fifo8_t input_fifo;
	pmb887x_fifo8_t output_fifo;
};

static void gprscu_process_fifo(pmb887x_gprscu_t *p) {
	bool control_conversion = (p->con & (GPRSCU_CON_CRC_CTRL | GPRSCU_CON_CIPH_CTRL)) != 0;
	bool segment_conversion = (p->segment & (GPRSCU_SEGMENT_CRC_CTRL | GPRSCU_SEGMENT_CIPH_CTRL)) != 0;
	if (control_conversion || segment_conversion)
		return;

	while (!pmb887x_fifo_is_empty(&p->input_fifo) && !pmb887x_fifo_is_full(&p->output_fifo))
		pmb887x_fifo8_push(&p->output_fifo, pmb887x_fifo8_pop(&p->input_fifo));
}

static uint32_t gprscu_status(pmb887x_gprscu_t *p) {
	uint32_t value = pmb887x_fifo_count(&p->output_fifo) << GPRSCU_STAT_OUTPUT_COUNT_SHIFT;
	value |= pmb887x_fifo_free_count(&p->input_fifo) << GPRSCU_STAT_INPUT_FREE_SHIFT;
	if (p->output_underrun)
		value |= GPRSCU_STAT_OUTPUT_UNDERRUN;
	return value;
}

static uint64_t gprscu_read_data(pmb887x_gprscu_t *p, unsigned int size) {
	uint64_t value = 0;

	for (uint32_t index = 0; index < size; index++) {
		if (pmb887x_fifo_is_empty(&p->output_fifo)) {
			p->output_underrun = true;
			break;
		}
		value |= (uint64_t) pmb887x_fifo8_pop(&p->output_fifo) << (index * 8);
	}

	gprscu_process_fifo(p);
	return value;
}

static void gprscu_write_data(pmb887x_gprscu_t *p, uint64_t value, unsigned int size) {
	for (uint32_t index = 0; index < size; index++) {
		if (pmb887x_fifo_is_full(&p->input_fifo))
			break;
		pmb887x_fifo8_push(&p->input_fifo, (uint8_t) (value >> (index * 8)));
	}

	gprscu_process_fifo(p);
}

static void gprscu_initialize(pmb887x_gprscu_t *p) {
	pmb887x_fifo_reset(&p->input_fifo);
	pmb887x_fifo_reset(&p->output_fifo);
	p->output_underrun = false;
	p->con &= ~(GPRSCU_CON_INIT | GPRSCU_CON_BUSY);
}

static uint64_t gprscu_io_read(void *opaque, hwaddr haddr, unsigned int size) {
	pmb887x_gprscu_t *p = opaque;
	uint64_t value;

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
			value = p->fcs;
			break;

		case GPRSCU_POLYNOM:
			value = p->polynom;
			break;

		case GPRSCU_SRC0 ... GPRSCU_SRC1:
			value = pmb887x_src_get(&p->src[(haddr - GPRSCU_SRC0) / sizeof(uint32_t)]);
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, UINT32_MAX, false);
			EPRINTF("unknown reg access: %02" PRIX64 "\n", haddr);
			exit(1);
	}

	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	return value;
}

static void gprscu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned int size) {
	pmb887x_gprscu_t *p = opaque;

	IO_DUMP(haddr + p->mmio.addr, size, value, true);

	switch (haddr) {
		case GPRSCU_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case GPRSCU_CON:
			p->con = value & (GPRSCU_CON_CONFIGURATION | GPRSCU_CON_COMMANDS);
			if ((p->con & GPRSCU_CON_INIT) != 0)
				gprscu_initialize(p);
			if ((p->con & GPRSCU_CON_SEGMENT_RESET) != 0) {
				p->segment = 0;
				p->con &= ~GPRSCU_CON_SEGMENT_RESET;
			}
			gprscu_process_fifo(p);
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
			gprscu_process_fifo(p);
			break;

		case GPRSCU_INPUT0 ... GPRSCU_INPUT1:
			p->input[(haddr - GPRSCU_INPUT0) / sizeof(uint32_t)] = value;
			break;

		case GPRSCU_KEY0 ... GPRSCU_KEY3:
			p->key[(haddr - GPRSCU_KEY0) / sizeof(uint32_t)] = value;
			break;

		case GPRSCU_FCS:
			p->fcs = value;
			break;

		case GPRSCU_POLYNOM:
			p->polynom = value;
			break;

		case GPRSCU_SRC0 ... GPRSCU_SRC1:
			pmb887x_src_set(&p->src[(haddr - GPRSCU_SRC0) / sizeof(uint32_t)], value);
			break;

		default:
			EPRINTF("unknown reg access: %02" PRIX64 "\n", haddr);
			exit(1);
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
	p->fcs = 0;
	p->polynom = 0;
	p->output_underrun = false;
	pmb887x_fifo_reset(&p->input_fifo);
	pmb887x_fifo_reset(&p->output_fifo);
}

static void gprscu_realize(DeviceState *dev, Error **errp) {
	pmb887x_gprscu_t *p = PMB887X_GPRSCU(dev);

	for (size_t index = 0; index < ARRAY_SIZE(p->src); index++)
		pmb887x_src_init(&p->src[index], p->irq[index]);
	pmb887x_fifo8_init(&p->input_fifo, GPRSCU_FIFO_SIZE);
	pmb887x_fifo8_init(&p->output_fifo, GPRSCU_FIFO_SIZE);
	gprscu_reset(dev);
}

static void gprscu_finalize(Object *obj) {
	pmb887x_gprscu_t *p = PMB887X_GPRSCU(obj);

	pmb887x_fifo8_free(&p->input_fifo);
	pmb887x_fifo8_free(&p->output_fifo);
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
	.instance_finalize = gprscu_finalize,
	.class_init = gprscu_class_init,
};

static void gprscu_register_types(void) {
	type_register_static(&gprscu_info);
}

type_init(gprscu_register_types)
