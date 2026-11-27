/* SPDX-License-Identifier: MIT */
/*
 * Define target-specific memory model: wasm64 backend.
 */

#ifndef TCG_TARGET_MO_H
#define TCG_TARGET_MO_H

/* The host promises no ordering: TCG emits the barriers the guest needs,
 * and tcg_out_mb honours them with atomic.fence. */
#define TCG_TARGET_DEFAULT_MO  0

#endif
