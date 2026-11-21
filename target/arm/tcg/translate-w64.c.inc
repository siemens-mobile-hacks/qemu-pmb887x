/*
 * WASM PORT FAST PATH (removable): the wasm64 translator, included by
 * translate.c under CONFIG_TCG_WASM64 where its hooks need it.
 *
 * The flag-setting register shifts inline (R4), the base of a constant
 * address (A2, A6), the superblocks and call inlining with their deferred
 * exits and the fill and blit matchers (A3, R6, R7), the PSR
 * continuation (R3) and the inline mrs (R3): see doc/wasm-fast-paths.md
 * for each row.  The LDM/STM block path (R8) is in translate-w64-block.c.inc,
 * after the decoder it takes its arguments from.  The hooks in translate.c
 * are under #ifdef CONFIG_TCG_WASM64.
 */

/*
 * The flag-setting register shifts, inline: on wasm64 a helper call
 * syncs every global around an import call, and `lsls r1, r5` sat on a
 * blit's hot path as the one helper left in it.  A 64-bit shift by the
 * count clamped to 63 covers every count from 1 to 255 at once: the
 * result is the low half, and the carry out is bit 32 of the shifted
 * value (LSL) or bit 0 after shifting a copy left by one first (LSR,
 * ASR - the sign fill of ASR gives CF = x[31] past 32, as the helper
 * does).  A count of 0 leaves CF alone.  ROR takes its count mod 32 and
 * its carry from the result's top bit whenever the count is not 0.
 */
static void gen_shift_cc_w64(TCGv_i32 var, int shiftop, TCGv_i32 shift)
{
    TCGv_i32 cnt = tcg_temp_new_i32();
    TCGv_i32 cf = tcg_temp_new_i32();
    TCGv_i32 zero = tcg_constant_i32(0);

    tcg_gen_andi_i32(cnt, shift, 0xff);
    if (shiftop == 3) {
        TCGv_i32 res = tcg_temp_new_i32();
        TCGv_i32 cnt5 = tcg_temp_new_i32();

        tcg_gen_andi_i32(cnt5, cnt, 0x1f);
        tcg_gen_rotr_i32(res, var, cnt5);
        tcg_gen_shri_i32(cf, res, 31);
        tcg_gen_movcond_i32(TCG_COND_NE, cpu_CF, cnt, zero, cf, cpu_CF);
        tcg_gen_mov_i32(var, res);
    } else {
        TCGv_i64 x = tcg_temp_new_i64();
        TCGv_i64 c64 = tcg_temp_new_i64();
        TCGv_i64 r = tcg_temp_new_i64();

        tcg_gen_umin_i32(cnt, cnt, tcg_constant_i32(63));
        tcg_gen_extu_i32_i64(c64, cnt);
        if (shiftop == 0) {
            tcg_gen_extu_i32_i64(x, var);
            tcg_gen_shl_i64(r, x, c64);
            tcg_gen_extrh_i64_i32(cf, r);
        } else {
            if (shiftop == 1) {
                tcg_gen_extu_i32_i64(x, var);
            } else {
                tcg_gen_ext_i32_i64(x, var);
            }
            tcg_gen_shli_i64(r, x, 1);
            if (shiftop == 1) {
                tcg_gen_shr_i64(r, r, c64);
            } else {
                tcg_gen_sar_i64(r, r, c64);
            }
            tcg_gen_extrl_i64_i32(cf, r);
            if (shiftop == 1) {
                tcg_gen_shr_i64(r, x, c64);
            } else {
                tcg_gen_sar_i64(r, x, c64);
            }
        }
        tcg_gen_andi_i32(cf, cf, 1);
        tcg_gen_extrl_i64_i32(var, r);
        tcg_gen_movcond_i32(TCG_COND_NE, cpu_CF, cnt, zero, cf, cpu_CF);
    }
}

/*
 * The base and bias that address the host object @p: env when this lone
 * CPU's env is the one the TB will run with (see w64_tb_env), so the TCG
 * optimizer keeps forwarding env values across the access and the backend
 * emits a constant address; a constant pointer otherwise.
 */
static TCGv_ptr w64_abs_base(DisasContext *s, const void *p, intptr_t *bias)
{
    if (!CPU_NEXT(first_cpu)) {
        *bias = (uintptr_t)p - (uintptr_t)s->w64_env;
        return tcg_env;
    }
    *bias = 0;
    return tcg_constant_ptr(p);
}

/*
 * gen_goto_ptr()'s exit through the per-TB slot and the pc cache (A2);
 * false where they do not apply.  @condexec: the condexec_bits value in
 * memory at this exit (what gen_set_condexec last stored, or 0 mid-TB —
 * see arm_tr_init_disas_context).
 */
static bool w64_goto_ptr(DisasContext *s, uint32_t condexec)
{
    /*
     * Every bx lr / pop {pc} / ldr pc and every msr CPSR_* ends here, so
     * the emitted test compares only what can differ at this exit from
     * what the translator knows.  hflags cannot: any insn that
     * changes them ends the TB (the TB's own flags are the proof), so a
     * plain branch exit stamps tb->flags into the slot statically and
     * the fill-time helper only ever fills a slot whose static words
     * match the CPU.  Likewise thumb (static unless gen_bx wrote it) and
     * condexec (always static: memory holds @condexec here).  Only an
     * exit after a CPSR write (w64_dynkey) has all three dynamic.  The
     * generation compare stands in for jump-cache validity and for the
     * rare VFP key inputs (helper_lookup_tb_ptr_lc).  M-profile keys on
     * more state than this covers: helper only.
     */
    if (!(tb_cflags(s->base.tb) & CF_NO_GOTO_PTR) &&
        !arm_dc_feature(s, ARM_FEATURE_M)) {
        struct W64LookupCache *lc = &s->base.tb->w64_lc;
        uint32_t key[3] = { s->base.tb->flags, s->w64_thumb, condexec };
        uint8_t mask = 0;

        if (s->w64_dynkey) {
            mask = W64_LC_DYN_FLAGS | W64_LC_DYN_THUMB | W64_LC_DYN_CONDEXEC;
        } else if (s->w64_thumb < 0) {
            mask = W64_LC_DYN_THUMB;
        }
        if (s->w64_lc_sites == 0) {
            s->w64_lc_sites = 1;
            memcpy(lc->key32, key, sizeof(key));
            lc->dynmask = mask;
        } else if (mask != lc->dynmask ||
                   memcmp(lc->key32, key, sizeof(key)) != 0) {
            /* a second exit keyed differently: it stays off the slot, but
             * the global cache is not the slot's and still covers it */
            TCGv_ptr tc = tcg_temp_new_ptr();

            gen_helper_lookup_tb_ptr_lc(tc, tcg_env, tcg_constant_ptr(NULL));
            tcg_gen_goto_ptr(tc);
            return true;
        }

        intptr_t lco;
        TCGv_ptr lcb = w64_abs_base(s, lc, &lco);
        TCGv_ptr tc = tcg_temp_new_ptr();

        {
        TCGLabel *slow = gen_new_label();
        TCGv_i32 a32 = tcg_temp_new_i32();
        TCGv_i32 b32 = tcg_temp_new_i32();

        tcg_gen_ld_i32(a32, lcb, lco + offsetof(struct W64LookupCache, pc));
        tcg_gen_brcond_i32(TCG_COND_NE, a32, cpu_R[15], slow);
        tcg_gen_ld_i32(a32, lcb, lco + offsetof(struct W64LookupCache, gen));
        tcg_gen_ld_i32(b32, tcg_env,
                       offsetof(ARMCPU, parent_obj.neg.tb_key_gen) -
                       offsetof(ARMCPU, env));
        tcg_gen_brcond_i32(TCG_COND_NE, a32, b32, slow);
        if (mask & W64_LC_DYN_FLAGS) {
            tcg_gen_ld_i32(a32, lcb,
                           lco + offsetof(struct W64LookupCache, key32[0]));
            tcg_gen_ld_i32(b32, tcg_env,
                           offsetof(CPUARMState, hflags.flags));
            tcg_gen_brcond_i32(TCG_COND_NE, a32, b32, slow);
        }
        if (mask & W64_LC_DYN_THUMB) {
            tcg_gen_ld_i32(a32, lcb,
                           lco + offsetof(struct W64LookupCache, key32[1]));
            tcg_gen_ld8u_i32(b32, tcg_env, offsetof(CPUARMState, thumb));
            tcg_gen_brcond_i32(TCG_COND_NE, a32, b32, slow);
        }
        if (mask & W64_LC_DYN_CONDEXEC) {
            tcg_gen_ld_i32(a32, lcb,
                           lco + offsetof(struct W64LookupCache, key32[2]));
            tcg_gen_ld_i32(b32, tcg_env,
                           offsetof(CPUARMState, condexec_bits));
            tcg_gen_brcond_i32(TCG_COND_NE, a32, b32, slow);
        }
        tcg_gen_ld_ptr(tc, lcb, lco + offsetof(struct W64LookupCache, tc));
        tcg_gen_goto_ptr(tc);
        gen_set_label(slow);
        }
        gen_helper_lookup_tb_ptr_lc(tc, tcg_env, tcg_constant_ptr(lc));
        tcg_gen_goto_ptr(tc);
        return true;
    }
    return false;
}

/*
 * Move the per-TB-entry instruction counter, for the paths that make one
 * entry stop meaning "tb->icount instructions ran": negative where an
 * early exit skipped some, positive where a loop pass re-ran the body
 * without coming back through the prologue.  Nothing to do unless that
 * counter is being charged inline.
 */
static void w64_acct_add(DisasContext *s, void *acct, int insns)
{
    /* off env: a loop pass would otherwise reload the cycle counter it
     * has just stored (w64_abs_base) */
    intptr_t ofs;
    TCGv_ptr base = w64_abs_base(s, acct, &ofs);
    TCGv_i64 v = tcg_temp_new_i64();

    tcg_gen_ld_i64(v, base, ofs);
    tcg_gen_addi_i64(v, v, insns);
    tcg_gen_st_i64(v, base, ofs);
}

static void w64_acct_charge(DisasContext *s, int insns)
{
    uint64_t *acct = w64_tb_acct_insns();
    int64_t *ls = w64_ls_count();

    if (!insns) {
        return;
    }
    if (acct) {
        w64_acct_add(s, acct, insns);
    }
    if (ls) {
        w64_acct_add(s, ls, insns);
    }
}

/*
 * The CF_COUNT_CYCLES cycles of the instructions translated so far: what
 * the prologue charges for them, counted like translator_loop counts them
 * (num_extra_cycles holds what translator_count_cycles added statically).
 */
static int w64_cycles(DisasContext *s)
{
    return s->base.num_insns + s->base.num_extra_cycles;
}

/*
 * Translation carries on at the target of a taken branch, which refilled
 * the pipeline: no result is still in flight to stall on.
 */
static void w64_branch_taken(DisasContext *s)
{
    s->arm9e_late = 0;
}

/*
 * Charge the next loop pass: the body is everything up to the back-edge,
 * which the pass runs without coming back through the prologue, so it
 * has to do the prologue's budget check too.
 */
static void w64_charge_cycles(DisasContext *s, TCGLabel *exit)
{
    intptr_t oc = offsetof(CPUState, neg.cycle_count) - sizeof(CPUState);
    intptr_t ol = offsetof(CPUState, neg.cycle_limit) - sizeof(CPUState);
    TCGv_i32 cycles, left;

    if (!(tb_cflags(s->base.tb) & CF_COUNT_CYCLES)) {
        return;
    }
    cycles = tcg_temp_new_i32();
    left = tcg_temp_new_i32();
    tcg_gen_ld_i32(cycles, tcg_env, oc);
    tcg_gen_ld_i32(left, tcg_env, ol);
    tcg_gen_sub_i32(left, left, cycles);
    tcg_gen_brcondi_i32(TCG_COND_LE, left, 0, exit);
    tcg_gen_addi_i32(cycles, cycles, w64_cycles(s));
    tcg_gen_st_i32(cycles, tcg_env, oc);
}

/*
 * WASM PORT FAST PATH (removable): a TB that is exactly the fill loop
 * w64-fill.c describes hands the passes after the one just charged to
 * HELPER(w64_fill).
 */
static void w64_fill_match(DisasContext *s)
{
    const uint16_t *w = s->w64_fill_words;
    unsigned rv = w[0] & 7, rp = (w[0] >> 3) & 7;
    unsigned rd = (w[1] >> 3) & 7, rc, rn, lg, width;
    uint32_t desc, charge, taken;

    if (!s->thumb || s->base.num_insns != 6 ||
        s->pc_curr != s->base.pc_first + 10 ||
        (w[0] & 0xffc0) != 0x6800 || (w[1] & 7) != rv) {
        return;
    }
    switch (w[1] & 0xffc0) {
    case 0x7000:
        lg = 0;
        break;
    case 0x8000:
        lg = 1;
        break;
    case 0x6000:
        lg = 2;
        break;
    default:
        return;
    }
    width = 1u << lg;
    if ((w[2] & 0xff00) == (0x3000 | rd << 8) && (w[2] & 0xff) == width) {
        rc = (w[3] >> 8) & 7;
        if (w[3] != (0x3000 | rc << 8 | 1)) {
            return;
        }
    } else if ((w[3] & 0xff00) == (0x3000 | rd << 8) &&
               (w[3] & 0xff) == width) {
        rc = (w[2] >> 8) & 7;
        if (w[2] != (0x3000 | rc << 8 | 1)) {
            return;
        }
    } else {
        return;
    }
    rn = (w[4] >> 3) & 7;
    if (w[4] != (0x4280 | rn << 3 | rc) || w[5] != 0xdbf9) {
        return;
    }
    if (ctpop32(1u << rv | 1u << rp | 1u << rd | 1u << rc | 1u << rn) != 5) {
        return;
    }
    desc = rv | rp << 4 | rd << 8 | rc << 12 | rn << 16 | lg << 20;
    if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
        desc |= W64_FILL_ICOUNT;
    }
    if (tb_cflags(s->base.tb) & CF_COUNT_CYCLES) {
        desc |= W64_FILL_CYCLES;
    }
    /* trans_B_cond_thumb charges a taken branch's cycles at run time */
    taken = s->arm9e_cycles ? ARM9E_THUMB_BCOND_TAKEN_CYCLES : 0;
    charge = s->base.num_insns | taken << 8 | w64_cycles(s) << 16;
    s->w64_fill_desc = desc;
    gen_helper_w64_fill(tcg_env, tcg_constant_ptr(s->base.tb),
                        tcg_constant_i32(desc), tcg_constant_i32(charge));
}

/*
 * WASM PORT FAST PATH (removable): a TB that starts with the colour-keyed
 * blit loop w64-fill.c describes hands the passes after the one just
 * charged to HELPER(w64_blit).  The loop's three forward branches have to
 * be joins, whose refunds the helper repeats.
 */
static void w64_blit_match(DisasContext *s)
{
    static const uint16_t pat[17][2] = {
        { 0x8819, 0xffff },     /* ldrh  r1, [r3] */
        { 0x4281, 0xffff },     /* cmp   r1, r0 */
        { 0xd007, 0xffff },     /* beq   skip */
        { 0x4c00, 0xff00 },     /* ldr   r4, =colour */
        { 0x42b9, 0xffff },     /* cmp   r1, r7 */
        { 0xd003, 0xffff },     /* beq   store */
        { 0x2400, 0xff00 },     /* movs  r4, #imm */
        { 0x2901, 0xffff },     /* cmp   r1, #1 */
        { 0xd000, 0xffff },     /* beq   store */
        { 0x1c0c, 0xffff },     /* adds  r4, r1, #0 */
        { 0x8014, 0xffff },     /* strh  r4, [r2] */
        { 0x9900, 0xff00 },     /* ldr   r1, [sp, #imm] */
        { 0x4463, 0xffff },     /* add   r3, ip */
        { 0x188a, 0xffff },     /* adds  r2, r1, r2 */
        { 0x3d01, 0xffff },     /* subs  r5, #1 */
        { 0x2d00, 0xffff },     /* cmp   r5, #0 */
        { 0xd1ee, 0xffff },     /* bne   loop */
    };
    const uint16_t *w = s->w64_fill_words;
    bool cyc = tb_cflags(s->base.tb) & CF_COUNT_CYCLES;
    uint32_t desc, charge, refund = 0, taken, lit;
    unsigned seen = 0;

    if (!s->thumb || s->base.num_insns != 17 ||
        s->pc_curr != s->base.pc_first + 32 ||
        s->w64_ft_n || s->w64_join_n != 3) {
        return;
    }
    for (int i = 0; i < 17; i++) {
        if ((w[i] & pat[i][1]) != pat[i][0]) {
            return;
        }
    }
    /* the joins skipped 8 (transparent), 4 (the literal) and 1 (#imm) */
    for (int i = 0; i < 3; i++) {
        unsigned path = ctz32(s->w64_join[i].insns);

        if (s->w64_join[i].insns != 1u << path || path == 1 || path > 3) {
            return;
        }
        path = path ? path - 1 : 0;
        seen |= 1u << path;
        refund |= (cyc ? s->w64_join[i].cycles : 0) << (path * 8);
    }
    if (seen != 7) {
        return;
    }
    desc = (w[6] & 0xff) | (w[11] & 0xff) << 8;
    if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
        desc |= W64_FILL_ICOUNT;
    }
    if (cyc) {
        desc |= W64_FILL_CYCLES;
    }
    lit = ((s->base.pc_first + 6 + 4) & ~3u) + (w[3] & 0xff) * 4;
    taken = s->arm9e_cycles ? ARM9E_THUMB_BCOND_TAKEN_CYCLES : 0;
    charge = s->base.num_insns | taken << 8 | w64_cycles(s) << 16;
    gen_helper_w64_blit(tcg_env, tcg_constant_i32(lit),
                        tcg_constant_i32(desc), tcg_constant_i32(refund),
                        tcg_constant_i32(charge));
}

/* the target of the Thumb b<cond> (T1) @insn at @pc */
static vaddr w64_bcond_dest(vaddr pc, uint16_t insn)
{
    return pc + 4 + (int8_t)insn * 2;
}

/*
 * WASM PORT FAST PATH (removable): the two TBs of a fill rectangle's row
 * (w64-fill.c), each found by the deferred exit it leaves for the other.
 * The loop's TB carries on after the loop into the row tail,
 *
 *         adds  Rd, Rpitch, Rd    @ to the next row
 *         adds  Rrow, #1
 *         ldr   Rlim, [sp, #imm]
 *         cmp   Rrow, Rlim
 *         blt   head
 *
 * and the head's TB is
 *
 *   head: movs  Rc, #0
 *         b     test
 *   test: cmp   Rc, Rn
 *         blt   loop
 *
 * Called from tb_stop before the deferred exits are emitted; the exit's
 * refund and slot are filled in as it is (w64_emit_deferred_taken).
 */
static void w64_rect_match(DisasContext *s)
{
    const uint16_t *w = s->w64_fill_words;
    uint32_t desc = s->w64_fill_desc;
    struct W64FillRect *r = &s->w64_rect;
    unsigned want, rd, rc, rn, pitch, row, lim, moving, fixed;
    vaddr dest;
    int i;

    memset(r, 0, sizeof(*r));
    if (!s->thumb || (tb_cflags(s->base.tb) & CF_USE_ICOUNT)) {
        return;
    }
    if (desc) {
        if (s->base.num_insns < 11 || (w[6] & 0xfe00) != 0x1800) {
            return;
        }
        rd = (desc >> 8) & 7;
        /* the loop's Rv, Rd, Rc change every pass; Rp, Rn do not */
        moving = 1u << (desc & 7) | 1u << rd | 1u << ((desc >> 12) & 7);
        fixed = 1u << ((desc >> 4) & 7) | 1u << ((desc >> 16) & 7);
        if ((w[6] & 7) != rd) {
            return;
        }
        if (((w[6] >> 6) & 7) == rd) {
            pitch = (w[6] >> 3) & 7;
        } else if (((w[6] >> 3) & 7) == rd) {
            pitch = (w[6] >> 6) & 7;
        } else {
            return;
        }
        row = (w[7] >> 8) & 7;
        lim = (w[8] >> 8) & 7;
        if (w[7] != (0x3000 | row << 8 | 1) || (w[8] & 0xf800) != 0x9800 ||
            w[9] != (0x4280 | lim << 3 | row) || (w[10] & 0xff00) != 0xdb00) {
            return;
        }
        /*
         * Rpitch and Rrow hold through the row; Rlim may be one the loop
         * rewrites anyway, but not one it reads unchanged.
         */
        if (((moving | 1u << row | 1u << lim) & 1u << pitch) ||
            ((moving | fixed | 1u << lim) & 1u << row) ||
            ((fixed | 1u << rd) & 1u << lim)) {
            return;
        }
        /* the loop and the tail are one linear run of 16-bit insns */
        dest = w64_bcond_dest(s->base.pc_first + 20, w[10]);
        want = 11;
        r->kind = W64_RECT_ROW;
        r->regs = pitch | row << 4 | lim << 8 | (w[8] & 0xff) << 16;
    } else {
        if (s->base.num_insns < 4) {
            return;
        }
        rc = (w[0] >> 8) & 7;
        rn = (w[2] >> 3) & 7;
        if (w[0] != (0x2000 | rc << 8) || (w[1] & 0xf800) != 0xe000 ||
            w[2] != (0x4280 | rn << 3 | rc) || rn == rc ||
            (w[3] & 0xff00) != 0xdb00) {
            return;
        }
        /* the b was absorbed: the test is at its target */
        dest = s->base.pc_first + 2 + 4 + sextract32(w[1], 0, 11) * 2;
        dest = w64_bcond_dest(dest + 2, w[3]);
        want = 4;
        r->kind = W64_RECT_HEAD;
        r->regs = rc | rn << 4;
    }
    for (i = 0; i < s->w64_ft_n; i++) {
        if (s->w64_ft[i].insns == want && s->w64_ft[i].dest == dest) {
            break;
        }
    }
    if (i == s->w64_ft_n) {
        r->kind = 0;
        return;
    }
    r->dest = dest;
    r->taken = s->arm9e_cycles ? ARM9E_THUMB_BCOND_TAKEN_CYCLES : 0;
    s->w64_rect_ft = i;
}

static bool w64_back_edge(DisasContext *s, int64_t diff)
{
    intptr_t off32 = offsetof(CPUState, neg.icount_decr.u32) -
                     sizeof(CPUState);
    intptr_t off16 = offsetof(CPUState, neg.icount_decr.u16.low) -
                     sizeof(CPUState);
    bool icount = tb_cflags(s->base.tb) & CF_USE_ICOUNT;
    vaddr pc_save = s->pc_save;
    bool conditional;
    TCGv_i32 count;
    TCGLabel *exit;

    if (s->pc_curr + diff != s->base.pc_first ||
        tcg_ctx->exitreq_label == NULL || s->condexec_mask || s->eci ||
        unlikely(s->ss_active) ||
        (tb_cflags(s->base.tb) & (CF_SINGLE_STEP | CF_MEMORY_TIMING))) {
        return false;
    }
    /* blx to the TB's start runs the other instruction set; MVE's LE needs
     * the TB to end (both as in w64_absorb) */
    if (s->w64_thumb != s->thumb || arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    /*
     * A conditional back-edge is the taken arm of a brcond, so the TB can
     * carry on into the fall-through exactly as it does for a deferred
     * taken path -- the loop body is then num_insns *here*, which is what
     * the back-edge has to charge for the next pass.
     */
    conditional = s->condjmp && (s->base.is_jmp == DISAS_NEXT ||
                                 s->base.is_jmp == DISAS_TOO_MANY);
    if (conditional) {
        if (s->w64_loop_insns) {
            return false;       /* one such exit per TB */
        }
        s->w64_loop_exit = gen_disas_label(s);
        s->w64_loop_insns = s->base.num_insns;
        s->w64_loop_cycles = w64_cycles(s);
        exit = s->w64_loop_exit.label;
    } else {
        exit = tcg_ctx->exitreq_label;
    }

    gen_update_pc(s, diff);
    count = tcg_temp_new_i32();
    tcg_gen_ld_i32(count, tcg_env, off32);
    if (icount) {
        tcg_gen_subi_i32(count, count, s->base.num_insns);
    }
    tcg_gen_brcondi_i32(TCG_COND_LT, count, 0, exit);
    if (icount) {
        tcg_gen_st16_i32(count, tcg_env, off16);
    }
    w64_charge_cycles(s, exit);
    w64_acct_charge(s, s->base.num_insns);
    if (conditional) {
        w64_fill_match(s);  /* WASM PORT FAST PATH (removable) */
        w64_blit_match(s);  /* WASM PORT FAST PATH (removable) */
    }
    tcg_gen_br(s->w64_loop.label);

    if (conditional) {
        s->pc_save = pc_save;   /* the fall-through never ran that store */
    } else {
        s->base.is_jmp = DISAS_NORETURN;
    }
    return true;
}

static bool w64_defer_taken(DisasContext *s, int64_t diff)
{
    /*
     * NORETURN here is the "gen_brcondi(l); gen_jmp(); gen_set_label(l);
     * gen_jmp()" idiom: arm_post_translate_insn would not place
     * condlabel, so the fall-through would be unreachable and the label
     * never set.
     */
    if (s->base.is_jmp != DISAS_NEXT && s->base.is_jmp != DISAS_TOO_MANY) {
        return false;
    }
    if (!s->condjmp || s->w64_ft_n >= W64_FT_MAX ||
        s->condexec_mask || s->eci || unlikely(s->ss_active) ||
        (tb_cflags(s->base.tb) & (CF_SINGLE_STEP | CF_MEMORY_TIMING))) {
        return false;
    }
    s->w64_ft[s->w64_ft_n].label = gen_disas_label(s);
    s->w64_ft[s->w64_ft_n].dest = s->pc_curr + diff;
    s->w64_ft[s->w64_ft_n].insns = s->base.num_insns;
    s->w64_ft[s->w64_ft_n].cycles = w64_cycles(s);
    tcg_gen_br(s->w64_ft[s->w64_ft_n].label.label);
    s->w64_ft_n++;
    /*
     * Leave is_jmp alone: arm_post_translate_insn places condlabel for
     * DISAS_NEXT/DISAS_TOO_MANY and the loop carries on into the
     * fall-through.
     */
    return true;
}

/*
 * Hand back what the prologue prepaid for instructions not run - unless
 * can_do_io says a device may already have read the prepaid clock in this
 * TB: a refund after that would move the clock the device saw *backwards*.
 * The translator clears can_do_io before the first instruction and sets it
 * only before the last one, which no deferred exit is, and a mid-TB MMIO
 * access rewinds the TB (cpu_io_recompile).  When it is set anyway the
 * instructions are left charged - time runs one TB ahead - and the guest
 * instruction count stays exact.
 */
static void w64_refund(DisasContext *dc, int skipped, int cycles)
{
    if (skipped > 0 && (tb_cflags(dc->base.tb) & CF_USE_ICOUNT)) {
        intptr_t off = offsetof(CPUState, neg.icount_decr.u16.low) -
                       sizeof(CPUState);
        intptr_t cdi = offsetof(CPUState, neg.can_do_io) - sizeof(CPUState);
        TCGv_i32 c = tcg_temp_new_i32();
        TCGv_i32 seen = tcg_temp_new_i32();
        TCGv_i32 r = tcg_temp_new_i32();

        tcg_gen_ld8u_i32(seen, tcg_env, cdi);
        tcg_gen_movcond_i32(TCG_COND_EQ, r, seen, tcg_constant_i32(0),
                            tcg_constant_i32(skipped), tcg_constant_i32(0));
        tcg_gen_ld16u_i32(c, tcg_env, off);
        tcg_gen_add_i32(c, c, r);
        tcg_gen_st16_i32(c, tcg_env, off);
    }
    if (skipped > 0) {
        w64_acct_charge(dc, -skipped);
    }
    /*
     * Without icount no device reads a prepaid clock: a mid-TB access
     * rewinds the TB (cpu_io_recompile), and the last
     * instruction, the only one with can_do_io, is past every deferred
     * exit.  So the cycles go back unconditionally.
     */
    if (cycles > 0 && (tb_cflags(dc->base.tb) & CF_COUNT_CYCLES)) {
        intptr_t oc = offsetof(CPUState, neg.cycle_count) - sizeof(CPUState);
        TCGv_i32 c = tcg_temp_new_i32();

        tcg_gen_ld_i32(c, tcg_env, oc);
        tcg_gen_subi_i32(c, c, cycles);
        tcg_gen_st_i32(c, tcg_env, oc);
    }
}

/* defined below, with the rest of the two-page stream machinery */
static bool w64_abs_cross_page(DisasContext *s, vaddr dest);

/*
 * Is translation off the TB's own linear stream -- inside an inlined
 * callee, or past a branch w64_absorb took to the other tracked page?
 * Both are tracked by the hull of what was translated rather than by a
 * linear span, so the bytes in between cost nothing.
 */
static inline bool w64_inl_stream(const DisasContext *s)
{
    return s->w64_inl_depth || s->w64_abs_rec >= 0;
}

static bool w64_absorb(DisasContext *s, int64_t diff)
{
    vaddr dest = s->pc_curr + diff;
    vaddr skip;

    if (s->base.is_jmp != DISAS_NEXT || s->condjmp || s->condexec_mask ||
        s->eci || unlikely(s->ss_active) ||
        (tb_cflags(s->base.tb) & (CF_SINGLE_STEP | CF_MEMORY_TIMING))) {
        return false;
    }
    /*
     * trans_BLX_i's target runs in the *other* instruction set -- it says so
     * by flipping w64_thumb -- and translating on would decode it with this
     * TB's flags.  M-profile's MVE loop instructions (trans_WLS, trans_LE)
     * emit their own control flow around gen_jmp and need it to end the TB.
     */
    if (s->w64_thumb != s->thumb || arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    /* the page the stream is on: an inlined callee's, not the entry page */
    if (((dest ^ s->page_start) & TARGET_PAGE_MASK) != 0) {
        if (w64_abs_cross_page(s, dest)) {
            return true;
        }
        return false;
    }
    if (w64_inl_stream(s)) {
        /*
         * Inside an inlined callee the bytes jumped over cost nothing: the
         * invalidation range is the hull of the instructions actually
         * translated (translation-block.h w64_inl), not a linear span.  So
         * a forward branch may go anywhere on the callee's page, and a
         * backward one anywhere *below* everything translated on it - a
         * shared epilogue placed before the function, say.  Backward into
         * translated code is a loop, and absorbing it would unroll it.
         */
        TranslationBlock *tb = s->base.tb;
        unsigned n = s->w64_inl_page;
        vaddr lo = -1;

        if (tb->w64_inl & W64_INL_PAGE(n)) {
            lo = s->page_start + tb->w64_inl_lo[n];
        }
        if (n == 0) {
            lo = MIN(lo, s->base.pc_first);
        }
        if (dest <= s->base.pc_next && dest >= lo) {
            return false;
        }
        if (dest > s->base.pc_next) {
            skip = dest - s->base.pc_next;
        } else {
            skip = 0;
        }
        s->base.pc_next = dest;
        if (!s->thumb && skip) {
            s->base.max_insns -= skip / 4;
        }
        w64_branch_taken(s);
        return true;
    }
    if (dest <= s->base.pc_next) {
        return false;
    }
    if (dest - s->base.pc_next > W64_ABSORB_MAX) {
        return false;
    }
    skip = dest - s->base.pc_next;
    s->base.pc_next = dest;
    if (!s->thumb) {
        s->base.max_insns -= skip / 4;
    }
    w64_branch_taken(s);
    return true;
}

/* record the bytes of the instruction at pc_curr on the page it is on */
static void w64_inl_track(DisasContext *s, vaddr end_incl)
{
    TranslationBlock *tb = s->base.tb;
    unsigned n = s->w64_inl_page;
    unsigned lo = s->pc_curr & ~TARGET_PAGE_MASK;
    unsigned hi = end_incl & ~TARGET_PAGE_MASK;
    unsigned bit = W64_INL_PAGE(n);

    if (!(tb->w64_inl & bit)) {
        tb->w64_inl_lo[n] = lo;
        tb->w64_inl_hi[n] = hi;
        tb->w64_inl |= bit;
    } else {
        tb->w64_inl_lo[n] = MIN(tb->w64_inl_lo[n], lo);
        tb->w64_inl_hi[n] = MAX(tb->w64_inl_hi[n], hi);
    }
    int rec = s->w64_inl_depth ? s->w64_inl_recidx[s->w64_inl_depth - 1]
                               : s->w64_abs_rec;

    if (rec >= 0) {
        struct W64InlRec *r = &w64_inl_pending[rec];

        r->lo = MIN(r->lo, lo);
        r->hi = MAX(r->hi, hi);
    }
}

/*
 * arm_tr_init_disas_context bounds an A32 TB to the instructions left on
 * its entry page, which is right for a linear stream and wrong the moment
 * the stream moves: a callee near the end of its own page, or a caller
 * resumed after one, has its own remainder.  Re-bound from where the
 * stream now is, never past the cap the TB started with.  (Thumb needs
 * nothing: it re-checks page_start after every instruction.)
 */
static void w64_inl_rebound(DisasContext *s, vaddr pc)
{
    if (!s->thumb) {
        int left = -(pc | TARGET_PAGE_MASK) / 4;

        s->base.max_insns = MIN(s->w64_max_insns0, s->base.num_insns + left);
    }
}

/*
 * Which tracked page a stream that moves to @dest may use: page 0 is the
 * TB's entry page, page 1 the one slot a TB has besides it.  Claiming
 * page 1 for the first time needs a nonfault probe, so an unmapped page
 * is a refusal here rather than a prefetch abort raised for instructions
 * that have not run.
 */
static bool w64_inl_pick_page(DisasContext *s, vaddr dest, uint8_t *on_page)
{
    DisasContextBase *db = &s->base;
    TranslationBlock *tb = db->tb;
    vaddr page = dest & TARGET_PAGE_MASK;
    vaddr page0 = db->pc_first & TARGET_PAGE_MASK;

    /*
     * A linear crossing owns page 1 and carries no hull for it, so once any
     * stream sets w64_inl, tb_page_span would read an unset hull for that
     * page: refuse the whole TB to another stream, even one that would stay
     * on page 0.
     */
    if (tb_page_addr1(tb) != -1 && !(tb->w64_inl & W64_INL_VPAGE1)) {
        return false;
    }
    if (page == page0) {
        *on_page = 0;
        return true;
    }
    if (tb->w64_inl & W64_INL_VPAGE1) {
        /* page 1 already holds a callee page: reuse it if it is this one */
        if (tb->w64_inl_vpage1 != page - page0) {
            return false;
        }
    } else {
        /*
         * Page 1 is free unless a linear crossing has fetched through it --
         * a crossing carries no hull, so tb_page_span would have nothing to
         * invalidate that page by.
         */
        void *host;
        int fl;

        if (db->host_addr[1] != NULL) {
            return false;
        }
        fl = probe_access_flags(s->w64_env, page, 0, MMU_INST_FETCH,
                                db->code_mmuidx, true, &host, 0);
        if ((fl & (TLB_INVALID_MASK | TLB_MMIO)) || host == NULL) {
            return false;
        }
        tb->w64_inl_vpage1 = page - page0;
        tb->w64_inl |= W64_INL_VPAGE1;
    }
    db->w64_page1 = page;
    *on_page = 1;
    return true;
}

/*
 * An unconditional direct branch off the page the stream is on used to end
 * the TB.  A TB tracks two guest pages, and unless an inlined callee has
 * claimed the second one it is sitting unused: take it, and translation
 * carries straight on at the target.
 *
 * This is the callee-inlining machinery with the call taken out -- the
 * target is static, so there is no guard, no miss path and no icount
 * refund; only the bookkeeping that makes a second stream safe.  The
 * bytes between the branch and its target cost nothing, because from here
 * the page is invalidated by the hull of what was actually translated on
 * it (translation-block.h w64_inl) rather than by a linear span.
 *
 * One per TB, and not from inside an inlined callee: the depth stack
 * restores @page_start on return, and moving the stream out from under it
 * would restore the wrong one.
 */
static bool w64_abs_cross_page(DisasContext *s, vaddr dest)
{
    DisasContextBase *db = &s->base;
    uint8_t on_page;

    if (s->w64_inl_depth || s->w64_abs_rec >= 0 ||
        w64_inl_pending_n >= W64_INL_REC || s->pc_save == -1 ||
        (tb_cflags(db->tb) & CF_COUNT_MASK)) {
        return false;
    }
    if (!w64_inl_pick_page(s, dest, &on_page)) {
        return false;
    }

    /* the branch belongs to the stream it ends, so charge it before moving */
    w64_inl_track(s, s->pc_curr + curr_insn_len(s) - 1);
    s->w64_inl_tracked = true;
    db->w64_lin_end = db->pc_next;
    {
        struct W64InlRec *rec = &w64_inl_pending[w64_inl_pending_n];

        rec->idx0 = db->num_insns;
        rec->idx1 = 0xffff;         /* closed by w64_inl_close_records */
        rec->lo = 0xffff;
        rec->hi = 0;
        rec->page = on_page;
        s->w64_abs_rec = w64_inl_pending_n++;
    }
    s->page_start = dest & TARGET_PAGE_MASK;
    s->w64_inl_page = on_page;
    db->pc_next = dest;
    w64_inl_rebound(s, dest);
    w64_branch_taken(s);
    return true;
}

static bool w64_inline_call(DisasContext *s, int64_t diff)
{
    DisasContextBase *db = &s->base;
    TranslationBlock *tb = db->tb;
    vaddr dest = s->pc_curr + diff;
    vaddr ret = db->pc_next;
    unsigned depth = s->w64_inl_depth;
    uint8_t on_page;

    if (tcg_ctx->addr_type == TCG_TYPE_I32) {
        dest = (uint32_t)dest;
    }
    if (s->condjmp || s->condexec_mask) {
        return false;
    }
    if (depth >= W64_INL_DEPTH || s->w64_inl_miss_n >= W64_INL_MISS ||
        w64_inl_pending_n >= W64_INL_REC) {
        return false;
    }
    if (db->is_jmp != DISAS_NEXT ||
        s->eci || unlikely(s->ss_active) ||
        (tb_cflags(tb) &
         (CF_SINGLE_STEP | CF_COUNT_MASK | CF_MEMORY_TIMING)) ||
        s->pc_save == -1 ||
        arm_dc_feature(s, ARM_FEATURE_M) ||
        s->w64_thumb != s->thumb) {
        return false;
    }
    /* a bl in the last word of its page returns to a page this TB has no
     * slot for */
    if (((ret ^ s->page_start) & TARGET_PAGE_MASK) != 0) {
        return false;
    }
    if (!w64_inl_pick_page(s, dest, &on_page)) {
        return false;
    }

    if (depth) {
        w64_inl_track(s, s->pc_curr + curr_insn_len(s) - 1);
    }
    s->w64_inl_tracked = true;
    {
        struct W64InlRec *r = &w64_inl_pending[w64_inl_pending_n];

        r->idx0 = db->num_insns;
        r->idx1 = 0xffff;
        r->lo = 0xffff;
        r->hi = 0;
        r->page = on_page;
        s->w64_inl_recidx[depth] = w64_inl_pending_n++;
    }
    s->w64_inl_ret[depth] = ret | s->thumb;
    s->w64_inl_pstart[depth] = s->page_start;
    s->w64_inl_page_save[depth] = s->w64_inl_page;
    s->w64_inl_depth = depth + 1;
    s->page_start = dest & TARGET_PAGE_MASK;
    s->w64_inl_page = on_page;
    /* after an absorbed cross-page b the linear range has ended already */
    if (depth == 0 && s->w64_abs_rec < 0) {
        db->w64_lin_end = ret;
    }
    db->pc_next = dest;
    w64_inl_rebound(s, dest);
    w64_branch_taken(s);
    return true;
}

/* bx lr inside an inlined callee: compare, and carry on after the call */
static bool w64_inline_return(DisasContext *s, int rm)
{
    unsigned depth = s->w64_inl_depth;
    unsigned m = s->w64_inl_miss_n;
    vaddr ret;
    TCGv_i32 lr, want;

    if (depth == 0 || rm != 14 || m >= W64_INL_MISS ||
        s->condexec_mask || s->eci || unlikely(s->ss_active) ||
        s->base.is_jmp != DISAS_NEXT || s->pc_save == -1) {
        return false;
    }
    if (s->condjmp) {
        /*
         * A conditional return (bxeq lr): its taken arm is a return when
         * lr matches, so it becomes a deferred taken path to the return
         * address - the join lands when the unconditional return brings
         * translation there - and the fall-through stays in the callee.
         */
        if (s->w64_ft_n >= W64_FT_MAX) {
            return false;
        }
    }
    ret = s->w64_inl_ret[depth - 1];
    if (!s->condjmp) {
        depth--;
        w64_inl_track(s, s->pc_curr + curr_insn_len(s) - 1);
        s->w64_inl_tracked = true;
    }

    /* the return address as lr holds it */
    lr = load_reg(s, 14);
    want = tcg_temp_new_i32();
    gen_pc_plus_diff(s, want, (int64_t)ret - (int64_t)s->pc_curr);
    s->w64_inl_miss[m].label = gen_disas_label(s);
    s->w64_inl_miss[m].insns = s->base.num_insns;
    s->w64_inl_miss[m].cycles = w64_cycles(s);
    tcg_gen_brcond_i32(TCG_COND_NE, lr, want,
                       s->w64_inl_miss[m].label.label);
    s->w64_inl_miss_n = m + 1;

    if (s->condjmp) {
        s->w64_ft[s->w64_ft_n].label = gen_disas_label(s);
        s->w64_ft[s->w64_ft_n].dest = ret & ~(vaddr)1;
        s->w64_ft[s->w64_ft_n].insns = s->base.num_insns;
        s->w64_ft[s->w64_ft_n].cycles = w64_cycles(s);
        tcg_gen_br(s->w64_ft[s->w64_ft_n].label.label);
        s->w64_ft_n++;
        return true;
    }

    s->w64_inl_depth = depth;
    w64_inl_pending[s->w64_inl_recidx[depth]].idx1 = s->base.num_insns;
    s->page_start = s->w64_inl_pstart[depth];
    s->w64_inl_page = s->w64_inl_page_save[depth];
    if (depth == 0 && s->w64_abs_rec < 0) {
        s->base.w64_lin_end = 0;
    }
    s->base.pc_next = ret & ~(vaddr)1;
    w64_inl_rebound(s, s->base.pc_next);
    w64_branch_taken(s);
    return true;
}

/* callees the TB ended inside: their stream runs to the TB's end */
static void w64_inl_close_records(DisasContext *dc)
{
    for (unsigned r = 0; r < w64_inl_pending_n; r++) {
        if (w64_inl_pending[r].idx1 == 0xffff) {
            w64_inl_pending[r].idx1 = dc->base.num_insns;
        }
    }
}

/* the mismatched-return exits, from tb_stop */
static void w64_emit_inline_misses(DisasContext *dc)
{
    for (unsigned i = 0; i < dc->w64_inl_miss_n; i++) {
        set_disas_label(dc, dc->w64_inl_miss[i].label);
        w64_refund(dc, dc->base.num_insns - dc->w64_inl_miss[i].insns,
                   w64_cycles(dc) - dc->w64_inl_miss[i].cycles);
        gen_bx(dc, load_reg(dc, 14));
        gen_goto_ptr(dc, 0);
    }
}

/*
 * A deferred taken path (w64_defer_taken) exits to its target at the end of
 * the TB -- but the fall-through it let the TB carry on into often arrives
 * at that very address a few instructions later, which is what an
 * `if (cond) { ... }` is.  Place the label there instead and the branch
 * costs no exit at all.
 *
 * The taken path skipped the instructions between, and gen_tb_start prepaid
 * for them, so it refunds them on its way in; the fall-through branches
 * over that refund.  Freeing the slot also lets the next conditional branch
 * be deferred, so a run of them folds into one TB.
 *
 * Called from insn_start, before tcg_gen_insn_start, so the refund belongs
 * to the instruction before and everything after the join unwinds as @pc.
 */
static void w64_try_join(DisasContext *s, vaddr pc)
{
    unsigned i = 0;

    if (s->condexec_mask || s->eci) {
        return;
    }
    /*
     * The taken path arrives with the pipeline refilled, so nothing it ran
     * can stall the join instruction; the instructions after the join are
     * charged once for both paths, so the fall-through must not owe a
     * stall there either (arm9e-cycles.h).
     */
    if (s->arm9e_cycles && s->arm9e_late) {
        return;
    }
    while (i < s->w64_ft_n) {
        int skipped = s->base.num_insns - 1 - s->w64_ft[i].insns;
        int cycles = w64_cycles(s) - 1 - s->w64_ft[i].cycles;
        DisasLabel l = s->w64_ft[i].label;

        if (s->w64_ft[i].dest != pc) {
            i++;
            continue;
        }
        if (s->pc_save != l.pc_save) {
            gen_update_pc(s, l.pc_save - s->pc_curr);
        }
        /* WASM PORT FAST PATH (removable): for w64_blit_match() */
        if (s->w64_join_n < ARRAY_SIZE(s->w64_join)) {
            s->w64_join[s->w64_join_n].insns = MAX(skipped, 0);
            s->w64_join[s->w64_join_n].cycles = MAX(cycles, 0);
        }
        s->w64_join_n = MIN(s->w64_join_n + 1, UINT8_MAX);
        if (skipped > 0 || cycles > 0) {
            TCGLabel *cont = gen_new_label();

            tcg_gen_br(cont);
            set_disas_label(s, l);
            w64_refund(s, skipped, cycles);
            gen_set_label(cont);
        } else {
            set_disas_label(s, l);
        }
        s->w64_ft[i] = s->w64_ft[--s->w64_ft_n];
    }
}

/*
 * The exits tb_stop emits after the TB's last instruction leave from
 * points inside it, where the key is the TB's own: whatever ended the TB
 * (a BLX to the other instruction set, a bx, a CPSR write) is not on
 * their path.  Left as that instruction set them, a Thumb TB's deferred
 * exits after a BLX <imm> to ARM code were keyed ARM, and their inline
 * lookup could never hit.
 */
static void w64_key_static(DisasContext *dc)
{
    dc->w64_thumb = dc->thumb;
    dc->w64_dynkey = false;
}

/* the deferred taken paths and the loop's interrupt exit, from tb_stop */
static void w64_emit_deferred_taken(DisasContext *dc)
{
    w64_key_static(dc);
    if (dc->w64_loop_insns) {
        set_disas_label(dc, dc->w64_loop_exit);
        w64_refund(dc, dc->base.num_insns - dc->w64_loop_insns,
                   w64_cycles(dc) - dc->w64_loop_cycles);
        /*
         * The refund leaves the cycle budget unspent again, which an
         * exit-requested return would take for icount's: leave as from
         * the end of a TB, the pc already written.
         */
        if (tb_cflags(dc->base.tb) & CF_USE_ICOUNT) {
            tcg_gen_br(tcg_ctx->exitreq_label);
        } else {
            tcg_gen_exit_tb(NULL, 0);
        }
    }
    for (unsigned i = 0; i < dc->w64_ft_n; i++) {
        int skipped = dc->base.num_insns - dc->w64_ft[i].insns;
        int cycles = w64_cycles(dc) - dc->w64_ft[i].cycles;
        int64_t diff;

        set_disas_label(dc, dc->w64_ft[i].label);
        w64_refund(dc, skipped, cycles);
        diff = dc->w64_ft[i].dest - dc->pc_curr;
        if (~dc->w64_slots & 3) {
            if (i == dc->w64_rect_ft && skipped <= UINT8_MAX &&
                cycles <= UINT16_MAX) {
                /* WASM PORT FAST PATH (removable): what w64_refund did */
                dc->w64_rect.refund_insns = MAX(skipped, 0);
                dc->w64_rect.refund_cycles =
                    (tb_cflags(dc->base.tb) & CF_COUNT_CYCLES) ?
                    MAX(cycles, 0) : 0;
                dc->w64_rect.slot = ctz32(~dc->w64_slots);
                dc->w64_rect_ft = -2;   /* emitted as a goto_tb */
            }
            gen_goto_tb(dc, ctz32(~dc->w64_slots), diff);
        } else {
            gen_update_pc(dc, diff);
            gen_goto_ptr(dc, 0);
        }
    }
}

/*
 * A CPSR write ends the TB because it can move the key the next lookup
 * uses.  It cannot move most of it: gen_msr_mask clears CPSR_EXEC, so T,
 * IT and J are untouchable, and condexec and thumb therefore cannot
 * change.  What is left is hflags -- mode, E, IL, PAN -- and the
 * interrupt this write may just have unmasked.
 *
 * So test exactly those two in emitted code and carry on translating when
 * they hold.  A mode change *within* EL1 (svc -> irq, say) leaves hflags
 * equal and is still correct to continue through: the banked registers it
 * swaps are values in env, which the rest of the TB reloads anyway
 * because helper_cpsr_write clobbers every global.  A write whose mask
 * reaches no hflags input at all (`msr cpsr_f`, restoring the condition
 * flags) skips the hflags compare entirely.
 *
 * The interrupt test reproduces the old path exactly rather than
 * approximating it.  helper_cpsr_write has already run
 * cpsr_write_check_irq, so a pending interrupt has set icount_decr's high
 * half; taking the miss exit here reaches the same next TB, which stops
 * at its own prologue just as it did before.  Continuing instead would
 * push the interrupt to the end of this TB -- architecturally legal, but
 * a divergence from the native reference the lockstep gate compares.
 */
static bool w64_psr_can_continue(DisasContext *s)
{
    TranslationBlock *tb = s->base.tb;

    if (s->base.is_jmp != DISAS_NEXT ||
        s->eci || unlikely(s->ss_active) || s->pc_save == -1 ||
        (tb_cflags(tb) & (CF_SINGLE_STEP | CF_COUNT_MASK))) {
        return false;
    }
    if (s->condjmp || s->condexec_mask) {
        return false;
    }
    return s->w64_psr_miss_n < W64_PSR_MISS;
}

static TCGLabel *w64_psr_new_miss(DisasContext *s, TCGv_i32 val, uint32_t mask)
{
    unsigned n = s->w64_psr_miss_n++;

    s->w64_psr_miss[n].label = gen_disas_label(s);
    s->w64_psr_miss[n].dest = s->base.pc_next;
    s->w64_psr_miss[n].insns = s->base.num_insns;
    s->w64_psr_miss[n].cycles = w64_cycles(s);
    s->w64_psr_miss[n].val = val;
    s->w64_psr_miss[n].mask = mask;
    return s->w64_psr_miss[n].label.label;
}

static void w64_gen_irq_pending_br(TCGv_i32 a, TCGLabel *miss)
{
    tcg_gen_ld_i32(a, tcg_env,
                   offsetof(ARMCPU, parent_obj.interrupt_request) -
                   offsetof(ARMCPU, env));
    tcg_gen_brcondi_i32(TCG_COND_NE, a, 0, miss);
}

static bool w64_psr_continue(DisasContext *s, uint32_t mask)
{
    /* what cpsr_write() lets move hflags, plus PAN for good measure */
    const uint32_t hflags_in = CPSR_M | CPSR_E | CPSR_IL | CPSR_PAN;
    TCGLabel *miss;
    TCGv_i32 a;

    if (!w64_psr_can_continue(s)) {
        return false;
    }
    a = tcg_temp_new_i32();
    miss = w64_psr_new_miss(s, NULL, 0);
    if (mask & hflags_in) {
        tcg_gen_ld_i32(a, tcg_env, offsetof(CPUARMState, hflags.flags));
        tcg_gen_brcondi_i32(TCG_COND_NE, a, (uint32_t)s->base.tb->flags, miss);
    }
    w64_gen_irq_pending_br(a, miss);
    return true;
}

/*
 * cpsr_write(CPSRWriteByInstr) in emitted code, for the write that leaves
 * the mode alone: `msr cpsr_c` around a critical section, `msr cpsr_f`.
 * The helper carries no call flags, so the call alone cost a sync and
 * reload of every global besides its body.
 *
 * With the mask limited to NZCV, Q, A/I/F and M, and M unchanged, what
 * cpsr_write() does reduces to the flag fields and daif: uncached_cpsr
 * receives only M bits it already holds, so hflags cannot move and
 * HELPER(cpsr_write) skips its rebuild.  The SCR.AW/FW and NMFI filters
 * apply only with EL3, which is tested here at translation time.
 *
 * The tests run before anything is written, so the miss exit can hand the
 * untouched state to the helper.  A pending interrupt takes the miss too:
 * the helper then kicks icount_decr exactly as it did on the called path,
 * and the next TB stops at its prologue.
 */
static bool w64_cpsr_write_inline(DisasContext *s, TCGv_i32 val, uint32_t mask)
{
    const uint32_t inline_ok = CPSR_NZCV | CPSR_Q | CPSR_AIF | CPSR_M;
    uint32_t aif = mask & CPSR_AIF;
    TCGLabel *miss;
    TCGv_i32 t;

    if ((mask & ~inline_ok) || arm_dc_feature(s, ARM_FEATURE_EL3) ||
        !w64_psr_can_continue(s)) {
        return false;
    }
    t = tcg_temp_new_i32();
    miss = w64_psr_new_miss(s, val, mask);
    if (mask & CPSR_M) {
        /* cpsr_write() ORs in M4 before comparing: PMB887x ignores it */
        TCGv_i32 u = tcg_temp_new_i32();

        tcg_gen_ld_i32(u, tcg_env, offsetof(CPUARMState, uncached_cpsr));
        tcg_gen_ori_i32(t, val, 0x10);
        tcg_gen_xor_i32(t, t, u);
        tcg_gen_andi_i32(t, t, mask & CPSR_M);
        tcg_gen_brcondi_i32(TCG_COND_NE, t, 0, miss);
    }
    w64_gen_irq_pending_br(t, miss);

    if (mask & CPSR_NZCV) {
        tcg_gen_not_i32(cpu_ZF, val);
        tcg_gen_andi_i32(cpu_ZF, cpu_ZF, CPSR_Z);
        tcg_gen_mov_i32(cpu_NF, val);
        tcg_gen_extract_i32(cpu_CF, val, 29, 1);
        tcg_gen_shli_i32(cpu_VF, val, 3);
        tcg_gen_andi_i32(cpu_VF, cpu_VF, 0x80000000);
    }
    if (mask & CPSR_Q) {
        tcg_gen_extract_i32(t, val, 27, 1);
        tcg_gen_st_i32(t, tcg_env, offsetof(CPUARMState, QF));
    }
    if (aif) {
        TCGv_i32 d = tcg_temp_new_i32();

        /* the low word of the u64: little-endian host (wasm) */
        tcg_gen_ld_i32(d, tcg_env, offsetof(CPUARMState, daif));
        tcg_gen_andi_i32(d, d, ~aif);
        tcg_gen_andi_i32(t, val, aif);
        tcg_gen_or_i32(d, d, t);
        tcg_gen_st_i32(d, tcg_env, offsetof(CPUARMState, daif));
    }
    return true;
}

/* the guard's failed exits, from tb_stop */
static void w64_emit_psr_misses(DisasContext *dc)
{
    for (unsigned i = 0; i < dc->w64_psr_miss_n; i++) {
        int64_t diff = dc->w64_psr_miss[i].dest - dc->pc_curr;

        set_disas_label(dc, dc->w64_psr_miss[i].label);
        if (dc->w64_psr_miss[i].val) {
            gen_set_cpsr(dc->w64_psr_miss[i].val, dc->w64_psr_miss[i].mask);
        }
        w64_refund(dc, dc->base.num_insns - dc->w64_psr_miss[i].insns,
                   w64_cycles(dc) - dc->w64_psr_miss[i].cycles);
        gen_update_pc(dc, diff);
        dc->w64_dynkey = true;
        gen_goto_ptr(dc, 0);
    }
}

/*
 * HELPER(cpsr_read) in emitted code: cpsr_read() & ~CPSR_EXEC.  The EXEC
 * bits (T, IT, J, IL) are dropped, so thumb and condexec_bits are not
 * read; the flag globals are used where they live instead of being
 * synced for the helper and reloaded after it.  Critical sections (mrs,
 * orr #0xc0, msr cpsr_c) make this one of the most frequent helper calls
 * a TB used to make.
 */
static TCGv_i32 w64_gen_cpsr_read(void)
{
    TCGv_i32 r = tcg_temp_new_i32();
    TCGv_i32 t = tcg_temp_new_i32();

    tcg_gen_ld_i32(r, tcg_env, offsetof(CPUARMState, uncached_cpsr));
    tcg_gen_andi_i32(t, cpu_NF, 0x80000000);
    tcg_gen_or_i32(r, r, t);
    tcg_gen_setcondi_i32(TCG_COND_EQ, t, cpu_ZF, 0);
    tcg_gen_shli_i32(t, t, 30);
    tcg_gen_or_i32(r, r, t);
    tcg_gen_shli_i32(t, cpu_CF, 29);
    tcg_gen_or_i32(r, r, t);
    tcg_gen_andi_i32(t, cpu_VF, 0x80000000);
    tcg_gen_shri_i32(t, t, 3);
    tcg_gen_or_i32(r, r, t);
    tcg_gen_ld_i32(t, tcg_env, offsetof(CPUARMState, QF));
    tcg_gen_shli_i32(t, t, 27);
    tcg_gen_or_i32(r, r, t);
    tcg_gen_ld_i32(t, tcg_env, offsetof(CPUARMState, GE));
    tcg_gen_shli_i32(t, t, 16);
    tcg_gen_or_i32(r, r, t);
    /* the low word of the u64: little-endian host (wasm) */
    tcg_gen_ld_i32(t, tcg_env, offsetof(CPUARMState, daif));
    tcg_gen_andi_i32(t, t, CPSR_AIF);
    tcg_gen_or_i32(r, r, t);
    tcg_gen_andi_i32(r, r, ~CPSR_EXEC);
    return r;
}

/* gen_set_psr: a PSR write that does not end the TB where it need not */
static int w64_gen_set_psr(DisasContext *s, uint32_t mask, int spsr,
                           TCGv_i32 t0)
{
    if (spsr) {
        TCGv_i32 tmp;

        /* ??? This is also undefined in system mode.  */
        if (IS_USER(s)) {
            return 1;
        }
        tmp = load_cpu_field(spsr);
        tcg_gen_andi_i32(tmp, tmp, ~mask);
        tcg_gen_andi_i32(t0, t0, mask);
        tcg_gen_or_i32(tmp, tmp, t0);
        store_cpu_field(tmp, spsr);
        /* moves nothing the TB is keyed on, so it never needed to end one */
        if (s->base.is_jmp == DISAS_NEXT && !s->eci && !s->ss_active) {
            return 0;
        }
    } else {
        if (w64_cpsr_write_inline(s, t0, mask)) {
            return 0;
        }
        gen_set_cpsr(t0, mask);
        /* ends the TB only when the guard fails */
        if (w64_psr_continue(s, mask)) {
            return 0;
        }
    }
    /*
     * Continue through goto_ptr instead of a plain exit: helper_cpsr_write
     * requests the next-TB-start exit whenever an interrupt is pending,
     * which is all the loop round would have added.
     */
    gen_pc_plus_diff(s, cpu_R[15], curr_insn_len(s));
    s->base.is_jmp = DISAS_JUMP;
    s->w64_dynkey = true;
    return 0;
}

/*
 * After gen_helper_cpsr_write_eret(), with the new PC stored: continue
 * through goto_ptr, as the helper requests the next-TB-start exit for an
 * IRQ it unmasked.  Every exception return ends the TB: one that stayed
 * inside it would take such an IRQ only when something else next exits
 * to the loop, which the S75/EL71 GSM layer 1 notices and aborts on.
 */
static void w64_eret_end_tb(DisasContext *s)
{
    s->base.is_jmp = DISAS_JUMP;
    s->w64_dynkey = true;
}
