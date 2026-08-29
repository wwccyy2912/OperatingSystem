/*
 * utf8.h - Minimal UTF-8 helpers for byte-string text processing
 * Copyright (c) 2026 OpSys Project
 *
 * The whole system moves UTF-8 bytes around (shell lines, filenames,
 * terminal text).  These helpers give the byte offsets of character
 * boundaries and the terminal column width of a code point, so line
 * editors and cursor math never split a multi-byte character.
 */

#ifndef USER_LIB_LIBC_UTF8_H
#define USER_LIB_LIBC_UTF8_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of bytes of the UTF-8 sequence starting at s[0] (1..4), or 0
 * for a stray continuation byte / NUL / invalid lead. */
int utf8_seq_len(const char *s);

/* Decode the code point at s; returns bytes consumed (1..4) or 0 on
 * invalid input.  cp may be NULL. */
int utf8_decode(const char *s, uint32_t *cp);

/* Terminal column width of a code point: 0 (combining/control), 1
 * (ASCII/Latin), 2 (CJK wide).  Approximates wcwidth(). */
int utf8_char_width(uint32_t cp);

/* Byte offset of the character that ends before `pos` (i.e. the start
 * of the previous character).  Returns 0 when pos <= 0 or no previous
 * character exists.  The caller ensures pos <= strlen(s). */
int utf8_prev(const char *s, int pos);

/* Byte offset of the character that starts at/after `pos`.  Returns
 * pos for an empty string, or len (the NUL) when pos is at the end.
 * `len` is strlen(s). */
int utf8_next(const char *s, int pos, int len);

/* Byte offset of the character `delta` positions before/after `pos`
 * (delta negative = left, positive = right; moves whole code points). */
int utf8_advance(const char *s, int pos, int len, int delta);

/* Column width of the string up to `len` bytes (CJK counts 2). */
int utf8_str_width(const char *s, int len);

#ifdef __cplusplus
}
#endif

#endif /* USER_LIB_LIBC_UTF8_H */
