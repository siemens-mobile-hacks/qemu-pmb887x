#pragma once

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

#ifndef PMB887X_TRACE_PREFIX
#error "PMB887X_TRACE_PREFIX not defined!"
#endif

#define EPRINTF(fmt, ...) do { \
		error_report("[" PMB887X_TRACE_PREFIX "]: " fmt , ## __VA_ARGS__); \
	} while (0)

#define WPRINTF(fmt, ...) do { \
		warn_report("[" PMB887X_TRACE_PREFIX "]: " fmt , ## __VA_ARGS__); \
	} while (0)
