/*
 * Progress clock for QEMU_CLOCK_VIRTUAL.
 *
 * Guest time is derived from the CPU time actually consumed by the vCPU
 * thread instead of host wall-clock time, so time the host spends not
 * running the guest (preemption, scheduler jitter, lock waits) never shows
 * up in guest-visible timers such as the GSM TPU counter.  This lets the
 * Siemens firmware's tight "the interrupt handler finished within N TPU
 * ticks" checks hold without the per-instruction cost of icount.
 *
 * The clock is defined as
 *
 *     virtual = anchor_virtual + (src - anchor_src) - excluded
 *
 * where src is the vCPU thread CPU clock while the guest runs and the host
 * monotonic clock while the guest is halted.  Two corrections keep it
 * consistent with the rest of QEMU:
 *
 *  - it is clamped to the earliest pending main-loop virtual timer, so the
 *    guest never observes time past a frame boundary whose timer callback
 *    (TPU events, DSP baseband, ...) has not run yet;
 *  - host work with no guest-time equivalent (TB translation, waiting on the
 *    DSP worker thread) is excluded from the vCPU CPU time.
 *
 * QEMU_CLOCK_VIRTUAL is removed from the main loop's poll deadline (see
 * qemu_clock_use_for_deadline); instead a fixed-cadence QEMU_CLOCK_REALTIME
 * timer runs the due virtual timers, exactly the role icount2's tick
 * accounting plays for the cycle clock.
 */
#include "qemu/osdep.h"
#include "qemu/seqlock.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "qemu/option.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "system/cpu-timers.h"
#include "system/runstate.h"

bool use_ptime;

/*
 * Fallback cadence (real time) for servicing due virtual timers when the vCPU
 * is not exiting through the MMIO path (e.g. halted).  During active execution
 * the vCPU delivers timers itself (ptime_run_due), so this can be coarse.
 * Overridable with QEMU_PTIME_POLL_US.
 */
#define PTIME_POLL_NS_DEFAULT (125 * 1000)
static int64_t ptime_poll_ns = PTIME_POLL_NS_DEFAULT;
#define PTIME_POLL_NS ptime_poll_ns

enum {
    PTIME_STOPPED,
    PTIME_RUNNING,
    PTIME_IDLE,
};

static struct {
    QemuSeqLock lock;
    QemuSpin spin;
    int mode;
    CPUState *cpu;
    clockid_t thread_clock;

    int64_t anchor_virtual;
    int64_t anchor_src;
    int64_t excluded;

    /* an exclusion window is open: the clock is frozen at excl_start */
    bool excl_active;
    int64_t excl_start;

    /* earliest pending main-loop virtual timer, or -1 */
    int64_t head;
    /* set from the vCPU thread when the clock has reached head */
    bool timers_due;

    QEMUTimer *poll_timer;

    /* statistics */
    int64_t absorbed_total;
    int64_t excluded_total;
    int64_t idle_total;
    uint64_t absorb_count;
    uint64_t excl_count;
    uint64_t idle_count;
} ps;

static __thread int excl_depth;
static bool ptime_debug;
static QEMUTimer *debug_timer;

static int64_t thread_cputime(void)
{
    struct timespec ts;

    clock_gettime(ps.thread_clock, &ts);
    return ts.tv_sec * NANOSECONDS_PER_SECOND + ts.tv_nsec;
}

static int64_t src_now_locked(void)
{
    switch (ps.mode) {
    case PTIME_RUNNING:
        return ps.excl_active ? ps.excl_start : thread_cputime();
    case PTIME_IDLE:
        return get_clock();
    default:
        return ps.anchor_src;
    }
}

static int64_t raw_from_src(int64_t src)
{
    return ps.anchor_virtual + (src - ps.anchor_src) - ps.excluded;
}

int64_t ptime_get(void)
{
    int64_t t;
    unsigned s;

    do {
        s = seqlock_read_begin(&ps.lock);
        t = raw_from_src(src_now_locked());
    } while (seqlock_read_retry(&ps.lock, s));
    return t;
}

static void reanchor_locked(int mode)
{
    int64_t virtual = raw_from_src(src_now_locked());

    qatomic_set(&ps.mode, mode);
    ps.anchor_virtual = virtual;
    ps.anchor_src = src_now_locked();
    ps.excluded = 0;
}

/*
 * (Re)arm the fallback wake timer.  While the vCPU is halted the clock equals
 * real time, so the next virtual-timer deadline maps to an exact real instant;
 * wake there so the frame interrupt is delivered on time.  Otherwise fall back
 * to a fixed cadence as a safety net (the running vCPU delivers on its own).
 */
static void ptime_arm_wake(void)
{
    int64_t now = get_clock();
    int64_t when = now + PTIME_POLL_NS;
    unsigned s;
    int mode;
    int64_t head, av, asrc;

    if (ps.poll_timer == NULL) {
        return;
    }
    do {
        s = seqlock_read_begin(&ps.lock);
        mode = ps.mode;
        av = ps.anchor_virtual;
        asrc = ps.anchor_src;
    } while (seqlock_read_retry(&ps.lock, s));
    head = qatomic_read(&ps.head);

    if (mode == PTIME_IDLE && head >= 0) {
        int64_t deadline = asrc + (head - av);
        when = CLAMP(deadline, now, now + PTIME_POLL_NS);
    }
    timer_mod_ns(ps.poll_timer, when);
}

void ptime_timer_head(int64_t expire)
{
    qatomic_set(&ps.head, expire);
    if (qatomic_read(&ps.mode) == PTIME_IDLE) {
        ptime_arm_wake();
    }
}

/*
 * Run the due main-loop virtual timers from the vCPU thread.  Called from the
 * vCPU loop with the BQL held after the clock crossed the earliest deadline,
 * so the TPU/DSP interrupts are delivered within one TB of the virtual instant
 * they are due instead of waiting for the fallback realtime poll.
 */
void ptime_run_due(void)
{
    if (!qatomic_read(&ps.timers_due)) {
        return;
    }
    qatomic_set(&ps.timers_due, false);
    qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
    qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
}

/*
 * Fallback: kick the vCPU so it services due virtual timers (ptime_run_due).
 * All virtual timers are run on the vCPU thread, so their TPU/DSP state is
 * never touched concurrently from here.  This mainly matters while the vCPU is
 * halted (its own MMIO path is not exiting); during active execution the vCPU
 * usually reaches the deadline on its own first.
 */
static void ptime_poll(void *opaque)
{
    /*
     * Run due virtual timers here as well as on the vCPU: while the vCPU is
     * blocked (e.g. waiting on the DSP worker thread) it cannot run them
     * itself, and the TPU frame events must keep flowing.  Runs on the main
     * loop under the BQL, serialized with the vCPU's own ptime_run_due.
     */
    qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
    qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    ptime_arm_wake();
}

void ptime_vcpu_thread_start(CPUState *cpu)
{
    clockid_t clock;

    if (!use_ptime) {
        return;
    }
    if (pthread_getcpuclockid(pthread_self(), &clock) != 0) {
        error_report("progress-clock: cannot read the vCPU thread CPU clock");
        exit(1);
    }

    seqlock_write_lock(&ps.lock, &ps.spin);
    if (ps.cpu == NULL) {
        ps.cpu = cpu;
        ps.thread_clock = clock;
        reanchor_locked(PTIME_RUNNING);
    }
    seqlock_write_unlock(&ps.lock, &ps.spin);
}

static bool ptime_is_vcpu_thread(void)
{
    return use_ptime && ps.cpu != NULL && current_cpu == ps.cpu;
}

void ptime_enter_idle(void)
{
    if (!ptime_is_vcpu_thread()) {
        return;
    }
    seqlock_write_lock(&ps.lock, &ps.spin);
    if (ps.mode == PTIME_RUNNING) {
        reanchor_locked(PTIME_IDLE);
        ps.idle_count++;
    }
    seqlock_write_unlock(&ps.lock, &ps.spin);
    ptime_arm_wake();
}

void ptime_exit_idle(void)
{
    if (!ptime_is_vcpu_thread()) {
        return;
    }
    seqlock_write_lock(&ps.lock, &ps.spin);
    if (ps.mode == PTIME_IDLE) {
        ps.idle_total += get_clock() - ps.anchor_src;
        reanchor_locked(PTIME_RUNNING);
    }
    seqlock_write_unlock(&ps.lock, &ps.spin);
}

void ptime_exclude_begin(void)
{
    if (!ptime_is_vcpu_thread() || excl_depth++ > 0) {
        return;
    }
    seqlock_write_lock(&ps.lock, &ps.spin);
    if (ps.mode == PTIME_RUNNING && !ps.excl_active) {
        ps.excl_start = thread_cputime();
        ps.excl_active = true;
    }
    seqlock_write_unlock(&ps.lock, &ps.spin);
}

void ptime_exclude_end(void)
{
    int64_t now_src;
    int64_t raw;
    int64_t head;

    if (!ptime_is_vcpu_thread() || excl_depth == 0 || --excl_depth > 0) {
        return;
    }
    seqlock_write_lock(&ps.lock, &ps.spin);
    now_src = thread_cputime();
    if (ps.excl_active) {
        int64_t delta = now_src - ps.excl_start;

        ps.excl_active = false;
        if (delta > 0) {
            ps.excluded += delta;
            ps.excluded_total += delta;
        }
        ps.excl_count++;
    }
    raw = raw_from_src(now_src);
    seqlock_write_unlock(&ps.lock, &ps.spin);

    /*
     * Reuse the CPU-time reading taken above: if the clock has reached the
     * earliest pending virtual timer, break out of the current TB so the vCPU
     * loop can run it (ptime_run_due) and deliver the interrupt promptly.
     */
    head = qatomic_read(&ps.head);
    if (head >= 0 && raw >= head && ps.mode == PTIME_RUNNING) {
        qatomic_set(&ps.timers_due, true);
        cpu_exit(ps.cpu);
    }
}

void ptime_exclude_abort(void)
{
    if (excl_depth > 0) {
        excl_depth = 1;
        ptime_exclude_end();
    }
}

static void ptime_debug_timer(void *opaque)
{
    static int64_t last_wall, last_virtual, last_cpu;
    int64_t wall = get_clock();
    int64_t virtual = ptime_get();
    int64_t cpu = ps.cpu ? thread_cputime() : 0;

    fprintf(stderr,
            "ptime: mode=%d virtual=%.3f wall=%.3f cpu=%.3f (dv=%.1f dw=%.1f dc=%.1f ms) "
            "absorbed=%.3f/%" PRIu64 " excluded=%.3f/%" PRIu64 " idle=%.3f/%" PRIu64 "\n",
            ps.mode,
            (double) virtual / NANOSECONDS_PER_SECOND,
            (double) wall / NANOSECONDS_PER_SECOND,
            (double) cpu / NANOSECONDS_PER_SECOND,
            (double) (virtual - last_virtual) / SCALE_MS,
            (double) (wall - last_wall) / SCALE_MS,
            (double) (cpu - last_cpu) / SCALE_MS,
            (double) ps.absorbed_total / NANOSECONDS_PER_SECOND, ps.absorb_count,
            (double) ps.excluded_total / NANOSECONDS_PER_SECOND, ps.excl_count,
            (double) ps.idle_total / NANOSECONDS_PER_SECOND, ps.idle_count);
    last_wall = wall;
    last_virtual = virtual;
    last_cpu = cpu;
    timer_mod(debug_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
}

void ptime_configure(QemuOpts *opts, Error **errp)
{
    use_ptime = true;
    seqlock_init(&ps.lock);
    qemu_spin_init(&ps.spin);
    ps.mode = PTIME_STOPPED;
    ps.head = -1;

    const char *poll_us = g_getenv("QEMU_PTIME_POLL_US");
    if (poll_us != NULL && *poll_us != '\0') {
        ptime_poll_ns = MAX(atoi(poll_us), 1) * 1000LL;
    }

    ps.poll_timer = timer_new_ns(QEMU_CLOCK_REALTIME, ptime_poll, NULL);
    timer_mod_ns(ps.poll_timer, get_clock() + PTIME_POLL_NS);

    ptime_debug = g_strcmp0(g_getenv("QEMU_PTIME_DEBUG"), "1") == 0;
    if (ptime_debug) {
        debug_timer = timer_new_ms(QEMU_CLOCK_REALTIME, ptime_debug_timer, NULL);
        timer_mod(debug_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
    }
}
