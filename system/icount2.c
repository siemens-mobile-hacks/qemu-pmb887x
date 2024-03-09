#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "exec/exec-all.h"
#include "sysemu/cpus.h"
#include "sysemu/qtest.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qemu/seqlock.h"
#include "sysemu/replay.h"
#include "sysemu/runstate.h"
#include "hw/core/cpu.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/cpu-throttle.h"
#include "sysemu/cpu-timers-internal.h"

bool use_icount2 = false;

static void icount2_idle_timer(void *opaque) {
	if (timers_state.icount2_idle_deadline) {
		seqlock_write_lock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
		int64_t bias = qatomic_read_i64(&timers_state.icount2_bias);
		qatomic_set_i64(&timers_state.icount2_bias, bias + timers_state.icount2_idle_deadline);
		timers_state.icount2_idle_deadline = 0;
		seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
	}
	
	int64_t deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL, QEMU_TIMER_ATTR_ALL);
	if (deadline == 0) {
		qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
		qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
		deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL, QEMU_TIMER_ATTR_ALL);
	}
	
	if (deadline < 0 || deadline > 1000000)
		deadline = 1000000;
	
	timers_state.icount2_idle_deadline = deadline;
	timer_mod(timers_state.icount2_idle_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + deadline);
}

void icount2_sync(void) {
	if (timers_state.icount2_idle)
		return;
	
	int64_t deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL, QEMU_TIMER_ATTR_ALL);
	if (deadline < 0) {
		qatomic_set_i64(&timers_state.icount2_deadline, 0);
		return;
	}
	
	if (deadline == 0) {
		qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
		qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
		deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL, QEMU_TIMER_ATTR_ALL);
	}
	
	if (deadline < 0) {
		qatomic_set_i64(&timers_state.icount2_deadline, 0);
		return;
	}
	
	int64_t ticks = qatomic_read_i64(&timers_state.icount2_ticks);
	int64_t ns_per_tick = qatomic_read_i64(&timers_state.icount2_ns_per_tick);
	qatomic_set_i64(&timers_state.icount2_deadline, ticks + DIV_ROUND_UP(deadline, ns_per_tick));
}

void icount2_on_tick(void) {
	int64_t ticks = qatomic_read_i64(&timers_state.icount2_ticks);
	qatomic_set_i64(&timers_state.icount2_ticks, ticks + 1);
	
	int64_t deadline = qatomic_read_i64(&timers_state.icount2_deadline);
	if (deadline > 0 && ticks >= deadline) {
		bql_lock();
		icount2_sync();
		bql_unlock();
	}
}

static int64_t icount2_get_locked(void) {
	int64_t ticks = qatomic_read_i64(&timers_state.icount2_ticks);
	int64_t offset = qatomic_read_i64(&timers_state.icount2_offset);
	int64_t ns_per_tick = qatomic_read_i64(&timers_state.icount2_ns_per_tick);
	int64_t bias = qatomic_read_i64(&timers_state.icount2_bias);
	return bias + ((ticks - offset) * ns_per_tick);
}

int64_t icount2_get(void) {
	int64_t time;
	unsigned start;
	do {
		start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
		time = icount2_get_locked();
	} while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));
	return time;
}

void icount2_set_ns_per_tick(int64_t ns_per_tick) {
	seqlock_write_lock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
	int64_t ticks = qatomic_read_i64(&timers_state.icount2_ticks);
	int64_t offset = qatomic_read_i64(&timers_state.icount2_offset);
	int64_t old_ns_per_tick = qatomic_read_i64(&timers_state.icount2_ns_per_tick);
	int64_t bias = qatomic_read_i64(&timers_state.icount2_bias);
	qatomic_set_i64(&timers_state.icount2_bias, bias + ((ticks - offset) * old_ns_per_tick));
	qatomic_set_i64(&timers_state.icount2_offset, ticks);
	qatomic_set_i64(&timers_state.icount2_ns_per_tick, ns_per_tick);
	qatomic_set_i64(&timers_state.icount2_deadline, ticks);
	seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
}

void icount2_enter_sleep(void) {
	timers_state.icount2_idle = true;
	timers_state.icount2_idle_deadline = 0;
	icount2_idle_timer(NULL);
}

void icount2_exit_sleep(void) {
	timers_state.icount2_idle = false;
	timers_state.icount2_idle_deadline = 0;
	timer_del(timers_state.icount2_idle_timer);
	icount2_sync();
}

void icount2_configure(QemuOpts *opts, Error **errp) {
	use_icount2 = true;
	qatomic_set_i64(&timers_state.icount2_ns_per_tick, 1);
	
	timers_state.icount2_idle_timer = timer_new_ns(QEMU_CLOCK_REALTIME, icount2_idle_timer, NULL);
}
