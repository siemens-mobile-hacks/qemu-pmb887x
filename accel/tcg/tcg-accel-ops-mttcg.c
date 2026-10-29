/*
 * QEMU TCG Multi Threaded vCPUs implementation
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
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
#include "exec/cpu-common.h"
#include "system/tcg.h"
#include "system/replay.h"
#include "system/cpu-timers.h"
#include "system/cpus.h"
#include "exec/icount.h"
#include "qemu/main-loop.h"
#include "exec/translation-block.h"
#include "qemu/timer.h"
#include "qemu/notify.h"
#include "qemu/guest-random.h"
#include "hw/core/boards.h"
#include "accel/tcg/cpu-loop.h"
#include "tcg/startup.h"
#include "tcg-accel-ops.h"
#include "tcg-accel-ops-mttcg.h"

typedef struct MttcgForceRcuNotifier {
    Notifier notifier;
    CPUState *cpu;
} MttcgForceRcuNotifier;

static void do_nothing(CPUState *cpu, run_on_cpu_data d)
{
}

static int64_t mttcg_next_virtual_timer_ns(void)
{
    int64_t expire = qemu_clock_expire_time_ns(QEMU_CLOCK_VIRTUAL);

    if (expire < 0) {
        return -1;
    }
    return MAX(expire - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 0);
}

static int64_t mttcg_idle_virtual_timers(CPUState *cpu)
{
    cpu_clock_owner_progress(false);
    qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
    if (!cpu_thread_is_idle(cpu)) {
        return 0;
    }
    /* Interrupts are raised under the BQL, held until the thread sleeps. */
    cpu_clock_owner_progress(true);
    return mttcg_next_virtual_timer_ns();
}

static bool mttcg_wait_for_clock(CPUState *cpu, int64_t *realp)
{
    int64_t position, real, until;

    cpu_clock_owner_progress(false);
    real = *realp = cpu_clock_owner_limit_lag();
    position = cpu_clock_owner_position();
    if (position - real < CPU_CLOCK_OWNER_LEAD_NS) {
        return false;
    }
    until = position - CPU_CLOCK_OWNER_LEAD_NS / 2;
    if (cpu_get_real_clock() < until) {
        qemu_cpu_sleep_ns(cpu, MAX(until - real, CPU_CLOCK_OWNER_LEAD_NS / 2));
    }
    return true;
}

static void mttcg_force_rcu(Notifier *notify, void *data)
{
    CPUState *cpu = container_of(notify, MttcgForceRcuNotifier, notifier)->cpu;

    /*
     * Called with rcu_registry_lock held, using async_run_on_cpu() ensures
     * that there are no deadlocks.
     */
    async_run_on_cpu(cpu, do_nothing, RUN_ON_CPU_NULL);
}

/*
 * In the multi-threaded case each vCPU has its own thread. The TLS
 * variable current_cpu can be used deep in the code to find the
 * current CPUState for a given thread.
 */

static void *mttcg_cpu_thread_fn(void *arg)
{
    MttcgForceRcuNotifier force_rcu;
    CPUState *cpu = arg;

    assert(tcg_enabled());
    g_assert(!icount_enabled());

    rcu_register_thread();
    force_rcu.notifier.notify = mttcg_force_rcu;
    force_rcu.cpu = cpu;
    rcu_add_force_rcu_notifier(&force_rcu.notifier);
    tcg_register_thread();
    qemu_coroutine_forbid_current_thread();

    bql_lock();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->neg.can_do_io = true;
    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    if (qemu_virtual_timers_on_vcpu) {
        tcg_cflags_set(cpu, CF_COUNT_CYCLES);
        cpu_clock_set_owner(&cpu->neg.cycle_count, &cpu->neg.cycle_limit);
    }

    do {
        if (qemu_virtual_timers_on_vcpu) {
            qemu_process_cpu_events_timed(cpu, mttcg_idle_virtual_timers);
        } else {
            qemu_process_cpu_events(cpu);
        }

        if (cpu_can_run(cpu)) {
            int r;
            if (qemu_virtual_timers_on_vcpu) {
                int64_t real;

                qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
                if (mttcg_wait_for_clock(cpu, &real)) {
                    continue;
                }
                cpu_clock_owner_start_budget(real);
            }
            bql_unlock();
            r = tcg_cpu_exec(cpu);
            bql_lock();
            switch (r) {
            case EXCP_DEBUG:
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:
                /*
                 * Usually cpu->halted is set, but may have already been
                 * reset by another thread by the time we arrive here.
                 */
                break;
            case EXCP_ATOMIC:
                bql_unlock();
                cpu_exec_step_atomic(cpu);
                bql_lock();
            default:
                /* Ignore everything else? */
                break;
            }
        }
    } while (!cpu->unplug || cpu_can_run(cpu));

    tcg_cpu_destroy(cpu);
    bql_unlock();
    rcu_remove_force_rcu_notifier(&force_rcu.notifier);
    rcu_unregister_thread();
    return NULL;
}

void mttcg_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    g_assert(tcg_enabled());
    tcg_cpu_init_cflags(cpu, current_machine->smp.max_cpus > 1);

    /* create a thread per vCPU with TCG (MTTCG) */
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/TCG",
             cpu->cpu_index);

    qemu_thread_create(cpu->thread, thread_name, mttcg_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}
