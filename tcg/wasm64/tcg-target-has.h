/* SPDX-License-Identifier: MIT */
/*
 * Define target-specific opcode support: wasm64 backend.
 */

#ifndef TCG_TARGET_HAS_H
#define TCG_TARGET_HAS_H

/* extrl/extrh are separate real ops (see tcg-target.h); "extr" as a
 * plain move needs a canonical i32-in-i64 representation we don't have. */
#define TCG_TARGET_HAS_extr_i64_i32     0

#define TCG_TARGET_HAS_qemu_ldst_i128   0

#define TCG_TARGET_HAS_tst              0

#define TCG_TARGET_extract_valid(type, ofs, len) \
    ((ofs) + (len) <= (type) * 8 ? 1 : 0)
#define TCG_TARGET_sextract_valid(type, ofs, len) \
    ((ofs) + (len) <= (type) * 8 ? 1 : 0)
#define TCG_TARGET_deposit_valid(type, ofs, len) \
    ((ofs) + (len) <= (type) * 8 ? 1 : 0)

#endif
