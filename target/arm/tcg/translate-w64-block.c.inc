/*
 * WASM PORT FAST PATH (removable): one TLB probe for all the words of an
 * LDM/STM, where the per-word code probes for each (a quarter of the
 * probes a J2ME game executes are the later words of a push or a pop).
 * It is the generated code's own probe (tcg/wasm64 w64_tlb_probe) with
 * the key taken from the last word: the entry of the first word's page
 * matches it only if both words are on that page, aligned, and the page
 * is plain RAM for this access, which is when each per-word probe would
 * have hit.  Everything else goes to @slow, the per-word code.
 */
static TCGv_ptr w64_block_host(TCGv_i32 addr, int n, int mem_idx, bool store,
                               TCGLabel *slow)
{
    int fast = offsetof(CPUNegativeOffsetState,
                        tlb.f[mmuidx_to_fast_index(mem_idx)]) -
               sizeof(CPUNegativeOffsetState);
    TCGv_i32 t = tcg_temp_new_i32();
    TCGv_i32 key = tcg_temp_new_i32();
    TCGv_ptr entry = tcg_temp_new_ptr();
    TCGv_ptr host = tcg_temp_new_ptr();
    TCGv_i64 word = tcg_temp_new_i64();

    tcg_gen_ld_i32(t, tcg_env, fast + offsetof(CPUTLBDescFast, mask));
    tcg_gen_shri_i32(key, addr, TARGET_PAGE_BITS - CPU_TLB_ENTRY_BITS);
    tcg_gen_and_i32(t, t, key);
    tcg_gen_ext_i32_ptr(host, t);
    tcg_gen_ld_ptr(entry, tcg_env, fast + offsetof(CPUTLBDescFast, table));
    tcg_gen_add_ptr(entry, entry, host);
    tcg_gen_ld_i64(word, entry, store ? offsetof(CPUTLBEntry, addend_write)
                                      : offsetof(CPUTLBEntry, addend_read));
    tcg_gen_addi_i32(key, addr, 4 * (n - 1));
    tcg_gen_andi_i32(key, key, TARGET_PAGE_MASK | 3);
    tcg_gen_extrh_i64_i32(t, word);
    slow->cold = true;
    tcg_gen_brcond_i32(TCG_COND_NE, t, key, slow);

    tcg_gen_extrl_i64_i32(t, word);
    tcg_gen_add_i32(t, t, addr);
    tcg_gen_extu_i32_i64((TCGv_i64)host, t);
    return host;
}

static bool w64_block_fast(DisasContext *s, int n, bool user)
{
    return n > 1 && !user && s->be_data == MO_LE;
}

/* The fast arm of an STM and the label of the per-word code; the caller
 * sets the returned label after that code.  NULL: no fast arm. */
static TCGLabel *w64_stm_fast(DisasContext *s, arg_ldst_block *a,
                              TCGv_i32 addr, int n, int mem_idx)
{
    TCGLabel *slow, *done;
    TCGv_ptr host;
    int i, j;

    if (!w64_block_fast(s, n, a->u)) {
        return NULL;
    }
    slow = gen_new_label();
    done = gen_new_label();
    host = w64_block_host(addr, n, mem_idx, true, slow);
    for (i = j = 0; i < 16; i++) {
        if (a->list & (1 << i)) {
            tcg_gen_st_i32(load_reg(s, i), host, 4 * j++);
        }
    }
    /* where the per-word code leaves it */
    tcg_gen_addi_i32(addr, addr, 4 * (n - 1));
    tcg_gen_br(done);
    gen_set_label(slow);
    return done;
}

/*
 * The same for an LDM.  The base and the pc are stored after the two arms
 * meet, as the per-word code does for the base and as it may for the pc,
 * the last word: both arms load them into @base and @pc.
 */
static TCGLabel *w64_ldm_fast(DisasContext *s, arg_ldst_block *a,
                              TCGv_i32 addr, int n, int mem_idx,
                              bool exc_return, TCGv_i32 *base, TCGv_i32 *pc)
{
    TCGLabel *slow, *done;
    TCGv_ptr host;
    int i, j;

    if (!w64_block_fast(s, n, a->u) || exc_return) {
        return NULL;
    }
    slow = gen_new_label();
    done = gen_new_label();
    if (a->list & (1 << a->rn)) {
        *base = tcg_temp_new_i32();
    }
    if (a->list & (1 << 15)) {
        *pc = tcg_temp_new_i32();
    }
    host = w64_block_host(addr, n, mem_idx, false, slow);
    for (i = j = 0; i < 16; i++) {
        if (!(a->list & (1 << i))) {
            continue;
        }
        if (i == a->rn) {
            tcg_gen_ld_i32(*base, host, 4 * j++);
        } else if (i == 15) {
            tcg_gen_ld_i32(*pc, host, 4 * j++);
        } else {
            TCGv_i32 tmp = tcg_temp_new_i32();

            tcg_gen_ld_i32(tmp, host, 4 * j++);
            store_reg(s, i, tmp);
        }
    }
    tcg_gen_addi_i32(addr, addr, 4 * (n - 1));
    tcg_gen_br(done);
    gen_set_label(slow);
    return done;
}
