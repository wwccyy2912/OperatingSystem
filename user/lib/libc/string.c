/*
 * string.c - Standard string and memory operations
 * Copyright (c) 2026 OpSys Project
 *
 * Pure utility functions — no syscalls, no allocation (except strdup).
 */

#include "string.h"
#include "../libos/syscalls.h"  /* for malloc (used by strdup) */
#include <malloc.h>

/* ====================================================================
 * Length
 * ==================================================================== */

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len] != '\0')
        len++;
    return len;
}

/* ====================================================================
 * Comparison
 * ==================================================================== */

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
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

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++) != '\0')
        ;
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    char *d = dest;
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        d[i] = src[i];
    for (; i < n; i++)
        d[i] = '\0';
    return dest;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest + strlen(dest);
    while ((*d++ = *src++) != '\0')
        ;
    return dest;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *d = dest + strlen(dest);
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        d[i] = src[i];
    d[i] = '\0';
    return dest;
}

/* ====================================================================
 * Searching
 * ==================================================================== */

char *strchr(const char *s, int c)
{
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

char *strrchr(const char *s, int c)
{
    char ch = (char)c;
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

char *strstr(const char *haystack, const char *needle)
{
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

char *strpbrk(const char *s, const char *accept)
{
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

size_t strspn(const char *s, const char *accept)
{
    size_t count = 0;
    while (*s != '\0') {
        int found = 0;
        const char *a = accept;
        while (*a != '\0') {
            if (*s == *a) { found = 1; break; }
            a++;
        }
        if (!found) break;
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t count = 0;
    while (*s != '\0') {
        const char *r = reject;
        while (*r != '\0') {
            if (*s == *r) return count;
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

char *strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (!copy) return NULL;
    strcpy(copy, s);
    return copy;
}

static const char *s_error_strings[] = {
    [0]     = "Success",
    [1]     = "Operation not permitted",
    [2]     = "No such file or directory",
    [3]     = "No such process",
    [4]     = "Interrupted system call",
    [5]     = "I/O error",
    [6]     = "No such device or address",
    [7]     = "Argument list too long",
    [8]     = "Exec format error",
    [9]     = "Bad file descriptor",
    [10]    = "No child processes",
    [11]    = "Resource temporarily unavailable",
    [12]    = "Cannot allocate memory",
    [13]    = "Permission denied",
    [14]    = "Bad address",
    [16]    = "Device or resource busy",
    [17]    = "File exists",
    [18]    = "Invalid cross-device link",
    [19]    = "No such device",
    [20]    = "Not a directory",
    [21]    = "Is a directory",
    [22]    = "Invalid argument",
    [23]    = "Too many open files in system",
    [24]    = "Too many open files",
    [28]    = "No space left on device",
    [29]    = "Illegal seek",
    [30]    = "Read-only file system",
    [34]    = "Numerical result out of range",
    [38]    = "Function not implemented",
};

#define ERR_STR_COUNT (sizeof(s_error_strings) / sizeof(s_error_strings[0]))

char *strerror(int errnum)
{
    if (errnum >= 0 && (size_t)errnum < ERR_STR_COUNT && s_error_strings[errnum])
        return (char *)s_error_strings[errnum];
    return (char *)"Unknown error";
}

/* Thread-unsafe strtok (uses internal state) */
static char *s_strtok_save = NULL;

char *strtok(char *str, const char *delim)
{
    return strtok_r(str, delim, &s_strtok_save);
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
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
        *end = '\0';
        *saveptr = end + 1;
    } else {
        *saveptr = NULL;
    }
    return str;
}

/* ====================================================================
 * Memory operations
 * ==================================================================== */

void *memset(void *dest, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    unsigned char val = (unsigned char)c;
    for (size_t i = 0; i < n; i++)
        d[i] = val;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
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

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == ch)
            return (void *)(p + i);
    }
    return NULL;
}
