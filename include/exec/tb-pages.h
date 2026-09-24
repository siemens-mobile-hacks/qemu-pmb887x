/*
 * How many guest pages one TranslationBlock's code may occupy: an entry
 * page and one more, either the linear crossing into the next page or
 * (wasm64) the page of an inlined callee.
 *
 * Its own header because both translation-block.h (the page lists a TB is
 * linked into) and translator.h (the host pointer cached per page) need it,
 * and the former uses target-poisoned identifiers that the latter must not
 * pull in.
 *
 * A third page was measured and is not worth it: an absorbed call
 * relocates its caller's boundary rather than deleting it.
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
