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
#include "system/cpu-timers.h"
#include "system/cpus.h"
#include "system/qtest.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qemu/seqlock.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "exec/icount.h"
#include "system/cpu-timers-internal.h"

static void rtcap_do_nothing(CPUState *cpu, run_on_cpu_data unused)
{
}

/*
 * ICOUNT: Instruction Counter
 *
 * this module is split off from cpu-timers because the icount part
 * is TCG-specific, and does not need to be built for other accels.
 */
static bool icount_sleep = true;
/* Arbitrarily pick 1MIPS as the minimum allowable speed.  */
#define MAX_ICOUNT_SHIFT 10

bool icount_align_option;

/*
 * Real-time cap (see exec/icount.h).  With sleep=off the virtual clock is
 * instruction-proportional and never waits for the host: a halted guest
 * warps to its next timer at once and a guest running faster than one insn
 * per 2^shift ns runs its clocks ahead of wall time (idle-screen
 * countdowns, animations).  The cap keeps QEMU_CLOCK_VIRTUAL within
 * RTCAP_SLACK of "allowed" virtual time: the vCPU thread sleeps
 * (kick-interruptible) before a warp or after a budget round that would
 * overrun.  Virtual time itself stays deterministic, only wall pacing
 * changes.
 *
 * For the guest's first RTCAP_WIN seconds of its own clock (the boot,
 * which must not be slowed further) lag is banked: allowed = wall time
 * since the VM started, so a guest that fell behind may catch up as fast
 * as it can.  After that the bank is capped at RTCAP_BUDGET: a stall is
 * repaid, but never by more than that much sprinted clock.  The window is
 * guest time because the boot costs the same virtual time on every host
 * while its wall time ranges from seconds to minutes.
 *
 * The switch needs no re-anchor: the forgive branch below only fires when
 * vtarget < allowed - SLACK - budget and then lowers allowed, so excess
 * can only shrink at the flip.
 */
bool icount_rtcap;
static bool icount_rtcap_budget;   /* past the window: the bank is capped */
static int64_t rtcap_v0, rtcap_r0;
static bool rtcap_vcpu_waiting;
#define RTCAP_SLACK_NS  (2 * SCALE_MS)
#define RTCAP_WIN_NS    (30 * NANOSECONDS_PER_SECOND)
#define RTCAP_BUDGET_NS (500 * SCALE_MS)

int64_t icount_rtcap_excess_ns(int64_t vtarget)
{
    int64_t r = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t allowed = rtcap_v0 + (r - rtcap_r0);

    /*
     * End of the banked window.  vtarget is a virtual-time value at both
     * call sites, so this costs one compare and no clock read; the switch
     * is one-way, so the two call sites disagreeing for an instant is
     * harmless.
     */
    if (!icount_rtcap_budget && vtarget >= RTCAP_WIN_NS) {
        qatomic_set(&icount_rtcap_budget, true);
    }

    /*
     * Past the window lag is forgiven down to RTCAP_BUDGET_NS.  Test the
     * target, not the current virtual time: while the vCPU sleeps for wall
     * time to reach a warp target, the current time falls "behind" by
     * design, and re-anchoring on it would restart the wait forever.
     *
     * Read plainly: this thread is the only writer, and wasm has no relaxed
     * atomic load.  The store above is atomic for the browser thread's sake
     * (icount_rtcap_mode).
     */
    if (icount_rtcap_budget &&
        vtarget < allowed - RTCAP_SLACK_NS - RTCAP_BUDGET_NS) {
        rtcap_v0 = vtarget + RTCAP_BUDGET_NS;
        rtcap_r0 = r;
        allowed = vtarget + RTCAP_BUDGET_NS;
    }
    return vtarget - (allowed + RTCAP_SLACK_NS);
}

void icount_rtcap_set_waiting(bool waiting)
{
    qatomic_set(&rtcap_vcpu_waiting, waiting);
}

/* Polled from the browser main thread while the vCPU thread writes it. */
int icount_rtcap_mode(void)
{
    return !icount_rtcap ? 0 : qatomic_read(&icount_rtcap_budget) ? 3 : 1;
}

/* Do not count executed instructions */
ICountMode use_icount = ICOUNT_DISABLED;

static void icount_enable_precise(void)
{
    /* Fixed conversion of insn to ns via "shift" option */
    use_icount = ICOUNT_PRECISE;
}

static void icount_enable_adaptive(void)
{
    /* Runtime adaptive algorithm to compute shift */
    use_icount = ICOUNT_ADAPTATIVE;
}

/*
 * The current number of executed instructions is based on what we
 * originally budgeted minus the current state of the decrementing
 * icount counters in extra/u16.low.
 */
static int64_t icount_get_executed(CPUState *cpu)
{
    return (cpu->icount_budget -
            (cpu->neg.icount_decr.u16.low + cpu->icount_extra));
}

/*
 * Update the global shared timer_state.qemu_icount to take into
 * account executed instructions. This is done by the TCG vCPU
 * thread so the main-loop can see time has moved forward.
 */
static void icount_update_locked(CPUState *cpu)
{
    int64_t executed = icount_get_executed(cpu);

    /*
     * Nothing has run since the last commit: the two stores below would
     * both write back what is already there, and the qemu_icount one is a
     * seq_cst i64.atomic.store on wasm, to a line the main loop reads.
     * A virtual-clock read that follows another one inside the same TB
     * lands here - and on a guest that touches two device registers in one
     * TB that is every second read.
     */
    if (!executed) {
        return;
    }

    cpu->icount_budget -= executed;

#ifdef __EMSCRIPTEN__
    /*
     * A plain store, deliberately.
     *
     * qatomic_set() is __ATOMIC_RELAXED, but wasm has no relaxed atomics:
     * LLVM lowers it to i64.atomic.store, which the wasm spec defines as
     * sequentially consistent, so an engine emits a locked exchange for it.
     * This is the only write to timers_state.qemu_icount in the program and
     * it comes from the vCPU thread; the field is QEMU_ALIGNED(64), so the
     * plain store is a single naturally-aligned i64.store that no reader can
     * see torn.  What a reader can see is a *stale* value - but it could
     * already: this writer does not hold the seqlock's write lock (stock
     * QEMU commits the running slice from inside a read section), so the
     * only guarantee readers ever had is "some value this thread published".
     */
    timers_state.qemu_icount += executed;
#else
    qatomic_set(&timers_state.qemu_icount,
                timers_state.qemu_icount + executed);
#endif
}

/*
 * Update the global shared timer_state.qemu_icount to take into
 * account executed instructions. This is done by the TCG vCPU
 * thread so the main-loop can see time has moved forward.
 */
void icount_update(CPUState *cpu)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    icount_update_locked(cpu);
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

/*
 * The vm_clock seqlock read section, for this file's readers only.
 *
 * seqlock_read_begin()/retry() bracket the section with smp_rmb(), i.e.
 * __atomic_thread_fence(ACQUIRE).  LLVM lowers that to wasm's atomic.fence,
 * which the wasm spec defines as sequentially consistent, so an engine
 * emits a real locked operation for it - twice per icount_get(), and an
 * idle S75 reads the virtual clock about 1.2M times a second.
 *
 * Every *shared* location these sections touch is reached through
 * qatomic_read()/qatomic_set() (qemu_icount, qemu_icount_bias,
 * icount_time_shift, the sequence itself), and on wasm a relaxed qatomic_*
 * on shared memory lowers to iN.atomic.load/store - which wasm also defines
 * as sequentially consistent.  The engine therefore already orders them
 * against the writer's stores, and the fence adds nothing.  The rest of the
 * section is the calling thread's own CPUState.
 *
 * What is still needed is to stop the *compiler* moving those accesses
 * across the sequence reads: LLVM sees __ATOMIC_RELAXED and may reorder
 * them, whatever the backend later emits.  barrier() does that for free.
 *
 * This argument does not generalise: a seqlock whose payload is plain
 * (non-qatomic) loads still needs the real smp_rmb(), because nothing then
 * orders those loads at all.  Hence a local pair rather than a change to
 * seqlock.h.
 */
#ifdef __EMSCRIPTEN__
#define ICOUNT_SEQ_RMB() barrier()
#else
#define ICOUNT_SEQ_RMB() smp_rmb()
#endif

static inline unsigned icount_seq_read_begin(void)
{
    unsigned ret = qatomic_read(&timers_state.vm_clock_seqlock.sequence);

    ICOUNT_SEQ_RMB();
    return ret & ~1;
}

static inline int icount_seq_read_retry(unsigned start)
{
    ICOUNT_SEQ_RMB();
    return unlikely(qatomic_read(&timers_state.vm_clock_seqlock.sequence)
                    != start);
}

static int64_t icount_get_raw_locked(void)
{
    CPUState *cpu = current_cpu;

    if (cpu && cpu->running) {
        if (!cpu->neg.can_do_io) {
            error_report("Bad icount read");
            exit(1);
        }
        /*
         * Take into account what has run.
         *
         * Publishing here is not needed for the value this returns -
         * qemu_icount + icount_get_executed(cpu) is the same number -
         * and the store is not free: it is ~1.5M writes a second on a
         * polling guest, to a line other threads read.  Not publishing
         * was measured on the EL71 busy state (2026-09-14) and is a
         * 7.8 % MIPS *regression*, twice: a global icount that only
         * moves at slice boundaries changes how the main loop paces
         * itself, and that costs more than the store.  Leave it.
         */
        icount_update_locked(cpu);
    }
    /* The read is protected by the seqlock, but needs atomic to avoid UB */
    return qatomic_read(&timers_state.qemu_icount);
}

static int64_t icount_get_locked(void)
{
    int64_t icount = icount_get_raw_locked();
    return qatomic_read(&timers_state.qemu_icount_bias) + icount_to_ns(icount);
}

int64_t icount_get_raw(void)
{
    int64_t icount;
    unsigned start;

    do {
        start = icount_seq_read_begin();
        icount = icount_get_raw_locked();
    } while (icount_seq_read_retry(start));

    return icount;
}

/* Return the virtual CPU time, based on the instruction counter.  */
int64_t icount_get(void)
{
    int64_t icount;
    unsigned start;

    do {
        start = icount_seq_read_begin();
        icount = icount_get_locked();
    } while (icount_seq_read_retry(start));

    return icount;
}

int64_t icount_to_ns(int64_t icount)
{
    return icount << qatomic_read(&timers_state.icount_time_shift);
}

/*
 * Correlation between real and virtual time is always going to be
 * fairly approximate, so ignore small variation.
 * When the guest is idle real and virtual time will be aligned in
 * the IO wait loop.
 */
#define ICOUNT_WOBBLE (NANOSECONDS_PER_SECOND / 10)

static void icount_adjust(void)
{
    int64_t cur_time;
    int64_t cur_icount;
    int64_t delta;

    /* If the VM is not running, then do nothing.  */
    if (!runstate_is_running()) {
        return;
    }

    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    cur_time = REPLAY_CLOCK_LOCKED(REPLAY_CLOCK_VIRTUAL_RT,
                                   cpu_get_clock_locked());
    cur_icount = icount_get_locked();

    delta = cur_icount - cur_time;
    /* FIXME: This is a very crude algorithm, somewhat prone to oscillation.  */
    if (delta > 0
        && timers_state.last_delta + ICOUNT_WOBBLE < delta * 2
        && timers_state.icount_time_shift > 0) {
        /* The guest is getting too far ahead.  Slow time down.  */
        qatomic_set(&timers_state.icount_time_shift,
                    timers_state.icount_time_shift - 1);
    }
    if (delta < 0
        && timers_state.last_delta - ICOUNT_WOBBLE > delta * 2
        && timers_state.icount_time_shift < MAX_ICOUNT_SHIFT) {
        /* The guest is getting too far behind.  Speed time up.  */
        qatomic_set(&timers_state.icount_time_shift,
                    timers_state.icount_time_shift + 1);
    }
    timers_state.last_delta = delta;
    qatomic_set(&timers_state.qemu_icount_bias,
                cur_icount - (timers_state.qemu_icount
                              << timers_state.icount_time_shift));
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static void icount_adjust_rt(void *opaque)
{
    timer_mod(timers_state.icount_rt_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    icount_adjust();
}

static void icount_adjust_vm(void *opaque)
{
    timer_mod(timers_state.icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
    icount_adjust();
}

int64_t icount_round(int64_t count)
{
    int shift = qatomic_read(&timers_state.icount_time_shift);
    return (count + (1 << shift) - 1) >> shift;
}

static void icount_warp_rt(void)
{
    unsigned seq;
    int64_t warp_start;

    /*
     * The icount_warp_timer is rescheduled soon after vm_clock_warp_start
     * changes from -1 to another value, so the race here is okay.
     */
    do {
        seq = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        warp_start = timers_state.vm_clock_warp_start;
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, seq));

    if (warp_start == -1) {
        return;
    }

    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (runstate_is_running()) {
        int64_t clock = REPLAY_CLOCK_LOCKED(REPLAY_CLOCK_VIRTUAL_RT,
                                            cpu_get_clock_locked());
        int64_t warp_delta;

        warp_delta = clock - timers_state.vm_clock_warp_start;
        if (icount_enabled() == ICOUNT_ADAPTATIVE) {
            /*
             * In adaptive mode, do not let QEMU_CLOCK_VIRTUAL run too far
             * ahead of real time (it might already be ahead so careful not
             * to go backwards).
             */
            int64_t cur_icount = icount_get_locked();
            int64_t delta = clock - cur_icount;

            if (delta < 0) {
                delta = 0;
            }
            warp_delta = MIN(warp_delta, delta);
        }
        qatomic_set(&timers_state.qemu_icount_bias,
                    timers_state.qemu_icount_bias + warp_delta);
    }
    timers_state.vm_clock_warp_start = -1;
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);

    if (qemu_clock_expired(QEMU_CLOCK_VIRTUAL)) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}

static void icount_timer_cb(void *opaque)
{
    /*
     * No need for a checkpoint because the timer already synchronizes
     * with CHECKPOINT_CLOCK_VIRTUAL_RT.
     */
    icount_warp_rt();
}

void icount_start_warp_timer_full(bool notify)
{
    int64_t clock;
    int64_t deadline;

    assert(icount_enabled());

    /*
     * Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        return;
    }

    if (replay_mode != REPLAY_MODE_PLAY) {
        if (!all_cpu_threads_idle()) {
            return;
        }

        if (qtest_enabled()) {
            /* When testing, qtest commands advance icount.  */
            return;
        }

        replay_checkpoint(CHECKPOINT_CLOCK_WARP_START);
    } else {
        /* warp clock deterministically in record/replay mode */
        if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_START)) {
            /*
             * vCPU is sleeping and warp can't be started.
             * It is probably a race condition: notification sent
             * to vCPU was processed in advance and vCPU went to sleep.
             * Therefore we have to wake it up for doing something.
             */
            if (replay_has_event()) {
                qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
            }
            return;
        }
    }

    /* We want to use the earliest deadline from ALL vm_clocks */
    deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                          ~QEMU_TIMER_ATTR_EXTERNAL);
    if (deadline < 0) {
        if (!icount_sleep) {
            warn_report_once("icount sleep disabled and no active timers");
        }
        return;
    }

    if (deadline > 0) {
        /*
         * Ensure QEMU_CLOCK_VIRTUAL proceeds even when the virtual CPU goes to
         * sleep.  Otherwise, the CPU might be waiting for a future timer
         * interrupt to wake it up, but the interrupt never comes because
         * the vCPU isn't running any insns and thus doesn't advance the
         * QEMU_CLOCK_VIRTUAL.
         */
        if (!icount_sleep) {
            /*
             * We never let VCPUs sleep in no sleep icount mode.
             * If there is a pending QEMU_CLOCK_VIRTUAL timer we just advance
             * to the next QEMU_CLOCK_VIRTUAL event and notify it.
             * It is useful when we want a deterministic execution time,
             * isolated from host latencies.
             */
            if (icount_rtcap && !qemu_in_vcpu_thread()) {
                /*
                 * Under the real-time cap the vCPU thread paces its own
                 * warps (rr_idle_advance).  It handles this deadline when
                 * its timed wait ends; if it is parked in the untimed halt
                 * wait instead, hand the deadline back to it.
                 */
                if (!qatomic_read(&rtcap_vcpu_waiting)) {
                    /*
                     * qemu_cpu_kick cannot wake a halted rr thread out
                     * of its untimed halt-cond wait (the kick only sets
                     * exit_request, which cpu_thread_is_idle ignores).
                     * Mirror qemu_timer_notify_cb() and use
                     * async_run_on_cpu(), which does.
                     */
                    async_run_on_cpu(first_cpu, rtcap_do_nothing,
                                     RUN_ON_CPU_NULL);
                }
                return;
            }
            seqlock_write_lock(&timers_state.vm_clock_seqlock,
                               &timers_state.vm_clock_lock);
            qatomic_set(&timers_state.qemu_icount_bias,
                        timers_state.qemu_icount_bias + deadline);
            seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                                 &timers_state.vm_clock_lock);
            /*
             * @notify false: the caller runs the deadline itself on this
             * thread straight after, and that notifies.  See
             * rr_idle_advance().
             */
            if (notify) {
                qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
            }
        } else {
            /*
             * We do stop VCPUs and only advance QEMU_CLOCK_VIRTUAL after some
             * "real" time, (related to the time left until the next event) has
             * passed. The QEMU_CLOCK_VIRTUAL_RT clock will do this.
             * This avoids that the warps are visible externally; for example,
             * you will not be sending network packets continuously instead of
             * every 100ms.
             */
            clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
            seqlock_write_lock(&timers_state.vm_clock_seqlock,
                               &timers_state.vm_clock_lock);
            if (timers_state.vm_clock_warp_start == -1
                || timers_state.vm_clock_warp_start > clock) {
                timers_state.vm_clock_warp_start = clock;
            }
            seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                                 &timers_state.vm_clock_lock);
            timer_mod_anticipate(timers_state.icount_warp_timer,
                                 clock + deadline);
        }
    } else if (deadline == 0) {
        if (notify) {
            qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
        }
    }
}

void icount_start_warp_timer(void)
{
    icount_start_warp_timer_full(true);
}

void icount_account_warp_timer(void)
{
    if (!icount_sleep) {
        return;
    }

    /*
     * Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        return;
    }

    replay_async_events();

    /* warp clock deterministically in record/replay mode */
    if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_ACCOUNT)) {
        return;
    }

    timer_del(timers_state.icount_warp_timer);
    icount_warp_rt();
}

bool icount_configure(QemuOpts *opts, Error **errp)
{
    const char *option = qemu_opt_get(opts, "shift");
    bool sleep = qemu_opt_get_bool(opts, "sleep", true);
    bool align = qemu_opt_get_bool(opts, "align", false);
    long time_shift = -1;

    if (qemu_opt_get_bool(opts, "precise-clocks", false)) {
        if (option || qemu_opt_get(opts, "align") || qemu_opt_get(opts, "sleep")) {
            error_setg(errp, "precise-clocks=on and other options are incompatible");
            return false;
        }
        icount2_configure(opts, errp);
        return true;
    }

    if (!option) {
        if (qemu_opt_get(opts, "align") != NULL) {
            error_setg(errp, "Please specify shift option when using align");
            return false;
        }
        return true;
    }

    if (align && !sleep) {
        error_setg(errp, "align=on and sleep=off are incompatible");
        return false;
    }

    if (sleep && qemu_opt_get_bool(opts, "rtcap", false)) {
        error_setg(errp, "rtcap=on requires sleep=off");
        return false;
    }

    if (strcmp(option, "auto") != 0) {
        if (qemu_strtol(option, NULL, 0, &time_shift) < 0
            || time_shift < 0 || time_shift > MAX_ICOUNT_SHIFT) {
            error_setg(errp, "icount: Invalid shift value");
            return false;
        }
    } else if (icount_align_option) {
        error_setg(errp, "shift=auto and align=on are incompatible");
        return false;
    } else if (!icount_sleep) {
        error_setg(errp, "shift=auto and sleep=off are incompatible");
        return false;
    }

    icount_sleep = sleep;
    if (icount_sleep) {
        timers_state.icount_warp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL_RT,
                                         icount_timer_cb, NULL);
    } else {
#ifdef __EMSCRIPTEN__
        icount_rtcap = qemu_opt_get_bool(opts, "rtcap", true);
#else
        icount_rtcap = qemu_opt_get_bool(opts, "rtcap", false);
#endif
        rtcap_v0 = 0;
        rtcap_r0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }

    icount_align_option = align;

    if (time_shift >= 0) {
        timers_state.icount_time_shift = time_shift;
        icount_enable_precise();
        return true;
    }

    icount_enable_adaptive();

    /*
     * 125MIPS seems a reasonable initial guess at the guest speed.
     * It will be corrected fairly quickly anyway.
     */
    timers_state.icount_time_shift = 3;

    /*
     * Have both realtime and virtual time triggers for speed adjustment.
     * The realtime trigger catches emulated time passing too slowly,
     * the virtual time trigger catches emulated time passing too fast.
     * Realtime triggers occur even when idle, so use them less frequently
     * than VM triggers.
     */
    timers_state.vm_clock_warp_start = -1;
    timers_state.icount_rt_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL_RT,
                                   icount_adjust_rt, NULL);
    timer_mod(timers_state.icount_rt_timer,
                   qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    timers_state.icount_vm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        icount_adjust_vm, NULL);
    timer_mod(timers_state.icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
    return true;
}

void icount_notify_exit(void)
{
    assert(icount_enabled());

    if (current_cpu) {
        qemu_cpu_kick(current_cpu);
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}
