/* SPDX-License-Identifier: MIT */
/* These opcodes for use between the tci generator and interpreter. */
DEF(tci_movi, 1, 0, 1, TCG_OPF_NOT_PRESENT)
DEF(tci_movl, 1, 0, 1, TCG_OPF_NOT_PRESENT)
DEF(tci_setcarry, 0, 0, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_clz32, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_ctz32, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_divs32, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_divu32, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_rems32, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_remu32, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_rotl32, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_rotr32, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_setcond32, 1, 2, 1, TCG_OPF_NOT_PRESENT)
DEF(tci_movcond32, 1, 2, 1, TCG_OPF_NOT_PRESENT)
DEF(tci_qemu_ld_rrr, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_qemu_st_rrr, 0, 3, 0, TCG_OPF_NOT_PRESENT)
/*
 * Immediate-form ops: the register allocator passes small constants
 * directly (tcg_target_const_match + out_rri in the outop tables),
 * saving the separate tci_movi materialization.  Signed 16-bit
 * immediates for the ALU forms, 12-bit for the setcond form.
 */
DEF(tci_add_ri, 1, 2, 1, TCG_OPF_NOT_PRESENT)
DEF(tci_and_ri, 1, 2, 1, TCG_OPF_NOT_PRESENT)
DEF(tci_or_ri, 1, 2, 1, TCG_OPF_NOT_PRESENT)
DEF(tci_xor_ri, 1, 2, 1, TCG_OPF_NOT_PRESENT)
DEF(tci_andc_ri, 1, 2, 1, TCG_OPF_NOT_PRESENT)
DEF(tci_setcond32_ri, 1, 2, 1, TCG_OPF_NOT_PRESENT)
#ifdef __EMSCRIPTEN__
/* wasm only: TB header carrying the guest-insn count, executed at every
 * TB entry (cpu_tb_exec call, goto_tb chain jump, goto_ptr target) so the
 * icount2 clock advances per executed TB even while TBs are chained. */
DEF(tci_tbhdr, 0, 0, 1, TCG_OPF_NOT_PRESENT)
#endif
