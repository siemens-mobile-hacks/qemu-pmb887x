/*
 * WASM PORT FAST PATH (removable): constant temps without a GHashTable.
 *
 * tcg_constant_internal finds a TB's constant temps in a GHashTable per
 * type, which tcg_func_start empties for every TB.  A lookup makes two
 * indirect calls (hash and compare), and the table grows from 8 buckets
 * and shrinks back again once per TB.  Here each type has an open-addressed
 * table of 2 * TCG_MAX_TEMPS slots, so it is never more than half full, and
 * tcg_func_start empties it by moving to a new generation.  A TB gets the
 * same temps in the same order.  A thread translates with one TCGContext
 * only, so the tables are per thread.
 *
 * Only wasm64 builds take it (W64_CONSTS, set in tcg.c).  To remove: delete
 * this file, its #include in tcg.c, and the branches there that test
 * W64_CONSTS.
 */

#define W64C_BITS 10
QEMU_BUILD_BUG_ON((1 << W64C_BITS) < 2 * TCG_MAX_TEMPS);

typedef struct {
    int64_t val;
    uint32_t gen;
    uint16_t idx;
} W64ConstEnt;

static __thread W64ConstEnt *w64c_tab[TCG_TYPE_COUNT];
static __thread uint32_t w64c_gen = 1;

static TCGTemp *tcg_temp_alloc(TCGContext *s);

/*
 * Define W64C_CHECK to keep the GHashTables too and abort when the two
 * disagree on a temp.
 */
static TCGTemp *w64c_check(TCGContext *s, TCGType type, int64_t val,
                           TCGTemp *ts, bool fresh)
{
#ifdef W64C_CHECK
    static uint64_t n;
    GHashTable *h = s->const_table[type];
    TCGTemp *ref;

    if (h == NULL) {
        h = g_hash_table_new(g_int64_hash, g_int64_equal);
        s->const_table[type] = h;
    }
    ref = g_hash_table_lookup(h, &val);
    if (fresh && ref == NULL) {
        g_hash_table_insert(h, &ts->val, ts);
        ref = ts;
    }
    if (ref != ts || ts->kind != TEMP_CONST || ts->type != type ||
        ts->val != val) {
        fprintf(stderr, "W64C_CHECK: type %d val %llx fresh %d: temp %d, "
                "GHashTable %d\n", type, (unsigned long long)val, fresh,
                (int)temp_idx(ts), ref ? (int)temp_idx(ref) : -1);
        abort();
    }
    if ((++n & 0xfffff) == 0) {
        fprintf(stderr, "W64C_CHECK: %llu lookups ok\n",
                (unsigned long long)n);
    }
#endif
    return ts;
}

/* tcg_func_start: forget the last TB's constants */
static void w64c_reset(TCGContext *s)
{
    if (++w64c_gen == 0) {
        for (int t = 0; t < TCG_TYPE_COUNT; t++) {
            if (w64c_tab[t]) {
                memset(w64c_tab[t], 0, sizeof(W64ConstEnt) << W64C_BITS);
            }
        }
        w64c_gen = 1;
    }
}

/* tcg_constant_internal */
static TCGTemp *w64c_constant(TCGContext *s, TCGType type, int64_t val)
{
    W64ConstEnt *tab = w64c_tab[type];
    uint32_t mask = (1 << W64C_BITS) - 1;
    uint32_t h = ((uint64_t)val * 0x9e3779b97f4a7c15ull) >> (64 - W64C_BITS);
    TCGTemp *ts;

    if (tab == NULL) {
        tab = g_new0(W64ConstEnt, 1 << W64C_BITS);
        w64c_tab[type] = tab;
    }
    for (;; h = (h + 1) & mask) {
        W64ConstEnt *e = &tab[h];

        if (e->gen != w64c_gen) {
            ts = tcg_temp_alloc(s);
            ts->base_type = type;
            ts->type = type;
            ts->kind = TEMP_CONST;
            ts->temp_allocated = 1;
            ts->val = val;
            e->val = val;
            e->idx = temp_idx(ts);
            e->gen = w64c_gen;
            return w64c_check(s, type, val, ts, true);
        }
        if (e->val == val) {
            return w64c_check(s, type, val, &s->temps[e->idx], false);
        }
    }
}
