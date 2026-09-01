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
 * wchar.c - Wide character and string utilities (C11 §7.29)
 * Copyright (c) 2026 OpSys Project
 *
 * Implements the wide string/memory operations and the wide numeric
 * conversions.  The numeric routines delegate to the narrow strtol
 * family after copying the wide input into a narrow buffer.
 
 *
 * ------------------------------------------------------------------
 * Structure (wchar):
 *   wcs strings (32-bit wchar_t) + mbrtowc/wcrtomb/mbsrtowcs/... +
 *   wcwidth — UTF-8 on the byte side (no locale switching).
 * How it works:
 *   mbrtowc assembles UTF-8 sequences incrementally via mbstate_t;
 *   wcrtomb emits UTF-8; char16_t conversions handle surrogate
 *   pairs through the state field.
 * Purpose:
 *   Wide/multibyte interop for a UTF-8 system.
 * Caveats:
 *   wchar_t is 32-bit; overlong/surrogate encodings are rejected.
 * ------------------------------------------------------------------
 */

#include "wchar.h"
#include "uchar.h" /* char16_t / char32_t (mbrtoc16 & co.) */
#include "string.h"
#include "stdlib.h"
#include "utf8.h"
#include <malloc.h>
#include <stdint.h>

/* ====================================================================
 * Length
 * ==================================================================== */

size_t wcslen(const wchar_t *s) {
    size_t len = 0;
    while (s[len] != L'\0')
        len++;
    return len;
}

/* ====================================================================
 * Comparison
 * ==================================================================== */

int wcscmp(const wchar_t *a, const wchar_t *b) {
    while (*a != L'\0' && *a == *b) {
        a++;
        b++;
    }
    if (*a < *b)
        return -1;
    if (*a > *b)
        return 1;
    return 0;
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            if (a[i] < b[i])
                return -1;
            return 1;
        }
        if (a[i] == L'\0')
            return 0;
    }
    return 0;
}

/* ====================================================================
 * Copying / concatenation
 * ==================================================================== */

wchar_t *wcscpy(wchar_t *dest, const wchar_t *src) {
    wchar_t *d = dest;
    while ((*d++ = *src++) != L'\0')
        ;
    return dest;
}

wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n) {
    wchar_t *d = dest;
    size_t   i;
    for (i = 0; i < n && src[i] != L'\0'; i++)
        d[i] = src[i];
    for (; i < n; i++)
        d[i] = L'\0';
    return dest;
}

wchar_t *wcscat(wchar_t *dest, const wchar_t *src) {
    wchar_t *d = dest + wcslen(dest);
    while ((*d++ = *src++) != L'\0')
        ;
    return dest;
}

wchar_t *wcsncat(wchar_t *dest, const wchar_t *src, size_t n) {
    wchar_t *d = dest + wcslen(dest);
    size_t   i;
    for (i = 0; i < n && src[i] != L'\0'; i++)
        d[i] = src[i];
    d[i] = L'\0';
    return dest;
}

/* ====================================================================
 * Searching
 * ==================================================================== */

wchar_t *wcschr(const wchar_t *s, wchar_t c) {
    while (*s != L'\0') {
        if (*s == c)
            return (wchar_t *)s;
        s++;
    }
    if (c == L'\0')
        return (wchar_t *)s;
    return NULL;
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
    const wchar_t *last = NULL;
    while (*s != L'\0') {
        if (*s == c)
            last = s;
        s++;
    }
    if (c == L'\0')
        return (wchar_t *)s;
    return (wchar_t *)last;
}

wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle) {
    if (*needle == L'\0')
        return (wchar_t *)haystack;

    size_t nlen = wcslen(needle);
    while (*haystack != L'\0') {
        if (*haystack == *needle) {
            if (wcsncmp(haystack, needle, nlen) == 0)
                return (wchar_t *)haystack;
        }
        haystack++;
    }
    return NULL;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept) {
    while (*s != L'\0') {
        const wchar_t *a = accept;
        while (*a != L'\0') {
            if (*s == *a)
                return (wchar_t *)s;
            a++;
        }
        s++;
    }
    return NULL;
}

size_t wcsspn(const wchar_t *s, const wchar_t *accept) {
    size_t count = 0;
    while (*s != L'\0') {
        int            found = 0;
        const wchar_t *a     = accept;
        while (*a != L'\0') {
            if (*s == *a) {
                found = 1;
                break;
            }
            a++;
        }
        if (!found)
            break;
        count++;
        s++;
    }
    return count;
}

size_t wcscspn(const wchar_t *s, const wchar_t *reject) {
    size_t count = 0;
    while (*s != L'\0') {
        const wchar_t *r = reject;
        while (*r != L'\0') {
            if (*s == *r)
                return count;
            r++;
        }
        count++;
        s++;
    }
    return count;
}

wchar_t *wcstok(wchar_t *str, const wchar_t *delim, wchar_t **ptr) {
    if (str == NULL)
        str = *ptr;
    if (str == NULL)
        return NULL;

    /* Skip leading delimiters */
    str += wcsspn(str, delim);
    if (*str == L'\0') {
        *ptr = NULL;
        return NULL;
    }

    /* Find end of token */
    wchar_t *end = str + wcscspn(str, delim);
    if (*end != L'\0') {
        *end = L'\0';
        *ptr = end + 1;
    } else {
        *ptr = NULL;
    }
    return str;
}

/* ====================================================================
 * Wide memory operations
 * ==================================================================== */

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == c)
            return (wchar_t *)(s + i);
    }
    return NULL;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            if (a[i] < b[i])
                return -1;
            return 1;
        }
    }
    return 0;
}

wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n) {
    return (wchar_t *)memcpy(dest, src, n * sizeof(wchar_t));
}

wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n) {
    return (wchar_t *)memmove(dest, src, n * sizeof(wchar_t));
}

wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++)
        s[i] = c;
    return s;
}

/* ====================================================================
 * Numeric conversion (delegate to strtol family)
 *
 * The wide input is copied into a narrow buffer (one byte per wide
 * char, truncating non-ASCII), the narrow strto* function does the
 * parse, and the consumed-byte offset is mapped back to a wchar_t
 * pointer for the caller's endptr.
 * ==================================================================== */

long wcstol(const wchar_t *s, wchar_t **endptr, int base) {
    size_t len = wcslen(s);
    char  *buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        if (endptr)
            *endptr = (wchar_t *)s;
        return 0;
    }
    for (size_t i = 0; i <= len; i++)
        buf[i] = (char)s[i];

    char *nend = NULL;
    long  val  = strtol(buf, &nend, base);
    if (endptr)
        *endptr = (wchar_t *)(s + (size_t)(nend - buf));
    free(buf);
    return val;
}

unsigned long wcstoul(const wchar_t *s, wchar_t **endptr, int base) {
    size_t len = wcslen(s);
    char  *buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        if (endptr)
            *endptr = (wchar_t *)s;
        return 0;
    }
    for (size_t i = 0; i <= len; i++)
        buf[i] = (char)s[i];

    char         *nend = NULL;
    unsigned long val  = strtoul(buf, &nend, base);
    if (endptr)
        *endptr = (wchar_t *)(s + (size_t)(nend - buf));
    free(buf);
    return val;
}

long long wcstoll(const wchar_t *s, wchar_t **endptr, int base) {
    size_t len = wcslen(s);
    char  *buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        if (endptr)
            *endptr = (wchar_t *)s;
        return 0;
    }
    for (size_t i = 0; i <= len; i++)
        buf[i] = (char)s[i];

    char     *nend = NULL;
    long long val  = strtoll(buf, &nend, base);
    if (endptr)
        *endptr = (wchar_t *)(s + (size_t)(nend - buf));
    free(buf);
    return val;
}

unsigned long long wcstoull(const wchar_t *s, wchar_t **endptr, int base) {
    size_t len = wcslen(s);
    char  *buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        if (endptr)
            *endptr = (wchar_t *)s;
        return 0;
    }
    for (size_t i = 0; i <= len; i++)
        buf[i] = (char)s[i];

    char              *nend = NULL;
    unsigned long long val  = strtoull(buf, &nend, base);
    if (endptr)
        *endptr = (wchar_t *)(s + (size_t)(nend - buf));
    free(buf);
    return val;
}

/* ====================================================================
 * Multibyte <-> wide character conversion (C11 §7.29.6)
 *
 * The execution encoding is UTF-8 end to end, so these implement the
 * standard stateful entry points on top of the utf8 helpers.  mbstate
 * tracks an incomplete multi-byte sequence (or a pending UTF-16
 * surrogate for mbrtoc16/c16rtomb).
 * ==================================================================== */

int mbsinit(const mbstate_t *ps) {
    return (ps == NULL) || (ps->__len == 0);
}

/* Pending-surrogate sentinel for mbrtoc16/c16rtomb state (distinct
 * from a pending-byte count, which is always <= 4). */
#define MBSTATE_SURROGATE ((size_t)0xFFFFFFFFu)

static void MbstatePutSurrogate(mbstate_t *ps, unsigned v) {
    ps->__buf[0] = (char)(v & 0xFF);
    ps->__buf[1] = (char)((v >> 8) & 0xFF);
    ps->__buf[2] = (char)((v >> 16) & 0xFF);
    ps->__buf[3] = (char)((v >> 24) & 0xFF);
    ps->__len    = MBSTATE_SURROGATE;
}

/* Returns the pending surrogate, or -1 when none; clears the state. */
static int MbstateGetSurrogate(mbstate_t *ps) {
    if (ps->__len != MBSTATE_SURROGATE)
        return -1;
    unsigned v = (unsigned)(unsigned char)ps->__buf[0] |
                 ((unsigned)(unsigned char)ps->__buf[1] << 8) |
                 ((unsigned)(unsigned char)ps->__buf[2] << 16) |
                 ((unsigned)(unsigned char)ps->__buf[3] << 24);
    ps->__len = 0;
    return (int)v;
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) {
    static mbstate_t s_internal;
    if (!ps)
        ps = &s_internal;
    if (!s) { /* reset */
        ps->__len = 0;
        return 0;
    }
    if (n == 0)
        return (size_t)-2;

    /* Assemble the sequence: pending bytes from a previous call plus
     * fresh bytes from s. */
    unsigned char seq[4];
    size_t        have = 0;
    if (ps->__len > 0 && ps->__len <= 4) {
        memcpy(seq, ps->__buf, ps->__len);
        have = ps->__len;
    }
    size_t used = 0;
    while (used < n && have < 4)
        seq[have++] = (unsigned char)s[used++];

    int need;
    if (seq[0] < 0x80)
        need = 1;
    else if (seq[0] < 0xC2) { /* stray continuation / invalid lead */
        ps->__len = 0;
        return (size_t)-1;
    } else if (seq[0] <= 0xDF)
        need = 2;
    else if (seq[0] <= 0xEF)
        need = 3;
    else if (seq[0] <= 0xF4)
        need = 4;
    else {
        ps->__len = 0;
        return (size_t)-1;
    }

    if (have < (size_t)need) { /* incomplete: park and wait */
        memcpy(ps->__buf, seq, have);
        ps->__len = have;
        return (size_t)-2;
    }

    for (size_t i = 1; i < (size_t)need; i++)
        if ((seq[i] & 0xC0) != 0x80) {
            ps->__len = 0;
            return (size_t)-1;
        }

    uint32_t cp;
    switch (need) {
    case 1: cp = seq[0]; break;
    case 2: cp = ((uint32_t)(seq[0] & 0x1F) << 6) | (seq[1] & 0x3F); break;
    case 3: cp = ((uint32_t)(seq[0] & 0x0F) << 12) |
                 ((uint32_t)(seq[1] & 0x3F) << 6) | (seq[2] & 0x3F); break;
    default: cp = ((uint32_t)(seq[0] & 0x07) << 18) |
                  ((uint32_t)(seq[1] & 0x3F) << 12) |
                  ((uint32_t)(seq[2] & 0x3F) << 6) | (seq[3] & 0x3F); break;
    }

    /* Reject overlong encodings and surrogates. */
    if ((need == 2 && cp < 0x80) || (need == 3 && cp < 0x800) ||
        (need == 4 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        ps->__len = 0;
        return (size_t)-1;
    }

    ps->__len = 0;
    if (pwc)
        *pwc = (wchar_t)cp;
    return used;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps) {
    return mbrtowc(NULL, s, n, ps);
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps) {
    (void)ps;
    if (!s)
        return 1; /* UTF-8 has no shift state */
    uint32_t cp = (uint32_t)wc;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return (size_t)-1;
    if (cp < 0x80) {
        s[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        s[0] = (char)(0xC0 | (cp >> 6));
        s[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        s[0] = (char)(0xE0 | (cp >> 12));
        s[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        s[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    s[0] = (char)(0xF0 | (cp >> 18));
    s[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    s[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    s[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t mbsrtowcs(wchar_t *dest, const char **src, size_t len, mbstate_t *ps) {
    static mbstate_t s_internal;
    if (!ps)
        ps = &s_internal;
    if (!src || !*src)
        return 0;
    const char *s     = *src;
    size_t      count = 0;
    while (*s) {
        wchar_t wc;
        size_t  r = mbrtowc(&wc, s, strlen(s), ps);
        if (r == (size_t)-1 || r == (size_t)-2) {
            *src = s;
            return (size_t)-1;
        }
        if (dest) {
            if (count >= len) {
                *src = s;
                return count; /* buffer full */
            }
            dest[count] = wc;
        }
        count++;
        s += r;
    }
    if (dest) {
        dest[count] = L'\0';
        *src        = NULL;
    } else {
        *src = s;
    }
    return count;
}

size_t wcsrtombs(char *dest, const wchar_t **src, size_t len, mbstate_t *ps) {
    (void)ps;
    if (!src || !*src)
        return 0;
    const wchar_t *s     = *src;
    size_t         total = 0;
    while (*s) {
        char  tmp[4];
        size_t r = wcrtomb(tmp, *s, NULL);
        if (r == (size_t)-1) {
            *src = s;
            return (size_t)-1;
        }
        if (dest) {
            if (total + r > len) {
                *src = s;
                return total; /* no room for this character */
            }
            memcpy(dest + total, tmp, r);
        }
        total += r;
        s++;
    }
    if (dest) {
        if (total + 1 > len) { /* no room for the terminating NUL */
            *src = s;
            return total;
        }
        dest[total] = '\0';
        *src        = NULL;
    } else {
        *src = s;
    }
    return total;
}

/* Terminal column width of a wide character (wcwidth(3)): -1 for
 * control/format, 0 for combining, 1 narrow, 2 CJK wide. */
int wcwidth(wchar_t wc) {
    uint32_t cp = (uint32_t)wc;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return -1;
    return Utf8CharWidth(cp);
}

/* ====================================================================
 * char16_t / char32_t conversions (C11 §7.28.2) — UTF-16 on the
 * char16_t side, UTF-8 on the byte side.
 * ==================================================================== */

size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps) {
    wchar_t wc;
    size_t  r = mbrtowc(&wc, s, n, ps);
    if (r != (size_t)-1 && r != (size_t)-2 && pc32)
        *pc32 = (char32_t)wc;
    return r;
}

size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps) {
    return wcrtomb(s, (wchar_t)c32, ps);
}

size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps) {
    static mbstate_t s_internal;
    if (!ps)
        ps = &s_internal;
    if (!s) {
        ps->__len = 0;
        return 0;
    }
    /* A pending low surrogate from a previous astral character. */
    int low = MbstateGetSurrogate(ps);
    if (low >= 0) {
        if (pc16)
            *pc16 = (char16_t)low;
        return (size_t)-3; /* complete char, 0 bytes consumed */
    }
    wchar_t wc;
    size_t  r = mbrtowc(&wc, s, n, ps);
    if (r == (size_t)-1 || r == (size_t)-2)
        return r;
    if (wc > 0xFFFF) {
        uint32_t cp = (uint32_t)wc - 0x10000;
        if (pc16)
            *pc16 = (char16_t)(0xD800 + (cp >> 10));
        MbstatePutSurrogate(ps, 0xDC00 + (cp & 0x3FF));
    } else if (pc16) {
        *pc16 = (char16_t)wc;
    }
    return r;
}

size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps) {
    static mbstate_t s_internal;
    if (!ps)
        ps = &s_internal;
    if (!s) {
        ps->__len = 0;
        return 1;
    }
    unsigned c = (unsigned)c16;
    if (c >= 0xD800 && c <= 0xDBFF) { /* high surrogate: wait for the low */
        MbstatePutSurrogate(ps, c);
        return 0;
    }
    int hi = MbstateGetSurrogate(ps);
    if (hi >= 0) {
        if (!(c >= 0xDC00 && c <= 0xDFFF)) { /* malformed pair */
            ps->__len = 0;
            return (size_t)-1;
        }
        uint32_t cp = 0x10000 + (((uint32_t)hi - 0xD800) << 10) + (c - 0xDC00);
        return wcrtomb(s, (wchar_t)cp, NULL);
    }
    if (c >= 0xDC00 && c <= 0xDFFF) /* lone low surrogate */
        return (size_t)-1;
    return wcrtomb(s, (wchar_t)c, NULL);
}
