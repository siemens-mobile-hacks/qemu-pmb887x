#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"

#define TYPE_PMB887X_USB "pmb887x-usb"
#define PMB887X_USB(obj) OBJECT_CHECK(pmb887x_usb_t, (obj), TYPE_PMB887X_USB)

#define USB_ID_BASE 0xF047C000
#define USB_ENDPOINT_COUNT 11
#define USB_SLOT_COUNT 12
#define USB_EP0_OUT_SLOT 0
#define USB_EP0_IN_SLOT 1
#define USB_EP_LOW_COUNT 8

#define USB_EP_CONFIG_STRIDE (USB_EP_CONFIG1 - USB_EP_CONFIG0)
#define USB_SLOT_STRIDE (USB_EP_DATA1 - USB_EP_DATA0)
#define USB_EP_MAX_PACKET_STRIDE (USB_EP_MAX_PACKET_HIGH1 - USB_EP_MAX_PACKET_HIGH0)
#define USB_EP_FIFO_STRIDE (USB_EP_FIFO_START1 - USB_EP_FIFO_START0)
#define USB_EP_COUNT_STRIDE (USB_EP_COUNT1 - USB_EP_COUNT0)

#define USB_COUNT_MAX ((USB_EP_COUNT_HIGH_VALUE << 8) | USB_EP_COUNT_LOW_VALUE)
#define USB_FIFO_SIZE_UNIT 16
#define USB_FIFO_EMPTY_DATA 0xFF

#define USB_SUSPEND_CYCLES 144000
#define USB_IDLE_CYCLES 96000
#define USB_REMOTE_WAKEUP_K_CYCLES 256000
#define USB_BUS_RESET_DETECT_NS 14000

typedef struct pmb887x_usb_t pmb887x_usb_t;
typedef struct pmb887x_usb_slot_t pmb887x_usb_slot_t;
typedef struct pmb887x_usb_endpoint_t pmb887x_usb_endpoint_t;
typedef enum pmb887x_usb_bus_phase_t pmb887x_usb_bus_phase_t;

enum pmb887x_usb_bus_phase_t {
	USB_BUS_OFF,
	USB_BUS_WAIT_SUSPEND,
	USB_BUS_WAIT_IDLE,
	USB_BUS_IDLE,
	USB_BUS_DRIVE_K,
};

struct pmb887x_usb_slot_t {
	uint8_t config;
	uint8_t control;
	uint16_t used_bytes;
	bool armed;
};

struct pmb887x_usb_endpoint_t {
	uint8_t max_packet_high;
	uint8_t max_packet_low;
	uint8_t fifo_start;
	uint8_t fifo_size;
};

struct pmb887x_usb_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	DebugChannel dbg;
	uint32_t revision;

	qemu_irq irq;
	pmb887x_clc_reg_t clc;

	uint32_t cfg;
	uint8_t control;
	uint8_t device_address;
	uint8_t ep_enable_low;
	uint8_t ep_enable_high;

	uint8_t global_status;
	uint8_t global_enable;
	uint8_t event_status;
	uint8_t event_enable;
	uint8_t dma0_enable;
	uint8_t dma1_enable;
	uint8_t ep_a_status_low;
	uint8_t ep_a_status_high;
	uint8_t ep_a_enable_low;
	uint8_t ep_a_enable_high;
	uint8_t ep_b_status_low;
	uint8_t ep_b_status_high;
	uint8_t ep_b_enable_low;
	uint8_t ep_b_enable_high;

	pmb887x_usb_slot_t slots[USB_SLOT_COUNT];
	pmb887x_usb_endpoint_t endpoints[USB_ENDPOINT_COUNT];

	bool resume;
	bool wakeup_end;
	bool suspended;
	pmb887x_usb_bus_phase_t bus_phase;
	int64_t bus_phase_start_ns;
	uint32_t bus_phase_cycles;
	uint32_t bus_phase_hz;
	QEMUTimer *bus_timer;
	QEMUTimer *bus_reset_timer;
};

static const uint8_t USB_SLOT_CONFIG_RESET[USB_SLOT_COUNT] = {
	0x24, 0x2C, 0x28, 0x20, 0x28, 0x20, 0x28, 0x20, 0x28, 0x20, 0x28, 0x20,
};

static uint32_t usb_slot_to_endpoint(uint32_t slot) {
	return slot == USB_EP0_OUT_SLOT ? 0 : slot - 1;
}

static bool usb_slot_is_in(uint32_t slot) {
	return (USB_SLOT_CONFIG_RESET[slot] & USB_EP_CONFIG_IN) != 0;
}

static uint32_t usb_slot_fifo_bytes(pmb887x_usb_t *p, uint32_t slot) {
	uint32_t code = (p->endpoints[usb_slot_to_endpoint(slot)].fifo_size & USB_EP_FIFO_SIZE_SIZE) >>
		USB_EP_FIFO_SIZE_SIZE_SHIFT;
	return code ? USB_FIFO_SIZE_UNIT << code : 0;
}

static uint32_t usb_slot_get_count(pmb887x_usb_t *p, uint32_t slot) {
	uint32_t used_bytes = p->slots[slot].used_bytes;
	if (!usb_slot_is_in(slot))
		return used_bytes;

	uint32_t size = usb_slot_fifo_bytes(p, slot);
	uint32_t free_bytes = size > used_bytes ? size - used_bytes : 0;
	return MIN(free_bytes, USB_COUNT_MAX);
}

static uint32_t usb_slot_get_level(pmb887x_usb_t *p, uint32_t slot) {
	uint32_t size = usb_slot_fifo_bytes(p, slot);
	uint32_t used_bytes = p->slots[slot].used_bytes;

	if (size == 0) {
		if (usb_slot_is_in(slot))
			return USB_EP_COUNT_HIGH_FULL | USB_EP_COUNT_HIGH_HIGH_LEVEL;
		return USB_EP_COUNT_HIGH_EMPTY | USB_EP_COUNT_HIGH_LOW_LEVEL;
	}

	uint32_t level = 0;
	if (used_bytes == 0)
		level |= USB_EP_COUNT_HIGH_EMPTY;
	if (used_bytes >= size)
		level |= USB_EP_COUNT_HIGH_FULL;
	if (used_bytes * 4 <= size)
		level |= USB_EP_COUNT_HIGH_LOW_LEVEL;
	if (used_bytes * 4 >= size * 3)
		level |= USB_EP_COUNT_HIGH_HIGH_LEVEL;
	return level;
}

static uint32_t usb_slot_get_count_high(pmb887x_usb_t *p, uint32_t slot) {
	uint32_t value = ((usb_slot_get_count(p, slot) >> 8) & USB_EP_COUNT_HIGH_VALUE) | usb_slot_get_level(p, slot);
	if (p->slots[slot].armed)
		value |= USB_EP_COUNT_HIGH_ARMED;
	return value;
}

static uint32_t usb_slot_get_packed_count(pmb887x_usb_t *p, uint32_t slot) {
	return (usb_slot_get_count_high(p, slot) << 8) | (usb_slot_get_count(p, slot) & USB_EP_COUNT_LOW_VALUE);
}

static bool usb_endpoint_is_enabled(pmb887x_usb_t *p, uint32_t endpoint) {
	if (endpoint < USB_EP_LOW_COUNT)
		return (p->ep_enable_low & BIT(endpoint)) != 0;
	return (p->ep_enable_high & BIT(endpoint - USB_EP_LOW_COUNT)) != 0;
}

static void usb_update_irq(pmb887x_usb_t *p) {
	qemu_set_irq(p->irq, (p->global_status & p->global_enable) != 0);
}

static void usb_raise_event(pmb887x_usb_t *p, uint32_t events) {
	p->event_status |= events;
	if (p->event_enable & events)
		p->global_status |= USB_GLOBAL_INT_STATUS_EVENT;
	usb_update_irq(p);
}

static void usb_raise_ep_a(pmb887x_usb_t *p, uint32_t endpoint) {
	if (endpoint < USB_EP_LOW_COUNT) {
		p->ep_a_status_low |= BIT(endpoint);
		if (p->ep_a_enable_low & BIT(endpoint))
			p->global_status |= USB_GLOBAL_INT_STATUS_EP_A_LOW;
	} else {
		p->ep_a_status_high |= BIT(endpoint - USB_EP_LOW_COUNT);
		if (p->ep_a_enable_high & BIT(endpoint - USB_EP_LOW_COUNT))
			p->global_status |= USB_GLOBAL_INT_STATUS_EP_A_HIGH;
	}
	usb_update_irq(p);
}

static void usb_bus_timer_arm(pmb887x_usb_t *p) {
	p->bus_phase_hz = pmb887x_clc_get_hz(&p->clc);
	if (p->bus_phase_hz == 0) {
		timer_del(p->bus_timer);
		return;
	}
	uint64_t duration_ns = muldiv64(p->bus_phase_cycles, NANOSECONDS_PER_SECOND, p->bus_phase_hz);
	timer_mod(p->bus_timer, p->bus_phase_start_ns + duration_ns);
}

static void usb_bus_start_phase(pmb887x_usb_t *p, pmb887x_usb_bus_phase_t phase, uint32_t cycles) {
	p->bus_phase = phase;
	p->bus_phase_cycles = cycles;
	p->bus_phase_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	usb_bus_timer_arm(p);
}

static void usb_bus_drive_k(pmb887x_usb_t *p) {
	p->suspended = false;
	p->wakeup_end = true;
	usb_bus_start_phase(p, USB_BUS_DRIVE_K, USB_REMOTE_WAKEUP_K_CYCLES);
}

static void usb_bus_update(pmb887x_usb_t *p) {
	bool active = (p->cfg & USB_CFG_PHY_ENABLE) && (p->control & USB_CONTROL_ENABLE);
	if (!active) {
		p->bus_phase = USB_BUS_OFF;
		p->suspended = false;
		timer_del(p->bus_timer);
		return;
	}
	if (p->bus_phase == USB_BUS_OFF)
		usb_bus_start_phase(p, USB_BUS_WAIT_SUSPEND, USB_SUSPEND_CYCLES);
}

static void usb_bus_timer_expired(void *opaque) {
	pmb887x_usb_t *p = opaque;

	switch (p->bus_phase) {
		case USB_BUS_WAIT_SUSPEND:
			p->suspended = true;
			usb_raise_event(p, USB_EVENT_INT_STATUS_SUSPEND);
			usb_bus_start_phase(p, USB_BUS_WAIT_IDLE, USB_IDLE_CYCLES);
			break;

		case USB_BUS_WAIT_IDLE:
			p->bus_phase = USB_BUS_IDLE;
			usb_raise_event(p, USB_EVENT_INT_STATUS_IDLE);
			if (p->resume)
				usb_bus_drive_k(p);
			break;

		case USB_BUS_DRIVE_K:
			usb_bus_start_phase(p, USB_BUS_WAIT_SUSPEND, USB_SUSPEND_CYCLES);
			break;

		case USB_BUS_OFF:
		case USB_BUS_IDLE:
			break;
	}
}

static void usb_clock_update(void *opaque) {
	pmb887x_usb_t *p = opaque;

	bool timed = p->bus_phase == USB_BUS_WAIT_SUSPEND ||
		p->bus_phase == USB_BUS_WAIT_IDLE ||
		p->bus_phase == USB_BUS_DRIVE_K;
	if (!timed)
		return;

	int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	uint64_t elapsed_cycles = muldiv64(now_ns - p->bus_phase_start_ns, p->bus_phase_hz, NANOSECONDS_PER_SECOND);
	p->bus_phase_cycles -= MIN(elapsed_cycles, p->bus_phase_cycles);
	p->bus_phase_start_ns = now_ns;
	usb_bus_timer_arm(p);
}

static void usb_bus_reset(pmb887x_usb_t *p) {
	p->device_address &= USB_DEVICE_ADDRESS_ENABLE;
	p->ep_enable_low = BIT(0);
	p->ep_enable_high = 0;
	usb_raise_event(p, USB_EVENT_INT_STATUS_RESET);
}

static void usb_bus_reset_timer_expired(void *opaque) {
	pmb887x_usb_t *p = opaque;

	if (!(p->cfg & USB_CFG_PHY_ENABLE) && (p->control & USB_CONTROL_ENABLE))
		usb_bus_reset(p);
}

static void usb_bus_reset_detect_start(pmb887x_usb_t *p) {
	timer_mod(p->bus_reset_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + USB_BUS_RESET_DETECT_NS);
}

static uint32_t usb_get_line_state(pmb887x_usb_t *p) {
	if (!(p->cfg & USB_CFG_PHY_ENABLE))
		return USB_PHY_CONTROL_LINE_STATE_SE0;
	bool drive_enabled = p->revision == 0 || (p->cfg & USB_CFG_DRIVE_ENABLE);
	if (p->bus_phase == USB_BUS_DRIVE_K && drive_enabled)
		return USB_PHY_CONTROL_LINE_STATE_K;
	return USB_PHY_CONTROL_LINE_STATE_J;
}

static uint32_t usb_get_phy_control(pmb887x_usb_t *p) {
	uint32_t value = usb_get_line_state(p);
	if (p->suspended)
		value |= USB_PHY_CONTROL_SUSPEND;
	if (p->wakeup_end)
		value |= USB_PHY_CONTROL_WAKEUP_END;
	if (p->resume)
		value |= USB_PHY_CONTROL_RESUME;
	return value;
}

static void usb_set_phy_control(pmb887x_usb_t *p, uint32_t value) {
	p->resume = (value & USB_PHY_CONTROL_RESUME) != 0;
	if (!p->resume)
		return;

	if (!(p->cfg & USB_CFG_PHY_ENABLE)) {
		usb_raise_event(p, USB_EVENT_INT_STATUS_WAKEUP_DONE);
		return;
	}
	if (p->bus_phase == USB_BUS_IDLE)
		usb_bus_drive_k(p);
}

static void usb_set_cfg(pmb887x_usb_t *p, uint32_t value) {
	bool phy_disabled = (p->cfg & USB_CFG_PHY_ENABLE) && !(value & USB_CFG_PHY_ENABLE);

	p->cfg = value;
	if (phy_disabled)
		usb_bus_reset_detect_start(p);
	usb_bus_update(p);
}

static void usb_slot_set_config(pmb887x_usb_t *p, uint32_t slot, uint32_t value) {
	uint32_t fixed_bits = USB_EP_CONFIG_CONTROL | USB_EP_CONFIG_IN;

	p->slots[slot].config = (USB_SLOT_CONFIG_RESET[slot] & fixed_bits) | (value & ~fixed_bits);
}

static void usb_slot_set_control(pmb887x_usb_t *p, uint32_t slot, uint32_t value) {
	pmb887x_usb_slot_t *s = &p->slots[slot];
	uint32_t endpoint = usb_slot_to_endpoint(slot);

	s->control = value & ~(USB_EP_CONTROL_FLUSH | USB_EP_CONTROL_ARM);

	if (value & USB_EP_CONTROL_FLUSH) {
		bool queued_in_data = usb_slot_is_in(slot) && s->used_bytes && (s->control & USB_EP_CONTROL_IN_ENABLE);
		s->used_bytes = 0;
		if (queued_in_data)
			usb_raise_ep_a(p, endpoint);
	}
	if ((value & USB_EP_CONTROL_ARM) && !usb_slot_is_in(slot) && usb_endpoint_is_enabled(p, endpoint))
		s->armed = true;
}

static void usb_slot_write_data(pmb887x_usb_t *p, uint32_t slot) {
	pmb887x_usb_slot_t *s = &p->slots[slot];

	if (usb_slot_is_in(slot) && s->used_bytes < usb_slot_fifo_bytes(p, slot))
		s->used_bytes++;
}

static void usb_reset_core(pmb887x_usb_t *p) {
	for (uint32_t slot = 0; slot < USB_SLOT_COUNT; slot++) {
		p->slots[slot].config = USB_SLOT_CONFIG_RESET[slot];
		p->slots[slot].control = 0;
		p->slots[slot].armed = false;
	}
	for (uint32_t endpoint = 0; endpoint < USB_ENDPOINT_COUNT; endpoint++) {
		p->endpoints[endpoint].max_packet_high = 0;
		p->endpoints[endpoint].max_packet_low = 0;
	}
	p->global_status = 0;
	p->global_enable = 0;
	p->event_status = 0;
	p->event_enable = 0;
	p->dma0_enable = 0;
	p->dma1_enable = 0;
	p->ep_a_enable_low = 0;
	p->ep_a_enable_high = 0;
	p->ep_b_enable_low = 0;
	p->ep_b_enable_high = 0;
	usb_update_irq(p);
}

static void usb_set_control(pmb887x_usb_t *p, uint32_t value) {
	bool core_turned_on = !(p->control & USB_CONTROL_ENABLE) && (value & USB_CONTROL_ENABLE);

	p->control = value;
	if (!(p->control & USB_CONTROL_ENABLE)) {
		usb_reset_core(p);
	} else if (core_turned_on && !(p->cfg & USB_CFG_PHY_ENABLE)) {
		usb_bus_reset_detect_start(p);
	}
	usb_bus_update(p);
}

static void usb_reset_state(pmb887x_usb_t *p) {
	timer_del(p->bus_timer);
	timer_del(p->bus_reset_timer);

	memset(p->slots, 0, sizeof(p->slots));
	memset(p->endpoints, 0, sizeof(p->endpoints));
	p->endpoints[0].fifo_size = 1 << USB_EP_FIFO_SIZE_SIZE_SHIFT;
	p->cfg = 0;
	p->control = USB_CONTROL_ENABLE;
	p->device_address = 0;
	p->ep_enable_low = 0;
	p->ep_enable_high = 0;
	p->ep_a_status_low = 0;
	p->ep_a_status_high = 0;
	p->ep_b_status_low = 0;
	p->ep_b_status_high = 0;
	p->resume = false;
	p->wakeup_end = false;
	p->suspended = false;
	p->bus_phase = USB_BUS_OFF;
	usb_reset_core(p);
	p->event_status = USB_EVENT_INT_STATUS_RESET;
}

static void usb_reset_input(void *opaque, int _id, int level) {
	pmb887x_usb_t *p = opaque;

	if (!level)
		return;
	usb_reset_state(p);
	if (p->revision == 0)
		pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
}

static uint64_t usb_io_read(void *opaque, hwaddr haddr, unsigned _size) {
	pmb887x_usb_t *p = opaque;

	bool core_register = haddr < USB_CLC && haddr != USB_CONTROL;
	if (core_register && !(p->control & USB_CONTROL_ENABLE))
		return 0;

	switch (haddr) {
		case USB_EP_ENABLE_LOW:
			return p->ep_enable_low;

		case USB_EP_ENABLE_HIGH:
			return p->ep_enable_high;

		case USB_DEVICE_ADDRESS:
			return p->device_address;

		case USB_CONTROL:
			return p->control;

		case USB_EP_CONFIG0 ... USB_EP_CONFIG11:
			if ((haddr - USB_EP_CONFIG0) % USB_EP_CONFIG_STRIDE == 0)
				return p->slots[(haddr - USB_EP_CONFIG0) / USB_EP_CONFIG_STRIDE].config;
			return 0;

		case USB_EP_MAX_PACKET_HIGH0 ... USB_EP_MAX_PACKET_LOW10: {
			uint32_t offset = haddr - USB_EP_MAX_PACKET_HIGH0;
			pmb887x_usb_endpoint_t *ep = &p->endpoints[offset / USB_EP_MAX_PACKET_STRIDE];
			switch (offset % USB_EP_MAX_PACKET_STRIDE) {
				case USB_EP_MAX_PACKET_HIGH0 - USB_EP_MAX_PACKET_HIGH0:
					return ep->max_packet_high;
				case USB_EP_MAX_PACKET_LOW0 - USB_EP_MAX_PACKET_HIGH0:
					return ep->max_packet_low;
			}
			return 0;
		}

		case USB_GLOBAL_INT_STATUS:
			return p->global_status;

		case USB_GLOBAL_INT_ENABLE:
			return p->global_enable;

		case USB_DMA0_INT_ENABLE:
			return p->dma0_enable;

		case USB_DMA1_INT_ENABLE:
			return p->dma1_enable;

		case USB_EVENT_INT_STATUS:
			return p->event_status;

		case USB_EVENT_INT_ENABLE:
			return p->event_enable;

		case USB_EP_A_INT_STATUS_LOW:
			return p->ep_a_status_low;

		case USB_EP_A_INT_ENABLE_LOW:
			return p->ep_a_enable_low;

		case USB_EP_A_INT_STATUS_HIGH:
			return p->ep_a_status_high;

		case USB_EP_A_INT_ENABLE_HIGH:
			return p->ep_a_enable_high;

		case USB_EP_B_INT_STATUS_LOW:
			return p->ep_b_status_low;

		case USB_EP_B_INT_ENABLE_LOW:
			return p->ep_b_enable_low;

		case USB_EP_B_INT_STATUS_HIGH:
			return p->ep_b_status_high;

		case USB_EP_B_INT_ENABLE_HIGH:
			return p->ep_b_enable_high;

		case USB_EP_DATA0 ... USB_EP_COUNT_HIGH11: {
			uint32_t slot = (haddr - USB_EP_DATA0) / USB_SLOT_STRIDE;
			switch ((haddr - USB_EP_DATA0) % USB_SLOT_STRIDE) {
				case USB_EP_DATA0 - USB_EP_DATA0:
					return USB_FIFO_EMPTY_DATA;
				case USB_EP_CONTROL0 - USB_EP_DATA0:
					return p->slots[slot].control;
				case USB_EP_COUNT_LOW0 - USB_EP_DATA0:
					return usb_slot_get_count(p, slot) & USB_EP_COUNT_LOW_VALUE;
				case USB_EP_COUNT_HIGH0 - USB_EP_DATA0:
					return usb_slot_get_count_high(p, slot);
			}
			return 0;
		}

		case USB_PHY_CONTROL:
			return usb_get_phy_control(p);

		case USB_EP_FIFO_START0 ... USB_EP_FIFO_SIZE10: {
			uint32_t offset = haddr - USB_EP_FIFO_START0;
			pmb887x_usb_endpoint_t *ep = &p->endpoints[offset / USB_EP_FIFO_STRIDE];
			switch (offset % USB_EP_FIFO_STRIDE) {
				case USB_EP_FIFO_START0 - USB_EP_FIFO_START0:
					return ep->fifo_start;
				case USB_EP_FIFO_SIZE0 - USB_EP_FIFO_START0:
					return ep->fifo_size;
			}
			return 0;
		}

		case USB_GLOBAL_EVENT_INT_STATUS:
			return p->global_status | (p->event_status << USB_GLOBAL_EVENT_INT_STATUS_EVENT_SHIFT);

		case USB_DMA_EP_A_INT_STATUS:
			return p->ep_a_status_low << USB_DMA_EP_A_INT_STATUS_EP_A_LOW_SHIFT;

		case USB_GLOBAL_EVENT_EP_B_INT_ENABLE:
			return p->global_enable |
				(p->event_enable << USB_GLOBAL_EVENT_EP_B_INT_ENABLE_EVENT_SHIFT) |
				(p->ep_b_enable_low << USB_GLOBAL_EVENT_EP_B_INT_ENABLE_EP_B_LOW_SHIFT) |
				(p->ep_b_enable_high << USB_GLOBAL_EVENT_EP_B_INT_ENABLE_EP_B_HIGH_SHIFT);

		case USB_DMA_EP_A_INT_ENABLE:
			return p->dma0_enable |
				(p->dma1_enable << USB_DMA_EP_A_INT_ENABLE_DMA1_SHIFT) |
				(p->ep_a_enable_low << USB_DMA_EP_A_INT_ENABLE_EP_A_LOW_SHIFT) |
				(p->ep_a_enable_high << USB_DMA_EP_A_INT_ENABLE_EP_A_HIGH_SHIFT);

		case USB_EP0_COUNT:
			return usb_slot_get_packed_count(p, USB_EP0_OUT_SLOT) |
				(usb_slot_get_packed_count(p, USB_EP0_IN_SLOT) << USB_EP0_COUNT_IN_SHIFT);

		case USB_EP_COUNT0 ... USB_EP_COUNT9:
			if ((haddr - USB_EP_COUNT0) % USB_EP_COUNT_STRIDE == 0)
				return usb_slot_get_packed_count(p, (haddr - USB_EP_COUNT0) / USB_EP_COUNT_STRIDE + 2);
			return 0;

		case USB_CLC:
			return pmb887x_clc_get(&p->clc);

		case USB_CFG:
			return p->cfg;

		case USB_ID:
			return USB_ID_BASE | p->revision;
	}

	return 0;
}

static void usb_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned _size) {
	pmb887x_usb_t *p = opaque;

	switch (haddr) {
		case USB_EP_ENABLE_LOW:
			p->ep_enable_low = value;
			break;

		case USB_EP_ENABLE_HIGH:
			p->ep_enable_high = value;
			break;

		case USB_DEVICE_ADDRESS:
			p->device_address = value;
			break;

		case USB_CONTROL:
			usb_set_control(p, value);
			break;

		case USB_EP_CONFIG0 ... USB_EP_CONFIG11:
			if ((haddr - USB_EP_CONFIG0) % USB_EP_CONFIG_STRIDE == 0)
				usb_slot_set_config(p, (haddr - USB_EP_CONFIG0) / USB_EP_CONFIG_STRIDE, value);
			break;

		case USB_EP_MAX_PACKET_HIGH0 ... USB_EP_MAX_PACKET_LOW10: {
			uint32_t offset = haddr - USB_EP_MAX_PACKET_HIGH0;
			pmb887x_usb_endpoint_t *ep = &p->endpoints[offset / USB_EP_MAX_PACKET_STRIDE];
			switch (offset % USB_EP_MAX_PACKET_STRIDE) {
				case USB_EP_MAX_PACKET_HIGH0 - USB_EP_MAX_PACKET_HIGH0:
					ep->max_packet_high = value;
					break;
				case USB_EP_MAX_PACKET_LOW0 - USB_EP_MAX_PACKET_HIGH0:
					ep->max_packet_low = value;
					break;
			}
			break;
		}

		case USB_GLOBAL_INT_STATUS:
			p->global_status &= ~value;
			usb_update_irq(p);
			break;

		case USB_GLOBAL_INT_ENABLE:
			p->global_enable = value;
			usb_update_irq(p);
			break;

		case USB_DMA0_INT_ENABLE:
			p->dma0_enable = value;
			break;

		case USB_DMA1_INT_ENABLE:
			p->dma1_enable = value;
			break;

		case USB_EVENT_INT_STATUS:
			p->event_status &= ~value;
			break;

		case USB_EVENT_INT_ENABLE:
			p->event_enable = value;
			break;

		case USB_EP_A_INT_STATUS_LOW:
			p->ep_a_status_low &= ~value;
			break;

		case USB_EP_A_INT_ENABLE_LOW:
			p->ep_a_enable_low = value;
			break;

		case USB_EP_A_INT_STATUS_HIGH:
			p->ep_a_status_high &= ~value;
			break;

		case USB_EP_A_INT_ENABLE_HIGH:
			p->ep_a_enable_high = value;
			break;

		case USB_EP_B_INT_STATUS_LOW:
			p->ep_b_status_low &= ~value;
			break;

		case USB_EP_B_INT_ENABLE_LOW:
			p->ep_b_enable_low = value;
			break;

		case USB_EP_B_INT_STATUS_HIGH:
			p->ep_b_status_high &= ~value;
			break;

		case USB_EP_B_INT_ENABLE_HIGH:
			p->ep_b_enable_high = value;
			break;

		case USB_EP_DATA0 ... USB_EP_COUNT_HIGH11: {
			uint32_t slot = (haddr - USB_EP_DATA0) / USB_SLOT_STRIDE;
			switch ((haddr - USB_EP_DATA0) % USB_SLOT_STRIDE) {
				case USB_EP_DATA0 - USB_EP_DATA0:
					usb_slot_write_data(p, slot);
					break;
				case USB_EP_CONTROL0 - USB_EP_DATA0:
					usb_slot_set_control(p, slot, value);
					break;
			}
			break;
		}

		case USB_PHY_CONTROL:
			usb_set_phy_control(p, value);
			break;

		case USB_EP_FIFO_START0 ... USB_EP_FIFO_SIZE10: {
			uint32_t offset = haddr - USB_EP_FIFO_START0;
			pmb887x_usb_endpoint_t *ep = &p->endpoints[offset / USB_EP_FIFO_STRIDE];
			switch (offset % USB_EP_FIFO_STRIDE) {
				case USB_EP_FIFO_START0 - USB_EP_FIFO_START0:
					ep->fifo_start = value;
					break;
				case USB_EP_FIFO_SIZE0 - USB_EP_FIFO_START0:
					ep->fifo_size = value;
					break;
			}
			break;
		}

		case USB_GLOBAL_EVENT_INT_STATUS:
			p->global_status &= ~value;
			p->event_status &= ~(value >> USB_GLOBAL_EVENT_INT_STATUS_EVENT_SHIFT);
			usb_update_irq(p);
			break;

		case USB_GLOBAL_EVENT_EP_B_INT_ENABLE:
			p->global_enable = value;
			p->event_enable = value >> USB_GLOBAL_EVENT_EP_B_INT_ENABLE_EVENT_SHIFT;
			p->ep_b_enable_low = value >> USB_GLOBAL_EVENT_EP_B_INT_ENABLE_EP_B_LOW_SHIFT;
			p->ep_b_enable_high = value >> USB_GLOBAL_EVENT_EP_B_INT_ENABLE_EP_B_HIGH_SHIFT;
			usb_update_irq(p);
			break;

		case USB_DMA_EP_A_INT_ENABLE:
			p->dma0_enable = value;
			p->dma1_enable = value >> USB_DMA_EP_A_INT_ENABLE_DMA1_SHIFT;
			p->ep_a_enable_low = value >> USB_DMA_EP_A_INT_ENABLE_EP_A_LOW_SHIFT;
			p->ep_a_enable_high = value >> USB_DMA_EP_A_INT_ENABLE_EP_A_HIGH_SHIFT;
			break;

		case USB_CLC:
			pmb887x_clc_set(&p->clc, value);
			break;

		case USB_CFG:
			usb_set_cfg(p, value);
			break;
	}
}

static const MemoryRegionOps usb_io_ops = {
	.read = usb_io_read,
	.write = usb_io_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = {
		.min_access_size = 1,
		.max_access_size = 4,
	},
	.impl = {
		.min_access_size = 4,
		.max_access_size = 4,
	},
};

static void usb_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_usb_t *p = PMB887X_USB(obj);
	pmb887x_clc_init(&p->clc, dev);
	pmb887x_clc_set_callback(&p->clc, usb_clock_update, p);

	memory_region_init_io(&p->mmio, obj, &usb_io_ops, p, TYPE_PMB887X_USB, USB_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq);
	qdev_init_gpio_in_named(dev, usb_reset_input, "RESET", 1);
}

static void usb_reset(DeviceState *dev) {
	pmb887x_usb_t *p = PMB887X_USB(dev);

	usb_reset_state(p);
	pmb887x_clc_set(&p->clc, MOD_CLC_DISR);
}

static void usb_realize(DeviceState *dev, Error **errp) {
	pmb887x_usb_t *p = PMB887X_USB(dev);
	pmb887x_mod_debug_init(&p->dbg, dev, &p->mmio, &pmb887x_usb_regs);

	p->bus_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usb_bus_timer_expired, p);
	p->bus_reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usb_bus_reset_timer_expired, p);
	usb_reset(dev);
}

static const Property usb_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_usb_t, revision, 0),
};

static void usb_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);

	device_class_set_props(dc, usb_properties);
	device_class_set_legacy_reset(dc, usb_reset);
	dc->realize = usb_realize;
}

static const TypeInfo usb_info = {
	.name = TYPE_PMB887X_USB,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(pmb887x_usb_t),
	.instance_init = usb_init,
	.class_init = usb_class_init,
};

static void usb_register_types(void) {
	type_register_static(&usb_info);
}

type_init(usb_register_types)
