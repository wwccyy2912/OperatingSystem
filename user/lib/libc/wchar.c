/*
 * wchar.c - Wide character and string utilities (C11 §7.29)
 * Copyright (c) 2026 OpSys Project
 *
 * Implements the wide string/memory operations and the wide numeric
 * conversions.  The numeric routines delegate to the narrow strtol
 * family after copying the wide input into a narrow buffer.
 */

#include "wchar.h"
#include "string.h"
#include "stdlib.h"
#include <malloc.h>

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
