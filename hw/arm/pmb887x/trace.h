#pragma once

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace_common.h"

#ifndef PMB887X_TRACE_ID
#error "PMB887X_TRACE_ID not defined!"
#endif

#ifndef PMB887X_TRACE_PREFIX
#error "PMB887X_TRACE_PREFIX not defined!"
#endif

#define PMB887X_MOD_CONST_NAME_(a, b) a ## b
#define PMB887X_MOD_CONST_NAME(b) PMB887X_MOD_CONST_NAME_(PMB887X_TRACE_, b)

#define DPRINTF(fmt, ...) do { \
		if (pmb887x_trace_log_enabled(PMB887X_MOD_CONST_NAME(PMB887X_TRACE_ID))) { \
			qemu_log_mask(LOG_TRACE, "[" PMB887X_TRACE_PREFIX "]: " fmt , ## __VA_ARGS__); \
		} \
	} while (0)

#define EPRINTF(fmt, ...) do { \
		error_report("[" PMB887X_TRACE_PREFIX "]: " fmt , ## __VA_ARGS__); \
	} while (0)

#define WPRINTF(fmt, ...) do { \
		warn_report("[" PMB887X_TRACE_PREFIX "]: " fmt , ## __VA_ARGS__); \
	} while (0)

#define IO_DUMP(...) do { \
		if (pmb887x_trace_io_enabled(PMB887X_MOD_CONST_NAME(PMB887X_TRACE_ID))) { \
			pmb887x_dump_io(__VA_ARGS__); \
		} \
	} while (0)
