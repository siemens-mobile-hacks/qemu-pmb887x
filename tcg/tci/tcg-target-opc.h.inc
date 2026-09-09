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
/*
 * Size-specialized guest memory ops: the generic qemu_ld/st interpreter
 * cases recompute the TLB compare masks (s_mask/a_mask/atom) and dispatch
 * on the size for every access.  For mops of the exact family
 * MO_ALIGN|MO_ATOM_NONE|size(|sign) - which on pmb887x/ARMv5 is every
 * plain guest data access - the generic probe tci_tlb_probe() reduces to
 *     (addr & (TARGET_PAGE_MASK | size-1)) == tlb_addr
 * (a_mask == s_mask, atom bits = NONE, LE host) so these variants bake that
 * in: no mask math, no atom branch, no size switch, straight to the access
 * on a hit.  The rrm immediate field carries only the mmu_idx; the case
 * reconstructs the full MemOp (see TCI_SPEC_LD/ST in tcg/tci.c).
 * The emitters (tgen_qemu_ld/st) pick them only on the exact family match;
 * everything else keeps the generic path.
 */
DEF(tci_qemu_ld8, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_qemu_ld8s, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_qemu_ld16, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_qemu_ld16s, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_qemu_ld32, 1, 2, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_qemu_st8, 0, 3, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_qemu_st16, 0, 3, 0, TCG_OPF_NOT_PRESENT)
DEF(tci_qemu_st32, 0, 3, 0, TCG_OPF_NOT_PRESENT)
