/*
 * How many guest pages one TranslationBlock's code may occupy: an entry
 * page and a linear crossing into the next one.  Everything that tracks a
 * TB's pages is written for N of them, so this is the one place to change.
 *
 * Its own header because both translation-block.h (the page lists a TB is
 * linked into) and translator.h (the host pointer cached per page) need it,
 * and the former uses target-poisoned identifiers that the latter must not
 * pull in.
 *
 * A third page was built and measured (round 42) and is not worth it.  The
 * wasm64 backend translates a direct call's callee into its caller's TB, so
 * a TB calling two callees on two different pages is refused its second
 * one -- 9 808 exits per Mi on the video workload, 11 % of every TB
 * boundary.  Granting it removes all of them and moves the *total* boundary
 * count by 0.87 %, because an absorbed call relocates its caller's boundary
 * rather than deleting it; the exits come back as xwBx and xwDefer, the mix
 * shifts from chained to indirect, and translation work doubles.  No
 * measurable clock change on video either way.
 *
 * Nor is the page-set census (target/arm/tcg/translate.c w64_pgset) a bound
 * on this number: at two pages it says no TB ever asks for a fourth, and at
 * three, 4 740 per Mi do.  It can only count the pages a TB reaches, and
 * granting one lets the TB run far enough to want the next -- so it prices
 * the slot after this one and nothing beyond it.
 *
 * TB_PAGE_TAG is the mask the PageDesc lists tag each link with, so
 * TB_PAGES-1 must fit in it, and it must fit in CODE_GEN_ALIGN's low bits.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef EXEC_TB_PAGES_H
#define EXEC_TB_PAGES_H

#define TB_PAGES 2
#define TB_PAGE_TAG 3

#endif
