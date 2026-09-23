#include "qemu/osdep.h"
#include "qapi/error.h"
#include "exec/icount.h"
#include "system/cpu-timers.h"

/* icount - Instruction Counter API */

ICountMode use_icount = ICOUNT_DISABLED;

bool icount_configure(QemuOpts *opts, Error **errp)
{
    /* signal error */
    error_setg(errp, "cannot configure icount, TCG support not available");

    return false;
}
int64_t icount_get_raw(void)
{
    abort();
    return 0;
}
void icount_start_warp_timer(void)
{
    abort();
}
void icount_account_warp_timer(void)
{
    abort();
}
void icount_notify_exit(void)
{
    abort();
}

/*
 * Precise clocks
 * */
bool use_icount2;

void icount2_advance(uint32_t cycles) {
	abort();
}

void icount2_sync(void)
{
	abort();
}

int64_t icount2_get(void)
{
	abort();
	return 0;
}

void icount2_enter_sleep(void)
{
	abort();
}

void icount2_exit_sleep(void)
{
	abort();
}

void icount2_wakeup(int cpu_index, bool halted, int mask, int interrupt_request)
{
	abort();
}

int64_t icount2_get_horizon(void)
{
	abort();
	return 0;
}

int64_t icount2_get_horizon_delay(int64_t target)
{
	abort();
	return 0;
}

void icount2_set_limit(int64_t (*fn)(void *opaque, int64_t want), void *opaque)
{
}

void icount2_limit_advanced(void)
{
}
