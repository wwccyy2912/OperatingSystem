/*
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details: <https://www.gnu.org/licenses/>.
 *
 * ime.h - Minimal pinyin input-method engine (library form)
 * Copyright (c) 2026 OpSys Project
 *
 * The keyboard service emits only ASCII (scancode -> US-ASCII); there
 * is no way to type Chinese directly.  This engine gives a caller
 * (the shell's line editor, or a TUI input line) a pinyin lookup: the
 * user types lowercase ASCII letters (the composition), the caller
 * queries candidates on space/digit, and commits the chosen hanzi as
 * UTF-8 bytes into its line buffer.  Candidates are limited to code
 * points that exist in font_cjk.h, so every committed character is
 * renderable by term/gui.
 */

#ifndef USER_LIB_LIBIME_IME_H
#define USER_LIB_LIBIME_IME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Longest composition (pinyin) the caller should accumulate. */
#define IME_MAX_PINYIN 16

/* Max candidates returned for one pinyin. */
#define IME_MAX_CAND 20

/*
 * Look up the candidates for a lowercase-ASCII pinyin (NUL-terminated,
 * no tones).  Fills chars_out[0..n-1] with pointers into a static
 * table; each points at one UTF-8 hanzi (3 bytes + NUL).  Returns the
 * number of candidates (0 when the pinyin has no match).
 */
int ImeLookup(const char *pinyin, const char **chars_out /* IME_MAX_CAND */);

/*
 * True when `pinyin` is a PREFIX of at least one table entry (i.e. it
 * could still grow into a valid syllable).  The line editor uses this
 * to decide whether to keep accumulating letters into the composition.
 */
int ImePrefix(const char *pinyin);

#ifdef __cplusplus
}
#endif

#endif /* USER_LIB_LIBIME_IME_H */
