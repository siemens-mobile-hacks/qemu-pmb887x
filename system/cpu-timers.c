/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/cpus.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qemu/seqlock.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "system/cpu-timers.h"
#include "system/cpu-timers-internal.h"
#include "exec/icount.h"

/* clock and ticks */

static int64_t cpu_get_ticks_locked(void)
{
    int64_t ticks = timers_state.cpu_ticks_offset;
    if (timers_state.cpu_ticks_enabled) {
        ticks += cpu_get_host_ticks();
    }

    if (timers_state.cpu_ticks_prev > ticks) {
        /* Non increasing ticks may happen if the host uses software suspend. */
        timers_state.cpu_ticks_offset += timers_state.cpu_ticks_prev - ticks;
        ticks = timers_state.cpu_ticks_prev;
    }

    timers_state.cpu_ticks_prev = ticks;
    return ticks;
}

/*
 * return the time elapsed in VM between vm_start and vm_stop.
 * cpu_get_ticks() uses units of the host CPU cycle counter.
 */
int64_t cpu_get_ticks(void)
{
    int64_t ticks;

    qemu_spin_lock(&timers_state.vm_clock_lock);
    ticks = cpu_get_ticks_locked();
    qemu_spin_unlock(&timers_state.vm_clock_lock);
    return ticks;
}

int64_t cpu_get_clock_locked(void)
{
    int64_t time;

    time = timers_state.cpu_clock_offset;
    if (timers_state.cpu_ticks_enabled) {
        time += get_clock();
    }

    return time;
}

/*
 * Return the monotonic time elapsed in VM, i.e.,
 * the time between vm_start and vm_stop
 */
static int64_t cpu_get_raw_clock(void)
{
    int64_t ti;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        ti = cpu_get_clock_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return ti;
}

#define CPU_CLOCK_PS_PER_S 1000000000000ULL
#define CPU_CLOCK_BUDGET_MIN 64
#define CPU_CLOCK_BUDGET_MAX (1u << 30)

/*
 * The owner and threads waking it write under the mutex; a reader that meets
 * a write in progress waits for it on the mutex rather than spin. The anchor
 * is INT64_MAX while the guest waits for an interrupt.
 */
static __thread bool cpu_clock_owner;
static QemuSeqLock cpu_clock_owner_seq;
static QemuMutex cpu_clock_owner_lock;
static int64_t cpu_clock_owner_anchor = INT64_MAX;
static uint32_t cpu_clock_owner_anchor_cycles;
static uint32_t *cpu_clock_owner_cycles;
static uint32_t *cpu_clock_owner_cycle_limit;
static uint32_t cpu_clock_cycle_ps = CPU_CLOCK_PS_PER_S / (100 * 1000 * 1000);

/*
 * A thread may read the running clock just before qemu_virtual_clock_cap
 * holds it: never return less than a time already returned, timer code
 * relies on the clock not going back.
 */
static int64_t cpu_clock_monotonic(int64_t time)
{
    int64_t last = qatomic_read(&qemu_virtual_clock_last);

    while (time > last) {
        int64_t seen = qatomic_cmpxchg(&qemu_virtual_clock_last, last, time);

        if (seen == last) {
            return time;
        }
        last = seen;
    }
    return last;
}

static int64_t cpu_clock_cycles_ns(uint32_t cycles, uint32_t cycle_ps)
{
    return (uint64_t)cycles * cycle_ps / 1000;
}

static int64_t cpu_clock_position_locked(uint32_t cycles)
{
    if (cpu_clock_owner_anchor == INT64_MAX) {
        return INT64_MAX;
    }
    return cpu_clock_owner_anchor +
           cpu_clock_cycles_ns(cycles - cpu_clock_owner_anchor_cycles,
                               cpu_clock_cycle_ps);
}

int64_t cpu_clock_owner_position(void)
{
    uint32_t *cycles = qatomic_read(&cpu_clock_owner_cycles);
    int64_t position;
    unsigned start;

    if (!cycles) {
        return INT64_MAX;
    }
    start = seqlock_read_begin(&cpu_clock_owner_seq);
    position = cpu_clock_position_locked(qatomic_read(cycles));
    if (seqlock_read_retry(&cpu_clock_owner_seq, start)) {
        qemu_mutex_lock(&cpu_clock_owner_lock);
        position = cpu_clock_position_locked(qatomic_read(cycles));
        qemu_mutex_unlock(&cpu_clock_owner_lock);
    }
    return position;
}

int64_t cpu_get_real_clock(void)
{
    int64_t ti = MIN(cpu_get_raw_clock(), cpu_clock_owner_position());

    return MIN(ti, qatomic_read(&qemu_virtual_clock_cap));
}

int64_t cpu_get_clock(void)
{
    int64_t position;

    if (!qemu_virtual_timers_on_vcpu) {
        return cpu_get_raw_clock();
    }
    if (cpu_clock_owner) {
        position = cpu_clock_owner_position();
        if (position != INT64_MAX) {
            return cpu_clock_monotonic(position);
        }
    }
    return cpu_clock_monotonic(cpu_get_real_clock());
}

static void cpu_clock_owner_write(int64_t anchor, uint32_t cycles,
                                  uint32_t cycle_ps)
{
    seqlock_write_begin(&cpu_clock_owner_seq);
    cpu_clock_owner_anchor = anchor;
    cpu_clock_owner_anchor_cycles = cycles;
    cpu_clock_cycle_ps = cycle_ps;
    seqlock_write_end(&cpu_clock_owner_seq);
}

void cpu_clock_set_owner(uint32_t *cycles, uint32_t *cycle_limit)
{
    cpu_clock_owner = true;
    qatomic_set(&cpu_clock_owner_cycle_limit, cycle_limit);
    qatomic_store_release(&cpu_clock_owner_cycles, cycles);
}

void cpu_clock_set_cpu_hz(uint64_t hz)
{
    uint32_t *counter = qatomic_load_acquire(&cpu_clock_owner_cycles);
    uint32_t cycle_ps;
    uint32_t cycles;

    if (!hz) {
        return;
    }
    cycle_ps = MAX(CPU_CLOCK_PS_PER_S / hz, 1);
    if (!counter) {
        cpu_clock_cycle_ps = cycle_ps;
        return;
    }
    qemu_mutex_lock(&cpu_clock_owner_lock);
    cycles = qatomic_read(counter);
    cpu_clock_owner_write(cpu_clock_position_locked(cycles), cycles, cycle_ps);
    qemu_mutex_unlock(&cpu_clock_owner_lock);
}

void cpu_clock_owner_progress(bool idle)
{
    int64_t now = cpu_get_clock();
    uint32_t cycles;

    qemu_mutex_lock(&cpu_clock_owner_lock);
    cycles = qatomic_read(cpu_clock_owner_cycles);
    if (idle) {
        cpu_clock_owner_write(INT64_MAX, cycles, cpu_clock_cycle_ps);
    } else if (cpu_clock_owner_anchor == INT64_MAX) {
        cpu_clock_owner_write(now, cycles, cpu_clock_cycle_ps);
    } else {
        cpu_clock_owner_write(cpu_clock_position_locked(cycles), cycles,
                              cpu_clock_cycle_ps);
    }
    qemu_mutex_unlock(&cpu_clock_owner_lock);
}

void cpu_clock_owner_wake(void)
{
    uint32_t *counter = qatomic_load_acquire(&cpu_clock_owner_cycles);
    int64_t now;

    if (!counter) {
        return;
    }
    now = cpu_get_clock();
    qemu_mutex_lock(&cpu_clock_owner_lock);
    if (cpu_clock_owner_anchor == INT64_MAX) {
        cpu_clock_owner_write(now, qatomic_read(counter), cpu_clock_cycle_ps);
    }
    qemu_mutex_unlock(&cpu_clock_owner_lock);
}

/* Caller must hold BQL which serves as mutex for vm_clock_seqlock. */
void cpu_clock_owner_limit_lag(void)
{
    int64_t position = cpu_clock_owner_position();

    if (position == INT64_MAX ||
        cpu_get_raw_clock() - position <= CPU_CLOCK_OWNER_LAG_MAX_NS) {
        return;
    }
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (timers_state.cpu_ticks_enabled) {
        int64_t lag = cpu_get_clock_locked() - position;

        if (lag > CPU_CLOCK_OWNER_LAG_MAX_NS) {
            timers_state.cpu_clock_offset -= lag - CPU_CLOCK_OWNER_LAG_MAX_NS;
        }
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static uint32_t cpu_clock_owner_budget(int64_t deadline)
{
    uint32_t cycle_ps = qatomic_read(&cpu_clock_cycle_ps);
    int64_t position = cpu_clock_owner_position();
    int64_t until = cpu_get_real_clock() + CPU_CLOCK_OWNER_LEAD_NS;
    uint64_t cycles;

    if (deadline >= 0) {
        until = MIN(until, deadline);
    }
    cycles = muldiv64(MAX(until - position, 0), 1000, cycle_ps);
    return MAX(MIN(cycles, CPU_CLOCK_BUDGET_MAX), CPU_CLOCK_BUDGET_MIN);
}

void cpu_clock_owner_shorten_budget(void)
{
    uint32_t *cycles = qatomic_load_acquire(&cpu_clock_owner_cycles);
    uint32_t *cycle_limit = qatomic_read(&cpu_clock_owner_cycle_limit);
    int64_t deadline;
    uint32_t limit, old;

    if (!cycles) {
        return;
    }
    deadline = qemu_clock_expire_time_ns(QEMU_CLOCK_VIRTUAL);
    limit = qatomic_read(cycles) + cpu_clock_owner_budget(deadline);
    old = qatomic_read(cycle_limit);
    while ((int32_t)(limit - old) < 0) {
        uint32_t seen = qatomic_cmpxchg(cycle_limit, old, limit);

        if (seen == old) {
            break;
        }
        old = seen;
    }
}

void cpu_clock_owner_start_budget(void)
{
    qatomic_set(cpu_clock_owner_cycle_limit,
                qatomic_read(cpu_clock_owner_cycles) + CPU_CLOCK_BUDGET_MAX);
    /* Then a limit lowered meanwhile cannot be missed. */
    smp_mb();
    cpu_clock_owner_shorten_budget();
}

/*
 * enable cpu_get_ticks()
 * Caller must hold BQL which serves as mutex for vm_clock_seqlock.
 */
void cpu_enable_ticks(void)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (!timers_state.cpu_ticks_enabled) {
        timers_state.cpu_ticks_offset -= cpu_get_host_ticks();
        timers_state.cpu_clock_offset -= get_clock();
        timers_state.cpu_ticks_enabled = 1;
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
}

/*
 * disable cpu_get_ticks() : the clock is stopped. You must not call
 * cpu_get_ticks() after that.
 * Caller must hold BQL which serves as mutex for vm_clock_seqlock.
 */
void cpu_disable_ticks(void)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (timers_state.cpu_ticks_enabled) {
        timers_state.cpu_ticks_offset += cpu_get_host_ticks();
        timers_state.cpu_clock_offset = cpu_get_clock_locked();
        timers_state.cpu_ticks_enabled = 0;
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static bool icount_state_needed(void *opaque)
{
    return icount_enabled();
}

static bool warp_timer_state_needed(void *opaque)
{
    TimersState *s = opaque;
    return s->icount_warp_timer != NULL;
}

static bool adjust_timers_state_needed(void *opaque)
{
    TimersState *s = opaque;
    return s->icount_rt_timer != NULL;
}

static bool icount_shift_state_needed(void *opaque)
{
    return icount_enabled() == ICOUNT_ADAPTATIVE;
}

/*
 * Subsection for warp timer migration is optional, because may not be created
 */
static const VMStateDescription icount_vmstate_warp_timer = {
    .name = "timer/icount/warp_timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = warp_timer_state_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(vm_clock_warp_start, TimersState),
        VMSTATE_TIMER_PTR(icount_warp_timer, TimersState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription icount_vmstate_adjust_timers = {
    .name = "timer/icount/timers",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = adjust_timers_state_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(icount_rt_timer, TimersState),
        VMSTATE_TIMER_PTR(icount_vm_timer, TimersState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription icount_vmstate_shift = {
    .name = "timer/icount/shift",
    .version_id = 2,
    .minimum_version_id = 2,
    .needed = icount_shift_state_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_INT16(icount_time_shift, TimersState),
        VMSTATE_INT64(last_delta, TimersState),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * This is a subsection for icount migration.
 */
static const VMStateDescription icount_vmstate_timers = {
    .name = "timer/icount",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = icount_state_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(qemu_icount_bias, TimersState),
        VMSTATE_INT64(qemu_icount, TimersState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &icount_vmstate_warp_timer,
        &icount_vmstate_adjust_timers,
        &icount_vmstate_shift,
        NULL
    }
};

static const VMStateDescription vmstate_timers = {
    .name = "timer",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(cpu_ticks_offset, TimersState),
        VMSTATE_UNUSED(8),
        VMSTATE_INT64_V(cpu_clock_offset, TimersState, 2),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &icount_vmstate_timers,
        NULL
    }
};

static void do_nothing(CPUState *cpu, run_on_cpu_data unused)
{
}

static void timer_notify_main_loop(void)
{
#ifdef __EMSCRIPTEN__
    /*
     * A timer armed on the main-loop thread - nearly always a device
     * timer callback re-arming itself - is seen by the deadline the
     * next main_loop_wait() computes before it sleeps, so the notify
     * wakes nobody.  It is not free: qemu_notify_bh makes that next
     * iteration return without sleeping, one more BQL round trip
     * that also ends the vCPU's deferred hold.  Through a KE970 boot
     * (icount=none) this was ~90 % of the rearm notifies, 10-14 k/s.
     */
    if (qemu_in_main_loop_thread()) {
        return;
    }
#endif
    qemu_notify_event();
}

void qemu_timer_notify_cb(void *opaque, QEMUClockType type)
{
    if (type != QEMU_CLOCK_VIRTUAL) {
        timer_notify_main_loop();
        return;
    }

    if (qemu_virtual_timers_on_vcpu) {
        if (cpu_clock_owner && qatomic_read(&current_cpu->running)) {
            cpu_clock_owner_shorten_budget();
        } else if (!qemu_in_vcpu_thread() && first_cpu) {
            cpu_clock_owner_shorten_budget();
            cpu_clock_owner_wake();
            qemu_cond_broadcast(first_cpu->halt_cond);
        }
        return;
    }

    if (!icount_enabled()) {
        timer_notify_main_loop();
        return;
    }

    if (qemu_in_vcpu_thread()) {
        /*
         * A CPU is currently running; send it out of the
         * tcg_cpu_exec() loop so it will recalculate its
         * icount deadline immediately - but only when the new deadline
         * falls inside the budget it is running on.  A timer re-armed
         * (or added) beyond the remaining budget cannot be missed: the
         * budget ends first and the loop recomputes.  Measured on the
         * pmb887x boot: 37k notifies/s on the vCPU thread (every
         * timer_mod from a device callback or from the idle warp's own
         * timer run), 1.2 % of them inside the budget - each needless
         * exit_request cost an empty cpu_exec round plus a
         * TB_EXIT_REQUESTED unwind of the first TB.
         */
        if (icount_enabled()) {
            CPUState *cpu = current_cpu;
            int64_t deadline, left;

            if (!cpu->neg.can_do_io) {
                /* Reading the virtual clock here would hit
                 * icount_get_raw_locked()'s "Bad icount read" exit when
                 * the caller cannot do io - keep the stock unconditional
                 * cpu_exit instead. */
                cpu_exit(current_cpu);
                return;
            }
            deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                                  QEMU_TIMER_ATTR_ALL);
            left = cpu->neg.icount_decr.u16.low + cpu->icount_extra;

            if (deadline >= 0 && icount_round(deadline) >= left) {
                return;
            }
        }
        cpu_exit(current_cpu);
    } else if (first_cpu) {
        /*
         * cpu_exit() is not enough to kick a halted CPU out of
         * qemu_tcg_wait_io_event.  async_run_on_cpu, instead,
         * causes cpu_thread_is_idle to return false.  This way,
         * handle_icount_deadline can run.
         * If we have no CPUs at all for some reason, we don't
         * need to do anything.
         */
        async_run_on_cpu(first_cpu, do_nothing, RUN_ON_CPU_NULL);
    }
}

TimersState timers_state;

/* initialize timers state and the cpu throttle for convenience */
void cpu_timers_init(void)
{
    seqlock_init(&timers_state.vm_clock_seqlock);
    qemu_spin_init(&timers_state.vm_clock_lock);
    seqlock_init(&cpu_clock_owner_seq);
    qemu_mutex_init(&cpu_clock_owner_lock);
    vmstate_register(NULL, 0, &vmstate_timers, &timers_state);
}
