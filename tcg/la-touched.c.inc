/*
 * WASM PORT FAST PATH (removable): liveness without whole-temp walks.
 *
 * liveness_pass_1 resets every temp at each basic-block end and walks every
 * temp at each conditional branch and call-clobbering op.  An EBB or const
 * temp only ever ends a reset as TS_DEAD with no register preference, and
 * stays so until an op reads it (or a mov hands it a preference), so the
 * walks here visit the globals, the TEMP_TB temps and the EBB/const temps
 * touched since the last reset.  The states and preferences come out as the
 * full walks leave them.  The register allocator's block-end walk only has
 * work for the TEMP_TB temps, and its conditional-branch walk only asserts.
 * An EL71 boot translates ~74k TBs of ~24 globals and ~52 other temps; the
 * full walks took ~2900 steps per TB.
 *
 * Only wasm64 builds take it (LA_W64, set in tcg.c).  To remove: delete
 * this file, its #include in tcg.c, the branches there that test LA_W64 and
 * LA_W64_RA, and the la_w64_touch calls.
 */

#if LA_W64 && !defined(CONFIG_DEBUG_TCG)
#define LA_W64_RA 1
#else
#define LA_W64_RA 0
#endif

static __thread uint16_t la_w64_tb[TCG_MAX_TEMPS];  /* TEMP_TB, ascending */
static __thread uint16_t la_w64_e[TCG_MAX_TEMPS];   /* touched EBB/const */
static __thread uint8_t la_w64_on[TCG_MAX_TEMPS];
static __thread int la_w64_tb_n, la_w64_e_n;

static void temp_save(TCGContext *s, TCGTemp *ts, TCGRegSet allocated_regs);

/* la_func_end, plus the TEMP_TB list and an empty touched list */
static void la_w64_start(TCGContext *s, int ng, int nt)
{
    la_w64_tb_n = 0;
    la_w64_e_n = 0;
    memset(la_w64_on, 0, sizeof(la_w64_on));
    for (int i = 0; i < ng; ++i) {
        s->temps[i].state = TS_DEAD | TS_MEM;
        la_reset_pref(&s->temps[i]);
    }
    for (int i = ng; i < nt; ++i) {
        s->temps[i].state = TS_DEAD;
        la_reset_pref(&s->temps[i]);
        if (s->temps[i].kind == TEMP_TB) {
            la_w64_tb[la_w64_tb_n++] = i;
        }
    }
}

/* ts may stop being dead with no preference: an op reads it, or a mov
   hands it one */
static inline void la_w64_touch(TCGContext *s, TCGTemp *ts)
{
    size_t i;

    if (!LA_W64) {
        return;
    }
    i = temp_idx(ts);
    if (i >= s->nb_globals && ts->kind != TEMP_TB && !la_w64_on[i]) {
        la_w64_on[i] = 1;
        la_w64_e[la_w64_e_n++] = i;
    }
}

/*
 * Define LA_W64_CHECK to verify, at every walk, what the walks rest on: an
 * EBB/const temp off the touched list is dead with no preference, so the
 * full walk would have left it as it is.
 */
static void la_w64_check(TCGContext *s)
{
#ifdef LA_W64_CHECK
    static uint64_t walks;

    for (int i = s->nb_globals; i < s->nb_temps; i++) {
        TCGTemp *ts = &s->temps[i];

        if (ts->kind != TEMP_TB && !la_w64_on[i] && ts->state_ptr &&
            (ts->state != TS_DEAD || *la_temp_pref(ts) != 0)) {
            fprintf(stderr, "LA_W64_CHECK: temp %d kind %d state %llu pref "
                    "%llx off the list\n", i, ts->kind,
                    (unsigned long long)ts->state,
                    (unsigned long long)*la_temp_pref(ts));
            abort();
        }
    }
    if ((++walks & 0xfffff) == 0) {
        fprintf(stderr, "LA_W64_CHECK: %llu walks ok\n",
                (unsigned long long)walks);
    }
#endif
}

static void la_w64_reset_touched(TCGContext *s)
{
    la_w64_check(s);
    for (int k = 0; k < la_w64_e_n; k++) {
        TCGTemp *ts = &s->temps[la_w64_e[k]];

        ts->state = TS_DEAD;
        la_reset_pref(ts);
        la_w64_on[la_w64_e[k]] = 0;
    }
    la_w64_e_n = 0;
}

/* la_func_end */
static void la_w64_func_end(TCGContext *s, int ng)
{
    for (int i = 0; i < ng; ++i) {
        s->temps[i].state = TS_DEAD | TS_MEM;
        la_reset_pref(&s->temps[i]);
    }
    for (int k = 0; k < la_w64_tb_n; k++) {
        TCGTemp *ts = &s->temps[la_w64_tb[k]];

        ts->state = TS_DEAD;
        la_reset_pref(ts);
    }
    la_w64_reset_touched(s);
}

/* la_bb_end */
static void la_w64_bb_end(TCGContext *s, int ng)
{
    for (int i = 0; i < ng; ++i) {
        tcg_debug_assert(s->temps[i].kind == TEMP_FIXED ||
                         s->temps[i].kind == TEMP_GLOBAL);
        s->temps[i].state = TS_DEAD | TS_MEM;
        la_reset_pref(&s->temps[i]);
    }
    for (int k = 0; k < la_w64_tb_n; k++) {
        TCGTemp *ts = &s->temps[la_w64_tb[k]];

        ts->state = TS_DEAD | TS_MEM;
        la_reset_pref(ts);
    }
    la_w64_reset_touched(s);
}

/* la_bb_sync */
static void la_w64_bb_sync(TCGContext *s, int ng)
{
    la_global_sync(s, ng);
    for (int k = 0; k < la_w64_tb_n; k++) {
        TCGTemp *ts = &s->temps[la_w64_tb[k]];
        int state = ts->state;

        ts->state = state | TS_MEM;
        if (state == TS_DEAD) {
            la_reset_pref(ts);
        }
    }
}

static inline void la_w64_cross_one(TCGTemp *ts, TCGRegSet mask)
{
    if (!(ts->state & TS_DEAD)) {
        TCGRegSet *pset = la_temp_pref(ts);
        TCGRegSet set = *pset & mask;

        /* If the combination is not possible, restart.  */
        if (set == 0) {
            set = tcg_target_available_regs[ts->type] & mask;
        }
        *pset = set;
    }
}

/* la_cross_call */
static void la_w64_cross_call(TCGContext *s)
{
    TCGRegSet mask = ~tcg_target_call_clobber_regs;

    la_w64_check(s);
    for (int i = 0; i < s->nb_globals; i++) {
        la_w64_cross_one(&s->temps[i], mask);
    }
    for (int k = 0; k < la_w64_tb_n; k++) {
        la_w64_cross_one(&s->temps[la_w64_tb[k]], mask);
    }
    for (int k = 0; k < la_w64_e_n; k++) {
        la_w64_cross_one(&s->temps[la_w64_e[k]], mask);
    }
}

/* tcg_reg_alloc_bb_end's walk over the non-global temps */
static void la_w64_ra_bb_end(TCGContext *s, TCGRegSet allocated_regs)
{
    for (int k = 0; k < la_w64_tb_n; k++) {
        temp_save(s, &s->temps[la_w64_tb[k]], allocated_regs);
    }
}
