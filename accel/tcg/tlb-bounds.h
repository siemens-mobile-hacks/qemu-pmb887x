/*
 * softmmu size bounds
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef ACCEL_TCG_TLB_BOUNDS_H
#define ACCEL_TCG_TLB_BOUNDS_H

#define CPU_TLB_DYN_MIN_BITS 6
#define CPU_TLB_DYN_MAX_BITS (32 - TARGET_PAGE_BITS)
#define CPU_TLB_DYN_DEFAULT_BITS 8
/* cap for the fill-driven growth in tlb_set_page_full */
#define TLB_FILL_GROW_MAX_BITS 14

#endif /* ACCEL_TCG_TLB_BOUNDS_H */
