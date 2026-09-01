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
int Utf8SeqLen(const char *s);

/* Decode the code point at s; returns bytes consumed (1..4) or 0 on
 * invalid input.  cp may be NULL. */
int Utf8Decode(const char *s, uint32_t *cp);

/* Terminal column width of a code point: 0 (combining/control), 1
 * (ASCII/Latin), 2 (CJK wide).  Approximates wcwidth(). */
int Utf8CharWidth(uint32_t cp);

/* Byte offset of the character that ends before `pos` (i.e. the start
 * of the previous character).  Returns 0 when pos <= 0 or no previous
 * character exists.  The caller ensures pos <= strlen(s). */
int Utf8Prev(const char *s, int pos);

/* Byte offset of the character that starts at/after `pos`.  Returns
 * pos for an empty string, or len (the NUL) when pos is at the end.
 * `len` is strlen(s). */
int Utf8Next(const char *s, int pos, int len);

/* Byte offset of the character `delta` positions before/after `pos`
 * (delta negative = left, positive = right; moves whole code points). */
int Utf8Advance(const char *s, int pos, int len, int delta);

/* Column width of the string up to `len` bytes (CJK counts 2). */
int Utf8StrWidth(const char *s, int len);

#ifdef __cplusplus
}
#endif

#endif /* USER_LIB_LIBC_UTF8_H */
