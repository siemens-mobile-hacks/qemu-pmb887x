#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"
#include "hw/arm/pmb887x/mod.h"

#define TYPE_PMB887X_KEYPAD	"pmb887x-keypad"
#define PMB887X_KEYPAD(obj)	OBJECT_CHECK(pmb887x_keypad_t, (obj), TYPE_PMB887X_KEYPAD)

#define KEYPAD_INPUTS			8
#define KEYPAD_OUTPUTS			8
#define KEYPAD_COLUMNS			9
#define KEYPAD_PORTS			3
#define KEYPAD_RELEASED_PORT	0xFFFFFFFF
#define KEYPAD_DELAY_UNIT_NS	(10 * SCALE_MS)
#define KEYPAD_RESCAN_NS		(100 * SCALE_MS)
#define KEYPAD_CLOCK_HZ			32768
#define KEYPAD_PHASE_CYCLES		9
#define KEYPAD_SCAN_CYCLES		(KEYPAD_COLUMNS * KEYPAD_PHASE_CYCLES)
#define KEYPAD_SCAN_NS			(KEYPAD_SCAN_CYCLES * NANOSECONDS_PER_SECOND / KEYPAD_CLOCK_HZ)

enum keypad_irq {
	KEYPAD_IRQ_INT0,
	KEYPAD_IRQ_INT1,
	KEYPAD_IRQ_INT2,
	KEYPAD_IRQ_INT3,
	KEYPAD_IRQS,
};

typedef struct pmb887x_keypad_t pmb887x_keypad_t;
typedef struct keypad_input_t keypad_input_t;

struct keypad_input_t {
	pmb887x_keypad_t *keypad;
	uint32_t index;
};

struct pmb887x_keypad_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	DebugChannel dbg;
	uint32_t revision;

	qemu_irq irq[KEYPAD_IRQS];
	pmb887x_src_reg_t src[KEYPAD_IRQS];
	keypad_input_t inputs[KEYPAD_INPUTS];
	qemu_irq outputs[KEYPAD_OUTPUTS];
	QEMUTimer *scan_timer;
	QEMUTimer *int2_timer;

	uint32_t con;
	uint32_t isr;
	uint32_t port[KEYPAD_PORTS];
	uint8_t high_inputs;
	bool is_scanning;
	bool is_debounce_pending;
	bool is_pressed;
};

static int64_t keypad_get_debounce_ns(pmb887x_keypad_t *p) {
	uint32_t delay = (p->con & KEYPAD_CON_NEXT_REPEAT_DELAY) >> KEYPAD_CON_NEXT_REPEAT_DELAY_SHIFT;
	return (delay + 1) * KEYPAD_DELAY_UNIT_NS;
}

static int64_t keypad_get_int2_delay_ns(pmb887x_keypad_t *p) {
	uint32_t delay = (p->con & KEYPAD_CON_FIRST_REPEAT_DELAY) >> KEYPAD_CON_FIRST_REPEAT_DELAY_SHIFT;
	return delay * KEYPAD_DELAY_UNIT_NS;
}

static void keypad_drive_column(pmb887x_keypad_t *p, uint32_t column) {
	for (uint32_t i = 0; i < KEYPAD_OUTPUTS; i++)
		qemu_set_irq(p->outputs[i], i != column);
}

static void keypad_drive_idle(pmb887x_keypad_t *p) {
	for (uint32_t i = 0; i < KEYPAD_OUTPUTS; i++)
		qemu_set_irq(p->outputs[i], 0);
}

static void keypad_scan(pmb887x_keypad_t *p, uint32_t port[KEYPAD_PORTS]) {
	p->is_scanning = true;
	for (uint32_t i = 0; i < KEYPAD_PORTS; i++)
		port[i] = KEYPAD_RELEASED_PORT;
	for (uint32_t column = 0; column < KEYPAD_COLUMNS; column++) {
		keypad_drive_column(p, column);
		port[column / 4] = deposit32(port[column / 4], (column % 4) * 8, 8, p->high_inputs);
	}
	keypad_drive_idle(p);
	p->is_scanning = false;
}

static void keypad_schedule_scan(pmb887x_keypad_t *p, int64_t delay_ns) {
	timer_mod_ns(p->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns + KEYPAD_SCAN_NS);
}

static void keypad_handle_scan_timer(void *opaque) {
	pmb887x_keypad_t *p = opaque;
	uint32_t port[KEYPAD_PORTS];

	p->is_debounce_pending = false;
	keypad_scan(p, port);

	bool is_pressed = false;
	for (uint32_t i = 0; i < KEYPAD_PORTS; i++)
		is_pressed |= port[i] != KEYPAD_RELEASED_PORT;

	if (is_pressed) {
		if (memcmp(port, p->port, sizeof(port)) != 0) {
			memcpy(p->port, port, sizeof(port));
			p->isr = KEYPAD_ISR_INT0;
			pmb887x_src_update(&p->src[KEYPAD_IRQ_INT0], 0, MOD_SRC_SETR);
			timer_mod_ns(p->int2_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + keypad_get_int2_delay_ns(p));
		}
		keypad_schedule_scan(p, KEYPAD_RESCAN_NS);
	} else if (p->is_pressed) {
		memcpy(p->port, port, sizeof(port));
		p->isr = KEYPAD_ISR_INT1;
		timer_del(p->int2_timer);
		pmb887x_src_update(&p->src[KEYPAD_IRQ_INT3], 0, MOD_SRC_SETR);
	}
	p->is_pressed = is_pressed;
}

static void keypad_handle_int2_timer(void *opaque) {
	pmb887x_keypad_t *p = opaque;
	pmb887x_src_update(&p->src[KEYPAD_IRQ_INT2], 0, MOD_SRC_SETR);
}

static void keypad_handle_input(void *opaque, int _n, int level) {
	keypad_input_t *input = opaque;
	pmb887x_keypad_t *p = input->keypad;

	p->high_inputs = deposit32(p->high_inputs, input->index, 1, level != 0);
	if (p->is_scanning || p->is_debounce_pending)
		return;

	p->is_debounce_pending = true;
	keypad_schedule_scan(p, keypad_get_debounce_ns(p));
}

static uint64_t keypad_io_read(void *opaque, hwaddr haddr, unsigned _size) {
	pmb887x_keypad_t *p = opaque;
	uint64_t value = 0;

	switch (haddr) {
		case KEYPAD_ID:
			value = 0xF046C000 | p->revision;
			break;
		case KEYPAD_CON:
			value = p->con;
			break;
		case KEYPAD_ISR:
			value = p->isr;
			break;
		case KEYPAD_PORT0:
		case KEYPAD_PORT1:
		case KEYPAD_PORT2:
			value = p->port[(haddr - KEYPAD_PORT0) / 4];
			break;
		case KEYPAD_INT0_SRC:
		case KEYPAD_INT1_SRC:
		case KEYPAD_INT2_SRC:
		case KEYPAD_INT3_SRC:
			value = pmb887x_src_get(&p->src[(haddr - KEYPAD_INT0_SRC) / 4]);
			break;
		default:
			DEBUG_GUEST_ERROR(&p->dbg, "unknown reg read: %02"PRIX64"\n", haddr);
			break;
	}

	return value;
}

static void keypad_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned _size) {
	pmb887x_keypad_t *p = opaque;

	switch (haddr) {
		case KEYPAD_CON:
			p->con = value & (KEYPAD_CON_NEXT_REPEAT_DELAY | KEYPAD_CON_FIRST_REPEAT_DELAY);
			break;
		case KEYPAD_INT0_SRC:
		case KEYPAD_INT1_SRC:
		case KEYPAD_INT2_SRC:
		case KEYPAD_INT3_SRC:
			pmb887x_src_set(&p->src[(haddr - KEYPAD_INT0_SRC) / 4], value);
			break;
		default:
			DEBUG_GUEST_ERROR(&p->dbg, "unknown reg write: %02"PRIX64" = %08"PRIX64"\n", haddr, value);
			break;
	}
}

static const MemoryRegionOps io_ops = {
	.read			= keypad_io_read,
	.write			= keypad_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 4,
		.max_access_size	= 4
	}
};

static void keypad_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_keypad_t *p = PMB887X_KEYPAD(obj);

	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-keypad", KEYPAD_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	for (size_t i = 0; i < ARRAY_SIZE(p->irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);

	p->high_inputs = MAKE_64BIT_MASK(0, KEYPAD_INPUTS);
	for (uint32_t i = 0; i < KEYPAD_INPUTS; i++) {
		g_autofree char *name = g_strdup_printf("IN%u", i);
		p->inputs[i] = (keypad_input_t) { p, i };
		qdev_init_gpio_in_named_with_opaque(dev, keypad_handle_input, &p->inputs[i], name, 1);
	}
	for (uint32_t i = 0; i < KEYPAD_OUTPUTS; i++) {
		g_autofree char *name = g_strdup_printf("OUT%u", i);
		qdev_init_gpio_out_named(dev, &p->outputs[i], name, 1);
	}
}

static void keypad_realize(DeviceState *dev, Error **errp) {
	pmb887x_keypad_t *p = PMB887X_KEYPAD(dev);

	pmb887x_mod_debug_init(&p->dbg, dev, &p->mmio, &pmb887x_keypad_regs);
	for (size_t i = 0; i < ARRAY_SIZE(p->src); i++) {
		if (!p->irq[i]) {
			error_setg(errp, "%s: IRQ %zu is not connected", object_get_typename(OBJECT(dev)), i);
			return;
		}
		pmb887x_src_init(&p->src[i], p->irq[i]);
	}
	p->scan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, keypad_handle_scan_timer, p);
	p->int2_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, keypad_handle_int2_timer, p);
}

static void keypad_reset(DeviceState *dev) {
	pmb887x_keypad_t *p = PMB887X_KEYPAD(dev);

	timer_del(p->scan_timer);
	timer_del(p->int2_timer);
	p->con = 0;
	p->isr = 0;
	p->is_debounce_pending = false;
	p->is_pressed = false;
	for (size_t i = 0; i < ARRAY_SIZE(p->port); i++)
		p->port[i] = KEYPAD_RELEASED_PORT;
	for (size_t i = 0; i < ARRAY_SIZE(p->src); i++)
		pmb887x_src_reset(&p->src[i]);
	keypad_drive_idle(p);
}

static const Property keypad_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_keypad_t, revision, 0),
};

static void keypad_class_init(ObjectClass *klass, const void *_data) {
	DeviceClass *dc = DEVICE_CLASS(klass);

	device_class_set_props(dc, keypad_properties);
	device_class_set_legacy_reset(dc, keypad_reset);
	dc->realize = keypad_realize;
}

static const TypeInfo keypad_info = {
	.name			= TYPE_PMB887X_KEYPAD,
	.parent			= TYPE_SYS_BUS_DEVICE,
	.instance_size	= sizeof(pmb887x_keypad_t),
	.instance_init	= keypad_init,
	.class_init		= keypad_class_init,
};

static void keypad_register_types(void) {
	type_register_static(&keypad_info);
}
type_init(keypad_register_types)
