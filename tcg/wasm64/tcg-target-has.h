/* SPDX-License-Identifier: MIT */
/*
 * Define target-specific opcode support: wasm64 backend.
 */

#ifndef TCG_TARGET_HAS_H
#define TCG_TARGET_HAS_H

/* extrl is a plain move: an I32 value in an i64 local has no canonical
 * upper half, and the readers that need one zero-extend (w64_get_u32). */
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
