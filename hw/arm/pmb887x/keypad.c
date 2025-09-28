/*
 * Keypad
 * */
#define PMB887X_TRACE_ID		KEYPAD
#define PMB887X_TRACE_PREFIX	"pmb887x-keypad"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "ui/input.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_KEYPAD	"pmb887x-keypad"
#define PMB887X_KEYPAD(obj)	OBJECT_CHECK(pmb887x_keypad_t, (obj), TYPE_PMB887X_KEYPAD)

#define KEYPAD_PORTS	3
#define KEYPAD_MAX_IN	8
#define KEYPAD_MAX_OUT	(4 * KEYPAD_PORTS)

enum {
	IRQ_KEY_PRESS,
	IRQ_KEY_UNK0,
	IRQ_KEY_UNK1,
	IRQ_KEY_RELEASE,
};

typedef struct pmb887x_keypad_t pmb887x_keypad_t;

struct pmb887x_keypad_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	qemu_irq irq[4];
	pmb887x_src_reg_t src[4];
	
	uint8_t extension;
	
	uint8_t state[KEYPAD_MAX_OUT][KEYPAD_MAX_IN];
	uint32_t port[KEYPAD_PORTS];
	uint32_t con;
	
	bool *pressed;
	uint32_t *map;
	uint32_t map_size;
};

static void keypad_handle_event(DeviceState *dev, QemuConsole *src, InputEvent *evt) {
	pmb887x_keypad_t *p = PMB887X_KEYPAD(dev);
	
	bool pressed = evt->u.key.data->down;
	int keycode = qemu_input_key_value_to_qcode(evt->u.key.data->key);
	
	if (keycode >= p->map_size || !p->map)
		return;
	
	if (p->pressed[keycode] == pressed)
		return;
	p->pressed[keycode] = pressed;
	
	for (int i = 0; i < KEYPAD_MAX_OUT; i++) {
		if (!(p->map[keycode] & (1 << (8 + i))))
			continue;
		
		for (int j = 0; j < KEYPAD_MAX_IN; j++) {
			if (!(p->map[keycode] & (1 << j)))
				continue;
			
			if (pressed) {
				p->state[i][j]++;
				pmb887x_src_update(&p->src[IRQ_KEY_PRESS], 0, MOD_SRC_SETR);
			} else {
				p->state[i][j]--;
				pmb887x_src_update(&p->src[IRQ_KEY_RELEASE], 0, MOD_SRC_SETR);
			}
			
			uint8_t port = i / 4;
			uint8_t shift = (i % 4) * 8;
			uint32_t mask = (1 << j) << shift;
			
			if (p->state[i][j]) {
				p->port[port] &= ~mask;
			} else {
				p->port[port] |= mask;
			}
		}
	}
}

static uint32_t get_reg_index_by_addr(hwaddr haddr) {
	switch (haddr) {
		case KEYPAD_PORT0:			return 0;
		case KEYPAD_PORT1:			return 1;
		case KEYPAD_PORT2:			return 2;
		case KEYPAD_PRESS_SRC:		return 0;
		case KEYPAD_UNK0_SRC:		return 1;
		case KEYPAD_UNK1_SRC:		return 2;
		case KEYPAD_RELEASE_SRC:	return 3;
		default:					hw_error("Unknown reg: %08lx", haddr);
	}
}

static uint64_t keypad_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_keypad_t *p = opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case KEYPAD_ID:
			value = 0xF046C021;
			break;
		
		case KEYPAD_CON:
			value = p->con;
			break;
		
		case KEYPAD_PORT0:
		case KEYPAD_PORT1:
		case KEYPAD_PORT2:
			value = p->port[get_reg_index_by_addr(haddr)];
			break;
		
		case KEYPAD_PRESS_SRC:
		case KEYPAD_UNK0_SRC:
		case KEYPAD_UNK1_SRC:
		case KEYPAD_RELEASE_SRC:
			value = pmb887x_src_get(&p->src[get_reg_index_by_addr(haddr)]);
			break;
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void keypad_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_keypad_t *p = opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case KEYPAD_CON:
			p->con = value;
			break;
		
		case KEYPAD_PRESS_SRC:
		case KEYPAD_UNK0_SRC:
		case KEYPAD_UNK1_SRC:
		case KEYPAD_RELEASE_SRC:
			pmb887x_src_set(&p->src[get_reg_index_by_addr(haddr)], value);
			break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
}

static const MemoryRegionOps io_ops = {
	.read			= keypad_io_read,
	.write			= keypad_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static QemuInputHandler keypad_input_handler = {
    .name  = "QEMU PMB887X Keypad",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = keypad_handle_event,
};

static void keypad_init(Object *obj) {
	pmb887x_keypad_t *p = PMB887X_KEYPAD(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-keypad", KEYPAD_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (int i = 0; i < ARRAY_SIZE(p->src); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq[i]);
}

static void keypad_realize(DeviceState *dev, Error **errp) {
	pmb887x_keypad_t *p = PMB887X_KEYPAD(dev);
	
	p->con = 0;
	p->extension = 0;
	p->pressed = g_new0(bool, p->map_size);
	
	for (int i = 0; i < KEYPAD_MAX_OUT; i++) {
		for (int j = 0; j < KEYPAD_MAX_IN; j++)
			p->state[i][j] = 0;
	}
	
	for (int i = 0; i < KEYPAD_PORTS; i++)
		p->port[i] = 0xFFFFFFFF;
	
	for (int i = 0; i < ARRAY_SIZE(p->src); i++) {
		if (!p->irq[i])
			hw_error("pmb887x-keypad: irq %d not set", i);
		pmb887x_src_init(&p->src[i], p->irq[i]);
	}
	
    qemu_input_handler_register(dev, &keypad_input_handler);
}

static const Property keypad_properties[] = {
	DEFINE_PROP_ARRAY("map", pmb887x_keypad_t, map_size, map, qdev_prop_uint32, uint32_t),
};

static void keypad_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, keypad_properties);
	dc->realize = keypad_realize;
}

static const TypeInfo keypad_info = {
    .name          	= TYPE_PMB887X_KEYPAD,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_keypad_t),
    .instance_init 	= keypad_init,
    .class_init    	= keypad_class_init,
};

static void keypad_register_types(void) {
	type_register_static(&keypad_info);
}
type_init(keypad_register_types)
