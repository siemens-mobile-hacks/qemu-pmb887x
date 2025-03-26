#include "qemu/osdep.h"
#include "qapi/error.h"
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

void icount2_on_tick(void) {
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

void icount2_set_ns_per_tick(int64_t ns_per_tick)
{
	abort();
}
