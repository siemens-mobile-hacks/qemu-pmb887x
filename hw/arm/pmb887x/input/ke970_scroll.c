/*
 * LG KE970 scroll wheel.
 *
 * A discrete board circuit: two EM-0611 Hall switches (U601 / U602 on the LCD FPCB, both fed 
 * from V_SCROLL_2.8) sit under a magnetised dial, their outputs are 90 degrees out of phase 
 * - a 2-phase quadrature encoder.
 *
 * SCROLL_KEY_A -> DSPOUT0 (GPIO 15, IS=ALT2) -> CAPCOM1 CC6, rising-edge capture
 * SCROLL_KEY_B -> DSPOUT1 (GPIO 62)          -> sampled as a level only
 * */
#define PMB887X_TRACE_ID		KEYPAD
#define PMB887X_TRACE_PREFIX	"ke970-scroll"

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/timer.h"

#include "hw/arm/pmb887x/trace.h"

#define TYPE_PMB887X_KE970_SCROLL	"ke970-scroll"
#define PMB887X_KE970_SCROLL(obj)	OBJECT_CHECK(pmb887x_ke970_scroll_t, (obj), TYPE_PMB887X_KE970_SCROLL)

typedef struct pmb887x_ke970_scroll_t pmb887x_ke970_scroll_t;

struct pmb887x_ke970_scroll_t {
	SysBusDevice parent_obj;

	/* config */
	uint32_t steps;				// active edges of A per detent
	uint32_t edge_us;			// half period of A
	uint32_t repeat_delay_us;	// hold time before auto-repeat starts
	uint32_t detent_us;			// gap between detents once repeating

	/* wiring */
	qemu_irq a_out;
	qemu_irq b_out;

	/* runtime */
	QEMUTimer *timer;
	bool cw_held;
	bool ccw_held;
	int dir;				// +1 = CW, -1 = CCW, 0 = idle
	bool repeating;			// past the initial repeat delay
	uint32_t toggles_left;	// remaining A transitions in the current detent
	bool a_level;
};

static void ke970_scroll_set_a(pmb887x_ke970_scroll_t *p, bool level) {
	p->a_level = level;
	qemu_set_irq(p->a_out, level);
}

static void ke970_scroll_stop(pmb887x_ke970_scroll_t *p) {
	timer_del(p->timer);
	p->dir = 0;
	p->toggles_left = 0;
	ke970_scroll_set_a(p, false);
}

static void ke970_scroll_arm(pmb887x_ke970_scroll_t *p, uint32_t delay_us) {
	timer_mod_ns(p->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t) delay_us * 1000);
}

static int ke970_scroll_held_dir(pmb887x_ke970_scroll_t *p) {
	return p->cw_held ? 1 : (p->ccw_held ? -1 : 0);
}

/* Begin one detent: park B for the direction, then pulse A `steps` times. */
static void ke970_scroll_start_detent(pmb887x_ke970_scroll_t *p, int dir) {
	p->dir = dir;
	p->toggles_left = p->steps * 2;

	/* B must be settled before the first active edge of A. */
	qemu_set_irq(p->b_out, dir > 0);
	ke970_scroll_set_a(p, false);

	DPRINTF("detent %s, %u pulses\n", dir > 0 ? "CW" : "CCW", p->steps);
	ke970_scroll_arm(p, p->edge_us);
}

static void ke970_scroll_timer_cb(void *opaque) {
	pmb887x_ke970_scroll_t *p = opaque;

	if (p->toggles_left > 0) {
		ke970_scroll_set_a(p, !p->a_level);
		p->toggles_left--;

		if (p->toggles_left > 0) {
			ke970_scroll_arm(p, p->edge_us);
			return;
		}

		/*
		 * Burst done. If the key is still held, auto-repeat: a long delay after
		 * the first detent, then a steady slower rate, like keyboard typematic.
		 */
		p->dir = ke970_scroll_held_dir(p);
		if (!p->dir) {
			p->repeating = false;
			return;
		}

		ke970_scroll_arm(p, p->repeating ? p->detent_us : p->repeat_delay_us);
		p->repeating = true;
		return;
	}

	/* Repeat delay elapsed. */
	int dir = ke970_scroll_held_dir(p);
	if (!dir) {
		p->dir = 0;
		p->repeating = false;
		return;
	}
	ke970_scroll_start_detent(p, dir);
}

static void ke970_scroll_update(pmb887x_ke970_scroll_t *p) {
	int dir = ke970_scroll_held_dir(p);

	/*
	 * A burst in flight always runs to completion so the firmware only ever
	 * sees whole detents; the timer callback picks up the new state at the end.
	 */
	if (dir && p->dir == 0) {
		p->repeating = false;
		ke970_scroll_start_detent(p, dir);
	}
}

static void ke970_scroll_cw_handler(void *opaque, int id, int level) {
	pmb887x_ke970_scroll_t *p = opaque;
	if (p->cw_held == !!level)
		return;
	p->cw_held = !!level;
	ke970_scroll_update(p);
}

static void ke970_scroll_ccw_handler(void *opaque, int id, int level) {
	pmb887x_ke970_scroll_t *p = opaque;
	if (p->ccw_held == !!level)
		return;
	p->ccw_held = !!level;
	ke970_scroll_update(p);
}

static void ke970_scroll_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_ke970_scroll_t *p = PMB887X_KE970_SCROLL(obj);

	qdev_init_gpio_out_named(dev, &p->a_out, "A_OUT", 1);
	qdev_init_gpio_out_named(dev, &p->b_out, "B_OUT", 1);
	qdev_init_gpio_in_named(dev, ke970_scroll_cw_handler, "CW_IN", 1);
	qdev_init_gpio_in_named(dev, ke970_scroll_ccw_handler, "CCW_IN", 1);
}

static void ke970_scroll_realize(DeviceState *dev, Error **errp) {
	pmb887x_ke970_scroll_t *p = PMB887X_KE970_SCROLL(dev);

	if (!p->steps) {
		error_setg(errp, "ke970-scroll: 'steps' must be non-zero");
		return;
	}

	if (!p->edge_us || !p->detent_us || !p->repeat_delay_us) {
		error_setg(errp, "ke970-scroll: 'edge_us', 'detent_us' and 'repeat_delay_us' must be non-zero");
		return;
	}

	p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ke970_scroll_timer_cb, p);
}

static void ke970_scroll_unrealize(DeviceState *dev) {
	pmb887x_ke970_scroll_t *p = PMB887X_KE970_SCROLL(dev);
	timer_free(p->timer);
	p->timer = NULL;
}

static void ke970_scroll_reset(DeviceState *dev) {
	pmb887x_ke970_scroll_t *p = PMB887X_KE970_SCROLL(dev);

	p->cw_held = false;
	p->ccw_held = false;
	p->repeating = false;
	if (p->timer)
		ke970_scroll_stop(p);
	qemu_set_irq(p->b_out, 0);
}

static const Property ke970_scroll_properties[] = {
	DEFINE_PROP_UINT32("steps", pmb887x_ke970_scroll_t, steps, 2),
	DEFINE_PROP_UINT32("edge_us", pmb887x_ke970_scroll_t, edge_us, 2000),
	DEFINE_PROP_UINT32("repeat_delay_us", pmb887x_ke970_scroll_t, repeat_delay_us, 400000),
	DEFINE_PROP_UINT32("detent_us", pmb887x_ke970_scroll_t, detent_us, 200000),
};

static void ke970_scroll_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);
	device_class_set_props(dc, ke970_scroll_properties);
	device_class_set_legacy_reset(dc, ke970_scroll_reset);
	dc->realize = ke970_scroll_realize;
	dc->unrealize = ke970_scroll_unrealize;
}

static const TypeInfo ke970_scroll_info = {
	.name			= TYPE_PMB887X_KE970_SCROLL,
	.parent			= TYPE_SYS_BUS_DEVICE,
	.instance_size	= sizeof(pmb887x_ke970_scroll_t),
	.instance_init	= ke970_scroll_init,
	.class_init		= ke970_scroll_class_init,
};

static void ke970_scroll_register_types(void) {
	type_register_static(&ke970_scroll_info);
}
type_init(ke970_scroll_register_types)
