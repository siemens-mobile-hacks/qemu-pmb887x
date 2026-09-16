/*
 * Typed dispatch for helper calls with simple integer signatures.
 *
 * Shared by the two interpreters in this tree: tcg/tci.c, which tags
 * from an ffi_cif, and tcg/wasm64/w64-interp.c, which has no libffi and
 * tags from TCGHelperInfo's typemask.  Both produce the same tag, so
 * both can use this switch.
 *
 * On wasm every ffi_call() marshals its arguments through JS
 * (ffi_call_js, ~1.7 us); calling through an exactly-typed function
 * pointer is a plain call_indirect.  A wasm indirect call must match
 * the callee's type exactly, which is why the signatures are
 * enumerated rather than constructed.
 *
 * Argument words are already in stack[0..n-1], one 8-byte slot each;
 * the result is written back to stack[0].  The caller defines
 * TCI_CLS_U32 / TCI_CLS_U64 / TCI_TAG_UNCLASSIFIED and computes the
 * tag; anything this switch does not know returns false.
 */
/* i32 helper arguments are stored by the backend with a 4-byte store at
 * offset 0 of the 8-byte slot (INDEX_op_st32); read them back the same
 * way - casting the whole slot to uint32_t reads the wrong half on a
 * big-endian host. */
static inline uint32_t tci_slot_u32(uint64_t *stack, unsigned i)
{
    uint32_t v;

    memcpy(&v, &stack[i], sizeof(v));
    return v;
}

static inline void tci_slot_set_u32(uint64_t *stack, unsigned i, uint32_t v)
{
    memcpy(&stack[i], &v, sizeof(v));
}

static bool QEMU_DISABLE_CFI tci_call_direct(void *func, uint32_t tag,
                                             uint64_t *stack)
{
    switch (tag) {
    case 0x0000: /* (void) -> void */
        ((void (*)(void))func)();
        return true;
    case 0x0008: /* (void) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(void))func)());
        return true;
    case 0x0010: /* (void) -> u64 */
        stack[0] = ((uint64_t (*)(void))func)();
        return true;
    case 0x0021: /* (i32) -> void */
        ((void (*)(uint32_t))func)(tci_slot_u32(stack, 0));
        return true;
    case 0x0041: /* (i64) -> void */
        ((void (*)(uint64_t))func)(stack[0]);
        return true;
    case 0x0029: /* (i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t))func)(tci_slot_u32(stack, 0)));
        return true;
    case 0x0049: /* (i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t))func)(stack[0]));
        return true;
    case 0x0031: /* (i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t))func)(tci_slot_u32(stack, 0));
        return true;
    case 0x0051: /* (i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t))func)(stack[0]);
        return true;
    case 0x00a2: /* (i32,i32) -> void */
        ((void (*)(uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1));
        return true;
    case 0x00c2: /* (i64,i32) -> void */
        ((void (*)(uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1));
        return true;
    case 0x0122: /* (i32,i64) -> void */
        ((void (*)(uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1]);
        return true;
    case 0x0142: /* (i64,i64) -> void */
        ((void (*)(uint64_t, uint64_t))func)(stack[0], stack[1]);
        return true;
    case 0x00aa: /* (i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1)));
        return true;
    case 0x00ca: /* (i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1)));
        return true;
    case 0x012a: /* (i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1]));
        return true;
    case 0x014a: /* (i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t))func)(stack[0], stack[1]));
        return true;
    case 0x00b2: /* (i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1));
        return true;
    case 0x00d2: /* (i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1));
        return true;
    case 0x0132: /* (i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1]);
        return true;
    case 0x0152: /* (i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t))func)(stack[0], stack[1]);
        return true;
    case 0x02a3: /* (i32,i32,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2));
        return true;
    case 0x02c3: /* (i64,i32,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2));
        return true;
    case 0x0323: /* (i32,i64,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2));
        return true;
    case 0x0343: /* (i64,i64,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2));
        return true;
    case 0x04a3: /* (i32,i32,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2]);
        return true;
    case 0x04c3: /* (i64,i32,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2]);
        return true;
    case 0x0523: /* (i32,i64,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2]);
        return true;
    case 0x0543: /* (i64,i64,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2]);
        return true;
    case 0x02ab: /* (i32,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2)));
        return true;
    case 0x02cb: /* (i64,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2)));
        return true;
    case 0x032b: /* (i32,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2)));
        return true;
    case 0x034b: /* (i64,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2)));
        return true;
    case 0x04ab: /* (i32,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2]));
        return true;
    case 0x04cb: /* (i64,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2]));
        return true;
    case 0x052b: /* (i32,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2]));
        return true;
    case 0x054b: /* (i64,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2]));
        return true;
    case 0x02b3: /* (i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2));
        return true;
    case 0x02d3: /* (i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2));
        return true;
    case 0x0333: /* (i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2));
        return true;
    case 0x0353: /* (i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2));
        return true;
    case 0x04b3: /* (i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2]);
        return true;
    case 0x04d3: /* (i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2]);
        return true;
    case 0x0533: /* (i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2]);
        return true;
    case 0x0553: /* (i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2]);
        return true;
    case 0x0aa4: /* (i32,i32,i32,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3));
        return true;
    case 0x0ac4: /* (i64,i32,i32,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3));
        return true;
    case 0x0b24: /* (i32,i64,i32,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3));
        return true;
    case 0x0b44: /* (i64,i64,i32,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3));
        return true;
    case 0x0ca4: /* (i32,i32,i64,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3));
        return true;
    case 0x0cc4: /* (i64,i32,i64,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3));
        return true;
    case 0x0d24: /* (i32,i64,i64,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], tci_slot_u32(stack, 3));
        return true;
    case 0x0d44: /* (i64,i64,i64,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], tci_slot_u32(stack, 3));
        return true;
    case 0x12a4: /* (i32,i32,i32,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3]);
        return true;
    case 0x12c4: /* (i64,i32,i32,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3]);
        return true;
    case 0x1324: /* (i32,i64,i32,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), stack[3]);
        return true;
    case 0x1344: /* (i64,i64,i32,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), stack[3]);
        return true;
    case 0x14a4: /* (i32,i32,i64,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], stack[3]);
        return true;
    case 0x14c4: /* (i64,i32,i64,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], stack[3]);
        return true;
    case 0x1524: /* (i32,i64,i64,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], stack[3]);
        return true;
    case 0x1544: /* (i64,i64,i64,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3]);
        return true;
    case 0x0aac: /* (i32,i32,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3)));
        return true;
    case 0x0acc: /* (i64,i32,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3)));
        return true;
    case 0x0b2c: /* (i32,i64,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3)));
        return true;
    case 0x0b4c: /* (i64,i64,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3)));
        return true;
    case 0x0cac: /* (i32,i32,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3)));
        return true;
    case 0x0ccc: /* (i64,i32,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3)));
        return true;
    case 0x0d2c: /* (i32,i64,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], tci_slot_u32(stack, 3)));
        return true;
    case 0x0d4c: /* (i64,i64,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], tci_slot_u32(stack, 3)));
        return true;
    case 0x12ac: /* (i32,i32,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3]));
        return true;
    case 0x12cc: /* (i64,i32,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3]));
        return true;
    case 0x132c: /* (i32,i64,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), stack[3]));
        return true;
    case 0x134c: /* (i64,i64,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), stack[3]));
        return true;
    case 0x14ac: /* (i32,i32,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], stack[3]));
        return true;
    case 0x14cc: /* (i64,i32,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], stack[3]));
        return true;
    case 0x152c: /* (i32,i64,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], stack[3]));
        return true;
    case 0x154c: /* (i64,i64,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3]));
        return true;
    case 0x0ab4: /* (i32,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3));
        return true;
    case 0x0ad4: /* (i64,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3));
        return true;
    case 0x0b34: /* (i32,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3));
        return true;
    case 0x0b54: /* (i64,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3));
        return true;
    case 0x0cb4: /* (i32,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3));
        return true;
    case 0x0cd4: /* (i64,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3));
        return true;
    case 0x0d34: /* (i32,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], tci_slot_u32(stack, 3));
        return true;
    case 0x0d54: /* (i64,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], tci_slot_u32(stack, 3));
        return true;
    case 0x12b4: /* (i32,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3]);
        return true;
    case 0x12d4: /* (i64,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3]);
        return true;
    case 0x1334: /* (i32,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), stack[3]);
        return true;
    case 0x1354: /* (i64,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), stack[3]);
        return true;
    case 0x14b4: /* (i32,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], stack[3]);
        return true;
    case 0x14d4: /* (i64,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], stack[3]);
        return true;
    case 0x1534: /* (i32,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], stack[3]);
        return true;
    case 0x1554: /* (i64,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3]);
        return true;
    case 0x2aa5: /* (i32,i32,i32,i32,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2ac5: /* (i64,i32,i32,i32,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2b25: /* (i32,i64,i32,i32,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2b45: /* (i64,i64,i32,i32,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2ca5: /* (i32,i32,i64,i32,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2cc5: /* (i64,i32,i64,i32,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2d25: /* (i32,i64,i64,i32,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2d45: /* (i64,i64,i64,i32,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x32a5: /* (i32,i32,i32,i64,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x32c5: /* (i64,i32,i32,i64,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x3325: /* (i32,i64,i32,i64,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x3345: /* (i64,i64,i32,i64,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x34a5: /* (i32,i32,i64,i64,i32) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x34c5: /* (i64,i32,i64,i64,i32) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x3525: /* (i32,i64,i64,i64,i32) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x3545: /* (i64,i64,i64,i64,i32) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x4aa5: /* (i32,i32,i32,i32,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4ac5: /* (i64,i32,i32,i32,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4b25: /* (i32,i64,i32,i32,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4b45: /* (i64,i64,i32,i32,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4ca5: /* (i32,i32,i64,i32,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4cc5: /* (i64,i32,i64,i32,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4d25: /* (i32,i64,i64,i32,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4d45: /* (i64,i64,i64,i32,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], stack[2], tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x52a5: /* (i32,i32,i32,i64,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], stack[4]);
        return true;
    case 0x52c5: /* (i64,i32,i32,i64,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], stack[4]);
        return true;
    case 0x5325: /* (i32,i64,i32,i64,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), stack[3], stack[4]);
        return true;
    case 0x5345: /* (i64,i64,i32,i64,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), stack[3], stack[4]);
        return true;
    case 0x54a5: /* (i32,i32,i64,i64,i64) -> void */
        ((void (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], stack[3], stack[4]);
        return true;
    case 0x54c5: /* (i64,i32,i64,i64,i64) -> void */
        ((void (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], stack[3], stack[4]);
        return true;
    case 0x5525: /* (i32,i64,i64,i64,i64) -> void */
        ((void (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x5545: /* (i64,i64,i64,i64,i64) -> void */
        ((void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x2aad: /* (i32,i32,i32,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4)));
        return true;
    case 0x2acd: /* (i64,i32,i32,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4)));
        return true;
    case 0x2b2d: /* (i32,i64,i32,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4)));
        return true;
    case 0x2b4d: /* (i64,i64,i32,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4)));
        return true;
    case 0x2cad: /* (i32,i32,i64,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4)));
        return true;
    case 0x2ccd: /* (i64,i32,i64,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4)));
        return true;
    case 0x2d2d: /* (i32,i64,i64,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4)));
        return true;
    case 0x2d4d: /* (i64,i64,i64,i32,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4)));
        return true;
    case 0x32ad: /* (i32,i32,i32,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4)));
        return true;
    case 0x32cd: /* (i64,i32,i32,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4)));
        return true;
    case 0x332d: /* (i32,i64,i32,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4)));
        return true;
    case 0x334d: /* (i64,i64,i32,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4)));
        return true;
    case 0x34ad: /* (i32,i32,i64,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], stack[3], tci_slot_u32(stack, 4)));
        return true;
    case 0x34cd: /* (i64,i32,i64,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], stack[3], tci_slot_u32(stack, 4)));
        return true;
    case 0x352d: /* (i32,i64,i64,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], stack[3], tci_slot_u32(stack, 4)));
        return true;
    case 0x354d: /* (i64,i64,i64,i64,i32) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], stack[3], tci_slot_u32(stack, 4)));
        return true;
    case 0x4aad: /* (i32,i32,i32,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]));
        return true;
    case 0x4acd: /* (i64,i32,i32,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]));
        return true;
    case 0x4b2d: /* (i32,i64,i32,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]));
        return true;
    case 0x4b4d: /* (i64,i64,i32,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]));
        return true;
    case 0x4cad: /* (i32,i32,i64,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), stack[4]));
        return true;
    case 0x4ccd: /* (i64,i32,i64,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), stack[4]));
        return true;
    case 0x4d2d: /* (i32,i64,i64,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], tci_slot_u32(stack, 3), stack[4]));
        return true;
    case 0x4d4d: /* (i64,i64,i64,i32,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], stack[2], tci_slot_u32(stack, 3), stack[4]));
        return true;
    case 0x52ad: /* (i32,i32,i32,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], stack[4]));
        return true;
    case 0x52cd: /* (i64,i32,i32,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], stack[4]));
        return true;
    case 0x532d: /* (i32,i64,i32,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), stack[3], stack[4]));
        return true;
    case 0x534d: /* (i64,i64,i32,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), stack[3], stack[4]));
        return true;
    case 0x54ad: /* (i32,i32,i64,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], stack[3], stack[4]));
        return true;
    case 0x54cd: /* (i64,i32,i64,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], stack[3], stack[4]));
        return true;
    case 0x552d: /* (i32,i64,i64,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], stack[3], stack[4]));
        return true;
    case 0x554d: /* (i64,i64,i64,i64,i64) -> u32 */
        tci_slot_set_u32(stack, 0, ((uint32_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3], stack[4]));
        return true;
    case 0x2ab5: /* (i32,i32,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2ad5: /* (i64,i32,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2b35: /* (i32,i64,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2b55: /* (i64,i64,i32,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2cb5: /* (i32,i32,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2cd5: /* (i64,i32,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2d35: /* (i32,i64,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x2d55: /* (i64,i64,i64,i32,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t))func)(stack[0], stack[1], stack[2], tci_slot_u32(stack, 3), tci_slot_u32(stack, 4));
        return true;
    case 0x32b5: /* (i32,i32,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x32d5: /* (i64,i32,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x3335: /* (i32,i64,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x3355: /* (i64,i64,i32,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint32_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x34b5: /* (i32,i32,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x34d5: /* (i64,i32,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint32_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x3535: /* (i32,i64,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint32_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x3555: /* (i64,i64,i64,i64,i32) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t))func)(stack[0], stack[1], stack[2], stack[3], tci_slot_u32(stack, 4));
        return true;
    case 0x4ab5: /* (i32,i32,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4ad5: /* (i64,i32,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4b35: /* (i32,i64,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4b55: /* (i64,i64,i32,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4cb5: /* (i32,i32,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4cd5: /* (i64,i32,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint32_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4d35: /* (i32,i64,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint32_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x4d55: /* (i64,i64,i64,i32,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t))func)(stack[0], stack[1], stack[2], tci_slot_u32(stack, 3), stack[4]);
        return true;
    case 0x52b5: /* (i32,i32,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], stack[4]);
        return true;
    case 0x52d5: /* (i64,i32,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint64_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), tci_slot_u32(stack, 2), stack[3], stack[4]);
        return true;
    case 0x5335: /* (i32,i64,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint32_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], tci_slot_u32(stack, 2), stack[3], stack[4]);
        return true;
    case 0x5355: /* (i64,i64,i32,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, uint64_t, uint64_t))func)(stack[0], stack[1], tci_slot_u32(stack, 2), stack[3], stack[4]);
        return true;
    case 0x54b5: /* (i32,i32,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), tci_slot_u32(stack, 1), stack[2], stack[3], stack[4]);
        return true;
    case 0x54d5: /* (i64,i32,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint32_t, uint64_t, uint64_t, uint64_t))func)(stack[0], tci_slot_u32(stack, 1), stack[2], stack[3], stack[4]);
        return true;
    case 0x5535: /* (i32,i64,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t))func)(tci_slot_u32(stack, 0), stack[1], stack[2], stack[3], stack[4]);
        return true;
    case 0x5555: /* (i64,i64,i64,i64,i64) -> u64 */
        stack[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))func)(stack[0], stack[1], stack[2], stack[3], stack[4]);
        return true;
    default:
        return false;
    }
}
