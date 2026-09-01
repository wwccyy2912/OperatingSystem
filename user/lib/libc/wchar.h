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
 * wchar.h - Wide character and string utilities (C11 §7.29)
 * Copyright (c) 2026 OpSys Project
 *
 * Wide counterparts of the <string.h>, <stdlib.h>, <stdio.h> and
 * <ctype.h> facilities, plus multibyte <-> wchar conversion helpers.
 * The string/memory and numeric routines are implemented in wchar.c;
 * the formatted-I/O and mbs routines are declared here for completeness.
 */

#ifndef LIBC_WCHAR_H
#define LIBC_WCHAR_H

#include <stddef.h>
#include <time.h>   /* struct tm (wcsftime) */
#include <stdarg.h> /* va_list (v*wprintf) */

/* ====================================================================
 * Types (C11 §7.29.1)
 * ==================================================================== */

/* wchar_t is provided by GCC's <stddef.h>. */
typedef unsigned int wint_t;

typedef struct {
    char   __buf[8]; /* pending multibyte bytes */
    size_t __len;    /* valid bytes stored in __buf */
} mbstate_t;

/* ====================================================================
 * Constants (C11 §7.29.1)
 * ==================================================================== */

#ifndef WEOF
#define WEOF ((wint_t) - 1)
#endif

/* ====================================================================
 * Wide-string copying / concatenation (C11 §7.29.4)
 * ==================================================================== */

wchar_t *wcscpy(wchar_t *dest, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dest, const wchar_t *src);
wchar_t *wcsncat(wchar_t *dest, const wchar_t *src, size_t n);
size_t   wcsxfrm(wchar_t *dest, const wchar_t *src, size_t n);

/* ====================================================================
 * Wide-string length / comparison (C11 §7.29.4)
 * ==================================================================== */

size_t wcslen(const wchar_t *s);
int    wcscmp(const wchar_t *a, const wchar_t *b);
int    wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int    wcscoll(const wchar_t *a, const wchar_t *b);

/* ====================================================================
 * Wide-string searching (C11 §7.29.4)
 * ==================================================================== */

wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
size_t   wcsspn(const wchar_t *s, const wchar_t *accept);
size_t   wcscspn(const wchar_t *s, const wchar_t *reject);
wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);
wchar_t *wcstok(wchar_t *str, const wchar_t *delim, wchar_t **ptr);

/* ====================================================================
 * Wide memory operations (C11 §7.29.4)
 * ==================================================================== */

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
int      wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);

/* ====================================================================
 * Wide numeric conversion (C11 §7.29.4)
 * ==================================================================== */

double             wcstod(const wchar_t *s, wchar_t **endptr);
long               wcstol(const wchar_t *s, wchar_t **endptr, int base);
unsigned long      wcstoul(const wchar_t *s, wchar_t **endptr, int base);
long long          wcstoll(const wchar_t *s, wchar_t **endptr, int base);
unsigned long long wcstoull(const wchar_t *s, wchar_t **endptr, int base);

/* ====================================================================
 * Wide formatted I/O (C11 §7.29.2) — declarations only
 * ==================================================================== */

int Fwprintf(void *stream, const wchar_t *fmt, ...);
int Fwscanf(void *stream, const wchar_t *fmt, ...);
int Wprintf(const wchar_t *fmt, ...);
int Wscanf(const wchar_t *fmt, ...);
int Swprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...);
int Swscanf(const wchar_t *s, const wchar_t *fmt, ...);

int Vfwprintf(void *stream, const wchar_t *fmt, va_list ap);
int Vwprintf(const wchar_t *fmt, va_list ap);
int Vswprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap);

/* ====================================================================
 * Wide character I/O (C11 §7.29.3) — declarations only
 * ==================================================================== */

wint_t   Fgetwc(void *stream);
wchar_t *fgetws(wchar_t *s, int n, void *stream);
wint_t   Fputwc(wchar_t c, void *stream);
int      Fputws(const wchar_t *s, void *stream);
wint_t   Getwc(void *stream);
wint_t   Getwchar(void);
wint_t   Putwc(wchar_t c, void *stream);
wint_t   Putwchar(wchar_t c);
wint_t   Ungetwc(wint_t c, void *stream);

/* ====================================================================
 * Multibyte <-> wchar conversion (C11 §7.29.6) — declarations only
 * ==================================================================== */

int    mbsinit(const mbstate_t *ps);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
size_t mbsrtowcs(wchar_t *dest, const char **src, size_t len, mbstate_t *ps);
size_t wcsrtombs(char *dest, const wchar_t **src, size_t len, mbstate_t *ps);

/* Terminal display width of a wide character: -1 control, 0 combining,
 * 1 narrow, 2 CJK wide (approximation of the classic wcwidth). */
int wcwidth(wchar_t wc);

/* ====================================================================
 * Time formatting (C11 §7.29.5.1) — declaration only
 * ==================================================================== */

size_t Wcsftime(wchar_t *s, size_t max, const wchar_t *fmt, const struct tm *t);

#endif /* LIBC_WCHAR_H */
