/*
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

int fwprintf(void *stream, const wchar_t *fmt, ...);
int fwscanf(void *stream, const wchar_t *fmt, ...);
int wprintf(const wchar_t *fmt, ...);
int wscanf(const wchar_t *fmt, ...);
int swprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...);
int swscanf(const wchar_t *s, const wchar_t *fmt, ...);

int vfwprintf(void *stream, const wchar_t *fmt, va_list ap);
int vwprintf(const wchar_t *fmt, va_list ap);
int vswprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap);

/* ====================================================================
 * Wide character I/O (C11 §7.29.3) — declarations only
 * ==================================================================== */

wint_t   fgetwc(void *stream);
wchar_t *fgetws(wchar_t *s, int n, void *stream);
wint_t   fputwc(wchar_t c, void *stream);
int      fputws(const wchar_t *s, void *stream);
wint_t   getwc(void *stream);
wint_t   getwchar(void);
wint_t   putwc(wchar_t c, void *stream);
wint_t   putwchar(wchar_t c);
wint_t   ungetwc(wint_t c, void *stream);

/* ====================================================================
 * Multibyte <-> wchar conversion (C11 §7.29.6) — declarations only
 * ==================================================================== */

int    mbsinit(const mbstate_t *ps);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
size_t mbsrtowcs(wchar_t *dest, const char **src, size_t len, mbstate_t *ps);
size_t wcsrtombs(char *dest, const wchar_t **src, size_t len, mbstate_t *ps);

/* ====================================================================
 * Time formatting (C11 §7.29.5.1) — declaration only
 * ==================================================================== */

size_t wcsftime(wchar_t *s, size_t max, const wchar_t *fmt, const struct tm *t);

#endif /* LIBC_WCHAR_H */
