/*
 * ARM9E-S streaming memory traffic of a loop
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "arm9e-cycles.h"

/* Accesses through one base register in one loop iteration. */
typedef struct ARM9EBase {
    int32_t stride;
    bool is_stride_unknown;
    uint32_t loads;
    uint32_t stores;
    bool has_load_multiple;
    bool has_store_multiple;
} ARM9EBase;

typedef struct ARM9ELoop {
    ARM9EBase bases[16];
    bool has_unknown_flow;
} ARM9ELoop;

static void arm9e_loop_access(ARM9ELoop *loop, uint32_t base, bool is_load,
                              bool is_multiple)
{
    ARM9EBase *b = &loop->bases[base];

    if (is_load) {
        b->loads++;
        b->has_load_multiple |= is_multiple;
    } else {
        b->stores++;
        b->has_store_multiple |= is_multiple;
    }
}

static void arm9e_loop_advance(ARM9ELoop *loop, uint32_t base, int32_t bytes)
{
    loop->bases[base].stride += bytes;
}

/*
 * Cycles of a loop body instruction when the loop repeats: the branch that
 * closes the loop is taken, conditional branches inside the body are not.
 */
static uint32_t arm9e_loop_insn_cycles(uint32_t insn, bool is_thumb,
                                       bool is_closing)
{
    bool is_conditional_branch;

    if (is_thumb) {
        is_conditional_branch = (insn & 0xf000) == 0xd000 &&
                                extract32(insn, 8, 4) < 0xe;
        if (is_conditional_branch) {
            return is_closing ? 3 : 1;
        }
        return arm9e_thumb_cycles(insn);
    }
    is_conditional_branch = (insn & 0x0f000000) == 0x0a000000 &&
                            (insn >> 28) < 0xe;
    if (is_conditional_branch && !is_closing) {
        return 1;
    }
    return arm9e_a32_cycles(insn);
}

static void arm9e_a32_note(ARM9ELoop *loop, uint32_t insn)
{
    uint32_t rn = extract32(insn, 16, 4);
    uint32_t rd = extract32(insn, 12, 4);
    bool is_load = insn & (1 << 20);
    bool is_up = insn & (1 << 23);
    bool is_writeback = !(insn & (1 << 24)) || (insn & (1 << 21));

    if ((insn >> 28) == 0xf) {
        loop->has_unknown_flow |= (insn & 0x0e000000) == 0x0a000000;
        return;
    }
    if ((insn & 0x0f000000) == 0x0b000000 ||
        (insn & 0x0f000000) == 0x0f000000 ||
        (insn & 0x0ffffff0) == 0x012fff10 ||
        (insn & 0x0ffffff0) == 0x012fff30) {
        loop->has_unknown_flow = true;
        return;
    }
    if ((insn & 0x0e000000) == 0x08000000) {
        uint32_t list = extract32(insn, 0, 16);

        if (is_load && (list & (1 << 15))) {
            loop->has_unknown_flow = true;
            return;
        }
        arm9e_loop_access(loop, rn, is_load, true);
        if (insn & (1 << 21)) {
            arm9e_loop_advance(loop, rn, (is_up ? 4 : -4) * ctpop16(list));
        }
        return;
    }
    if ((insn & 0x0c000000) == 0x04000000) {
        int32_t offset = extract32(insn, 0, 12);

        if (is_load && rd == 15) {
            loop->has_unknown_flow = true;
            return;
        }
        arm9e_loop_access(loop, rn, is_load, false);
        if (!is_writeback) {
            return;
        }
        if (insn & (1 << 25)) {
            loop->bases[rn].is_stride_unknown = true;
        } else {
            arm9e_loop_advance(loop, rn, is_up ? offset : -offset);
        }
        return;
    }
    if ((insn & 0x0e000090) == 0x00000090 && (insn & 0x60)) {
        int32_t offset = (extract32(insn, 8, 4) << 4) | extract32(insn, 0, 4);
        bool is_ldrd = !is_load && (insn & 0x60) == 0x40;

        arm9e_loop_access(loop, rn, is_load || is_ldrd, false);
        if (!is_writeback) {
            return;
        }
        if (insn & (1 << 22)) {
            arm9e_loop_advance(loop, rn, is_up ? offset : -offset);
        } else {
            loop->bases[rn].is_stride_unknown = true;
        }
        return;
    }
    if ((insn & 0x0c000000) == 0) {
        uint32_t opcode = extract32(insn, 21, 4);
        bool is_test = opcode >= 8 && opcode <= 11;
        bool is_add_imm = (insn & 0x0fe00000) == 0x02800000;
        bool is_sub_imm = (insn & 0x0fe00000) == 0x02400000;

        if (!is_test && rd == 15) {
            loop->has_unknown_flow = true;
            return;
        }
        if ((is_add_imm || is_sub_imm) && rd == rn) {
            int32_t imm = ror32(extract32(insn, 0, 8),
                                2 * extract32(insn, 8, 4));

            arm9e_loop_advance(loop, rn, is_add_imm ? imm : -imm);
        }
    }
}

static void arm9e_thumb_note(ARM9ELoop *loop, uint32_t insn)
{
    uint32_t r0 = extract32(insn, 0, 3);
    uint32_t r3 = extract32(insn, 3, 3);
    uint32_t r8 = extract32(insn, 8, 3);
    bool is_load = insn & (1 << 11);

    switch (insn >> 11) {
    case 0x03:
        if ((insn & (1 << 10)) && r0 == r3) {
            int32_t imm = extract32(insn, 6, 3);

            arm9e_loop_advance(loop, r0, (insn & (1 << 9)) ? -imm : imm);
        }
        break;
    case 0x06:
        arm9e_loop_advance(loop, r8, extract32(insn, 0, 8));
        break;
    case 0x07:
        arm9e_loop_advance(loop, r8, -(int32_t) extract32(insn, 0, 8));
        break;
    case 0x08: {
        uint32_t rd = r0 | (extract32(insn, 7, 1) << 3);

        if ((insn & 0xff00) == 0x4700 ||
            ((insn & 0xfc00) == 0x4400 && (insn & 0x0300) != 0x0100 &&
             rd == 15)) {
            loop->has_unknown_flow = true;
        }
        break;
    }
    case 0x0a ... 0x0b:
        arm9e_loop_access(loop, r3, extract32(insn, 9, 3) >= 3, false);
        break;
    case 0x0c ... 0x11:
        arm9e_loop_access(loop, r3, is_load, false);
        break;
    case 0x17:
        if ((insn & 0xff00) == 0xbd00) {
            loop->has_unknown_flow = true;
        }
        break;
    case 0x18 ... 0x19: {
        uint32_t list = extract32(insn, 0, 8);

        arm9e_loop_access(loop, r8, is_load, true);
        if (!is_load || !(list & (1 << r8))) {
            arm9e_loop_advance(loop, r8, 4 * ctpop8(list));
        }
        break;
    }
    case 0x1b:
        if ((insn & 0xff00) == 0xdf00) {
            loop->has_unknown_flow = true;
        }
        break;
    case 0x1d ... 0x1f:
        loop->has_unknown_flow = true;
        break;
    }
}

/*
 * A base register that advances each iteration streams through memory: its
 * loads fill a cache line per line it advances and its stores drain through
 * the write buffer. A load that is alone in its line waits for the whole
 * line, loads that share a line wait for its first word. Stores through a
 * base the loop also loads from hit the lines those loads fill. The bus
 * serves fills and drains in turn, so the iteration takes the longer of the
 * stalled instructions and the bus.
 */
uint32_t arm9e_loop_memory_cycles(const uint32_t *insns, size_t count,
                                  bool is_thumb,
                                  const TCGMemoryTiming *timing)
{
    uint32_t line_bytes = timing->line_bytes;
    uint32_t line_fill = timing->read_first_mcycles +
                         (line_bytes / 4 - 1) * timing->read_word_mcycles;
    ARM9ELoop loop = {};
    uint32_t late = 0;
    uint64_t compute = 0;
    uint64_t read_stall = 0;
    uint64_t read_bus = 0;
    uint64_t read_bytes = 0;
    uint64_t write_bus = 0;

    for (size_t i = 0; i < count; i++) {
        compute += arm9e_loop_insn_cycles(insns[i], is_thumb, i == count - 1);
        if (is_thumb) {
            compute += arm9e_thumb_stall(&late, insns[i]);
            arm9e_thumb_note(&loop, insns[i]);
        } else {
            compute += arm9e_a32_stall(&late, insns[i]);
            arm9e_a32_note(&loop, insns[i]);
        }
    }
    if (loop.has_unknown_flow) {
        return 0;
    }

    for (uint32_t r = 0; r < 15; r++) {
        const ARM9EBase *b = &loop.bases[r];
        uint64_t stride = abs(b->stride);

        if (r == 13 || b->is_stride_unknown || !stride) {
            continue;
        }
        if (b->loads) {
            uint64_t bytes = MIN(stride, line_bytes * b->loads);
            bool is_line_per_load = b->has_load_multiple ||
                                    bytes == line_bytes * b->loads;
            uint32_t stall = is_line_per_load ? line_fill :
                             timing->read_first_mcycles;

            read_stall += stall * bytes / line_bytes;
            read_bus += line_fill * bytes / line_bytes;
            read_bytes += bytes;
        } else if (b->has_store_multiple) {
            uint64_t bytes = MIN(stride, line_bytes * b->stores);

            write_bus += timing->write_line_mcycles * bytes / line_bytes;
        } else if (b->stores) {
            write_bus += timing->write_mcycles * b->stores;
        }
    }
    if (read_bytes && write_bus) {
        read_bus += timing->turnaround_mcycles * read_bytes / line_bytes;
    }

    compute *= 1000;
    return (MAX(compute + read_stall, read_bus + write_bus) - compute + 500) /
           1000;
}
