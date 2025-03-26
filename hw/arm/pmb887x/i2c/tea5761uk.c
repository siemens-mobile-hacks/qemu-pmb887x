/*
 * Philips TEA5761UK
 * */
#define PMB887X_TRACE_ID		FM_RADIO
#define PMB887X_TRACE_PREFIX	"pmb887x-tea5761uk"

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

#define TYPE_PMB887X_PMIC	"pmb887x-tea5761uk"
#define PMB887X_PMIC(obj)	OBJECT_CHECK(pmb887x_fmradio_t, (obj), TYPE_PMB887X_PMIC)

typedef struct {
	I2CSlave parent_obj;
	int reg_id;
	uint8_t rcycle;
	uint8_t wcycle;
	uint8_t regs[256];
	uint32_t revision;
} pmb887x_fmradio_t;

static const uint8_t default_regs[256] = {
	0x00, 0x00, 0x80, 0x00, 0x08, 0xD2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x2B, 0x57, 0x61
};

static int pmic_event(I2CSlave *s, enum i2c_event event) {
	pmb887x_fmradio_t *p = PMB887X_PMIC(s);

    switch (event) {
		case I2C_START_SEND:
			// Nothing
		break;
		
		case I2C_START_RECV:
			p->rcycle = 0;
			p->wcycle = 0;
		break;
		
		case I2C_NACK:
			p->rcycle = 0;
			p->wcycle = 0;
		break;
		
		case I2C_FINISH:
			// Nothing
			p->rcycle = 0;
			p->wcycle = 0;
		break;
		
		case I2C_START_SEND_ASYNC:
			// Nothing
		break;
	}
    
    return 0;
}

static uint8_t pmic_recv(I2CSlave *s) {
	pmb887x_fmradio_t *p = PMB887X_PMIC(s);
	
	uint8_t data = p->regs[p->rcycle];
	DPRINTF("read reg %02X: %02X\n", p->rcycle, data);
	p->rcycle++;
	
	return data;
}

static int pmic_send(I2CSlave *s, uint8_t data) {
	pmb887x_fmradio_t *p = PMB887X_PMIC(s);
	
	DPRINTF("write reg %02X: %02X\n", p->wcycle, data);
	p->wcycle++;
	
    return 0;
}

static void pmic_realize(DeviceState *dev, Error **errp) {
	pmb887x_fmradio_t *p = PMB887X_PMIC(dev);
	memcpy(p->regs, default_regs, sizeof(default_regs));
}

static void pmic_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	dc->realize = pmic_realize;
	
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    k->event = &pmic_event;
    k->recv = &pmic_recv;
    k->send = &pmic_send;
}

static const TypeInfo pmic_info = {
    .name          	= TYPE_PMB887X_PMIC,
    .parent        	= TYPE_I2C_SLAVE,
    .instance_size 	= sizeof(pmb887x_fmradio_t),
    .class_init    	= pmic_class_init,
};

static void pmic_register_types(void) {
	type_register_static(&pmic_info);
}
type_init(pmic_register_types)
