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

#ifndef TIMERS_STATE_H
#define TIMERS_STATE_H

/* timers state, for sharing between icount and cpu-timers */

typedef struct TimersState {
    /* Protected by BQL.  */
    int64_t cpu_ticks_prev;
    int64_t cpu_ticks_offset;

    /*
     * Protect fields that can be respectively read outside the
     * BQL, and written from multiple threads.
     */
    QemuSeqLock vm_clock_seqlock;
    QemuSpin vm_clock_lock;

    int16_t cpu_ticks_enabled;

    /* Conversion factor from emulated instructions to virtual clock ticks.  */
    int16_t icount_time_shift;
    /* Icount delta used for shift auto adjust. */
    int64_t last_delta;

    /* Compensate for varying guest execution speed.  */
    int64_t qemu_icount_bias;

    int64_t vm_clock_warp_start;
    int64_t cpu_clock_offset;

    /*
     * Only written by the TCG thread - and written *often*: a guest
     * that polls a device register commits the running slice on every
     * virtual-clock read (icount_get_raw_locked), which is 1.5M writes
     * a second on the EL71 and 4.5M on the S75.  Give it a cache line
     * of its own.  Every field above is read by other threads (the
     * seqlock and the spin lock most of all), and a store to a line
     * another core holds measures 52 ns here against 2.2 ns to an
     * uncontended one - see doc/performance-handoff.md.
     */
    int64_t qemu_icount QEMU_ALIGNED(64);
    char qemu_icount_pad[64 - sizeof(int64_t)];

    /* Precise cycle counter */
    int64_t icount2_ticks;
    int64_t icount2_offset;
    int64_t icount2_bias;
    int64_t icount2_deadline;
    uint32_t icount2_frequency;
    int64_t icount2_adjust_realtime;
    int64_t icount2_adjust_ticks;
    int64_t icount2_adjust_error;
    bool icount2_adjust_initialized;
    bool icount2_adjust_locked;
    int64_t icount2_idle_realtime;
    int64_t icount2_idle_deadline;
    bool icount2_idle;
    bool icount2_idle_wakeup;
    QEMUTimer *icount2_idle_timer;

    /* for adjusting icount */
    QEMUTimer *icount_rt_timer;
    QEMUTimer *icount_vm_timer;
    QEMUTimer *icount_warp_timer;
} TimersState;

extern TimersState timers_state;

/*
 * icount needs this internal from cpu-timers when adjusting the icount shift.
 */
int64_t cpu_get_clock_locked(void);

#endif /* TIMERS_STATE_H */
