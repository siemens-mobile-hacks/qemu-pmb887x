/*
 * wasm64 interpreter tier: the recorded op stream.
 *
 * A wasm module costs ~86 us to close and carries only 4.85 TBs, because
 * the batch closes the moment one of its members must run.  96 % of the
 * boot's module time is that fixed per-close cost (playbook § 0f).  The
 * only way to stop closing is to have another way to run a TB before its
 * module exists, and this is it: the backend records what it emitted, in
 * a form a C interpreter in the main module can execute.
 *
 * The record is written as the wasm is written, from the same
 * already-register-allocated operands, so the two are the same program
 * by construction.  Anything the recorder cannot encode marks the TB
 * un-interpretable and it takes today's path -- correctness never
 * depends on the recorder being complete.
 *
 * Encoding: a stream of uint32_t.  The first word of every op is
 *
 *     [op:8][a0:8][a1:8][a2:8]
 *
 * and ops that need more read a fixed number of following words, known
 * from op alone.  A 64-bit immediate is two words, low first.  Register
 * numbers are TCG target registers (0..31), which is why TCI's own
 * bytecode cannot be reused -- it packs them into 4 bits.
 *
 * The op set is exactly what tcg/wasm64/tcg-target.c.inc emits: the
 * carry-arithmetic family, divs2/divu2, negsetcond, extract2 and the
 * 128-bit qemu_ld2/st2 are all C_NotImplemented there and so never
 * appear here either.
 */
#ifndef W64_INTERP_H
#define W64_INTERP_H

/*
 * Binary and unary operations are laid out arithmetically so that the
 * ~100 (operation, type, operand-form) combinations cost a handful of
 * case ranges in the interpreter instead of an enum written out by hand.
 */
enum {
    WI_ALU_ADD, WI_ALU_SUB, WI_ALU_MUL, WI_ALU_AND, WI_ALU_OR, WI_ALU_XOR,
    WI_ALU_ANDC, WI_ALU_ORC, WI_ALU_EQV, WI_ALU_NAND, WI_ALU_NOR,
    WI_ALU_SHL, WI_ALU_SHR, WI_ALU_SAR, WI_ALU_ROTL, WI_ALU_ROTR,
    WI_ALU_DIVS, WI_ALU_DIVU, WI_ALU_REMS, WI_ALU_REMU,
    WI_ALU_CLZ, WI_ALU_CTZ, WI_ALU_MULUH, WI_ALU_MULSH,
    WI_ALU_N
};

enum {
    WI_UN_NEG, WI_UN_NOT, WI_UN_CTPOP,
    WI_UN_EXT8S, WI_UN_EXT8U, WI_UN_EXT16S, WI_UN_EXT16U,
    WI_UN_EXT32S, WI_UN_EXT32U,
    WI_UN_EXTRL, WI_UN_EXTRH,
    WI_UN_N
};

#define WI_ALU_STRIDE  32
#define WI_UN_STRIDE   16

#define WI_OP_END        0x00

#define WI_ALU32_RRR     0x01                               /* .. 0x18 */
#define WI_ALU64_RRR     (WI_ALU32_RRR + WI_ALU_STRIDE)     /* 0x21 */
#define WI_ALU32_RRI     (WI_ALU64_RRR + WI_ALU_STRIDE)     /* 0x41 */
#define WI_ALU64_RRI     (WI_ALU32_RRI + WI_ALU_STRIDE)     /* 0x61 */
#define WI_UN32          (WI_ALU64_RRI + WI_ALU_STRIDE)     /* 0x81 */
#define WI_UN64          (WI_UN32 + WI_UN_STRIDE)           /* 0x91 */

#define WI_FIRST_MISC    (WI_UN64 + WI_UN_STRIDE)           /* 0xa1 */
enum {
    WI_MOV32 = WI_FIRST_MISC,
    WI_MOV64,
    WI_MOVI32,          /* a0;          +1: imm32                       */
    WI_MOVI64,          /* a0;          +2: imm64                       */
    WI_SUBIR32,         /* a0 = imm - a2; +2: imm64                     */
    WI_SUBIR64,

    WI_LD8U,            /* a0 = dst, a1 = base; +1: ofs (signed)        */
    WI_LD8S32, WI_LD8S64,
    WI_LD16U, WI_LD16S32, WI_LD16S64,
    WI_LD32U, WI_LD32S64,
    WI_LD32, WI_LD64,
    WI_ST8,             /* a0 = val, a1 = base; +1: ofs                */
    WI_ST16, WI_ST32, WI_ST64,
    WI_STI32,           /* a1 = base;   +1: ofs, +2: imm64             */
    WI_STI64,

    WI_BSWAP16,         /* a0, a1, a2 = is64; +1: flags                */
    WI_BSWAP32,
    WI_BSWAP64,

    WI_SETCOND32,       /* a0, a1, a2;  +1: cond                       */
    WI_SETCOND64,
    WI_SETCONDI32,      /* a0, a1;      +1: cond, +2: imm64            */
    WI_SETCONDI64,
    WI_MOVCOND32,       /* a0 = ret, a1 = c1, a2 = c2;  +1: cond,
                         * +1: [f:8][vt:8][vf:8] where f bit 0 = c2 is
                         * an immediate, 1 = vt is, 2 = vf is; then the
                         * immediates present, two words each, in the
                         * order c2, vt, vf                            */
    WI_MOVCOND64,
    WI_BRCOND32,        /* a1, a2;      +1: cond, +1: target           */
    WI_BRCOND64,
    WI_BRCONDI32,       /* a1;          +1: cond, +2: imm64, +1: target */
    WI_BRCONDI64,

    WI_DEPOSIT32,       /* a0, a1, a2;  +1: [len:8][ofs:8]             */
    WI_DEPOSIT64,
    WI_EXTRACT32,       /* a0, a1;      +1: [len:8][ofs:8]             */
    WI_EXTRACT64,
    WI_SEXTRACT32,
    WI_SEXTRACT64,

    WI_MULU2_32,        /* a0 = lo, a1 = hi, a2 = x;  +1: y            */
    WI_MULU2_64,
    WI_MULS2_32,
    WI_MULS2_64,

    WI_QEMU_LD32,       /* a0 = dst, a1 = addr; +1: oi, +2: retaddr    */
    WI_QEMU_LD64,
    WI_QEMU_ST32,       /* a0 = val, a1 = addr; +1: oi, +2: retaddr    */
    WI_QEMU_ST64,

    WI_CALL,            /* +1: tag, +2: target, +2: retaddr            */
    WI_BR,              /* +1: target word index                       */
    WI_GOTO_TB,         /* a0 = which                                  */
    WI_EXIT_TB,         /* +1: arg                                     */
    WI_GOTO_PTR,        /* a0                                          */
    WI_MB,
    WI_OP_N
};

#define WI_W0(op, a0, a1, a2) \
    ((uint32_t)(op) | ((uint32_t)(a0) << 8) | \
     ((uint32_t)(a1) << 16) | ((uint32_t)(a2) << 24))

/*
 * One recorded TB.  `n` counts uint32_t words; `code` is the stream.
 * Branch targets are word offsets into it, resolved at finalize.
 */
struct w64_irec {
    uint32_t *code;
    uint32_t n;
    uint32_t hits;                      /* entries served interpreted */
};

/* the record store, keyed by tidx (tcg/wasm64/w64-interp.c) */
void w64_irec_put(uint32_t tidx, uint32_t *code, uint32_t n);
void w64_irec_drop(uint32_t tidx);
void w64_irec_flush(void);
bool w64_interp_on(void);

/*
 * Dispatcher gate, read on every TB entry: 0 off, 1 interpret only while
 * the TB has no module (the production tier), 2 interpret always
 * (W64_INTERP_ALL=1 -- full coverage for the correctness gate, and the
 * way the tier's own speed is priced).
 */
void w64_interp_init(void);
uint32_t w64_call_tag(uint32_t typemask, unsigned nargs);

/*
 * Run TB `tidx` interpreted if it has a record and has not yet earned a
 * module (W64_INTERP=T promotes after T entries).  Returns false if the
 * caller must take the compiled path; otherwise *res is the exit word
 * the emitted TB would have returned.
 */
bool w64_interp_try(uint32_t tidx, uint32_t icount, uintptr_t env,
                    uintptr_t sp, uintptr_t tp, uint32_t *res);

#endif /* W64_INTERP_H */
