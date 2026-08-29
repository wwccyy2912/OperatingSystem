/*
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
int ime_lookup(const char *pinyin, const char **chars_out /* IME_MAX_CAND */);

/*
 * True when `pinyin` is a PREFIX of at least one table entry (i.e. it
 * could still grow into a valid syllable).  The line editor uses this
 * to decide whether to keep accumulating letters into the composition.
 */
int ime_prefix(const char *pinyin);

#ifdef __cplusplus
}
#endif

#endif /* USER_LIB_LIBIME_IME_H */
