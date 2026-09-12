/* SPDX-License-Identifier: MIT */
/*
 * Define target-specific memory model: wasm64 backend.
 */

#ifndef TCG_TARGET_MO_H
#define TCG_TARGET_MO_H

/* Single linear memory, single vCPU thread: no ordering required. */
#define TCG_TARGET_DEFAULT_MO  0

#endif
