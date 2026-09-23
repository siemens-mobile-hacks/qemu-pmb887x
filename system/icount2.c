#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/cpus.h"
#include "system/qtest.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qemu/seqlock.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "system/cpu-timers.h"
#include "system/cpu-throttle.h"
#include "system/cpu-timers-internal.h"

bool use_icount2 = false;
static bool icount2_debug;

#define ICOUNT2_INITIAL_FREQUENCY 314000000
#define ICOUNT2_MIN_FREQUENCY 1000000
#define ICOUNT2_MAX_FREQUENCY 500000000
#define ICOUNT2_ADJUST_INTERVAL NANOSECONDS_PER_SECOND
#define ICOUNT2_ADJUST_P_GAIN 200000
#define ICOUNT2_ADJUST_I_GAIN 20000
#define ICOUNT2_ADJUST_MAX_CORRECTION 300000
#define ICOUNT2_ADJUST_MAX_ERROR (5 * NANOSECONDS_PER_SECOND)
#define ICOUNT2_ADJUST_SCALE 1000000

static int64_t icount2_get_locked(void);
static int64_t (*icount2_limit_fn)(void *opaque, int64_t want);
static bool icount2_idle_running_timers;
/* The vCPU was woken by one of its timers, at that timer's time. */
static bool icount2_idle_timer_wakeup;
static void *icount2_limit_opaque;

/* EXTERNAL timers follow the guest's progress, so they do not wake a sleeping vCPU. */
static int64_t icount2_idle_deadline(void) {
	return qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL, QEMU_TIMER_ATTR_ALL & ~QEMU_TIMER_ATTR_EXTERNAL);
}

static int64_t icount2_limit(int64_t want) {
	if (icount2_limit_fn == NULL)
		return INT64_MAX;
	return icount2_limit_fn(icount2_limit_opaque, want);
}

/*
 * A sleeping vCPU's clock follows the real one, as it does on hardware, up to
 * its next timer and no further than the hardware simulated beside it has got.
 */
static int64_t icount2_idle_catch_up(void) {
	int64_t now = icount2_get();
	int64_t deadline = icount2_idle_deadline();
	int64_t target = cpu_get_clock();

	if (deadline >= 0)
		target = MIN(target, now + deadline);
	if (target > now)
		target = MIN(target, icount2_limit(-1));
	if (target <= now)
		return now;

	seqlock_write_lock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
	qatomic_set(&timers_state.icount2_bias, qatomic_read(&timers_state.icount2_bias) + target - now);
	seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
	return target;
}

static void icount2_set_idle_end(int64_t end) {
	seqlock_write_lock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
	timers_state.icount2_idle_end = end;
	seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
}

/*
 * Once woken, the vCPU resumes where the wake found it: see icount2_exit_sleep().
 * An interrupt raised and dropped again before the vCPU thread looked leaves
 * it asleep, and its clock carries on.
 */
static bool icount2_woken(void) {
	if (!timers_state.icount2_idle_wakeup)
		return false;
	if (!all_cpu_threads_idle())
		return true;
	timers_state.icount2_idle_wakeup = false;
	icount2_idle_timer_wakeup = false;
	return false;
}

static void icount2_idle_timer(void *opaque) {
	if (!timers_state.icount2_idle || icount2_woken())
		return;

	for (;;) {
		int64_t now = icount2_idle_catch_up();
		int64_t deadline = icount2_idle_deadline();
		int64_t real;
		int64_t end;

		if (deadline == 0) {
			icount2_idle_running_timers = true;
			qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
			icount2_idle_running_timers = false;
			qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
			if (icount2_woken())
				return;
			continue;
		}

		end = deadline < 0 ? INT64_MAX : now + deadline;
		icount2_set_idle_end(end);
		if (deadline < 0)
			return;

		real = cpu_get_clock();
		if (real < end) {
			timer_mod(timers_state.icount2_idle_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + end - real);
			return;
		}

		/*
		 * Short of the end but ahead of the vCPUs, the limit is something the
		 * vCPUs must first reach, such as an interrupt on its way to them.
		 * Otherwise its owner calls icount2_limit_advanced() once it gets there.
		 */
		if (icount2_limit(end) <= now)
			return;
	}
}

/*
 * How far the vCPUs are known to have got: their clock while they run, or,
 * while they sleep, as far as the real clock has carried them towards their
 * next timer.
 */
int64_t icount2_get_horizon(void) {
	int64_t time;
	unsigned start;

	do {
		start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
		time = icount2_get_locked();
		if (timers_state.icount2_idle)
			time = MAX(time, MIN(cpu_get_clock_locked(), timers_state.icount2_idle_end));
	} while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));
	return time;
}

/* Real time until the horizon of sleeping vCPUs reaches @target, or -1 while they run. */
int64_t icount2_get_horizon_delay(int64_t target) {
	int64_t delay;
	unsigned start;

	do {
		start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
		if (!timers_state.icount2_idle) {
			delay = -1;
		} else {
			delay = MAX(MIN(target, timers_state.icount2_idle_end) - cpu_get_clock_locked(), 0);
		}
	} while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));
	return delay;
}

/*
 * Hardware simulated on another thread limits how far sleeping vCPUs may
 * advance. @fn returns the virtual time it has been simulated up to; given a
 * @want other than -1, it must call icount2_limit_advanced() once it gets
 * there.
 */
void icount2_set_limit(int64_t (*fn)(void *opaque, int64_t want), void *opaque) {
	g_assert(fn == NULL || icount2_limit_fn == NULL);
	icount2_limit_opaque = opaque;
	icount2_limit_fn = fn;
}

void icount2_limit_advanced(void) {
	if (qatomic_read(&timers_state.icount2_idle))
		timer_mod(timers_state.icount2_idle_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
}

void icount2_sync(void) {
	if (timers_state.icount2_idle)
		return;
	
	int64_t deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL, QEMU_TIMER_ATTR_ALL);
	if (deadline < 0) {
		qatomic_set(&timers_state.icount2_deadline, 0);
		return;
	}
	
	if (deadline == 0) {
		qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
		qemu_clock_notify_aio_contexts(QEMU_CLOCK_VIRTUAL);
		deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL, QEMU_TIMER_ATTR_ALL);
	}
	
	if (deadline < 0) {
		qatomic_set(&timers_state.icount2_deadline, 0);
		return;
	}
	
	int64_t ticks = qatomic_read(&timers_state.icount2_ticks);
	uint32_t frequency = qatomic_read(&timers_state.icount2_frequency);
	uint64_t instructions = muldiv64_round_up(deadline, frequency, NANOSECONDS_PER_SECOND);
	qatomic_set(&timers_state.icount2_deadline, ticks + instructions);
}

void icount2_advance(uint32_t cycles) {
	int64_t ticks = qatomic_read(&timers_state.icount2_ticks);
	int64_t new_ticks = ticks + cycles;
	qatomic_set(&timers_state.icount2_ticks, new_ticks);
	
	int64_t deadline = qatomic_read(&timers_state.icount2_deadline);
	if (deadline > 0 && new_ticks >= deadline) {
		bql_lock();
		icount2_sync();
		bql_unlock();
	}
}

static int64_t icount2_get_locked(void) {
	int64_t ticks = qatomic_read(&timers_state.icount2_ticks);
	int64_t offset = qatomic_read(&timers_state.icount2_offset);
	uint32_t frequency = qatomic_read(&timers_state.icount2_frequency);
	int64_t bias = qatomic_read(&timers_state.icount2_bias);
	return bias + muldiv64(ticks - offset, NANOSECONDS_PER_SECOND, frequency);
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

static void icount2_set_frequency_locked(uint32_t frequency) {
	int64_t ticks = qatomic_read(&timers_state.icount2_ticks);
	int64_t time = icount2_get_locked();

	qatomic_set(&timers_state.icount2_bias, time);
	qatomic_set(&timers_state.icount2_offset, ticks);
	qatomic_set(&timers_state.icount2_frequency, frequency);
	qatomic_set(&timers_state.icount2_deadline, ticks);
}

static void icount2_adjust(void) {
	int64_t realtime;
	int64_t ticks;
	int64_t elapsed;
	int64_t executed;

	if (!runstate_is_running() || timers_state.icount2_idle)
		return;

	seqlock_write_lock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
	if (timers_state.icount2_idle) {
		seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
		return;
	}

	realtime = cpu_get_clock_locked();
	ticks = qatomic_read(&timers_state.icount2_ticks);

	if (!timers_state.icount2_adjust_initialized) {
		timers_state.icount2_adjust_realtime = realtime;
		timers_state.icount2_adjust_ticks = ticks;
		timers_state.icount2_adjust_initialized = true;
		seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
		return;
	}

	elapsed = realtime - timers_state.icount2_adjust_realtime;
	executed = ticks - timers_state.icount2_adjust_ticks;

	if (elapsed <= 0) {
		seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
		return;
	}
	if (elapsed > UINT32_MAX) {
		timers_state.icount2_adjust_realtime = realtime;
		timers_state.icount2_adjust_ticks = ticks;
		seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
		return;
	}
	if (executed <= 0 || elapsed < ICOUNT2_ADJUST_INTERVAL) {
		seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
		return;
	}

	timers_state.icount2_adjust_realtime = realtime;
	timers_state.icount2_adjust_ticks = ticks;

	uint32_t target_frequency = CLAMP(
		muldiv64(executed, NANOSECONDS_PER_SECOND, elapsed),
		ICOUNT2_MIN_FREQUENCY,
		ICOUNT2_MAX_FREQUENCY
	);
	uint32_t frequency = qatomic_read(&timers_state.icount2_frequency);
	int64_t virtual_elapsed = muldiv64(executed, NANOSECONDS_PER_SECOND, frequency);
	int64_t error = virtual_elapsed - elapsed;
	int64_t correction = 0;
	uint32_t new_frequency;
	const char *mode;

	if (!timers_state.icount2_adjust_locked) {
		timers_state.icount2_adjust_error = 0;
		timers_state.icount2_adjust_locked = true;
		new_frequency = target_frequency;
		mode = "lock";
	} else {
		timers_state.icount2_adjust_error = CLAMP(
			timers_state.icount2_adjust_error + error,
			-ICOUNT2_ADJUST_MAX_ERROR,
			ICOUNT2_ADJUST_MAX_ERROR
		);
		correction = error * ICOUNT2_ADJUST_P_GAIN / elapsed;
		correction += timers_state.icount2_adjust_error * ICOUNT2_ADJUST_I_GAIN / NANOSECONDS_PER_SECOND;
		correction = CLAMP(correction, -ICOUNT2_ADJUST_MAX_CORRECTION, ICOUNT2_ADJUST_MAX_CORRECTION);
		new_frequency = CLAMP(
			frequency + (int64_t) frequency * correction / ICOUNT2_ADJUST_SCALE,
			ICOUNT2_MIN_FREQUENCY,
			ICOUNT2_MAX_FREQUENCY
		);
		mode = "pi";
	}
	if (icount2_debug) {
		fprintf(stderr,
			"icount2 adjust: virtual=%.3f ms realtime=%.3f ms error(v-rt)=%+.3f ms "
			"total_error=%+.3f ms cycles=%" PRId64 " frequency=%.3f MHz target=%.3f MHz "
			"next=%.3f MHz correction=%+.3f%% mode=%s\n",
			(double) virtual_elapsed / SCALE_MS,
			(double) elapsed / SCALE_MS,
			(double) error / SCALE_MS,
			(double) timers_state.icount2_adjust_error / SCALE_MS,
			executed,
			(double) frequency / 1000000,
			(double) target_frequency / 1000000,
			(double) new_frequency / 1000000,
			(double) correction * 100 / ICOUNT2_ADJUST_SCALE,
			mode);
	}
	if (new_frequency != frequency) {
		icount2_set_frequency_locked(new_frequency);
	}

	seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
}

static void icount2_adjust_rt(void *opaque) {
	timer_mod(
		timers_state.icount_rt_timer,
		qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + ICOUNT2_ADJUST_INTERVAL / SCALE_MS
	);
	icount2_adjust();
}

void icount2_enter_sleep(void) {
	seqlock_write_lock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
	timers_state.icount2_idle_realtime = cpu_get_clock_locked();
	timers_state.icount2_idle_end = icount2_get_locked();
	qatomic_set(&timers_state.icount2_idle, true);
	seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);

	if (icount2_debug) {
		fprintf(stderr,
			"icount2 sleep: event=enter realtime=%" PRId64 " virtual=%" PRId64
			" deadline=%" PRId64 " ns\n",
			timers_state.icount2_idle_realtime,
			icount2_get(),
			icount2_idle_deadline());
	}

	timers_state.icount2_idle_wakeup = false;
	icount2_idle_timer_wakeup = false;
	icount2_idle_timer(NULL);
}

void icount2_exit_sleep(void) {
	int64_t idle_elapsed = 0;

	timer_del(timers_state.icount2_idle_timer);
	if (!icount2_idle_timer_wakeup)
		icount2_idle_catch_up();

	seqlock_write_lock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
	if (timers_state.icount2_adjust_initialized || icount2_debug)
		idle_elapsed = MAX(cpu_get_clock_locked() - timers_state.icount2_idle_realtime, 0);
	if (timers_state.icount2_adjust_initialized) {
		timers_state.icount2_adjust_realtime += idle_elapsed;
	}
	qatomic_set(&timers_state.icount2_idle, false);
	seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);

	timers_state.icount2_idle_wakeup = false;
	icount2_sync();

	if (icount2_debug) {
		fprintf(stderr,
			"icount2 sleep: event=exit realtime=%" PRId64 " virtual=%" PRId64
			" idle_elapsed=%" PRId64 " ns\n",
			cpu_get_clock(),
			icount2_get(),
			idle_elapsed);
	}
}

void icount2_wakeup(int cpu_index, bool halted, int mask, int interrupt_request) {
	bool idle = timers_state.icount2_idle;
	bool already_waking = timers_state.icount2_idle_wakeup;

	if (icount2_debug && (idle || halted)) {
		fprintf(stderr,
			"icount2 sleep: event=interrupt realtime=%" PRId64 " virtual=%" PRId64
			" cpu=%d halted=%d idle=%d already_waking=%d mask=0x%08X request=0x%08X\n",
			cpu_get_clock(),
			icount2_get(),
			cpu_index,
			halted,
			idle,
			already_waking,
			(unsigned) mask,
			(unsigned) interrupt_request);
	}

	if (!idle || already_waking)
		return;

	timers_state.icount2_idle_wakeup = true;
	icount2_idle_timer_wakeup = icount2_idle_running_timers;
}

void icount2_configure(QemuOpts *opts, Error **errp) {
	use_icount2 = true;
	icount2_debug = g_strcmp0(g_getenv("QEMU_ICOUNT2_DEBUG"), "1") == 0;
	qatomic_set(&timers_state.icount2_frequency, ICOUNT2_INITIAL_FREQUENCY);
	timers_state.icount2_adjust_initialized = false;
	timers_state.icount2_adjust_locked = false;
	timers_state.icount2_adjust_error = 0;
	
	timers_state.icount2_idle_timer = timer_new_ns(QEMU_CLOCK_REALTIME, icount2_idle_timer, NULL);
	timers_state.icount_rt_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL_RT, icount2_adjust_rt, NULL);
	timer_mod(
		timers_state.icount_rt_timer,
		qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + ICOUNT2_ADJUST_INTERVAL / SCALE_MS
	);
}
