/*
 * PMB8876 MCI: ARM PrimeCell PL180 MultiMedia Card Interface at F7301000
 * (the CLC/ID wrapper at F7300000 is pmb887x-mmci).
 *
 * Command and response logic as in qemu's pl181 (hw/sd/pl181.c), on an
 * "sd-bus" with the card from -drive if=sd. Data moves only through the
 * DMAC (DATACTRL.DMAEnable), with the MCI as flow controller: one word per
 * single request, LSREQ for the last word of the transfer. The Siemens
 * firmware's card driver needs:
 *   - read: DATACTRL enabled before the command; data only after the response;
 *   - write: DATACTRL enabled by the ISR after CmdRespEnd;
 *   - the DMAC TC of the channel before the MCI interrupt of a data command is
 *     handled (here the response interrupt comes first and the firmware waits
 *     for the DMA callback itself);
 *   - one IRQ (148) from STATUS & MASK0.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "system/debug-channel.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/mod.h"
#include "cpu.h"
#include "exec/tb-flush.h"
#include "accel/tcg/cpu-loop.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "qemu/rcu.h"
#include "qemu/main-loop.h"
#include "system/cpus.h"
#include "hw/arm/pmb887x/gen/cpu_meta.h"

#define TYPE_PMB887X_MCI "pmb887x-mci"
#define PMB887X_MCI(obj) OBJECT_CHECK(pmb887x_mci_t, (obj), TYPE_PMB887X_MCI)

#define TYPE_PMB887X_MCI_BUS "pmb887x-mci-bus"

#define MCI_ST_CMDCRCFAIL		(1 << 0)
#define MCI_ST_DATACRCFAIL		(1 << 1)
#define MCI_ST_CMDTIMEOUT		(1 << 2)
#define MCI_ST_DATATIMEOUT		(1 << 3)
#define MCI_ST_TXUNDERRUN		(1 << 4)
#define MCI_ST_RXOVERRUN		(1 << 5)
#define MCI_ST_CMDRESPEND		(1 << 6)
#define MCI_ST_CMDSENT			(1 << 7)
#define MCI_ST_DATAEND			(1 << 8)
#define MCI_ST_STARTBITERR		(1 << 9)
#define MCI_ST_DATABLOCKEND		(1 << 10)
#define MCI_ST_CMDACTIVE		(1 << 11)
#define MCI_ST_TXACTIVE			(1 << 12)
#define MCI_ST_RXACTIVE			(1 << 13)
#define MCI_ST_TXFIFOHALFEMPTY	(1 << 14)
#define MCI_ST_RXFIFOHALFFULL	(1 << 15)
#define MCI_ST_TXFIFOFULL		(1 << 16)
#define MCI_ST_RXFIFOFULL		(1 << 17)
#define MCI_ST_TXFIFOEMPTY		(1 << 18)
#define MCI_ST_RXFIFOEMPTY		(1 << 19)
#define MCI_ST_TXDATAAVLBL		(1 << 20)
#define MCI_ST_RXDATAAVLBL		(1 << 21)

#define MCI_ST_STATIC_MASK		0x7FF

#define MCI_CMD_DELAY_NS		2000

#define MCI_FIFO_BASE			0x80
#define MCI_FIFO_END			0xC0

/* DMA signal sets, matching the "dma" entries of the MCI in gen/cpu_modules.c */
static const char *const mci_dma_names[] = { "RX", "TX", "RX2", "TX2" };
#define MCI_DMA_LINES			4

typedef struct pmb887x_mci_t pmb887x_mci_t;

/* blocks the card was sent so far (the wasm page polls it: ui/wasm.c) */
uint32_t pmb887x_mci_write_blocks;
/* what the firmware knows of the card (ui/wasm.c wasm_card_state, the page) */
uint32_t pmb887x_mci_card_state;

struct pmb887x_mci_t {
	SysBusDevice parent_obj;
	MemoryRegion mmio;
	DebugChannel dbg;
	SDBus sdbus;
	qemu_irq irq;
	QEMUTimer *cmd_timer;

	uint32_t revision;

	uint32_t power;
	uint32_t clock;
	uint32_t cmdarg;
	uint32_t cmd;
	uint32_t respcmd;
	uint32_t response[4];
	uint32_t datatimer;
	uint32_t datalength;
	uint32_t datactrl;
	uint32_t status;
	uint32_t mask[2];

	/* data path */
	bool data_active;		/* data path running (after the command response for reads) */
	bool data_read;
	uint32_t data_remaining;	/* bytes */
	uint8_t chunk[512];		/* the data moves through here, up to a block at a time */
	uint32_t chunk_len;
	uint32_t chunk_pos;
	bool cmd_pending;
	bool read_armed;		/* read enabled before its command: starts after the response */

	/*
	 * DMA handshake on every request line the firmware may pick for the MCI
	 * (its DMA HAL takes line 6 or, when that is busy, line 13; both sel 1):
	 * all carry the same level, a CLR on any withdraws them all.
	 */
	qemu_irq dmac_sreq[MCI_DMA_LINES];
	qemu_irq dmac_lsreq[MCI_DMA_LINES];
	uint32_t dmac_clr;
};

static const uint8_t mci_id[] = {
	0x80, 0x11, 0x04, 0x00,		/* PeriphID: PL180 */
	0x0d, 0xf0, 0x05, 0xb1,		/* PCellID */
};

static void mci_update_irq(pmb887x_mci_t *p) {
	qemu_set_irq(p->irq, (p->status & p->mask[0]) != 0);
}

static void mci_update_dma(pmb887x_mci_t *p) {
	bool dma = p->data_active && (p->datactrl & MCI_DATACTRL_DMAENABLE) && p->data_remaining > 0;
	bool last = p->data_remaining <= 4;
	bool req = dma && !p->dmac_clr;

	for (int i = 0; i < MCI_DMA_LINES; i++) {
		qemu_set_irq(p->dmac_sreq[i], req && !last);
		qemu_set_irq(p->dmac_lsreq[i], req && last);
	}
}

static void mci_data_done(pmb887x_mci_t *p) {
	p->data_active = false;
	p->status &= ~(MCI_ST_RXACTIVE | MCI_ST_TXACTIVE | MCI_ST_RXDATAAVLBL);
	p->status |= MCI_ST_DATAEND | MCI_ST_DATABLOCKEND;
	DEBUG_LOG(&p->dbg, "data %s done\n", p->data_read ? "read" : "write");
}

static void mci_data_start(pmb887x_mci_t *p) {
	if (!(p->datactrl & MCI_DATACTRL_ENABLE) || p->data_active)
		return;

	p->data_read = (p->datactrl & MCI_DATACTRL_DIRECTION) != 0;
	p->data_remaining = p->datalength;
	p->data_active = p->data_remaining > 0;
	p->chunk_len = p->chunk_pos = 0;
	if (p->data_active)
		p->status |= p->data_read ? (MCI_ST_RXACTIVE | MCI_ST_RXDATAAVLBL) : (MCI_ST_TXACTIVE | MCI_ST_TXFIFOEMPTY);
	if (p->data_active && !p->data_read)
		qatomic_add(&pmb887x_mci_write_blocks, MAX(1, p->datalength / 512));
	DEBUG_LOG(&p->dbg, "data %s start: %u bytes, dma=%d\n", p->data_read ? "read" : "write",
		p->data_remaining, (p->datactrl & MCI_DATACTRL_DMAENABLE) != 0);
}

/*
 * The card's own I/O (hw/sd/sd.c: blk_pread when a block starts, blk_pwrite
 * when it ends, blk_* in some commands) runs where it is called: here the
 * vCPU thread, in an MMIO access. The wasm build cannot run a coroutine on
 * the vCPU thread (util/coroutine-wasm.c), so there each card call goes to
 * the main loop as a one-shot BH, and the vCPU waits for it with the BQL let
 * go. The guest sees no difference: the call is still over when the access
 * returns. Native builds call directly.
 */
enum { MCI_SD_CMD, MCI_SD_READ, MCI_SD_WRITE };

typedef struct {
	pmb887x_mci_t *p;
	int op;
	SDRequest *req;
	uint8_t *buf;
	size_t len;
	size_t ret;
	QemuEvent done;
} mci_sd_call_t;

static void mci_sd_call_run(mci_sd_call_t *c) {
	switch (c->op) {
		case MCI_SD_CMD:
			c->ret = sdbus_do_command(&c->p->sdbus, c->req, c->buf, c->len);
			break;
		case MCI_SD_READ:
			sdbus_read_data(&c->p->sdbus, c->buf, c->len);
			break;
		case MCI_SD_WRITE:
			sdbus_write_data(&c->p->sdbus, c->buf, c->len);
			break;
	}
}

#ifdef __EMSCRIPTEN__
static void mci_sd_call_bh(void *opaque) {
	mci_sd_call_t *c = opaque;
	mci_sd_call_run(c);
	qemu_event_set(&c->done);
}
#endif

static size_t mci_sd_call(pmb887x_mci_t *p, int op, SDRequest *req, uint8_t *buf, size_t len) {
	mci_sd_call_t c = { .p = p, .op = op, .req = req, .buf = buf, .len = len };
#ifdef __EMSCRIPTEN__
	if (qemu_in_vcpu_thread()) {
		bool locked = bql_locked();
		qemu_event_init(&c.done, false);
		aio_bh_schedule_oneshot(qemu_get_aio_context(), mci_sd_call_bh, &c);
		if (locked)
			bql_unlock();
		qemu_event_wait(&c.done);
		if (locked)
			bql_lock();
		qemu_event_destroy(&c.done);
		return c.ret;
	}
#endif
	mci_sd_call_run(&c);
	return c.ret;
}

static uint32_t mci_fifo_pop(pmb887x_mci_t *p) {
	if (!p->data_active || !p->data_read) {
		p->status |= MCI_ST_RXOVERRUN;
		return 0;
	}

	uint32_t value = 0;
	uint32_t n = MIN(4, p->data_remaining);
	for (uint32_t i = 0; i < n; i++) {
		if (p->chunk_pos == p->chunk_len) {
			p->chunk_len = MIN(sizeof(p->chunk), p->data_remaining - i);
			p->chunk_pos = 0;
			mci_sd_call(p, MCI_SD_READ, NULL, p->chunk, p->chunk_len);
		}
		value |= (uint32_t) p->chunk[p->chunk_pos++] << (i * 8);
	}
	p->data_remaining -= n;
	if (!p->data_remaining)
		mci_data_done(p);
	return value;
}

static void mci_fifo_push(pmb887x_mci_t *p, uint32_t value) {
	if (!p->data_active || p->data_read) {
		p->status |= MCI_ST_TXUNDERRUN;
		return;
	}

	uint32_t n = MIN(4, p->data_remaining);
	for (uint32_t i = 0; i < n; i++) {
		if (p->chunk_pos == p->chunk_len) {
			/* a new chunk: as long as a block, or what is left */
			p->chunk_len = MIN(sizeof(p->chunk), p->data_remaining - i);
			p->chunk_pos = 0;
		}
		p->chunk[p->chunk_pos++] = value >> (i * 8);
		if (p->chunk_pos == p->chunk_len) {
			mci_sd_call(p, MCI_SD_WRITE, NULL, p->chunk, p->chunk_len);
			p->chunk_len = p->chunk_pos = 0;
		}
	}
	p->data_remaining -= n;
	if (!p->data_remaining)
		mci_data_done(p);
}

static bool mci_cd_hidden(void);
static void mci_cd_saw_absent(void);
static void mci_cd_saw_read(pmb887x_mci_t *p);
static void mci_cd_power_off(pmb887x_mci_t *p);

static void mci_do_command(pmb887x_mci_t *p) {
	SDRequest request;
	uint8_t response[16];
	size_t rlen;

	request.cmd = p->cmd & MCI_COMMAND_CMDINDEX;
	request.arg = p->cmdarg;
	if (mci_cd_hidden()) {
		/* removed, and the firmware has not finished noticing: no card */
		rlen = 0;
		mci_cd_saw_absent();
	} else {
		rlen = mci_sd_call(p, MCI_SD_CMD, &request, response, sizeof(response));
	}
	p->respcmd = request.cmd;

	if (p->cmd & MCI_COMMAND_RESPONSE) {
		bool is_long = (p->cmd & MCI_COMMAND_LONGRSP) != 0;
		if (rlen == 0 || (rlen == 4 && is_long) || (rlen != 4 && rlen != 16)) {
			p->status |= MCI_ST_CMDTIMEOUT;
			DEBUG_LOG(&p->dbg, "CMD%d %08X: timeout\n", request.cmd, request.arg);
			return;
		}
		p->response[0] = ldl_be_p(&response[0]);
		if (rlen == 4) {
			p->response[1] = p->response[2] = p->response[3] = 0;
		} else {
			p->response[1] = ldl_be_p(&response[4]);
			p->response[2] = ldl_be_p(&response[8]);
			p->response[3] = ldl_be_p(&response[12]) & ~1;
		}
		p->status |= MCI_ST_CMDRESPEND;
		if (request.cmd == 17 || request.cmd == 18)
			mci_cd_saw_read(p);
		DEBUG_LOG(&p->dbg, "CMD%d %08X: %08X %08X %08X %08X\n", request.cmd, request.arg,
			p->response[0], p->response[1], p->response[2], p->response[3]);
	} else {
		p->status |= MCI_ST_CMDSENT;
		DEBUG_LOG(&p->dbg, "CMD%d %08X: sent\n", request.cmd, request.arg);
	}
}

static void mci_cmd_timer(void *opaque) {
	pmb887x_mci_t *p = opaque;

	if (!p->cmd_pending)
		return;
	p->cmd_pending = false;
	p->status &= ~MCI_ST_CMDACTIVE;
	mci_do_command(p);

	/* a read whose data path was enabled before the command starts now */
	if (p->read_armed) {
		p->read_armed = false;
		if (!(p->status & (MCI_ST_CMDTIMEOUT | MCI_ST_CMDCRCFAIL)))
			mci_data_start(p);
	}

	mci_update_dma(p);
	mci_update_irq(p);
}

static uint64_t mci_io_read(void *opaque, hwaddr haddr, unsigned size) {
	pmb887x_mci_t *p = opaque;
	uint64_t value = 0;

	if (haddr >= 0xFE0 && haddr < 0x1000)
		return mci_id[(haddr - 0xFE0) >> 2];

	if (haddr >= MCI_FIFO_BASE && haddr < MCI_FIFO_END) {
		value = mci_fifo_pop(p);
		mci_update_dma(p);
		mci_update_irq(p);
		return value;
	}

	switch (haddr) {
		case MCI_POWER:		value = p->power; break;
		case MCI_CLOCK:		value = p->clock; break;
		case MCI_ARGUMENT:	value = p->cmdarg; break;
		case MCI_COMMAND:	value = p->cmd; break;
		case MCI_RESPCMD:	value = p->respcmd; break;
		case MCI_RESPONSE0:	value = p->response[0]; break;
		case MCI_RESPONSE1:	value = p->response[1]; break;
		case MCI_RESPONSE2:	value = p->response[2]; break;
		case MCI_RESPONSE3:	value = p->response[3]; break;
		case MCI_DATATIMER:	value = p->datatimer; break;
		case MCI_DATALENGTH:	value = p->datalength; break;
		case MCI_DATACTRL:	value = p->datactrl; break;
		case MCI_DATACNT:	value = p->data_active ? p->data_remaining : 0; break;
		case MCI_STATUS:	value = p->status; break;
		case MCI_MASK0:		value = p->mask[0]; break;
		case MCI_MASK1:		value = p->mask[1]; break;
		case MCI_FIFOCNT:	value = p->data_active ? (p->data_remaining + 3) / 4 : 0; break;
	}
	return value;
}

static void mci_io_write(void *opaque, hwaddr haddr, uint64_t value, unsigned size) {
	pmb887x_mci_t *p = opaque;

	if (haddr >= MCI_FIFO_BASE && haddr < MCI_FIFO_END) {
		mci_fifo_push(p, value);
		mci_update_dma(p);
		mci_update_irq(p);
		return;
	}

	switch (haddr) {
		case MCI_POWER:
			p->power = value & 0xFF;
			if (!(p->power & 3))
				mci_cd_power_off(p);
			break;
		case MCI_CLOCK:
			p->clock = value & 0xFFF;
			break;
		case MCI_ARGUMENT:
			p->cmdarg = value;
			break;
		case MCI_COMMAND:
			p->cmd = value & 0x7FF;
			if (p->cmd & MCI_COMMAND_ENABLE) {
				p->cmd_pending = true;
				p->status |= MCI_ST_CMDACTIVE;
				timer_mod(p->cmd_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MCI_CMD_DELAY_NS);
			} else {
				p->cmd_pending = false;
				p->status &= ~MCI_ST_CMDACTIVE;
				timer_del(p->cmd_timer);
			}
			break;
		case MCI_DATATIMER:
			p->datatimer = value;
			break;
		case MCI_DATALENGTH:
			p->datalength = value & 0xFFFF;
			break;
		case MCI_DATACTRL:
			p->datactrl = value & 0xFF;
			if (!(p->datactrl & MCI_DATACTRL_ENABLE)) {
				p->data_active = false;
				p->status &= ~(MCI_ST_RXACTIVE | MCI_ST_TXACTIVE | MCI_ST_RXDATAAVLBL | MCI_ST_TXFIFOEMPTY);
				p->read_armed = false;
			} else if ((p->datactrl & MCI_DATACTRL_DIRECTION)) {
				/* read: the card sends data only after the command */
				if (!p->data_active)
					p->read_armed = true;
			} else {
				mci_data_start(p);
			}
			break;
		case MCI_CLEAR:
			p->status &= ~(value & MCI_ST_STATIC_MASK);
			break;
		case MCI_MASK0:
			p->mask[0] = value;
			break;
		case MCI_MASK1:
			p->mask[1] = value;
			break;
	}
	mci_update_dma(p);
	mci_update_irq(p);
}

static const MemoryRegionOps mci_io_ops = {
	.read = mci_io_read,
	.write = mci_io_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 4,
		.max_access_size = 4,
	},
};

static void mci_set_dmac_clr(pmb887x_mci_t *p, int set, int level) {
	if (level)
		p->dmac_clr |= 1 << set;
	else
		p->dmac_clr &= ~(1 << set);
	mci_update_dma(p);
}

static void mci_handle_dmac_clr0(void *opaque, int id, int level) { mci_set_dmac_clr(opaque, 0, level); }
static void mci_handle_dmac_clr1(void *opaque, int id, int level) { mci_set_dmac_clr(opaque, 1, level); }
static void mci_handle_dmac_clr2(void *opaque, int id, int level) { mci_set_dmac_clr(opaque, 2, level); }
static void mci_handle_dmac_clr3(void *opaque, int id, int level) { mci_set_dmac_clr(opaque, 3, level); }

static qemu_irq_handler const mci_dma_clr_handlers[MCI_DMA_LINES] = {
	mci_handle_dmac_clr0, mci_handle_dmac_clr1, mci_handle_dmac_clr2, mci_handle_dmac_clr3,
};

static void mci_handle_dmac_tc(void *opaque, int id, int level) {
	/* nothing: the MCI ends the transfer itself with LSREQ */
}

static void mci_init(Object *obj) {
	DeviceState *dev = DEVICE(obj);
	pmb887x_mci_t *p = PMB887X_MCI(obj);

	memory_region_init_io(&p->mmio, obj, &mci_io_ops, p, "pmb887x-mci", MCI_IO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &p->irq);

	for (int i = 0; i < MCI_DMA_LINES; i++) {
		char name[32];
		snprintf(name, sizeof(name), "DMAC_%s_CLR", mci_dma_names[i]);
		qdev_init_gpio_in_named_with_opaque(dev, mci_dma_clr_handlers[i], p, name, 1);
		snprintf(name, sizeof(name), "DMAC_%s_TC", mci_dma_names[i]);
		qdev_init_gpio_in_named(dev, mci_handle_dmac_tc, name, 1);
		snprintf(name, sizeof(name), "DMAC_%s_SREQ", mci_dma_names[i]);
		qdev_init_gpio_out_named(dev, &p->dmac_sreq[i], name, 1);
		snprintf(name, sizeof(name), "DMAC_%s_LSREQ", mci_dma_names[i]);
		qdev_init_gpio_out_named(dev, &p->dmac_lsreq[i], name, 1);
	}

	qbus_init(&p->sdbus, sizeof(p->sdbus), TYPE_PMB887X_MCI_BUS, dev, "sd-bus");
}

static void mci_reset(DeviceState *dev) {
	pmb887x_mci_t *p = PMB887X_MCI(dev);

	p->power = 0;
	p->clock = 0;
	p->cmdarg = 0;
	p->cmd = 0;
	p->respcmd = 0;
	memset(p->response, 0, sizeof(p->response));
	p->datatimer = 0;
	p->datalength = 0;
	p->datactrl = 0;
	p->status = 0;
	p->mask[0] = 0;
	p->mask[1] = 0;
	p->data_active = false;
	p->data_remaining = 0;
	p->cmd_pending = false;
	p->read_armed = false;
	p->dmac_clr = 0;
	if (p->cmd_timer)
		timer_del(p->cmd_timer);
}

/*
 * Card detect without a card-detect line.
 *
 * Phones without a card-detect line (EL71, E71) never look for a new card
 * by themselves. The firmware's card server (Nucleus objects named
 * "CARDSRV ") rescans the card only when something sets its rescan event:
 * the card-detect interrupt on phones that have one, a failed card select
 * during a block request, or its own one-shot timer, armed after every
 * burst of block requests, whose expiration routine sets the event 13 s
 * after the last request (if the server is idle then). A rescan reports a
 * card that went away "removed" (the FS drops its cache) and one that came
 * back "inserted" (the FS mounts and reads it again). After a removal there
 * are no block requests, so nothing would ever look again.
 * pmb887x-emu-web doc/sd-card.md, "Remount without a reboot".
 *
 * So when qemu's sd-card reports a media change (monitor "eject"/"change",
 * or wasm_card_*), the model runs the CARDSRV timer's expiration routine the
 * way Nucleus runs it, from the timer HISR, at an expiration of another
 * application timer, until the firmware has acted on the change. Nothing in
 * here is per firmware: the RAM is searched for Nucleus PLUS application
 * timer control blocks (TM_APP_TCB: id "TIME" at +12, name at +16,
 * expiration routine +24, its argument +28, enabled +32, the actual timer
 * at +48 whose information field +56 points back at the block, by its
 * virtual address, so only the offset in the page is compared). The CARDSRV
 * one gives the routine to run; the routine most of the enabled timers use
 * is where it is run: a translator hook (target/arm/cpu.h, arm_pc_hook_addr)
 * at its entry saves the registers, sends the guest to the CARDSRV routine
 * with LR = the hook address, and when it comes back there with the same SP
 * restores them and lets the original call go on.
 *
 * The goal, not a count of calls, decides when to stop:
 *  - a removal is done when the firmware's rescan has failed on the card and
 *    switched it off (until then the card stays hidden: commands time out);
 *  - an insertion is done when the firmware reads a block again (CMD17/18:
 *    the FS mounts). An insertion waits for the removal before it, so a
 *    quick eject + insert cannot slip past the "removed" the FS needs.
 * While a goal is open the hook stays armed: at each tick of the hooked
 * routine the CARDSRV routine is run if its own timer is not running
 * (Nucleus runs it only on an expired timer; on a running one its
 * NU_Reset_Timer fails: firmware exit 0x000F "cardsrv") and the last call
 * is CD_RETRY_MS (host time) ago, at most CD_MAX_CALLS times and
 * CD_GIVE_UP_MS per goal. A running CARDSRV timer (the firmware arms it
 * after card I/O, about 13 s of guest time) is moved up in Nucleus's active
 * timer list so that it expires with the next timer (mci_cd_expedite) and
 * Nucleus runs the routine itself; if that is not possible, it is waited for.
 * A firmware without such objects is left alone (the card still changes,
 * the firmware is not told).
 */
#define NU_TIMER_ID		0x54494D45	/* "TIME" */
#define NU_TIMER_SIZE	68
#define CD_RETRY_MS		1500
#define CD_MAX_CALLS	10
#define CD_GIVE_UP_MS	600000	/* a goal open this long (host time) is given up */

enum { CD_IDLE, CD_CALL, CD_RETURN };
enum { CD_WANT_NONE, CD_WANT_OUT, CD_WANT_IN };

static struct {
	uint32_t timer;			/* CARDSRV timer control block (virtual address) */
	uint32_t routine;		/* its expiration routine (bit 0 = Thumb) */
	uint32_t arg;			/* and its argument */
	uint32_t hook;			/* entry of the routine the enabled timers use */
	int state;
	int want;				/* goal: the firmware sees the card out / reads it */
	bool insert_waiting;	/* an insertion after the removal under way */
	bool present;			/* the medium (qemu's side) */
	bool gave_up;			/* the last goal was not reached */
	int calls;				/* CARDSRV routine runs for this goal */
	int hits, waits;		/* hook hits, and those with the CARDSRV timer running */
	int expedited;			/* times its running timer was moved up (mci_cd_expedite) */
	bool timer_wait;		/* the last tick waited for a running CARDSRV timer it could not move up */
	int64_t next_call;		/* realtime ms */
	int64_t goal_start;		/* realtime ms (0: not yet hit) */
	uint32_t saved[6];		/* r0-r3, r12, lr */
	uint32_t saved_sp;
	bool hide;				/* removed, the firmware has not seen it yet */
	bool absent_seen;		/* a command failed while hidden */
	pmb887x_mci_t *p;
} mci_cd = { .present = true };

static int mci_cd_fail_at;	/* why mci_cd_expedite last did nothing (its check number) */

/* for the page: 0 in, 1 out, 2 going out, 3 reading, 4 not seen (gave up);
 * | 0x10: waiting for the firmware's card timer (could not move it up) */
static void mci_cd_publish(void) {
	uint32_t st;
	if (mci_cd.want == CD_WANT_OUT || mci_cd.insert_waiting)
		st = 2;
	else if (mci_cd.want == CD_WANT_IN)
		st = 3;
	else if (mci_cd.gave_up)
		st = 4;
	else
		st = mci_cd.present ? 0 : 1;
	if ((st == 2 || st == 3) && mci_cd.timer_wait)
		st |= 0x10;
	qatomic_set(&pmb887x_mci_card_state, st);
}

static bool mci_cd_hidden(void) {
	return mci_cd.hide;
}

static void mci_cd_saw_absent(void) {
	mci_cd.absent_seen = true;
}

static void mci_cd_hook(CPUARMState *env, uintptr_t ra);

static void mci_cd_arm(void) {
	if (mci_cd.state == CD_IDLE) {
		mci_cd.state = CD_CALL;
		qatomic_set(&arm_pc_hook_fn, mci_cd_hook);
		qatomic_set(&arm_pc_hook_addr, mci_cd.hook & ~1);
		queue_tb_flush(first_cpu);
	}
}

static void mci_cd_disarm(CPUState *cs) {
	qatomic_set(&arm_pc_hook_addr, UINT32_MAX);
	queue_tb_flush(cs);
}

typedef struct {
	uint32_t timer, routine, arg;
	uint32_t hooks[16];
	unsigned hook_count[16];
	unsigned timers;
} mci_cd_scan_t;

/* one RAM range of the system address space: look for timer control blocks */
static bool mci_cd_scan_range(Int128 start, Int128 len, const MemoryRegion *mr,
		hwaddr offset, void *opaque) {
	mci_cd_scan_t *sc = opaque;
	uint64_t size = int128_get64(len), base = int128_get64(start);
	const uint8_t *ram;

	if (!memory_region_is_ram((MemoryRegion *) mr) || size < 1024 * 1024 || base > UINT32_MAX)
		return false;
	ram = (const uint8_t *) memory_region_get_ram_ptr((MemoryRegion *) mr) + offset;
	for (uint64_t o = 12; o + NU_TIMER_SIZE - 12 <= size; o += 4) {
		const uint8_t *t = ram + o - 12;
		uint32_t addr = base + o - 12;
		uint32_t routine, enabled;

		/* +56 is the block's own (virtual) address: RAM is also mapped
		 * elsewhere, so only its offset in the page has to match */
		uint32_t self = ldl_le_p(t + 56);
		if (ldl_le_p(ram + o) != NU_TIMER_ID || !self || ((self ^ addr) & 0xFFF))
			continue;
		routine = ldl_le_p(t + 24);
		enabled = ldl_le_p(t + 32);
		if (!routine || routine == UINT32_MAX)
			continue;
		sc->timers++;
		if (memcmp(t + 16, "CARDSRV ", 8) == 0) {
			sc->timer = self;
			sc->routine = routine;
			sc->arg = ldl_le_p(t + 28);
			continue;
		}
		if (!enabled)
			continue;
		for (int i = 0; i < ARRAY_SIZE(sc->hooks); i++) {
			if (sc->hook_count[i] && sc->hooks[i] != routine)
				continue;
			sc->hooks[i] = routine;
			sc->hook_count[i]++;
			break;
		}
	}
	return false;
}

/* find the CARDSRV timer and a hook point; false: no Nucleus card server */
static bool mci_cd_find(pmb887x_mci_t *p) {
	mci_cd_scan_t sc = {};
	unsigned best = 0;

	RCU_READ_LOCK_GUARD();
	flatview_for_each_range(address_space_to_flatview(&address_space_memory), mci_cd_scan_range, &sc);
	mci_cd.timer = sc.timer;
	mci_cd.routine = sc.routine;
	mci_cd.arg = sc.arg;
	mci_cd.hook = 0;
	for (int i = 0; i < ARRAY_SIZE(sc.hooks); i++) {
		if (sc.hook_count[i] > best && sc.hooks[i] != sc.routine) {
			best = sc.hook_count[i];
			mci_cd.hook = sc.hooks[i];
		}
	}
	DEBUG_LOG(&p->dbg, "card detect: %u Nucleus timers, CARDSRV routine %08X(%08X), hook %08X (%u enabled timers)\n",
			sc.timers, mci_cd.routine, mci_cd.arg, mci_cd.hook, best);
	return mci_cd.routine && mci_cd.hook;
}

/* a new goal: the hook runs the CARDSRV routine until it is reached */
static void mci_cd_set_goal(int want) {
	mci_cd.want = want;
	mci_cd.calls = mci_cd.hits = mci_cd.waits = mci_cd.expedited = 0;
	mci_cd.gave_up = false;
	mci_cd.next_call = 0;
	mci_cd.goal_start = 0;
	mci_cd.timer_wait = false;
	if (want != CD_WANT_NONE)
		mci_cd_arm();
	mci_cd_publish();
}

static void mci_cd_goal_done(const char *what) {
	pmb887x_mci_t *p = mci_cd.p;
	DEBUG_LOG(&p->dbg, "card detect: %s (%d call(s), %d hook hits, %d with its timer running, moved up %d, last no-move check %d)\n",
			what, mci_cd.calls, mci_cd.hits, mci_cd.waits, mci_cd.expedited, mci_cd_fail_at);
	mci_cd.want = CD_WANT_NONE;
	mci_cd.calls = 0;
	if (mci_cd.insert_waiting) {
		mci_cd.insert_waiting = false;
		mci_cd_set_goal(CD_WANT_IN);
	}
	mci_cd_publish();
}

/* (in the hook: no DEBUG_LOG there, its time stamp reads the virtual clock,
 * which icount forbids in the middle of a TB) */
static void mci_cd_give_up(void) {
	qemu_log_mask(LOG_UNIMP, "pmb887x-mci: card detect: the firmware did not %s after %d calls, giving up\n",
			mci_cd.want == CD_WANT_OUT ? "see the removal" : "read the card", mci_cd.calls);
	if (mci_cd.want == CD_WANT_OUT) {
		/* do not keep the card hidden forever */
		mci_cd.hide = false;
		mci_cd.absent_seen = false;
	}
	mci_cd.want = CD_WANT_NONE;
	mci_cd.gave_up = true;
	if (mci_cd.insert_waiting) {
		mci_cd.insert_waiting = false;
		mci_cd_set_goal(CD_WANT_IN);
	}
	mci_cd_publish();
}

#define TM_NEXT		12
#define TM_PREV		16

static bool mci_cd_rd(CPUState *cs, uint32_t va, uint32_t *v) {
	uint8_t b[4];
	if (cpu_memory_rw_debug(cs, va, b, 4, false))
		return false;
	*v = ldl_le_p(b);
	return true;
}

static bool mci_cd_wr(CPUState *cs, uint32_t va, uint32_t v) {
	uint8_t b[4];
	stl_le_p(b, v);
	return !cpu_memory_rw_debug(cs, va, b, 4, true);
}

static bool mci_cd_fail(int at) {
	mci_cd_fail_at = at;
	return false;
}

/* the active list, walked from T: its members, sorted (false: inconsistent) */
#define CD_MAX_MEMBERS	512
typedef struct {
	uint32_t m[CD_MAX_MEMBERS];
	int n;
	uint8_t *found;			/* host pointer of the one outside pointer */
	int hits;
} mci_cd_list_t;

static int mci_cd_cmp(const void *a, const void *b) {
	uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
	return x < y ? -1 : x > y;
}

static bool mci_cd_members(CPUState *cs, uint32_t t, mci_cd_list_t *l) {
	uint32_t v = t, nx, back;
	l->n = 0;
	do {
		if (l->n >= CD_MAX_MEMBERS || !mci_cd_rd(cs, v + TM_NEXT, &nx) ||
				!mci_cd_rd(cs, nx + TM_PREV, &back) || back != v)
			return false;
		l->m[l->n++] = v;
		v = nx;
	} while (v != t);
	qsort(l->m, l->n, sizeof(l->m[0]), mci_cd_cmp);
	return true;
}

static bool mci_cd_is_member(const mci_cd_list_t *l, uint32_t v) {
	return v >= l->m[0] && v <= l->m[l->n - 1] &&
		bsearch(&v, l->m, l->n, sizeof(v), mci_cd_cmp);
}

/* a TM_TCB-looking node at host p: type 0/1, both links are members */
static bool mci_cd_node_at(const mci_cd_list_t *l, const uint8_t *p) {
	return ldl_le_p(p) <= 1 && mci_cd_is_member(l, ldl_le_p(p + TM_NEXT)) &&
		mci_cd_is_member(l, ldl_le_p(p + TM_PREV));
}

/* TMD_Active_Timers_List: the one RAM word pointing at a member that is not
 * a member's own next/previous field */
static bool mci_cd_head_range(Int128 start, Int128 len, const MemoryRegion *mr,
		hwaddr offset, void *opaque) {
	mci_cd_list_t *l = opaque;
	uint64_t size = int128_get64(len), base = int128_get64(start);
	uint8_t *ram;

	if (!memory_region_is_ram((MemoryRegion *) mr) || size < 1024 * 1024 || base > UINT32_MAX)
		return false;
	ram = (uint8_t *) memory_region_get_ram_ptr((MemoryRegion *) mr) + offset;
	for (uint64_t o = 16; o + 8 <= size; o += 4) {
		if (!mci_cd_is_member(l, ldl_le_p(ram + o)))
			continue;
		if (mci_cd_node_at(l, ram + o - TM_NEXT) || mci_cd_node_at(l, ram + o - TM_PREV))
			continue;
		l->hits++;
		l->found = ram + o;
	}
	return false;
}

static uint8_t *mci_cd_head_ptr;	/* host pointer of TMD_Active_Timers_List */

static bool mci_cd_find_head(CPUState *cs, uint32_t t) {
	g_autofree mci_cd_list_t *l = g_new0(mci_cd_list_t, 1);

	if (mci_cd_head_ptr) {
		/* still a member of the list T is in? */
		if (mci_cd_members(cs, t, l) && mci_cd_is_member(l, ldl_le_p(mci_cd_head_ptr)))
			return true;
		mci_cd_head_ptr = NULL;
	}
	if (!mci_cd_members(cs, t, l))
		return mci_cd_fail(10);
	{
		RCU_READ_LOCK_GUARD();
		flatview_for_each_range(address_space_to_flatview(&address_space_memory), mci_cd_head_range, l);
	}
	qemu_log_mask(LOG_UNIMP, "pmb887x-mci: card detect: %d active timers, %d list head candidate(s)\n",
			l->n, l->hits);
	if (l->hits != 1)
		return mci_cd_fail(11);
	mci_cd_head_ptr = l->found;
	return true;
}

/*
 * The CARDSRV timer is running (armed after card I/O, about 13 s of guest
 * time): make it expire with the next timer instead of waiting, so Nucleus
 * runs its routine itself, on an expired timer, in its own context.
 *
 * Nucleus PLUS active timer list (TM_TCB, at +48 of a TM_APP_TCB): +0 type
 * (TM_TASK_TIMER 0, TM_APPL_TIMER 1), +4 remaining time, +8 information
 * (the TM_APP_TCB or the task), +12 next, +16 previous; circular, doubly
 * linked, sorted, the head's remaining time absolute, every other one a delta
 * to the one before. The head is the variable TMD_Active_Timers_List: found
 * once as the only RAM word that points at a member of the list and is not a
 * member's own link field (mci_cd_find_head), checked again each time.
 *
 * The CARDSRV entry T is unlinked (its delta added to the one after it) and
 * put right after the head H with delta 0: it expires when H does (the next
 * timer of all; the hooked routine's timers fire every 0.2-1 s). If T is the
 * head, its remaining time becomes 0 (the next one keeps its time). This
 * runs in the hook, i.e. in the timer HISR inside Nucleus's expiration loop,
 * which is the context Nucleus changes the list in. On any mismatch nothing
 * is written and the model waits as before.
 */
static bool mci_cd_expedite(CPUState *cs, CPUARMState *env) {
	uint32_t t = mci_cd.timer + 48, h, tn, tp, trem, tnrem, hn;

	if (!mci_cd_find_head(cs, t))
		return false;
	h = ldl_le_p(mci_cd_head_ptr);
	if (!mci_cd_rd(cs, t + TM_NEXT, &tn) || !mci_cd_rd(cs, t + TM_PREV, &tp) ||
			!mci_cd_rd(cs, t + 4, &trem))
		return mci_cd_fail(5);
	if (trem == 0 && (t == h || tp == h))
		return mci_cd_fail(7);			/* already due with the head */

	if (t == h) {
		if (tn != t) {
			if (!mci_cd_rd(cs, tn + 4, &tnrem) || !mci_cd_wr(cs, tn + 4, tnrem + trem))
				return mci_cd_fail(8);
		}
		mci_cd_wr(cs, t + 4, 0);
		return true;
	}

	/* unlink T: the one after it keeps its time (unless it wraps to H) */
	if (tn != h) {
		if (!mci_cd_rd(cs, tn + 4, &tnrem) || !mci_cd_wr(cs, tn + 4, tnrem + trem))
			return mci_cd_fail(8);
	}
	mci_cd_wr(cs, tp + TM_NEXT, tn);
	mci_cd_wr(cs, tn + TM_PREV, tp);
	/* and after H, delta 0 */
	if (!mci_cd_rd(cs, h + TM_NEXT, &hn))
		return mci_cd_fail(9);
	mci_cd_wr(cs, t + 4, 0);
	mci_cd_wr(cs, t + TM_PREV, h);
	mci_cd_wr(cs, t + TM_NEXT, hn);
	mci_cd_wr(cs, hn + TM_PREV, t);
	mci_cd_wr(cs, h + TM_NEXT, t);
	return true;
}

static void mci_cd_hook(CPUARMState *env, uintptr_t ra) {
	CPUState *cs = env_cpu(env);

	if (env->thumb != (mci_cd.hook & 1))
		return;

	if (mci_cd.state == CD_CALL) {
		uint8_t enabled[4];
		int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);	/* (virtual time cannot be read mid-TB under icount) */

		if (mci_cd.want == CD_WANT_NONE) {
			mci_cd.state = CD_IDLE;
			mci_cd_disarm(cs);
			return;
		}
		mci_cd.hits++;
		if (!mci_cd.goal_start)
			mci_cd.goal_start = now;
		if (now - mci_cd.goal_start > CD_GIVE_UP_MS) {
			mci_cd_give_up();
			if (mci_cd.want == CD_WANT_NONE) {
				mci_cd.state = CD_IDLE;
				mci_cd_disarm(cs);
			}
			return;
		}
		if (now < mci_cd.next_call)
			return;
		/* only on an expired timer, as Nucleus does (see above); a running
		 * one is waited for: its own expiry may do the rescan */
		if (cpu_memory_rw_debug(cs, mci_cd.timer + 32, enabled, 4, false) || ldl_le_p(enabled)) {
			mci_cd.waits++;
			if (mci_cd_expedite(cs, env)) {
				mci_cd.expedited++;
				mci_cd.timer_wait = false;
			} else {
				mci_cd.timer_wait = true;
			}
			mci_cd_publish();
			return;
		}
		if (mci_cd.timer_wait) {
			mci_cd.timer_wait = false;
			mci_cd_publish();
		}
		if (mci_cd.calls >= CD_MAX_CALLS) {
			mci_cd_give_up();
			if (mci_cd.want == CD_WANT_NONE) {
				mci_cd.state = CD_IDLE;
				mci_cd_disarm(cs);
			}
			return;
		}
		mci_cd.calls++;
		mci_cd.next_call = now + CD_RETRY_MS;
		/* the insn at the hook has not run: back to its start, exact icount */
		cpu_restore_state(cs, ra);
		for (int i = 0; i < 4; i++)
			mci_cd.saved[i] = env->regs[i];
		mci_cd.saved[4] = env->regs[12];
		mci_cd.saved[5] = env->regs[14];
		mci_cd.saved_sp = env->regs[13];
		env->regs[0] = mci_cd.arg;
		env->regs[14] = mci_cd.hook;
		env->regs[15] = mci_cd.routine & ~1;
		env->thumb = mci_cd.routine & 1;
		mci_cd.state = CD_RETURN;
		cpu_loop_exit(cs);
	}

	if (mci_cd.state == CD_RETURN && env->regs[13] == mci_cd.saved_sp) {
		/* the CARDSRV routine returned here: the original call goes on */
		for (int i = 0; i < 4; i++)
			env->regs[i] = mci_cd.saved[i];
		env->regs[12] = mci_cd.saved[4];
		env->regs[14] = mci_cd.saved[5];
		if (mci_cd.want != CD_WANT_NONE) {
			mci_cd.state = CD_CALL;		/* next tick: check again */
		} else {
			mci_cd.state = CD_IDLE;
			mci_cd_disarm(cs);
		}
	}
}

/* can the firmware be told? (finds the card server and the hook point) */
static bool mci_cd_ready(pmb887x_mci_t *p) {
	if (!first_cpu)
		return false;
	mci_cd.p = p;
	/* (again each time: the enabled timers change; not while a call is under way) */
	if (mci_cd.state != CD_RETURN && !mci_cd_find(p)) {
		DEBUG_LOG(&p->dbg, "card detect: no CARDSRV timer, the firmware is not told\n");
		return false;
	}
	return true;
}

/* a block read: an insertion the firmware was to notice is done */
static void mci_cd_saw_read(pmb887x_mci_t *p) {
	if (mci_cd.want == CD_WANT_IN && !mci_cd.hide)
		mci_cd_goal_done("the firmware reads the card again");
}

static void mci_cd_power_off(pmb887x_mci_t *p) {
	if (!mci_cd.hide || !mci_cd.absent_seen)
		return;
	/* the firmware's rescan saw no card and is done with it */
	mci_cd.hide = false;
	mci_cd.absent_seen = false;
	if (mci_cd.want == CD_WANT_OUT)
		mci_cd_goal_done("the firmware saw the removal");
}

/* sd-card: media changed (monitor eject/change, wasm_card_*) */
static void mci_bus_set_inserted(DeviceState *dev, bool inserted) {
	pmb887x_mci_t *p = PMB887X_MCI(dev);
	DEBUG_LOG(&p->dbg, "card %s\n", inserted ? "inserted" : "removed");
	if (mci_cd.want != CD_WANT_NONE)
		DEBUG_LOG(&p->dbg, "card detect: open goal so far: %d call(s), %d hook hits, %d with its timer running, moved up %d\n",
				mci_cd.calls, mci_cd.hits, mci_cd.waits, mci_cd.expedited);
	mci_cd.present = inserted;
	if (!mci_cd_ready(p)) {
		mci_cd_publish();
		return;
	}
	if (!inserted) {
		mci_cd.insert_waiting = false;
		if (mci_cd.want == CD_WANT_OUT || mci_cd.hide) {
			/* still going out: the same goal */
		} else {
			mci_cd.hide = true;
			mci_cd.absent_seen = false;
			mci_cd_set_goal(CD_WANT_OUT);
		}
	} else if (mci_cd.hide) {
		mci_cd.insert_waiting = true;
		DEBUG_LOG(&p->dbg, "card detect: insert held until the firmware has seen the removal\n");
	} else {
		mci_cd_set_goal(CD_WANT_IN);
	}
	mci_cd_publish();
}

static void mci_bus_class_init(ObjectClass *klass, const void *data) {
	SDBusClass *sbc = SD_BUS_CLASS(klass);
	sbc->set_inserted = mci_bus_set_inserted;
}

static void mci_realize(DeviceState *dev, Error **errp) {
	pmb887x_mci_t *p = PMB887X_MCI(dev);
	pmb887x_mod_debug_init(&p->dbg, dev, &p->mmio, &pmb887x_mci_regs);
	p->cmd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mci_cmd_timer, p);

	/* the card: -drive if=sd,format=raw,file=card.img */
	DriveInfo *dinfo = drive_get(IF_SD, 0, 0);
	if (dinfo) {
		DeviceState *card = qdev_new(TYPE_SD_CARD);
		if (!qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo), errp))
			return;
		if (!qdev_realize_and_unref(card, qdev_get_child_bus(dev, "sd-bus"), errp))
			return;
		DEBUG_LOG(&p->dbg, "card attached at boot\n");
	}
}

static const Property mci_properties[] = {
	DEFINE_PROP_UINT32("revision", pmb887x_mci_t, revision, 0),
};

static void mci_class_init(ObjectClass *klass, const void *data) {
	DeviceClass *dc = DEVICE_CLASS(klass);

	device_class_set_props(dc, mci_properties);
	device_class_set_legacy_reset(dc, mci_reset);
	dc->realize = mci_realize;
}

static const TypeInfo mci_info = {
	.name = TYPE_PMB887X_MCI,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(pmb887x_mci_t),
	.instance_init = mci_init,
	.class_init = mci_class_init,
};

static const TypeInfo mci_bus_info = {
	.name = TYPE_PMB887X_MCI_BUS,
	.parent = TYPE_SD_BUS,
	.instance_size = sizeof(SDBus),
	.class_init = mci_bus_class_init,
};

static void mci_register_types(void) {
	type_register_static(&mci_info);
	type_register_static(&mci_bus_info);
}

type_init(mci_register_types)
