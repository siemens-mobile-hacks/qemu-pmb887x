/*
 * System Control Unit
 * */
#define PMB887X_TRACE_ID		SCU
#define PMB887X_TRACE_PREFIX	"pmb887x-scu"

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"

#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/sccu.h"
#include "hw/arm/pmb887x/pcl.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_SCU	"pmb887x-scu"
#define PMB887X_SCU(obj)	OBJECT_CHECK(pmb887x_scu_t, (obj), TYPE_PMB887X_SCU)

typedef struct {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	pmb887x_src_reg_t dsp_src[5];
	pmb887x_src_reg_t unk_src[3];
	
	qemu_irq exti_irq[8];
	qemu_irq dsp_irq[5];
	qemu_irq unk_irq[3];
	
	uint32_t cpu_type;
	
	uint32_t wdtcon0;
	uint32_t wdtcon1;
	uint32_t romamcr;
	uint32_t ebuclc;
	uint32_t ebuclc1;
	uint32_t ebuclc2;
	uint32_t rtcif;
	uint32_t boot_flag;
	uint32_t dmars;
	uint32_t rst_req;
	uint32_t boot_cfg;
	uint32_t dsp_unk0;
	
	pmb887x_pcl_t *pcl;
	struct pmb887x_sccu_t *sccu;
	MemoryRegion *brom_mirror;
} pmb887x_scu_t;

static void scu_update_state(pmb887x_scu_t *p) {
	// Mount BROM image to 0x00000000?
	memory_region_set_enabled(p->brom_mirror, (p->romamcr & SCU_ROMAMCR_MOUNT_BROM) != 0);
}

static int get_src_index_by_addr(hwaddr haddr) {
	switch (haddr) {
		case SCU_EXTI0_SRC:		return 0;
		case SCU_EXTI1_SRC:		return 1;
		case SCU_EXTI2_SRC:		return 2;
		case SCU_EXTI3_SRC:		return 3;
		case SCU_EXTI4_SRC:		return 4;
		case SCU_EXTI5_SRC:		return 5;
		case SCU_EXTI6_SRC:		return 6;
		case SCU_EXTI7_SRC:		return 7;
		
		case SCU_DSP_SRC0:		return 0;
		case SCU_DSP_SRC1:		return 1;
		case SCU_DSP_SRC2:		return 2;
		case SCU_DSP_SRC3:		return 3;
		case SCU_DSP_SRC4:		return 4;
		
		case SCU_UNK0_SRC:		return 0;
		case SCU_UNK1_SRC:		return 1;
		case SCU_UNK2_SRC:		return 2;
	}
	return -1;
}

static uint64_t scu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_scu_t *p = (pmb887x_scu_t *) opaque;
	
	uint64_t value = 0;
	
	switch (haddr) {
		case SCU_CLC:
			value = pmb887x_sccu_clc_get(p->sccu);
		break;
		
		case SCU_ID:
			value = 0xF040C012;
		break;
		
		case SCU_MANID:
			value = 0x1823;
		break;
		
		case SCU_CHIPID:
			if (p->cpu_type == CPU_PMB8876) {
				value = 0x1B10;
			} else if (p->cpu_type == CPU_PMB8875) {
				value = 0x1A05;
			}
		break;
		
		case SCU_RST_REQ:
			value = p->rst_req;
		break;
		
		case SCU_RST_SR:
			// value = SCU_RST_SR_RSSTM | SCU_RST_SR_HDRST | SCU_RST_SR_RSEXT | 0x5000;
			value = SCU_RST_SR_PWDRST | SCU_RST_SR_RSSTM | SCU_RST_SR_RSEXT;
		break;
		
		case SCU_WDT_SR:
		{
			uint32_t counter = (p->wdtcon0 & SCU_WDTCON0_WDTPW) >> SCU_WDTCON0_WDTPW_SHIFT;
			value = (counter << SCU_WDT_SR_WDTTIM_SHIFT);
			
			if ((p->wdtcon1 & SCU_WDTCON1_WDTDR))
				value |= SCU_WDT_SR_WDTDS;
		}
		break;
		
		case SCU_WDTCON0:
			value = p->wdtcon0;
		break;
		
		case SCU_WDTCON1:
			value = p->wdtcon1;
		break;
		
		case SCU_EBUCLC:
			value = p->ebuclc;
		break;
		
		case SCU_EBUCLC1:
			value = p->ebuclc1 | SCU_EBUCLC1_READY;
		break;
		
		case SCU_EBUCLC2:
			value = p->ebuclc2 | SCU_EBUCLC2_READY;
		break;
		
		case SCU_RTCIF:
			value = p->rtcif;
		break;
		
		case SCU_ROMAMCR:
			value = p->romamcr;
		break;
		
		case SCU_BOOT_FLAG:
			value = p->boot_flag;
		break;
		
		case SCU_DMARS:
			value = p->dmars;
		break;
		
		case SCU_BOOT_CFG:
			value = p->boot_cfg;
		break;
		
		case SCU_EXTI:
			value = pmb887x_pcl_exti_read(p->pcl);
		break;
		
		case SCU_DSP_UNK0:
			value = p->dsp_unk0;
		break;
		
		case SCU_EXTI0_SRC:
		case SCU_EXTI1_SRC:
		case SCU_EXTI2_SRC:
		case SCU_EXTI3_SRC:
		case SCU_EXTI4_SRC:
		case SCU_EXTI5_SRC:
		case SCU_EXTI6_SRC:
		case SCU_EXTI7_SRC:
			value = pmb887x_pcl_exti_src_read(p->pcl, get_src_index_by_addr(haddr));
		break;
		
		case SCU_DSP_SRC0:
		case SCU_DSP_SRC1:
		case SCU_DSP_SRC2:
		case SCU_DSP_SRC3:
		case SCU_DSP_SRC4:
			value = pmb887x_src_get(&p->dsp_src[get_src_index_by_addr(haddr)]);
		break;
		
		case SCU_UNK0_SRC:
		case SCU_UNK1_SRC:
		case SCU_UNK2_SRC:
			value = pmb887x_src_get(&p->unk_src[get_src_index_by_addr(haddr)]);
		break;
		
		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
		//	exit(1);
		break;
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void scu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_scu_t *p = (pmb887x_scu_t *) opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case SCU_CLC:
			#ifdef PMB887X_IO_BRIDGE
			pmb8876_io_bridge_write(haddr + p->mmio.addr, size, value);
			#endif
			pmb887x_sccu_clc_set(p->sccu, value);
		break;
		
		case SCU_RST_REQ:
			p->rst_req = value;
		break;
		
		case SCU_WDTCON0:
			p->wdtcon0 = value;
		break;
		
		case SCU_WDTCON1:
			p->wdtcon1 = value;
		break;
		
		case SCU_EBUCLC:
			p->ebuclc = value;
		break;
		
		case SCU_EBUCLC1:
			p->ebuclc1 = value;
		break;
		
		case SCU_EBUCLC2:
			p->ebuclc2 = value;
		break;
		
		case SCU_RTCIF:
			p->rtcif = value;
		break;
		
		case SCU_ROMAMCR:
			p->romamcr = value;
		break;
		
		case SCU_BOOT_FLAG:
			p->boot_flag = value;
		break;
		
		case SCU_DMARS:
			p->dmars = value;
		break;
		
		case SCU_BOOT_CFG:
			p->boot_cfg = value;
		break;
		
		case SCU_EXTI:
			pmb887x_pcl_exti_write(p->pcl, value);
		break;
		
		case SCU_DSP_UNK0:
			p->dsp_unk0 = value;
		break;
		
		case SCU_EXTI0_SRC:
		case SCU_EXTI1_SRC:
		case SCU_EXTI2_SRC:
		case SCU_EXTI3_SRC:
		case SCU_EXTI4_SRC:
		case SCU_EXTI5_SRC:
		case SCU_EXTI6_SRC:
		case SCU_EXTI7_SRC:
			pmb887x_pcl_exti_src_write(p->pcl, get_src_index_by_addr(haddr), value);
		break;
		
		case SCU_DSP_SRC0:
		case SCU_DSP_SRC1:
		case SCU_DSP_SRC2:
		case SCU_DSP_SRC3:
		case SCU_DSP_SRC4:
			pmb887x_src_set(&p->dsp_src[get_src_index_by_addr(haddr)], value);
		break;
		
		case SCU_UNK0_SRC:
		case SCU_UNK1_SRC:
		case SCU_UNK2_SRC:
			pmb887x_src_set(&p->unk_src[get_src_index_by_addr(haddr)], value);
		break;
		
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
		//	exit(1);
		break;
	}
	
	scu_update_state(p);
}

static const MemoryRegionOps io_ops = {
	.read			= scu_io_read,
	.write			= scu_io_write,
	.endianness		= DEVICE_NATIVE_ENDIAN,
	.valid			= {
		.min_access_size	= 1,
		.max_access_size	= 4
	}
};

static void scu_init(Object *obj) {
	pmb887x_scu_t *p = PMB887X_SCU(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-scu", SCU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->exti_irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->exti_irq[i]);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->dsp_irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->dsp_irq[i]);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->unk_irq); i++)
		sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->unk_irq[i]);
}

static void scu_realize(DeviceState *dev, Error **errp) {
	pmb887x_scu_t *p = PMB887X_SCU(dev);
	
	pmb887x_pcl_init_exti(p->pcl, p->exti_irq, ARRAY_SIZE(p->exti_irq));
	
	for (size_t i = 0; i < ARRAY_SIZE(p->dsp_src); i++)
		pmb887x_src_init(&p->dsp_src[i], p->dsp_irq[i]);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->unk_src); i++)
		pmb887x_src_init(&p->unk_src[i], p->unk_irq[i]);
	
	// Default values
	p->wdtcon0		= SCU_WDTCON0_WDTLCK | (0xED68 << SCU_WDTCON0_WDTREL_SHIFT) | SCU_WDTCON0_ENDINIT;
	p->wdtcon1		= SCU_WDTCON1_WDTDR;
	p->romamcr		= SCU_ROMAMCR_MOUNT_BROM;
	
	scu_update_state(p);
}

static const Property scu_properties[] = {
	DEFINE_PROP_UINT32("cpu_type", pmb887x_scu_t, cpu_type, 0),
	DEFINE_PROP_LINK("sccu", pmb887x_scu_t, sccu, "pmb887x-sccu", struct pmb887x_sccu_t *),
	DEFINE_PROP_LINK("pcl", pmb887x_scu_t, pcl, "pmb887x-pcl", pmb887x_pcl_t *),
	DEFINE_PROP_LINK("brom_mirror", pmb887x_scu_t, brom_mirror, TYPE_MEMORY_REGION, MemoryRegion *),
};

static void scu_class_init(ObjectClass *klass, void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, scu_properties);
	dc->realize = scu_realize;
}

static const TypeInfo scu_info = {
    .name          	= TYPE_PMB887X_SCU,
    .parent        	= TYPE_SYS_BUS_DEVICE,
    .instance_size 	= sizeof(pmb887x_scu_t),
    .instance_init 	= scu_init,
    .class_init    	= scu_class_init,
};

static void scu_register_types(void) {
	type_register_static(&scu_info);
}
type_init(scu_register_types)
