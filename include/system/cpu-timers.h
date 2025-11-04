/*
 * CPU timers state API
 *
 * Copyright 2020 SUSE LLC
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#ifndef SYSTEM_CPU_TIMERS_H
#define SYSTEM_CPU_TIMERS_H

#include "qemu/timer.h"

/* init the whole cpu timers API, including icount, ticks, and cpu_throttle */
void cpu_timers_init(void);

/*
 * CPU Ticks and Clock
 */

/* Caller must hold BQL */
void cpu_enable_ticks(void);
/* Caller must hold BQL */
void cpu_disable_ticks(void);

/*
 * return the time elapsed in VM between vm_start and vm_stop.
 * cpu_get_ticks() uses units of the host CPU cycle counter.
 */
int64_t cpu_get_ticks(void);

/*
 * Returns the monotonic time elapsed in VM, i.e.,
 * the time between vm_start and vm_stop
 */
int64_t cpu_get_clock(void);

void qemu_timer_notify_cb(void *opaque, QEMUClockType type);

/* get/set VIRTUAL clock and VM elapsed ticks via the cpus accel interface */
int64_t cpus_get_virtual_clock(void);
void cpus_set_virtual_clock(int64_t new_time);
int64_t cpus_get_elapsed_ticks(void);

/*
 * Precise QEMU_CLOCK_VIRUAL
 * */
extern bool use_icount2;

#ifdef CONFIG_TCG
#define icount2_enabled() (use_icount2)
#else
#define icount2_enabled() (0)
#endif

void icount2_configure(QemuOpts *opts, Error **errp);
void icount2_on_tick(void);
void icount2_sync(void);
int64_t icount2_get(void);
void icount2_enter_sleep(void);
void icount2_exit_sleep(void);
void icount2_set_ns_per_tick(int64_t ns_per_tick);

#endif /* SYSTEM_CPU_TIMERS_H */
