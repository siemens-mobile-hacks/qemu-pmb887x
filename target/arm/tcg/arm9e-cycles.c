/*
 * ARM9E-S instruction timing, measured on EL71 (bsp/unit cpu-timing)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "arm9e-cycles.h"

uint32_t arm9e_a32_cycles(uint32_t insn)
{
    uint32_t registers;
    uint32_t cycles;

    if ((insn >> 28) == 0xf) {
        /* Unconditional space: only BLX immediate refills the pipeline. */
        return (insn & 0x0e000000) == 0x0a000000 ? 3 : 1;
    }

    /* B, BL, BX, BLX, SWI and undefined refill the pipeline. */
    if ((insn & 0x0e000000) == 0x0a000000 ||
        (insn & 0x0ffffff0) == 0x012fff10 ||
        (insn & 0x0ffffff0) == 0x012fff20 ||
        (insn & 0x0ffffff0) == 0x012fff30 ||
        (insn & 0x0f000000) == 0x0f000000) {
        return 3;
    }

    if ((insn & 0x0e000000) == 0x08000000) {
        registers = ctpop16(insn & 0xffff);
        if ((insn & (1 << 20)) && (insn & (1 << 15))) {
            return registers + 4;
        }
        return MAX(registers, 2);
    }

    if ((insn & 0x0f8000f0) == 0x00800090) {
        return (insn & (1 << 20)) ? 5 : 3;
    }
    if ((insn & 0x0fc000f0) == 0x00000090) {
        return (insn & (1 << 20)) ? 4 : 2;
    }
    if ((insn & 0x0fb00ff0) == 0x01000090) {
        return 2;
    }
    if ((insn & 0x0fbf0fff) == 0x010f0000) {
        return 2;
    }
    if ((insn & 0x0fb0fff0) == 0x0120f000 ||
        (insn & 0x0fb0f000) == 0x0320f000) {
        return (extract32(insn, 16, 4) & 0x7) ? 3 : 1;
    }

    /* LDRD and STRD. */
    if ((insn & 0x0e1000d0) == 0x000000d0) {
        return 2;
    }
    if ((insn & 0x0e000090) == 0x00000090) {
        return ((insn & (1 << 20)) && extract32(insn, 12, 4) == 15) ? 5 : 1;
    }

    if ((insn & 0x0c000000) == 0x04000000) {
        cycles = ((insn & (1 << 25)) && extract32(insn, 5, 7)) ? 2 : 1;
        if ((insn & (1 << 20)) && extract32(insn, 12, 4) == 15) {
            cycles += 4;
        }
        return cycles;
    }

    /* CLZ, saturating arithmetic and halfword multiplies. */
    if ((insn & 0x0f900000) == 0x01000000) {
        return 1;
    }

    if ((insn & 0x0c000000) == 0) {
        uint32_t opcode = extract32(insn, 21, 4);
        bool is_test = opcode >= 8 && opcode <= 11;

        cycles = (!is_test && extract32(insn, 12, 4) == 15) ? 3 : 1;
        if (!(insn & (1 << 25)) && (insn & (1 << 4)) && !(insn & (1 << 7))) {
            cycles++;
        }
        return cycles;
    }

    return 1;
}

uint32_t arm9e_thumb_cycles(uint32_t insn)
{
    uint32_t registers;

    if (insn > 0xffff) {
        /* QEMU combines the two halves of BL/BLX into one instruction. */
        return (insn & 0xf800e800) == 0xf000e800 ? 4 : 1;
    }

    /* B, BX, BLX, undefined, SWI and a BL/BLX suffix split from its prefix. */
    if ((insn & 0xf800) == 0xe000 || (insn & 0xf800) == 0xe800 ||
        (insn & 0xf800) == 0xf800 ||
        (insn & 0xff87) == 0x4700 || (insn & 0xff87) == 0x4780 ||
        (insn & 0xff00) == 0xde00 || (insn & 0xff00) == 0xdf00) {
        return 3;
    }

    /* MULS always updates the flags. */
    if ((insn & 0xffc0) == 0x4340) {
        return 4;
    }

    if ((insn & 0xf000) == 0xc000) {
        return MAX(ctpop8(insn & 0xff), 2);
    }
    if ((insn & 0xfe00) == 0xb400 || (insn & 0xfe00) == 0xbc00) {
        registers = ctpop8(insn & 0xff) + extract32(insn, 8, 1);
        if ((insn & 0xfe00) == 0xbc00 && (insn & (1 << 8))) {
            return registers + 4;
        }
        return MAX(registers, 2);
    }
    return 1;
}

/*
 * Store multiple reads each register when it stores it: only the first one
 * can stall.
 */
static uint32_t arm9e_first_stored(uint32_t list)
{
    return list & -list;
}

/*
 * Registers an instruction reads early enough to stall on a late result,
 * store data included.
 */
static uint32_t arm9e_a32_sources(uint32_t insn)
{
    uint32_t rn = 1u << extract32(insn, 16, 4);
    uint32_t rd = 1u << extract32(insn, 12, 4);
    uint32_t rs = 1u << extract32(insn, 8, 4);
    uint32_t rm = 1u << extract32(insn, 0, 4);
    uint32_t sources;

    if ((insn >> 28) == 0xf || (insn & 0x0e000000) == 0x0a000000) {
        return 0;
    }
    if ((insn & 0x0ffffff0) == 0x012fff10 ||
        (insn & 0x0ffffff0) == 0x012fff30) {
        return rm;
    }
    if ((insn & 0x0e000000) == 0x08000000) {
        return rn | ((insn & (1 << 20)) ? 0 :
                     arm9e_first_stored(extract32(insn, 0, 16)));
    }
    /* Multiply accumulators are read late. */
    if ((insn & 0x0f8000f0) == 0x00800090 ||
        (insn & 0x0fc000f0) == 0x00000090) {
        return rm | rs;
    }
    if ((insn & 0x0fb00ff0) == 0x01000090) {
        return rn | rm;
    }
    if ((insn & 0x0fb0fff0) == 0x0120f000 ||
        (insn & 0x0fff0ff0) == 0x016f0f10) {
        return rm;
    }
    if ((insn & 0x0f9000f0) == 0x01000050) {
        return rn | rm;
    }
    if ((insn & 0x0f900090) == 0x01000080) {
        return rm | rs;
    }
    if (((insn & 0x0f900000) == 0x01000000 && !(insn & 0x90)) ||
        (insn & 0x0fb0f000) == 0x0320f000) {
        return 0;
    }
    if ((insn & 0x0e000090) == 0x00000090) {
        sources = rn | ((insn & (1 << 22)) ? 0 : rm);
        if ((insn & (1 << 20)) || (insn & 0x60) == 0x40) {
            return sources;
        }
        return sources | rd | ((insn & 0x60) == 0x60 ? (rd << 1) : 0);
    }
    if ((insn & 0x0c000000) == 0x04000000) {
        return rn | ((insn & (1 << 25)) ? rm : 0) |
               ((insn & (1 << 20)) ? 0 : rd);
    }
    if ((insn & 0x0c000000) == 0) {
        uint32_t opcode = extract32(insn, 21, 4);

        sources = (opcode == 13 || opcode == 15) ? 0 : rn;
        if (!(insn & (1 << 25))) {
            sources |= rm | ((insn & (1 << 4)) ? rs : 0);
        }
        return sources;
    }
    if ((insn & 0x0f100010) == 0x0e000010) {
        return rd;
    }
    return 0;
}

/*
 * Registers an instruction writes late: a word load (the last register of
 * LDM) or a multiply for one cycle, a byte or halfword load for two.
 */
static uint32_t arm9e_a32_late(uint32_t insn)
{
    uint32_t rd = extract32(insn, 12, 4);

    if ((insn >> 28) == 0xf) {
        return 0;
    }
    if ((insn & 0x0e100000) == 0x08100000) {
        uint32_t list = extract32(insn, 0, 16);

        return (list && !(list & (1 << 15))) ? (1u << (31 - clz32(list))) : 0;
    }
    if ((insn & 0x0f8000f0) == 0x00800090 ||
        (insn & 0x0fc000f0) == 0x00000090) {
        return 1u << extract32(insn, 16, 4);
    }
    if ((insn & 0x0e000090) == 0x00000090 && (insn & 0x60)) {
        if (insn & (1 << 20)) {
            return rd != 15 ? (1u << (rd + 16)) : 0;
        }
        return ((insn & 0x60) == 0x40 && rd < 14) ? (1u << (rd + 1)) : 0;
    }
    if ((insn & 0x0c100000) == 0x04100000 && rd != 15) {
        return (insn & (1 << 22)) ? (1u << (rd + 16)) : (1u << rd);
    }
    return 0;
}

static uint32_t arm9e_thumb_sources(uint32_t insn)
{
    uint32_t r0 = 1u << extract32(insn, 0, 3);
    uint32_t r3 = 1u << extract32(insn, 3, 3);
    uint32_t r6 = 1u << extract32(insn, 6, 3);
    uint32_t r8 = 1u << extract32(insn, 8, 3);
    uint32_t sp = 1u << 13;

    if (insn > 0xffff) {
        return 0;
    }
    switch (insn >> 11) {
    case 0x00 ... 0x02:
        return r3;
    case 0x03:
        return r3 | ((insn & (1 << 10)) ? 0 : r6);
    case 0x05 ... 0x07:
        return r8;
    case 0x08:
        if (!(insn & (1 << 10))) {
            return r0 | r3;
        }
        if (extract32(insn, 8, 2) >= 2) {
            return 1u << extract32(insn, 3, 4);
        }
        return (1u << (extract32(insn, 0, 3) | (extract32(insn, 7, 1) << 3))) |
               (1u << extract32(insn, 3, 4));
    case 0x0a ... 0x0b:
        return r3 | r6 | (extract32(insn, 9, 3) < 3 ? r0 : 0);
    case 0x0c ... 0x11:
        return r3 | ((insn & (1 << 11)) ? 0 : r0);
    case 0x12 ... 0x13:
        return sp | ((insn & (1 << 11)) ? 0 : r8);
    case 0x14 ... 0x15:
        return (insn & (1 << 11)) ? sp : 0;
    case 0x16 ... 0x17:
        if ((insn & 0xfe00) == 0xb400) {
            uint32_t pushed = extract32(insn, 0, 8) |
                              ((insn & (1 << 8)) ? (1u << 14) : 0);

            return sp | arm9e_first_stored(pushed);
        }
        return sp;
    case 0x18 ... 0x19:
        return r8 | ((insn & (1 << 11)) ? 0 :
                     arm9e_first_stored(extract32(insn, 0, 8)));
    default:
        return 0;
    }
}

static uint32_t arm9e_thumb_late(uint32_t insn)
{
    uint32_t r0 = extract32(insn, 0, 3);

    if (insn > 0xffff) {
        return 0;
    }
    if ((insn & 0xf000) == 0x5000) {
        uint32_t op = extract32(insn, 9, 3);

        if (op < 3) {
            return 0;
        }
        return op == 4 ? (1u << r0) : (1u << (r0 + 16));
    }
    if ((insn & 0xf800) == 0x6800 || (insn & 0xffc0) == 0x4340) {
        return 1u << r0;
    }
    if ((insn & 0xf800) == 0x7800 || (insn & 0xf800) == 0x8800) {
        return 1u << (r0 + 16);
    }
    if ((insn & 0xf800) == 0x4800 || (insn & 0xf800) == 0x9800) {
        return 1u << extract32(insn, 8, 3);
    }
    if ((insn & 0xff00) == 0xbc00 || (insn & 0xf800) == 0xc800) {
        uint32_t list = extract32(insn, 0, 8);

        return list ? (1u << (31 - clz32(list))) : 0;
    }
    return 0;
}

static uint32_t arm9e_stall(uint32_t *late, uint32_t sources, uint32_t result)
{
    uint32_t stall = 0;

    if (sources & (*late >> 16)) {
        stall = 2;
    } else if (sources & *late) {
        stall = 1;
    }
    *late = (stall ? 0 : (*late >> 16)) | result;
    return stall;
}

uint32_t arm9e_a32_stall(uint32_t *late, uint32_t insn)
{
    return arm9e_stall(late, arm9e_a32_sources(insn), arm9e_a32_late(insn));
}

uint32_t arm9e_thumb_stall(uint32_t *late, uint32_t insn)
{
    return arm9e_stall(late, arm9e_thumb_sources(insn), arm9e_thumb_late(insn));
}
