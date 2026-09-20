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
 * string.c - Standard string and memory operations
 * Copyright (c) 2026 OpSys Project
 *
 * Pure utility functions — no syscalls, no allocation (except strdup).
 
 *
 * ------------------------------------------------------------------
 * Structure (string):
 *   mem and str families (length, copy, compare, search, tokenize) plus
 *   the bounded/BSD/GNU extensions (strnlen/strndup/strlcpy/strlcat/
 *   strsep/memmem) and the "C"-locale collation entry points
 *   (strcoll/strxfrm).
 * How it works:
 *   Byte-wise loops (word-at-a-time only where alignment is proven);
 *   all functions are NUL-termination aware per C semantics.
 * Purpose:
 *   Core string/memory primitives shared by every service.
 * Caveats:
 *   Bounds are the caller's responsibility (no _s variants wired in).
 * ------------------------------------------------------------------
 */

#include "string.h"
#include "../libos/syscalls.h" /* for malloc (used by strdup) */
#include <malloc.h>

/* ====================================================================
 * Length
 * ==================================================================== */

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len] != '\0')
        len++;
    return len;
}

/* ====================================================================
 * Comparison
 * ==================================================================== */

int strcmp(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

/* ====================================================================
 * Copying
 * ==================================================================== */

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0')
        ;
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char  *d = dest;
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        d[i] = src[i];
    for (; i < n; i++)
        d[i] = '\0';
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest + strlen(dest);
    while ((*d++ = *src++) != '\0')
        ;
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char  *d = dest + strlen(dest);
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        d[i] = src[i];
    d[i] = '\0';
    return dest;
}

/* ====================================================================
 * Searching
 * ==================================================================== */

char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s != '\0') {
        if (*s == ch)
            return (char *)s;
        s++;
    }
    if (ch == '\0')
        return (char *)s;
    return NULL;
}

char *strrchr(const char *s, int c) {
    char        ch   = (char)c;
    const char *last = NULL;
    while (*s != '\0') {
        if (*s == ch)
            last = s;
        s++;
    }
    if (ch == '\0')
        return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (*needle == '\0')
        return (char *)haystack;

    size_t nlen = strlen(needle);
    while (*haystack != '\0') {
        if (*haystack == *needle) {
            if (strncmp(haystack, needle, nlen) == 0)
                return (char *)haystack;
        }
        haystack++;
    }
    return NULL;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s != '\0') {
        const char *a = accept;
        while (*a != '\0') {
            if (*s == *a)
                return (char *)s;
            a++;
        }
        s++;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;
    while (*s != '\0') {
        int         found = 0;
        const char *a     = accept;
        while (*a != '\0') {
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

size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;
    while (*s != '\0') {
        const char *r = reject;
        while (*r != '\0') {
            if (*s == *r)
                return count;
            r++;
        }
        count++;
        s++;
    }
    return count;
}

/* ====================================================================
 * Utilities
 * ==================================================================== */

char *strdup(const char *s) {
    if (!s)
        return NULL;
    size_t len  = strlen(s) + 1;
    char  *copy = (char *)malloc(len);
    if (!copy)
        return NULL;
    strcpy(copy, s);
    return copy;
}

static const char *s_error_strings[] = {
    [0]  = "Success",
    [1]  = "Operation not permitted",
    [2]  = "No such file or directory",
    [3]  = "No such process",
    [4]  = "Interrupted system call",
    [5]  = "I/O error",
    [6]  = "No such device or address",
    [7]  = "Argument list too long",
    [8]  = "Exec format error",
    [9]  = "Bad file descriptor",
    [10] = "No child processes",
    [11] = "Resource temporarily unavailable",
    [12] = "Cannot allocate memory",
    [13] = "Permission denied",
    [14] = "Bad address",
    [16] = "Device or resource busy",
    [17] = "File exists",
    [18] = "Invalid cross-device link",
    [19] = "No such device",
    [20] = "Not a directory",
    [21] = "Is a directory",
    [22] = "Invalid argument",
    [23] = "Too many open files in system",
    [24] = "Too many open files",
    [28] = "No space left on device",
    [29] = "Illegal seek",
    [30] = "Read-only file system",
    [34] = "Numerical result out of range",
    [38] = "Function not implemented",
};

#define ERR_STR_COUNT (sizeof(s_error_strings) / sizeof(s_error_strings[0]))

char *strerror(int errnum) {
    if (errnum >= 0 && (size_t)errnum < ERR_STR_COUNT && s_error_strings[errnum])
        return (char *)s_error_strings[errnum];
    return (char *)"Unknown error";
}

/**
 * @brief strerror_r, GNU flavour: message text for @p errnum.
 *
 * The GNU signature returns char * (the XSI one returns int); the
 * string is either the caller's @p buf or strerror()'s static table
 * entry, so the result must never be freed.  @p buf is always
 * NUL-terminated when it is used, truncated to @p buflen - 1 bytes.
 *
 * @param errnum  Error number; unknown values give "Unknown error".
 * @param buf     Caller buffer, or NULL to use the static string.
 * @param buflen  Size of @p buf in bytes (0 = use the static string).
 * @return Pointer to the message (never NULL).
 */
char *strerror_r(int errnum, char *buf, size_t buflen) {
    const char *msg = strerror(errnum);
    if (!buf || buflen == 0)
        return (char *)msg;
    size_t len = strlen(msg);
    if (len > buflen - 1)
        len = buflen - 1;
    memcpy(buf, msg, len);
    buf[len] = '\0';
    return buf;
}

/* Thread-unsafe strtok (uses internal state) */
static char *s_strtok_save = NULL;

char *strtok(char *str, const char *delim) {
    return strtok_r(str, delim, &s_strtok_save);
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!str)
        str = *saveptr;
    if (!str)
        return NULL;

    /* Skip leading delimiters */
    str += strspn(str, delim);
    if (*str == '\0') {
        *saveptr = NULL;
        return NULL;
    }

    /* Find end of token */
    char *end = str + strcspn(str, delim);
    if (*end != '\0') {
        *end     = '\0';
        *saveptr = end + 1;
    } else {
        *saveptr = NULL;
    }
    return str;
}

/* ====================================================================
 * Bounded / BSD / GNU extensions
 *
 * strnlen/strndup bound the read, strlcpy/strlcat bound the write and
 * report the untruncated length (so callers can detect truncation),
 * strsep splits destructively on a byte set, memmem searches a binary
 * buffer.  All of them require non-NULL arguments exactly as the BSD /
 * GNU documentation specifies; the NULL cases that are cheap to check
 * (strndup, strsep, strlcpy/strlcat) fail safe rather than crash.
 * ==================================================================== */

/**
 * @brief Length of @p s, at most @p maxlen bytes.
 * @return Number of bytes before the NUL, capped at @p maxlen.
 */
size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n] != '\0')
        n++;
    return n;
}

/**
 * @brief Duplicate at most @p n bytes of @p s into malloc()'ed memory.
 * @return New NUL-terminated string, or NULL for NULL @p s or a failed
 *         allocation (errno is set by malloc in that case).
 */
char *strndup(const char *s, size_t n) {
    if (!s)
        return NULL;
    size_t len  = strnlen(s, n);
    char  *copy = (char *)malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

/**
 * @brief BSD strsep: split @p *stringp at the first byte of @p delim.
 *
 * The delimiter is replaced by NUL and @p *stringp advances past it.
 * Unlike strtok_r an empty token is returned for adjacent delimiters,
 * and the caller's pointer is set to NULL once the string is consumed.
 *
 * @return Pointer to the token, or NULL when @p *stringp is NULL.
 */
char *strsep(char **stringp, const char *delim) {
    if (!stringp || !*stringp || !delim)
        return NULL;
    char *start = *stringp;
    char *end   = start + strcspn(start, delim);
    if (*end != '\0') {
        *end     = '\0';
        *stringp = end + 1;
    } else {
        *stringp = NULL;
    }
    return start;
}

/**
 * @brief BSD strlcpy: copy @p src into @p dest, never writing more than
 *        @p size bytes (including the NUL).
 * @return strlen(src); >= @p size means the copy was truncated.
 */
size_t strlcpy(char *dest, const char *src, size_t size) {
    if (!src) {
        if (dest && size > 0)
            dest[0] = '\0';
        return 0;
    }
    size_t srclen = strlen(src);
    if (dest && size > 0) {
        size_t n = srclen < size - 1 ? srclen : size - 1;
        memcpy(dest, src, n);
        dest[n] = '\0';
    }
    return srclen;
}

/**
 * @brief BSD strlcat: append @p src to the NUL-terminated @p dest
 *        without ever writing more than @p size bytes.
 *
 * @p dest is scanned for its NUL only within @p size bytes, so a
 * caller whose buffer lost its terminator still cannot be overrun.
 *
 * @return strlen(dest) + strlen(src) for the untruncated result;
 *         >= @p size means the append was truncated.
 */
size_t strlcat(char *dest, const char *src, size_t size) {
    if (!dest || !src)
        return 0;
    size_t dlen = 0;
    while (dlen < size && dest[dlen] != '\0')
        dlen++;
    if (dlen == size)
        return size + strlen(src); /* no terminator in range: nothing to append to */
    size_t slen  = strlen(src);
    size_t avail = size - dlen - 1;
    size_t n     = slen < avail ? slen : avail;
    memcpy(dest + dlen, src, n);
    dest[dlen + n] = '\0';
    return dlen + slen;
}

/**
 * @brief Collate two strings (C11 §7.24.4.3).
 *
 * Only the "C" locale exists in v0.9, so this is byte comparison —
 * identical to strcmp() but locale-dependent by contract.
 */
int strcoll(const char *a, const char *b) {
    return strcmp(a, b);
}

/**
 * @brief Transform a string for collation (C11 §7.24.4.5).
 *
 * The "C" locale transformation is the identity, so this is a bounded
 * copy: at most @p n bytes total including the terminating NUL.
 *
 * @return strlen(src); >= @p n means @p dest was truncated.
 */
size_t strxfrm(char *dest, const char *src, size_t n) {
    if (!src) {
        if (dest && n > 0)
            dest[0] = '\0';
        return 0;
    }
    size_t len = strlen(src);
    if (dest && n > 0) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(dest, src, c);
        dest[c] = '\0';
    }
    return len;
}

/* ====================================================================
 * Memory operations
 * ==================================================================== */

void *memset(void *dest, int c, size_t n) {
    unsigned char *d   = (unsigned char *)dest;
    unsigned char  val = (unsigned char)c;
    for (size_t i = 0; i < n; i++)
        d[i] = val;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    /* Hot path (realloc, file blocks, term screen buffer): byte-align
     * both pointers to 8 bytes, then copy qwords, then the tail. */
    while (((uintptr_t)d & 7) && n > 0) {
        *d++ = *s++;
        n--;
    }
    if (n >= 8) {
        uint64_t       *dq = (uint64_t *)d;
        const uint64_t *sq = (const uint64_t *)s;
        do {
            *dq++ = *sq++;
            n -= 8;
        } while (n >= 8);
        d = (unsigned char *)dq;
        s = (const unsigned char *)sq;
    }
    while (n-- > 0)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p  = (const unsigned char *)s;
    unsigned char        ch = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == ch)
            return (void *)(p + i);
    }
    return NULL;
}

/**
 * @brief GNU memmem: first occurrence of @p needlelen bytes inside a
 *        @p haystacklen-byte buffer (the buffer need not be NUL-safe).
 *
 * @return Pointer to the match, NULL when absent.  An empty needle
 *         matches at the start, exactly like strstr().
 */
void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen) {
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    if (needlelen == 0)
        return (void *)haystack;
    if (!h || !n || haystacklen < needlelen)
        return NULL;
    for (size_t i = 0; i + needlelen <= haystacklen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0)
            return (void *)(h + i);
    }
    return NULL;
}
