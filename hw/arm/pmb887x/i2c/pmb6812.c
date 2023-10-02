/*
 * Infineon pmb6812
 * */
#define PMB887X_TRACE_ID		PMIC
#define PMB887X_TRACE_PREFIX	"pmb887x-pmb6812"

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
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_PMIC	"pmb887x-pmb6812"
#define PMB887X_PMIC(obj)	OBJECT_CHECK(pmb887x_pmic_t, (obj), TYPE_PMB887X_PMIC)

typedef struct {
	I2CSlave parent_obj;
	int reg_id;
	int wcycle;
	uint8_t regs[0xFF];
} pmb887x_pmic_t;

static const uint8_t regs_PMB6812[0xFF] = { 0 };

static int pmic_event(I2CSlave *s, enum i2c_event event) {
	pmb887x_pmic_t *p = PMB887X_PMIC(s);

    switch (event) {
		case I2C_START_SEND:
			// Nothing
		break;
		
		case I2C_START_RECV:
			p->wcycle = 0;
		break;
		
		case I2C_NACK:
			p->wcycle = 0;
		break;
		
		case I2C_FINISH:
			// Nothing
			p->wcycle = 0;
		break;
		
		case I2C_START_SEND_ASYNC:
			// Nothing
		break;
	}
    
    return 0;
}

static uint8_t pmic_recv(I2CSlave *s) {
	pmb887x_pmic_t *p = PMB887X_PMIC(s);
	
	uint8_t data = p->regs[p->reg_id];
	DPRINTF("read reg %02X: %02X\n", p->reg_id, data);
	p->reg_id = (p->reg_id + 1) % ARRAY_SIZE(p->regs);
	
	return data;
}

static int pmic_send(I2CSlave *s, uint8_t data) {
	pmb887x_pmic_t *p = PMB887X_PMIC(s);
	
	if (p->wcycle == 0) {
		p->reg_id = data % ARRAY_SIZE(p->regs);
	} else {
		DPRINTF("write reg %02X: %02X\n", p->reg_id, data);
		p->regs[p->reg_id] = data;
		p->reg_id = (p->reg_id + 1) % ARRAY_SIZE(p->regs);
	}
	
	p->wcycle++;
	
    return 0;
}

static void pmic_realize(DeviceState *dev, Error **errp) {
	pmb887x_pmic_t *p = PMB887X_PMIC(dev);
	memcpy(p->regs, regs_PMB6812, sizeof(regs_PMB6812));
}

static Property pmic_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void pmic_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, pmic_properties);
	dc->realize = pmic_realize;
	
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    k->event = &pmic_event;
    k->recv = &pmic_recv;
    k->send = &pmic_send;
}

static const TypeInfo pmic_info = {
    .name          	= TYPE_PMB887X_PMIC,
    .parent        	= TYPE_I2C_SLAVE,
    .instance_size 	= sizeof(pmb887x_pmic_t),
    .class_init    	= pmic_class_init,
};

static void pmic_register_types(void) {
	type_register_static(&pmic_info);
}
type_init(pmic_register_types)
