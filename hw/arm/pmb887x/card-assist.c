/*
 * PMB887X-WEB FEATURE (card assist): telling the firmware about a card change.
 *
 * -object pmb887x-card-assist,id=card[,slot=<sd-slot path>][,debug=on]
 *
 * The card is taken out and put back with this object's "inserted" property
 * (qom-set), not the slot's: the object drives the board's sd-slot and makes
 * sure the firmware notices, which phones without a card-detect line (EL71,
 * E71) never do by themselves. Without the object the slot works as upstream.
 * pmb887x-emu-web doc/sd-card.md, "Remount without a reboot".
 *
 * The firmware's card server (Nucleus objects named "CARDSRV ") rescans the
 * card only when something sets its rescan event: the card-detect interrupt
 * on phones that have one, a failed card select during a block request, or
 * its own one-shot timer, armed after every burst of block requests, whose
 * expiration routine sets the event 13 s after the last request (if the
 * server is idle then). A rescan reports a card that went away "removed" (the
 * FS drops its cache) and one that came back "inserted" (the FS mounts and
 * reads it again). After a removal there are no block requests, so nothing
 * would ever look again.
 *
 * So on a change the object runs the CARDSRV timer's expiration routine the
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
 *  - a removal is done when the firmware's rescan has failed on the card (a
 *    command without a card: sdbus_command_observer) and switched it off
 *    (the MCI's POWER register, polled meanwhile);
 *  - an insertion is done when the firmware reads a block again (CMD17/18:
 *    the FS mounts). An insertion waits for the removal before it: the slot
 *    stays empty until then, so a quick out + in cannot slip past the
 *    "removed" the FS needs.
 * While a goal is open the hook stays armed: at each tick of the hooked
 * routine the CARDSRV routine is run if its own timer is not running
 * (Nucleus runs it only on an expired timer; on a running one its
 * NU_Reset_Timer fails: firmware exit 0x000F "cardsrv") and the last call
 * is CD_RETRY_MS (host time) ago, at most CD_MAX_CALLS times and
 * CD_GIVE_UP_MS per goal. A running CARDSRV timer (the firmware arms it
 * after card I/O, about 13 s of guest time) is moved up in Nucleus's active
 * timer list so that it expires with the next timer (cd_expedite) and
 * Nucleus runs the routine itself; if that is not possible, it is waited for.
 * A firmware without such objects is left alone (the card still changes,
 * the firmware is not told).
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"
#include "cpu.h"
#include "exec/tb-flush.h"
#include "accel/tcg/cpu-loop.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/reset.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/card-assist.h"

#define TYPE_PMB887X_CARD_ASSIST "pmb887x-card-assist"
OBJECT_DECLARE_SIMPLE_TYPE(CardAssist, PMB887X_CARD_ASSIST)

#define TYPE_SD_SLOT "sd-slot"

struct CardAssist {
	Object parent_obj;
	char *slot_path;
	bool debug;
	QEMUTimer *power_timer;
};

uint32_t pmb887x_card_writes;
uint32_t pmb887x_card_state;
uint32_t pmb887x_card_unmount_state;

static CardAssist *ca;

#define CA_LOG(...) do { \
	if (ca && ca->debug) \
		info_report("pmb887x-card-assist: " __VA_ARGS__); \
} while (0)

#define NU_TIMER_ID		0x54494D45	/* "TIME" */
#define NU_TIMER_SIZE	68
#define CD_RETRY_MS		1500
#define CD_MAX_CALLS	10
#define CD_GIVE_UP_MS	600000	/* a goal open this long (host time) is given up */
#define CD_POWER_POLL_NS	(20 * SCALE_MS)

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
	bool present;			/* what was asked for */
	bool gave_up;			/* the last goal was not reached */
	int calls;				/* CARDSRV routine runs for this goal */
	int hits, waits;		/* hook hits, and those with the CARDSRV timer running */
	int expedited;			/* times its running timer was moved up (cd_expedite) */
	bool timer_wait;		/* the last tick waited for a running CARDSRV timer it could not move up */
	int64_t next_call;		/* realtime ms */
	int64_t goal_start;		/* realtime ms (0: not yet hit) */
	uint32_t saved[6];		/* r0-r3, r12, lr */
	uint32_t saved_sp;
	uint32_t ret_sp;		/* SP the injected call comes back with */
	uint32_t gbs_send;		/* GBS_SendMessage (bit 0 = Thumb), 0 not looked for, 1 not found */
	bool unmount;			/* pmb887x_card_unmount() checked it: the hook sends "Eject card" */
	bool hide;				/* removed, the firmware has not seen it yet */
	bool absent_seen;		/* a command failed while hidden */
	SDBus *bus;				/* the bus the firmware talks to the card on */
} cd = { .present = true };

static int cd_fail_at;	/* why cd_expedite last did nothing (its check number) */

/* ---------------- the slot ---------------- */

static Object *ca_slot(Error **errp) {
	Object *slot;
	bool ambiguous = false;

	if (ca && ca->slot_path) {
		slot = object_resolve_path_type(ca->slot_path, TYPE_SD_SLOT, &ambiguous);
	} else {
		slot = object_resolve_path_type("", TYPE_SD_SLOT, &ambiguous);
	}
	if (!slot)
		error_setg(errp, ambiguous ? "more than one sd-slot: name one with slot=" : "no sd-slot on this board");
	return slot;
}

static bool ca_slot_set(bool inserted, Error **errp) {
	Object *slot = ca_slot(errp);
	return slot && object_property_set_bool(slot, "inserted", inserted, errp);
}

/* ---------------- state for the page ---------------- */

/* 0 in, 1 out, 2 going out, 3 reading, 4 not seen (gave up);
 * | 0x10: waiting for the firmware's card timer (could not move it up) */
static void cd_publish(void) {
	uint32_t st;
	if (cd.want == CD_WANT_OUT || cd.insert_waiting)
		st = 2;
	else if (cd.want == CD_WANT_IN)
		st = 3;
	else if (cd.gave_up)
		st = 4;
	else
		st = cd.present ? 0 : 1;
	if ((st == 2 || st == 3) && cd.timer_wait)
		st |= 0x10;
	qatomic_set(&pmb887x_card_state, st);
}

/* ---------------- goals ---------------- */

static void cd_hook(CPUARMState *env, uintptr_t ra);
static bool cd_ready(void);

static void cd_arm(void) {
	if (cd.state == CD_IDLE) {
		cd.state = CD_CALL;
		qatomic_set(&arm_pc_hook_fn, cd_hook);
		qatomic_set(&arm_pc_hook_addr, cd.hook & ~1);
		queue_tb_flush(first_cpu);
	}
}

static void cd_disarm(CPUState *cs) {
	qatomic_set(&arm_pc_hook_addr, UINT32_MAX);
	queue_tb_flush(cs);
}

/* a new goal: the hook runs the CARDSRV routine until it is reached */
static void cd_set_goal(int want) {
	cd.want = want;
	cd.calls = cd.hits = cd.waits = cd.expedited = 0;
	cd.gave_up = false;
	cd.next_call = 0;
	cd.goal_start = 0;
	cd.timer_wait = false;
	if (want != CD_WANT_NONE)
		cd_arm();
	if (want == CD_WANT_OUT && ca)
		timer_mod(ca->power_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CD_POWER_POLL_NS);
	cd_publish();
}

/* the held insertion: the slot changes under the BQL, on the main loop;
 * opaque non-NULL: after a reset, which leaves the reading to the new firmware */
static void cd_insert_bh(void *opaque) {
	Error *err = NULL;

	if (!cd.present || cd.hide)
		return;
	if (!ca_slot_set(true, &err)) {
		error_report_err(err);
		return;
	}
	if (!opaque && cd_ready())
		cd_set_goal(CD_WANT_IN);
	cd_publish();
}

static void cd_release_insert_as(void *opaque) {
	if (!cd.insert_waiting)
		return;
	cd.insert_waiting = false;
	aio_bh_schedule_oneshot(qemu_get_aio_context(), cd_insert_bh, opaque);
}

static void cd_release_insert(void) {
	cd_release_insert_as(NULL);
}

static void cd_goal_done(const char *what) {
	CA_LOG("%s (%d call(s), %d hook hits, %d with its timer running, moved up %d, last no-move check %d)",
			what, cd.calls, cd.hits, cd.waits, cd.expedited, cd_fail_at);
	cd.want = CD_WANT_NONE;
	cd.calls = 0;
	cd_release_insert();
	cd_publish();
}

/* (in the hook: no virtual clock there, icount forbids reading it mid-TB) */
static void cd_give_up(void) {
	qemu_log_mask(LOG_UNIMP, "pmb887x-card-assist: the firmware did not %s after %d calls, giving up\n",
			cd.want == CD_WANT_OUT ? "see the removal" : "read the card", cd.calls);
	if (cd.want == CD_WANT_OUT) {
		/* the slot is empty anyway; do not hold an insertion forever */
		cd.hide = false;
		cd.absent_seen = false;
	}
	cd.want = CD_WANT_NONE;
	cd.gave_up = true;
	cd_release_insert();
	cd_publish();
}

/* ---------------- what the firmware does with the card ---------------- */

static bool cd_card_powered(void) {
	DeviceState *mci = cd.bus ? DEVICE(BUS(cd.bus)->parent) : NULL;
	uint64_t power = 0;

	if (!mci || !object_dynamic_cast(OBJECT(mci), TYPE_SYS_BUS_DEVICE))
		return true;
	memory_region_dispatch_read(sysbus_mmio_get_region(SYS_BUS_DEVICE(mci), 0), MCI_POWER, &power,
			MO_32, MEMTXATTRS_UNSPECIFIED);
	return (power & MCI_POWER_CTRL) != MCI_POWER_CTRL_POWER_OFF;
}

/* while the firmware is to see a removal: has its rescan failed and switched the card off? */
static void cd_power_poll(void *opaque) {
	if (!cd.hide)
		return;
	if (cd.absent_seen && !cd_card_powered()) {
		cd.hide = false;
		cd.absent_seen = false;
		if (cd.want == CD_WANT_OUT)
			cd_goal_done("the firmware saw the removal");
		return;
	}
	timer_mod(ca->power_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CD_POWER_POLL_NS);
}

static void cd_observe(SDBus *bus, const SDRequest *req, size_t rlen) {
	cd.bus = bus;
	if (rlen == 0) {
		if (cd.hide)
			cd.absent_seen = true;
		return;
	}
	switch (req->cmd) {
		case 17:
		case 18:
			/* a block read: an insertion the firmware was to notice is done */
			if (cd.want == CD_WANT_IN && !cd.hide)
				cd_goal_done("the firmware reads the card again");
			break;
		case 24:
		case 25:
		case 38:
			qatomic_inc(&pmb887x_card_writes);
			break;
	}
}

/* ---------------- Nucleus timers ---------------- */

typedef struct {
	uint32_t timer, routine, arg;
	uint32_t hooks[16];
	unsigned hook_count[16];
	unsigned timers;
} cd_scan_t;

/* one RAM range of the system address space: look for timer control blocks */
static bool cd_scan_range(Int128 start, Int128 len, const MemoryRegion *mr,
		hwaddr offset, void *opaque) {
	cd_scan_t *sc = opaque;
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
static bool cd_find(void) {
	cd_scan_t sc = {};
	unsigned best = 0;

	RCU_READ_LOCK_GUARD();
	flatview_for_each_range(address_space_to_flatview(&address_space_memory), cd_scan_range, &sc);
	cd.timer = sc.timer;
	cd.routine = sc.routine;
	cd.arg = sc.arg;
	cd.hook = 0;
	for (int i = 0; i < ARRAY_SIZE(sc.hooks); i++) {
		if (sc.hook_count[i] > best && sc.hooks[i] != sc.routine) {
			best = sc.hook_count[i];
			cd.hook = sc.hooks[i];
		}
	}
	CA_LOG("%u Nucleus timers, CARDSRV routine %08X(%08X), hook %08X (%u enabled timers)",
			sc.timers, cd.routine, cd.arg, cd.hook, best);
	return cd.routine && cd.hook;
}

#define TM_NEXT		12
#define TM_PREV		16

static bool cd_rd(CPUState *cs, uint32_t va, uint32_t *v) {
	uint8_t b[4];
	if (cpu_memory_rw_debug(cs, va, b, 4, false))
		return false;
	*v = ldl_le_p(b);
	return true;
}

static bool cd_wr(CPUState *cs, uint32_t va, uint32_t v) {
	uint8_t b[4];
	stl_le_p(b, v);
	return !cpu_memory_rw_debug(cs, va, b, 4, true);
}

static bool cd_fail(int at) {
	cd_fail_at = at;
	return false;
}

/* the active list, walked from T: its members, sorted (false: inconsistent) */
#define CD_MAX_MEMBERS	512
typedef struct {
	uint32_t m[CD_MAX_MEMBERS];
	int n;
	uint8_t *found;			/* host pointer of the one outside pointer */
	int hits;
} cd_list_t;

static int cd_cmp(const void *a, const void *b) {
	uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
	return x < y ? -1 : x > y;
}

static bool cd_members(CPUState *cs, uint32_t t, cd_list_t *l) {
	uint32_t v = t, nx, back;
	l->n = 0;
	do {
		if (l->n >= CD_MAX_MEMBERS || !cd_rd(cs, v + TM_NEXT, &nx) ||
				!cd_rd(cs, nx + TM_PREV, &back) || back != v)
			return false;
		l->m[l->n++] = v;
		v = nx;
	} while (v != t);
	qsort(l->m, l->n, sizeof(l->m[0]), cd_cmp);
	return true;
}

static bool cd_is_member(const cd_list_t *l, uint32_t v) {
	return v >= l->m[0] && v <= l->m[l->n - 1] &&
		bsearch(&v, l->m, l->n, sizeof(v), cd_cmp);
}

/* a TM_TCB-looking node at host p: type 0/1, both links are members */
static bool cd_node_at(const cd_list_t *l, const uint8_t *p) {
	return ldl_le_p(p) <= 1 && cd_is_member(l, ldl_le_p(p + TM_NEXT)) &&
		cd_is_member(l, ldl_le_p(p + TM_PREV));
}

/* TMD_Active_Timers_List: the one RAM word pointing at a member that is not
 * a member's own next/previous field */
static bool cd_head_range(Int128 start, Int128 len, const MemoryRegion *mr,
		hwaddr offset, void *opaque) {
	cd_list_t *l = opaque;
	uint64_t size = int128_get64(len), base = int128_get64(start);
	uint8_t *ram;

	if (!memory_region_is_ram((MemoryRegion *) mr) || size < 1024 * 1024 || base > UINT32_MAX)
		return false;
	ram = (uint8_t *) memory_region_get_ram_ptr((MemoryRegion *) mr) + offset;
	for (uint64_t o = 16; o + 8 <= size; o += 4) {
		if (!cd_is_member(l, ldl_le_p(ram + o)))
			continue;
		if (cd_node_at(l, ram + o - TM_NEXT) || cd_node_at(l, ram + o - TM_PREV))
			continue;
		l->hits++;
		l->found = ram + o;
	}
	return false;
}

static uint8_t *cd_head_ptr;	/* host pointer of TMD_Active_Timers_List */

static bool cd_find_head(CPUState *cs, uint32_t t) {
	g_autofree cd_list_t *l = g_new0(cd_list_t, 1);

	if (cd_head_ptr) {
		/* still a member of the list T is in? */
		if (cd_members(cs, t, l) && cd_is_member(l, ldl_le_p(cd_head_ptr)))
			return true;
		cd_head_ptr = NULL;
	}
	if (!cd_members(cs, t, l))
		return cd_fail(10);
	{
		RCU_READ_LOCK_GUARD();
		flatview_for_each_range(address_space_to_flatview(&address_space_memory), cd_head_range, l);
	}
	CA_LOG("%d active timers, %d list head candidate(s)", l->n, l->hits);
	if (l->hits != 1)
		return cd_fail(11);
	cd_head_ptr = l->found;
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
 * member's own link field (cd_find_head), checked again each time.
 *
 * The CARDSRV entry T is unlinked (its delta added to the one after it) and
 * put right after the head H with delta 0: it expires when H does (the next
 * timer of all; the hooked routine's timers fire every 0.2-1 s). If T is the
 * head, its remaining time becomes 0 (the next one keeps its time). This
 * runs in the hook, i.e. in the timer HISR inside Nucleus's expiration loop,
 * which is the context Nucleus changes the list in. On any mismatch nothing
 * is written and the object waits as before.
 */
static bool cd_expedite(CPUState *cs, CPUARMState *env) {
	uint32_t t = cd.timer + 48, h, tn, tp, trem, tnrem, hn;

	if (!cd_find_head(cs, t))
		return false;
	h = ldl_le_p(cd_head_ptr);
	if (!cd_rd(cs, t + TM_NEXT, &tn) || !cd_rd(cs, t + TM_PREV, &tp) ||
			!cd_rd(cs, t + 4, &trem))
		return cd_fail(5);
	if (trem == 0 && (t == h || tp == h))
		return cd_fail(7);			/* already due with the head */

	if (t == h) {
		if (tn != t) {
			if (!cd_rd(cs, tn + 4, &tnrem) || !cd_wr(cs, tn + 4, tnrem + trem))
				return cd_fail(8);
		}
		cd_wr(cs, t + 4, 0);
		return true;
	}

	/* unlink T: the one after it keeps its time (unless it wraps to H) */
	if (tn != h) {
		if (!cd_rd(cs, tn + 4, &tnrem) || !cd_wr(cs, tn + 4, tnrem + trem))
			return cd_fail(8);
	}
	cd_wr(cs, tp + TM_NEXT, tn);
	cd_wr(cs, tn + TM_PREV, tp);
	/* and after H, delta 0 */
	if (!cd_rd(cs, h + TM_NEXT, &hn))
		return cd_fail(9);
	cd_wr(cs, t + 4, 0);
	cd_wr(cs, t + TM_PREV, h);
	cd_wr(cs, t + TM_NEXT, hn);
	cd_wr(cs, hn + TM_PREV, t);
	cd_wr(cs, h + TM_NEXT, t);
	return true;
}

static void cd_hook_idle(CPUState *cs) {
	if (cd.want == CD_WANT_NONE) {
		cd.state = CD_IDLE;
		cd_disarm(cs);
	}
}

static void cd_hook_locked(CPUARMState *env, uintptr_t ra) {
	CPUState *cs = env_cpu(env);

	if (env->thumb != (cd.hook & 1))
		return;

	if (cd.state == CD_CALL && cd.unmount && cd.gbs_send > 1) {
		/* clean unmount: GBS_SendMessage(0x4100, 0x18, 0, 4, 0), as "Eject card" */
		cpu_restore_state(cs, ra);
		for (int i = 0; i < 4; i++)
			cd.saved[i] = env->regs[i];
		cd.saved[4] = env->regs[12];
		cd.saved[5] = env->regs[14];
		cd.saved_sp = env->regs[13];
		cd.ret_sp = env->regs[13] - 8;
		if (!cd_wr(cs, cd.ret_sp, 0)) {
			cd.unmount = false;
			qatomic_set(&pmb887x_card_unmount_state, 3);
			return;
		}
		env->regs[13] = cd.ret_sp;
		env->regs[0] = 0x4100;
		env->regs[1] = 0x18;
		env->regs[2] = 0;
		env->regs[3] = 4;
		env->regs[14] = cd.hook;
		env->regs[15] = cd.gbs_send & ~1;
		env->thumb = cd.gbs_send & 1;
		cd.state = CD_RETURN;
		cd.unmount = false;
		qatomic_set(&pmb887x_card_unmount_state, 2);
		cpu_loop_exit(cs);
	}

	if (cd.state == CD_CALL) {
		uint8_t enabled[4];
		int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

		if (cd.want == CD_WANT_NONE) {
			cd_hook_idle(cs);
			return;
		}
		cd.hits++;
		if (!cd.goal_start)
			cd.goal_start = now;
		if (now - cd.goal_start > CD_GIVE_UP_MS) {
			cd_give_up();
			cd_hook_idle(cs);
			return;
		}
		if (now < cd.next_call)
			return;
		/* only on an expired timer, as Nucleus does (see above); a running
		 * one is waited for: its own expiry may do the rescan */
		if (cpu_memory_rw_debug(cs, cd.timer + 32, enabled, 4, false) || ldl_le_p(enabled)) {
			cd.waits++;
			if (cd_expedite(cs, env)) {
				cd.expedited++;
				cd.timer_wait = false;
			} else {
				cd.timer_wait = true;
			}
			cd_publish();
			return;
		}
		if (cd.timer_wait) {
			cd.timer_wait = false;
			cd_publish();
		}
		if (cd.calls >= CD_MAX_CALLS) {
			cd_give_up();
			cd_hook_idle(cs);
			return;
		}
		cd.calls++;
		cd.next_call = now + CD_RETRY_MS;
		/* the insn at the hook has not run: back to its start, exact icount */
		cpu_restore_state(cs, ra);
		for (int i = 0; i < 4; i++)
			cd.saved[i] = env->regs[i];
		cd.saved[4] = env->regs[12];
		cd.saved[5] = env->regs[14];
		cd.saved_sp = env->regs[13];
		cd.ret_sp = env->regs[13];
		env->regs[0] = cd.arg;
		env->regs[14] = cd.hook;
		env->regs[15] = cd.routine & ~1;
		env->thumb = cd.routine & 1;
		cd.state = CD_RETURN;
		cpu_loop_exit(cs);
	}

	if (cd.state == CD_RETURN && env->regs[13] == cd.ret_sp) {
		/* the injected call returned here: the original call goes on */
		for (int i = 0; i < 4; i++)
			env->regs[i] = cd.saved[i];
		env->regs[12] = cd.saved[4];
		env->regs[13] = cd.saved_sp;
		env->regs[14] = cd.saved[5];
		if (cd.want != CD_WANT_NONE || cd.unmount)
			cd.state = CD_CALL;		/* next tick: check again */
		else {
			cd.state = CD_IDLE;
			cd_disarm(cs);
		}
	}
}

/*
 * The goals and the unmount are set on the main loop under the BQL; the hook
 * runs on the vCPU and takes it too, so neither sees the other half-done.
 * (cpu_loop_exit's longjmp gives it back.)
 */
static void cd_hook(CPUARMState *env, uintptr_t ra) {
	bool locked = bql_locked();

	if (!locked)
		bql_lock();
	cd_hook_locked(env, ra);
	if (!locked)
		bql_unlock();
}

/* can the firmware be told? (finds the card server and the hook point) */
static bool cd_ready(void) {
	if (!first_cpu)
		return false;
	/* (again each time, the enabled timers change; not while the hook is armed
	 * on the routine found before) */
	if (cd.state == CD_IDLE && !cd_find()) {
		CA_LOG("no CARDSRV timer, the firmware is not told");
		return false;
	}
	return true;
}

/* ---------------- clean unmount ---------------- */

/*
 * As the firmware's "Eject card": message 0x18 to the memory card file system
 * process (0x4100) with the drive (4). The file system writes its FAT and
 * directories to the card and drops its cache; the next access mounts the
 * card again. A card taken out after it gives no "Data lost" note (that one
 * is for a card removed while mounted). It is refused (nothing changes) while
 * a file on the card is open. GBS_SendMessage is found by the ElfPack function
 * library patterns (sdk/swilib/patterns.toml, function 0x100: ELKA Thumb,
 * NSG ARM).
 */
static const int16_t gbs_elka[] = {
	-1,0xB4,-1,0xB5,-1,0x1C,-1,0x1C,-1,0x1C,-1,0x9D,-1,-1,-1,0xF9,-1,0x0F,-1,0x92,-1,0x04,-1,0x0C,
	-1,0x1C,-1,0xA8,-1,0x99,-1,-1,-1,0xFC,-1,0x60,-1,0xC0,-1,0xA8,-1,-1,-1,0xFA,-1,0xBC,-1,0xBC,
	-1,0xB0,-1,0x47,
};
static const int16_t gbs_nsg[] = {
	-1,-1,-1,0xE9,-1,-1,-1,0xE5,-1,-1,-1,0xE1,-1,-1,-1,0xE1,-1,-1,-1,0xE1,-1,-1,-1,0xE1,-1,-1,-1,-1,
	-1,-1,-1,0xE1,-1,-1,-1,0xE5,-1,-1,-1,0xE1,-1,-1,-1,0xE1,-1,-1,-1,0xE2,-1,-1,-1,0xE1,-1,-1,-1,0xE1,
	-1,-1,-1,-1,-1,-1,-1,0xE8,-1,-1,-1,0xE5,-1,-1,-1,0xE2,-1,-1,-1,-1,-1,-1,-1,0xE8,
};

#define FLASH_BASE	0xA0000000u
#define FLASH_MAX	(64u << 20)
#define SCAN_CHUNK	(64u << 10)

/* matches starting in b[0 .. starts) (b holds starts + 128 bytes, a pattern is shorter) */
static int pat_scan(const uint8_t *b, uint32_t starts, uint32_t base, const int16_t *pat, int len,
		int align, uint32_t *at) {
	int hits = 0;
	for (uint32_t o = 0; o < starts; o += align) {
		int k = 0;
		while (k < len && (pat[k] < 0 || b[o + k] == pat[k]))
			k++;
		if (k == len) {
			hits++;
			*at = base + o;
		}
	}
	return hits;
}

static uint32_t find_gbs_send(CPUState *cs) {
	g_autofree uint8_t *buf = g_malloc(SCAN_CHUNK + 128);
	uint32_t at_t = 0, at_a = 0, ov = 0;
	int ht = 0, ha = 0;
	for (uint32_t off = 0; off < FLASH_MAX; off += SCAN_CHUNK) {
		/* (the last 128 bytes of the chunk before go first: no pattern is cut;
		 * each start is looked at once, the chunk's last 128 bytes next time) */
		if (cpu_memory_rw_debug(cs, FLASH_BASE + off, buf + 128, SCAN_CHUNK, false))
			break;
		uint32_t base = FLASH_BASE + off - ov;
		const uint8_t *b = buf + 128 - ov;
		ht += pat_scan(b, SCAN_CHUNK + ov - 128, base, gbs_elka, ARRAY_SIZE(gbs_elka), 2, &at_t);
		ha += pat_scan(b, SCAN_CHUNK + ov - 128, base, gbs_nsg, ARRAY_SIZE(gbs_nsg), 4, &at_a);
		memcpy(buf, buf + SCAN_CHUNK, 128);
		ov = 128;
	}
	if (ht + ha != 1)
		return 1;
	return ht ? at_t | 1 : at_a;
}

void pmb887x_card_unmount(void) {
	if (!ca || !cd.present || cd.hide || !cd_ready()) {
		qatomic_set(&pmb887x_card_unmount_state, 3);
		return;
	}
	if (!cd.gbs_send)
		cd.gbs_send = find_gbs_send(first_cpu);
	CA_LOG("unmount: GBS_SendMessage %08X", cd.gbs_send);
	if (cd.gbs_send == 1) {
		qatomic_set(&pmb887x_card_unmount_state, 3);
		return;
	}
	cd.unmount = true;
	qatomic_set(&pmb887x_card_unmount_state, 1);
	cd_arm();
}

/* ---------------- taking the card out and putting it back ---------------- */

bool pmb887x_card_set_inserted(bool inserted, Error **errp) {
	if (!ca)
		return ca_slot_set(inserted, errp);
	if (!ca_slot(errp))
		return false;

	CA_LOG("card %s", inserted ? "inserted" : "removed");
	if (cd.want != CD_WANT_NONE)
		CA_LOG("open goal so far: %d call(s), %d hook hits, %d with its timer running, moved up %d",
				cd.calls, cd.hits, cd.waits, cd.expedited);

	if (!inserted) {
		cd.insert_waiting = false;
		if (cd.unmount) {
			/* an "Eject card" still to send would reach the next card */
			cd.unmount = false;
			qatomic_set(&pmb887x_card_unmount_state, 3);
		}
		if (!ca_slot_set(false, errp))
			return false;
		cd.present = false;
		if (!cd_ready()) {
			cd_publish();
			return true;
		}
		if (cd.want != CD_WANT_OUT && !cd.hide) {
			cd.hide = true;
			cd.absent_seen = false;
			cd_set_goal(CD_WANT_OUT);
		}
		/* else: still going out, the same goal */
		cd_publish();
		return true;
	}

	if (cd.hide) {
		/* the slot stays empty until the firmware has seen the removal */
		cd.present = true;
		cd.insert_waiting = true;
		CA_LOG("insert held until the firmware has seen the removal");
		cd_publish();
		return true;
	}
	if (!ca_slot_set(true, errp))
		return false;
	cd.present = true;
	if (cd_ready())
		cd_set_goal(CD_WANT_IN);
	cd_publish();
	return true;
}

/* ---------------- the object ---------------- */

static bool ca_get_inserted(Object *obj, Error **errp) {
	return cd.present;
}

static void ca_set_inserted(Object *obj, bool inserted, Error **errp) {
	pmb887x_card_set_inserted(inserted, errp);
}

static void ca_get_u32(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp) {
	uint32_t value = qatomic_read((uint32_t *) opaque);
	visit_type_uint32(v, name, &value, errp);
}

static char *ca_get_slot(Object *obj, Error **errp) {
	return g_strdup(PMB887X_CARD_ASSIST(obj)->slot_path);
}

static void ca_set_slot(Object *obj, const char *value, Error **errp) {
	CardAssist *s = PMB887X_CARD_ASSIST(obj);
	g_free(s->slot_path);
	s->slot_path = g_strdup(value);
}

static bool ca_get_debug(Object *obj, Error **errp) {
	return PMB887X_CARD_ASSIST(obj)->debug;
}

static void ca_set_debug(Object *obj, bool value, Error **errp) {
	PMB887X_CARD_ASSIST(obj)->debug = value;
}

/* a reboot ends whatever the firmware was being told: the new one scans the slot itself */
static void ca_reset(void *opaque) {
	if (cd.state != CD_IDLE)
		cd_disarm(first_cpu);
	cd.state = CD_IDLE;
	cd.want = CD_WANT_NONE;
	cd.hide = false;
	cd.timer_wait = false;
	cd.gave_up = false;
	if (cd.unmount) {
		cd.unmount = false;
		qatomic_set(&pmb887x_card_unmount_state, 3);
	}
	cd_release_insert_as((void *) 1);
	cd_publish();
}

static void ca_complete(UserCreatable *uc, Error **errp) {
	CardAssist *s = PMB887X_CARD_ASSIST(uc);

	if (ca) {
		error_setg(errp, "only one " TYPE_PMB887X_CARD_ASSIST " can exist");
		return;
	}
	ca = s;
	s->power_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cd_power_poll, s);
	sdbus_command_observer = cd_observe;
	qemu_register_reset(ca_reset, NULL);
	cd_publish();
}

static bool ca_prepare_delete(UserCreatable *uc, Error **errp) {
	error_setg(errp, TYPE_PMB887X_CARD_ASSIST " cannot be deleted");
	return false;
}

static void ca_finalize(Object *obj) {
	CardAssist *s = PMB887X_CARD_ASSIST(obj);
	g_free(s->slot_path);
}

static void ca_class_init(ObjectClass *klass, const void *data) {
	UserCreatableClass *ucc = USER_CREATABLE_CLASS(klass);

	ucc->complete = ca_complete;
	ucc->prepare_delete = ca_prepare_delete;
	object_class_property_add_str(klass, "slot", ca_get_slot, ca_set_slot);
	object_class_property_set_description(klass, "slot", "QOM path of the sd-slot (default: the only one)");
	object_class_property_add_bool(klass, "debug", ca_get_debug, ca_set_debug);
	object_class_property_add_bool(klass, "inserted", ca_get_inserted, ca_set_inserted);
	object_class_property_set_description(klass, "inserted",
		"Whether a card is in the slot; the firmware is told of a change");
	object_class_property_add(klass, "state", "uint32", ca_get_u32, NULL, NULL, &pmb887x_card_state);
	object_class_property_set_description(klass, "state",
		"0 in, 1 out, 2 going out, 3 reading, 4 not seen; | 16 waiting for the firmware's timer");
	object_class_property_add(klass, "writes", "uint32", ca_get_u32, NULL, NULL, &pmb887x_card_writes);
	object_class_property_add(klass, "unmount-state", "uint32", ca_get_u32, NULL, NULL,
			&pmb887x_card_unmount_state);
}

static const TypeInfo ca_info = {
	.name = TYPE_PMB887X_CARD_ASSIST,
	.parent = TYPE_OBJECT,
	.instance_size = sizeof(CardAssist),
	.instance_finalize = ca_finalize,
	.class_init = ca_class_init,
	.interfaces = (const InterfaceInfo[]) {
		{ TYPE_USER_CREATABLE },
		{ NULL },
	},
};

static void ca_register_types(void) {
	type_register_static(&ca_info);
}

type_init(ca_register_types)
