/*
 * Dialog D1601XX or similar Power ASIC IC
 * */
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
#include "hw/i2c/i2c.h"

#define PASIC_DEBUG

#ifdef PASIC_DEBUG
#define DPRINTF(fmt, ...) do { fprintf(stderr, "[pmb887x-pasic]: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_PMB887X_PASIC	"pmb887x-pasic"
#define PMB887X_PASIC(obj)	OBJECT_CHECK(struct pmb887x_pasic_t, (obj), TYPE_PMB887X_PASIC)

struct pmb887x_pasic_t {
	I2CSlave parent_obj;
};

static int pasic_event(I2CSlave *s, enum i2c_event event) {
	struct pmb887x_pasic_t *p = PMB887X_PASIC(s);

    switch (event) {
		case I2C_START_SEND:
			DPRINTF("I2C_START_SEND\n");
		break;
		
		case I2C_START_RECV:
			DPRINTF("I2C_START_RECV\n");
		break;
		
		case I2C_FINISH:
			DPRINTF("I2C_FINISH\n");
		break;
		
		case I2C_NACK:
			DPRINTF("I2C_NACK\n");
		break;
		
		default:
			DPRINTF("I2C_???\n");
		break;
	}
    
    return 0;
}

static uint8_t pasic_recv(I2CSlave *s) {
	struct pmb887x_pasic_t *p = PMB887X_PASIC(s);
	
	DPRINTF("at24c_eeprom_recv\n");
	
	return 0;
}

static int pasic_send(I2CSlave *s, uint8_t data) {
	struct pmb887x_pasic_t *p = PMB887X_PASIC(s);
	
	DPRINTF("pasic_send %02X\n", data);
	
    return 0;
}

static void pasic_realize(DeviceState *dev, Error **errp) {
	struct pmb887x_pasic_t *p = PMB887X_PASIC(dev);
}

static Property pasic_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void pasic_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, pasic_properties);
	dc->realize = pasic_realize;
	
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    k->event = &pasic_event;
    k->recv = &pasic_recv;
    k->send = &pasic_send;
}

static const TypeInfo pasic_info = {
    .name          	= TYPE_PMB887X_PASIC,
    .parent        	= TYPE_I2C_SLAVE,
    .instance_size 	= sizeof(struct pmb887x_pasic_t),
    .class_init    	= pasic_class_init,
};

static void pasic_register_types(void) {
	type_register_static(&pasic_info);
}
type_init(pasic_register_types)
