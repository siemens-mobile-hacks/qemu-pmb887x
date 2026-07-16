/*
 * System Control Unit
 * */
#define PMB887X_TRACE_ID		SCU
#define PMB887X_TRACE_PREFIX	"pmb887x-scu"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/io_bridge.h"
#include "system/memory.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "hw/core/qdev-properties.h"

#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/dmac.h"
#include "hw/arm/pmb887x/pll.h"
#include "hw/arm/pmb887x/sccu.h"
#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_SCU	"pmb887x-scu"
#define PMB887X_SCU(obj)	OBJECT_CHECK(pmb887x_scu_t, (obj), TYPE_PMB887X_SCU)

typedef struct pmb887x_scu_t pmb887x_scu_t;

struct pmb887x_scu_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	
	pmb887x_src_reg_t dsp_src[5];
	pmb887x_src_reg_t unk_src[3];
	pmb887x_src_reg_t exti_src[8];
	
	qemu_irq exti_irq[8];
	qemu_irq dsp_irq[5];
	qemu_irq unk_irq[3];
	
	uint32_t cpu_type;
	uint32_t cpu_uid[3];
	uint32_t cpu_rev;
	
	uint32_t exti;
	uint32_t wdtcon0;
	uint32_t wdtcon1;
	uint32_t wdt_status;
	uint16_t wdt_counter;
	uint32_t wdt_frequency;
	int64_t wdt_start;
	uint32_t rst_status;
	uint32_t pending_reset_cause;
	bool stop_on_watchdog;
	uint32_t romamcr;
	uint32_t ebuclc;
	uint32_t ebuclc1;
	uint32_t ebuclc2;
	uint32_t rtcif;
	uint32_t boot_flag;
	uint32_t dmars;
	uint32_t rst_con;
	uint32_t rst_req;
	uint32_t boot_cfg;
	uint32_t dsp_unk0;

	uint32_t unk0;
	uint32_t scu_exti_unk;

	pmb887x_dmac_t *dmac;
	pmb887x_pll_t *pll;
	struct pmb887x_sccu_t *sccu;
	MemoryRegion *brom_mirror;
	QEMUTimer *wdt_timer;
};

static uint32_t scu_wdt_get_frequency(pmb887x_scu_t *p) {
	uint32_t divider = (p->wdt_status & SCU_WDT_SR_WDTIS) ? 256 : 16384;
	return pmb887x_pll_get_fsys(p->pll) / divider;
}

static uint16_t scu_wdt_get_counter(pmb887x_scu_t *p) {
	if ((p->wdt_status & SCU_WDT_SR_WDTDS) || !p->wdt_frequency)
		return p->wdt_counter;

	uint64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->wdt_start;
	uint64_t ticks = muldiv64(elapsed, p->wdt_frequency, NANOSECONDS_PER_SECOND);
	return p->wdt_counter + ticks;
}

static void scu_wdt_schedule(pmb887x_scu_t *p) {
	timer_del(p->wdt_timer);
	if ((p->wdt_status & SCU_WDT_SR_WDTDS) || !p->wdt_frequency)
		return;

	uint32_t ticks = 0x10000 - p->wdt_counter;
	int64_t duration = (int64_t) muldiv64(ticks, NANOSECONDS_PER_SECOND, p->wdt_frequency) + 1;

	p->wdt_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	timer_mod(p->wdt_timer, p->wdt_start + duration);
	DPRINTF("watchdog scheduled counter=%04X frequency=%u duration=%"PRId64" ns start=%"PRId64"\n",
		p->wdt_counter, p->wdt_frequency, duration, p->wdt_start);
}

static void scu_wdt_update_frequency(void *opaque) {
	pmb887x_scu_t *p = opaque;

	p->wdt_counter = scu_wdt_get_counter(p);
	p->wdt_frequency = scu_wdt_get_frequency(p);
	scu_wdt_schedule(p);
}

static void scu_wdt_enter_timeout(pmb887x_scu_t *p) {
	p->wdt_counter = 0xFFFC;
	p->wdt_status = (p->wdt_status & SCU_WDT_SR_WDTIS) | SCU_WDT_SR_WDTTO;
	scu_wdt_schedule(p);
}

static void scu_wdt_enter_requested_mode(pmb887x_scu_t *p) {
	p->wdt_status = 0;
	if ((p->wdtcon1 & SCU_WDTCON1_WDTIR))
		p->wdt_status |= SCU_WDT_SR_WDTIS;

	if ((p->wdtcon1 & SCU_WDTCON1_WDTDR)) {
		p->wdt_status |= SCU_WDT_SR_WDTDS;
	} else {
		p->wdt_counter = (p->wdtcon0 & SCU_WDTCON0_WDTREL) >> SCU_WDTCON0_WDTREL_SHIFT;
	}
	p->wdt_frequency = scu_wdt_get_frequency(p);
	DPRINTF("watchdog %s counter=%04X frequency=%u\n",
		(p->wdt_status & SCU_WDT_SR_WDTDS) ? "disabled" : "enabled",
		p->wdt_counter, p->wdt_frequency);
	scu_wdt_schedule(p);
}

static void scu_wdt_enter_prewarning(pmb887x_scu_t *p, uint32_t error) {
	p->wdt_status |= error | SCU_WDT_SR_WDTPR;
	p->wdt_status &= ~SCU_WDT_SR_WDTDS;
	p->wdt_counter = 0xFFFC;
	scu_wdt_schedule(p);
}

static void scu_wdt_timer_reset(void *opaque) {
	pmb887x_scu_t *p = opaque;
	DPRINTF("watchdog timer fired elapsed=%"PRId64" ns\n",
		qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->wdt_start);

	if ((p->wdt_status & SCU_WDT_SR_WDTPR)) {
		DPRINTF("watchdog reset\n");
		p->pending_reset_cause = SCU_RST_SR_WDTRST;
		if (p->stop_on_watchdog) {
			qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
			return;
		}
		qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
	} else {
		DPRINTF("watchdog overflow\n");
		scu_wdt_enter_prewarning(p, SCU_WDT_SR_WDTOE);
	}
}

static bool scu_wdt_password_is_valid(pmb887x_scu_t *p, uint32_t value) {
	uint32_t config = SCU_WDTCON0_ENDINIT | SCU_WDTCON0_WDTPW | SCU_WDTCON0_WDTREL;
	uint32_t expected = (
		(p->wdtcon0 & config) |
		SCU_WDTCON0_WDTHPW1 |
		(p->wdtcon1 & (SCU_WDTCON1_WDTIR | SCU_WDTCON1_WDTDR))
	);
	return value == expected;
}

static bool scu_wdt_modify_is_valid(uint32_t value) {
	uint32_t guards = SCU_WDTCON0_WDTLCK | SCU_WDTCON0_WDTHPW0 | SCU_WDTCON0_WDTHPW1;
	return (value & guards) == (SCU_WDTCON0_WDTLCK | SCU_WDTCON0_WDTHPW1);
}

static void scu_wdt_write_con0(pmb887x_scu_t *p, uint32_t value) {
	if ((p->wdt_status & SCU_WDT_SR_WDTPR)) {
		if (!scu_wdt_password_is_valid(p, value) && !scu_wdt_modify_is_valid(value))
			p->wdt_status |= SCU_WDT_SR_WDTAE;
		return;
	}

	if ((p->wdtcon0 & SCU_WDTCON0_WDTLCK)) {
		if (!scu_wdt_password_is_valid(p, value)) {
			scu_wdt_enter_prewarning(p, SCU_WDT_SR_WDTAE);
			return;
		}

		p->wdtcon0 &= ~SCU_WDTCON0_WDTLCK;
		if (!(p->wdt_status & SCU_WDT_SR_WDTTO))
			scu_wdt_enter_timeout(p);
		return;
	}

	if (!scu_wdt_modify_is_valid(value)) {
		scu_wdt_enter_prewarning(p, SCU_WDT_SR_WDTAE);
		return;
	}

	p->wdtcon0 = (
		(value & (SCU_WDTCON0_ENDINIT | SCU_WDTCON0_WDTPW | SCU_WDTCON0_WDTREL)) |
		SCU_WDTCON0_WDTLCK
	);
	if ((p->wdtcon0 & SCU_WDTCON0_ENDINIT))
		scu_wdt_enter_requested_mode(p);
}

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

		default:				abort();
	}
}

static uint64_t scu_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_scu_t *p = opaque;
	
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
				value = 0x1B00 | p->cpu_rev;
			} else if (p->cpu_type == CPU_PMB8875) {
				value = 0x1A00 | p->cpu_rev;
			}
			break;
		
		case SCU_RST_REQ:
			value = p->rst_req;
			break;
		
		case SCU_RST_SR:
			value = p->rst_status;
			break;

		case SCU_RST_CON:
			value = p->rst_con;
			break;
		
		case SCU_WDT_SR:
			value = p->wdt_status | ((uint32_t) scu_wdt_get_counter(p) << SCU_WDT_SR_WDTTIM_SHIFT);
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
			value = pmb887x_dmac_get_sel(p->dmac);
			break;
		
		case SCU_UID0 ... SCU_UID2:
			value = p->cpu_uid[(haddr - SCU_UID0) / 4];
			break;

		case SCU_EXTI_UNK:
			value = p->scu_exti_unk;
			break;

		case SCU_EXTI:
			value = p->exti;
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
			value = pmb887x_src_get(&p->exti_src[get_src_index_by_addr(haddr)]);
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

		case SCU_UNK0:
			value = p->unk0;
			break;

		default:
			IO_DUMP(haddr + p->mmio.addr, size, 0xFFFFFFFF, false);
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	IO_DUMP(haddr + p->mmio.addr, size, value, false);
	
	return value;
}

static void scu_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_scu_t *p = opaque;
	
	IO_DUMP(haddr + p->mmio.addr, size, value, true);
	
	switch (haddr) {
		case SCU_CLC:
			#if PMB887X_IO_BRIDGE
			pmb8876_io_bridge_write(haddr + p->mmio.addr, size, value);
			#endif
			pmb887x_sccu_clc_set(p->sccu, value);
		break;

		case SCU_RST_CON:
			if ((p->wdtcon0 & SCU_WDTCON0_ENDINIT))
				break;

			p->rst_con = value & (SCU_RST_CON_SWCFG | SCU_RST_CON_SWBRKIN | SCU_RST_CON_SWBOOT);
			p->pending_reset_cause = SCU_RST_SR_HDRST;
			qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
		break;
		
		case SCU_RST_REQ:
			p->rst_req = value;
		break;
		
		case SCU_WDTCON0:
			scu_wdt_write_con0(p, value);
		break;
		
		case SCU_WDTCON1:
			if (!(p->wdtcon0 & SCU_WDTCON0_ENDINIT) && !(p->wdt_status & SCU_WDT_SR_WDTPR))
				p->wdtcon1 = value & (SCU_WDTCON1_WDTIR | SCU_WDTCON1_WDTDR);
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
			pmb887x_dmac_set_sel(p->dmac, value);
		break;

		case SCU_EXTI_UNK:
			p->scu_exti_unk = value;
			break;

		case SCU_EXTI: {
			p->exti = value;
			DPRINTF("EXTI=%08"PRIX64"\n", value);
			for (uint32_t i = 0; i < ARRAY_SIZE(p->exti_irq); i++) {
				uint32_t falling = p->exti & (1 << (i * 2)) ? 1 : 0;
				uint32_t rising = p->exti & (1 << (i * 2 + 1)) ? 1 : 0;

				if (falling && rising) {
					DPRINTF("EXTI_%d: FALLING | RISING\n", i);
				} else if (falling) {
					DPRINTF("EXTI_%d: FALLING\n", i);
				} else if (rising) {
					DPRINTF("EXTI_%d: RISING\n", i);
				}
			}
			break;
		}
		
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
			pmb887x_src_set(&p->exti_src[get_src_index_by_addr(haddr)], value);
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

		case SCU_UNK0:
			p->unk0 = value;
			break;
		default:
			EPRINTF("unknown reg access: %02"PRIX64"\n", haddr);
			exit(1);
	}
	
	scu_update_state(p);
}

static void scu_handle_exti_change(pmb887x_scu_t *p, int id, int level) {
	DPRINTF("EXTI%d level changed %d\n", id, level);
}

static void scu_input_exti0_handler(void *opaque, int id, int level) {
	scu_handle_exti_change(opaque, 0, level);
}

static void scu_input_exti1_handler(void *opaque, int id, int level) {
	scu_handle_exti_change(opaque, 1, level);
}

static void scu_input_exti2_handler(void *opaque, int id, int level) {
	scu_handle_exti_change(opaque, 2, level);
}

static void scu_input_exti3_handler(void *opaque, int id, int level) {
	scu_handle_exti_change(opaque, 3, level);
}

static void scu_input_exti4_handler(void *opaque, int id, int level) {
	scu_handle_exti_change(opaque, 4, level);
}

static void scu_input_exti5_handler(void *opaque, int id, int level) {
	scu_handle_exti_change(opaque, 5, level);
}

static void scu_input_exti6_handler(void *opaque, int id, int level) {
	scu_handle_exti_change(opaque, 6, level);
}

static void scu_input_exti7_handler(void *opaque, int id, int level) {
	scu_handle_exti_change(opaque, 7, level);
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
	DeviceState *dev = DEVICE(obj);
	pmb887x_scu_t *p = PMB887X_SCU(obj);
	memory_region_init_io(&p->mmio, obj, &io_ops, p, "pmb887x-scu", SCU_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->exti_irq[0]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->exti_irq[1]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->exti_irq[2]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->exti_irq[3]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->exti_irq[4]);

	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->dsp_irq[0]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->dsp_irq[1]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->dsp_irq[2]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->dsp_irq[3]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->dsp_irq[4]);

	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->unk_irq[0]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->unk_irq[1]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->unk_irq[2]);

	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->exti_irq[5]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->exti_irq[6]);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->exti_irq[7]);

	qdev_init_gpio_in_named(dev, scu_input_exti0_handler, "EXTI0_IN", 1);
	qdev_init_gpio_in_named(dev, scu_input_exti1_handler, "EXTI1_IN", 1);
	qdev_init_gpio_in_named(dev, scu_input_exti2_handler, "EXTI2_IN", 1);
	qdev_init_gpio_in_named(dev, scu_input_exti3_handler, "EXTI3_IN", 1);
	qdev_init_gpio_in_named(dev, scu_input_exti4_handler, "EXTI4_IN", 1);
	qdev_init_gpio_in_named(dev, scu_input_exti5_handler, "EXTI5_IN", 1);
	qdev_init_gpio_in_named(dev, scu_input_exti6_handler, "EXTI6_IN", 1);
	qdev_init_gpio_in_named(dev, scu_input_exti7_handler, "EXTI7_IN", 1);
}

static void scu_reset(DeviceState *dev) {
	pmb887x_scu_t *p = PMB887X_SCU(dev);
	uint32_t reset_cause = p->pending_reset_cause ? p->pending_reset_cause : SCU_RST_SR_PWDRST;

	timer_del(p->wdt_timer);

	for (size_t i = 0; i < ARRAY_SIZE(p->exti_src); i++)
		pmb887x_src_reset(&p->exti_src[i]);
	for (size_t i = 0; i < ARRAY_SIZE(p->dsp_src); i++)
		pmb887x_src_reset(&p->dsp_src[i]);
	for (size_t i = 0; i < ARRAY_SIZE(p->unk_src); i++)
		pmb887x_src_reset(&p->unk_src[i]);

	p->exti = 0;

	p->wdtcon0 = SCU_WDTCON0_WDTLCK | (0xFFFC << SCU_WDTCON0_WDTREL_SHIFT);
	p->wdtcon1 = 0;
	p->wdt_counter = 0xFFFC;
	p->wdt_status = SCU_WDT_SR_WDTDS | SCU_WDT_SR_WDTTO;
	p->wdt_frequency = scu_wdt_get_frequency(p);
	p->wdt_start = 0;

	p->rst_status = (
		SCU_RST_SR_RSSTM |
		SCU_RST_SR_RSEXT |
		reset_cause
	);
	p->rst_req = 0;
	p->rst_con = 0;

	p->ebuclc = 0;
	p->ebuclc1 = 0;
	p->ebuclc2 = 0;
	p->rtcif = 0;
	p->boot_flag = 0;
	p->dmars = 0;
	p->boot_cfg = 0;
	p->dsp_unk0 = 0;
	p->unk0 = 0;
	p->scu_exti_unk = 0;
	p->pending_reset_cause = 0;

	DPRINTF("watchdog disabled (reset)\n");
	p->romamcr = SCU_ROMAMCR_MOUNT_BROM;

	scu_update_state(p);
}

static void scu_realize(DeviceState *dev, Error **errp) {
	pmb887x_scu_t *p = PMB887X_SCU(dev);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->exti_src); i++)
		pmb887x_src_init(&p->exti_src[i], p->exti_irq[i]);

	for (size_t i = 0; i < ARRAY_SIZE(p->dsp_src); i++)
		pmb887x_src_init(&p->dsp_src[i], p->dsp_irq[i]);
	
	for (size_t i = 0; i < ARRAY_SIZE(p->unk_src); i++)
		pmb887x_src_init(&p->unk_src[i], p->unk_irq[i]);
	
	p->wdt_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, scu_wdt_timer_reset, p);
	pmb887x_pll_add_freq_update_callback(p->pll, scu_wdt_update_frequency, p);
}

static const Property scu_properties[] = {
	DEFINE_PROP_UINT32("cpu_type", pmb887x_scu_t, cpu_type, 0),
	DEFINE_PROP_UINT32("cpu_rev", pmb887x_scu_t, cpu_rev, 0),
	DEFINE_PROP_UINT32("cpu_uid0", pmb887x_scu_t, cpu_uid[0], 0),
	DEFINE_PROP_UINT32("cpu_uid1", pmb887x_scu_t, cpu_uid[1], 0),
	DEFINE_PROP_UINT32("cpu_uid2", pmb887x_scu_t, cpu_uid[2], 0),
	DEFINE_PROP_BOOL("stop_on_watchdog", pmb887x_scu_t, stop_on_watchdog, false),
	DEFINE_PROP_LINK("pll", pmb887x_scu_t, pll, "pmb887x-pll", pmb887x_pll_t *),
	DEFINE_PROP_LINK("sccu", pmb887x_scu_t, sccu, "pmb887x-sccu", struct pmb887x_sccu_t *),
	DEFINE_PROP_LINK("dmac", pmb887x_scu_t, dmac, "pmb887x-dmac", pmb887x_dmac_t *),
	DEFINE_PROP_LINK("brom_mirror", pmb887x_scu_t, brom_mirror, TYPE_MEMORY_REGION, MemoryRegion *),
};

static void scu_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, scu_properties);
	device_class_set_legacy_reset(dc, scu_reset);
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
