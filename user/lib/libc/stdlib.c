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
 * stdlib.c - Standard library utilities
 * Copyright (c) 2026 OpSys Project
 *
 * Numeric conversion, random numbers, searching, sorting.
 * String/memory functions now live in string.c.
 
 *
 * ------------------------------------------------------------------
 * Structure (stdlib):
 *   atoi/strtol family -> ParseNumber; malloc/calloc/realloc/free ->
 *   runtime heap (malloc.c); getenv/setenv -> static environ table.
 * How it works:
 *   Numeric parsers delegate to a shared digit reader; memory calls
 *   forward to the runtime allocator; environ is a fixed-size array
 *   copied from the process image.
 * Purpose:
 *   C standard-library basics for user services.
 * Caveats:
 *   environ has a fixed cap; strtol overflow follows C99 (clamped,
 *   errno-style).
 * ------------------------------------------------------------------
 */

/* Limits for strtol/strtoll — defined here because GCC freestanding
 * limits.h can't chain to the system version via #include_next.
 */
#ifndef LONG_MIN
#define LONG_MIN (-2147483647L - 1L)
#endif
#ifndef LONG_MAX
#define LONG_MAX 2147483647L
#endif
#ifndef LLONG_MIN
#define LLONG_MIN (-9223372036854775807LL - 1LL)
#endif
#ifndef LLONG_MAX
#define LLONG_MAX 9223372036854775807LL
#endif

#include "stdlib.h"
#include "stdio.h"  /* for printf / __assert_fail */
#include "string.h" /* for memcpy */
#include <malloc.h> /* for malloc (aligned_alloc) */
#include <stdint.h> /* for uintptr_t */
#include "../libos/syscalls.h"

/* ====================================================================
 * Numeric conversion
 * ==================================================================== */

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s) {
    return strtol(s, NULL, 10);
}

long long atoll(const char *s) {
    return strtoll(s, NULL, 10);
}

/*
 * Core integer parser: parse a string in the given base (0 or 2-36).
 * If base == 0, auto-detect: 0x → 16, 0 → 8, else → 10.
 * Skips leading whitespace and handles an optional +/- sign.
 */
static unsigned long long ParseInt(const char *s, char **endptr, int base, int *neg) {
    *neg = 0;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\v' || *s == '\f' || *s == '\r')
        s++;

    /* Sign */
    if (*s == '-') {
        *neg = 1;
        s++;
    } else if (*s == '+')
        s++;

    /* Auto-detect base */
    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') {
                base = 16;
                s++;
            } else {
                base = 8;
            }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        /* Skip optional 0x/0X prefix */
        if (*s == '0' && (*(s + 1) == 'x' || *(s + 1) == 'X'))
            s += 2;
    }

    unsigned long long acc   = 0;
    const char        *start = s;

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            digit = *s - 'A' + 10;
        else
            break;

        if (digit >= base)
            break;

        acc = acc * (unsigned long long)base + (unsigned long long)digit;
        s++;
    }

    if (endptr)
        *endptr = (char *)(s == start ? (const char *)s : s);

    return acc;
}

long strtol(const char *s, char **endptr, int base) {
    int                neg;
    unsigned long long val = ParseInt(s, endptr, base, &neg);

    /* Clamp to LONG_MAX/LONG_MIN on overflow */
    if (val > (unsigned long long)(neg ? -(unsigned long long)LONG_MIN : LONG_MAX)) {
        val = neg ? -(unsigned long long)LONG_MIN : (unsigned long long)LONG_MAX;
    }

    return neg ? -(long)val : (long)val;
}

unsigned long strtoul(const char *s, char **endptr, int base) {
    int                neg;
    unsigned long long val = ParseInt(s, endptr, base, &neg);
    if (neg)
        val = -val;
    return (unsigned long)val;
}

long long strtoll(const char *s, char **endptr, int base) {
    int                neg;
    unsigned long long val = ParseInt(s, endptr, base, &neg);

    if (val > (unsigned long long)(neg ? -((unsigned long long)LLONG_MIN) : LLONG_MAX))
        val = neg ? -((unsigned long long)LLONG_MIN) : (unsigned long long)LLONG_MAX;

    return neg ? -(long long)val : (long long)val;
}

unsigned long long strtoull(const char *s, char **endptr, int base) {
    int                neg;
    unsigned long long val = ParseInt(s, endptr, base, &neg);
    if (neg)
        val = -val;
    return val;
}

/* ====================================================================
 * Absolute value
 * ==================================================================== */

int abs(int x) {
    return x < 0 ? -x : x;
}

long labs(long x) {
    return x < 0 ? -x : x;
}

long long llabs(long long x) {
    return x < 0 ? -x : x;
}

/* ====================================================================
 * Pseudo-random number generator (LCG, BSD-style)
 * ==================================================================== */

static unsigned long s_rand_next = 1;

int rand(void) {
    s_rand_next = s_rand_next * 1103515245UL + 12345UL;
    return (int)((s_rand_next / 65536UL) % 32768UL);
}

void srand(unsigned int seed) {
    s_rand_next = seed;
}

/* ====================================================================
 * Binary search
 * ==================================================================== */

void *bsearch(const void *key,
              const void *base,
              size_t      nmemb,
              size_t      size,
              int (*compar)(const void *, const void *)) {
    const char *p  = (const char *)base;
    size_t      lo = 0, hi = nmemb;

    while (lo < hi) {
        size_t      mid  = lo + (hi - lo) / 2;
        const void *midp = p + mid * size;
        int         c    = compar(key, midp);
        if (c == 0)
            return (void *)midp;
        else if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* ====================================================================
 * Quick sort (simple Hoare partition, unoptimised)
 * ==================================================================== */

static void Swap(char *a, char *b, size_t size) {
    for (size_t i = 0; i < size; i++) {
        char t = a[i];
        a[i]   = b[i];
        b[i]   = t;
    }
}

static void QsortRange(
    char *base, size_t lo, size_t hi, size_t size, int (*compar)(const void *, const void *)) {
    if (lo >= hi)
        return;

    /* Hoare partition with middle pivot */
    size_t pivot     = lo + (hi - lo) / 2;
    char  *pivot_ptr = base + pivot * size;

    size_t i = lo, j = hi;
    while (1) {
        while (compar(base + i * size, pivot_ptr) < 0)
            i++;
        while (compar(base + j * size, pivot_ptr) > 0)
            j--;
        if (i >= j)
            break;
        Swap(base + i * size, base + j * size, size);
        /* If we swapped the pivot, update pivot_ptr */
        if (i == pivot)
            pivot_ptr = base + j * size;
        else if (j == pivot)
            pivot_ptr = base + i * size;
        i++;
        j--;
    }

    if (j > lo)
        QsortRange(base, lo, j, size, compar);
    if (i < hi)
        QsortRange(base, i, hi, size, compar);
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (nmemb <= 1)
        return;
    QsortRange((char *)base, 0, nmemb - 1, size, compar);
}

/* ====================================================================
 * assert() support — __assert_fail
 * ==================================================================== */

void __assert_fail(const char *expr, const char *file, int line) {
    printf("ASSERTION FAILED: %s (%s:%d)\n", expr, file, line);
    abort();
}

/* ====================================================================
 * Process termination (C11 §7.22.4)
 * ==================================================================== */

#define QUICK_ATEXIT_MAX 32
static void (*s_quick_atexit[QUICK_ATEXIT_MAX])(void);
static int s_quick_atexit_count = 0;

_Noreturn void abort(void) {
    /* No SIGABRT delivery in v0.1 — terminate directly. */
    ThreadExit(134); /* 128 + SIGABRT(6) per POSIX */
    __builtin_unreachable();
}

_Noreturn void _Exit(int status) {
    ThreadExit(status);
    __builtin_unreachable();
}

int at_quick_exit(void (*func)(void)) {
    if (s_quick_atexit_count >= QUICK_ATEXIT_MAX)
        return -1;
    s_quick_atexit[s_quick_atexit_count++] = func;
    return 0;
}

_Noreturn void quick_exit(int status) {
    for (int i = s_quick_atexit_count - 1; i >= 0; i--) {
        if (s_quick_atexit[i])
            s_quick_atexit[i]();
    }
    _Exit(status);
}

/* ====================================================================
 * Environment (C11 §7.22.4.6-7)
 *
 * Process-local environment: a NULL-terminated array of "NAME=value"
 * strings.  The array is heap-allocated and grown on demand; entries
 * are strdup'd on Setenv(putenv installs the caller's string as-is,
 * matching POSIX).  Thread-safety: the shell is single-threaded at
 * env-mutation points; concurrent setenv from multiple threads is not
 * a supported pattern (documented).
 *
 * Design note (v0.5): the environment carries ONLY per-process user
 * preferences (PS1, EDITOR, LANG, ...).  It deliberately does NOT
 * carry security policy — command availability is decided by the
 * policy service (Capability → Policy DB → shell override), never by
 * environment variables.  See docs/permission_model.md.
 * ==================================================================== */

/* Initial capacity and growth step for the env pointer array. */
#define ENV_INIT_CAP 8

char **environ = NULL; /* NULL-terminated "NAME=value" array */

static size_t s_env_count = 0; /* entries in use (excl. NULL terminator) */
static size_t s_env_cap   = 0; /* allocated slots (incl. NULL terminator) */

/* NAME is valid iff non-empty and contains no '='. */
static int EnvNameValid(const char *name) {
    if (!name || name[0] == '\0')
        return 0;
    for (const char *p = name; *p; p++)
        if (*p == '=')
            return 0;
    return 1;
}

/* Index of the entry whose NAME matches (returns -1 when absent). */
static long EnvFind(const char *name) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i < s_env_count; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=')
            return (long)i;
    }
    return -1;
}

char *getenv(const char *name) {
    if (!name || !environ)
        return NULL;
    long i = EnvFind(name);
    if (i < 0)
        return NULL;
    char *eq = strchr(environ[i], '=');
    return eq ? eq + 1 : NULL;
}

int Setenv(const char *name, const char *value, int overwrite) {
    if (!EnvNameValid(name) || !value)
        return -1;

    long i = EnvFind(name);
    if (i >= 0 && !overwrite)
        return -1; /* already set and overwrite disallowed */

    /* Compose "NAME=value". */
    size_t nlen = strlen(name), vlen = strlen(value);
    char  *entry = malloc(nlen + vlen + 2);
    if (!entry)
        return -1;
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value, vlen);
    entry[nlen + 1 + vlen] = '\0';

    if (i >= 0) {
        /* Replace: drop the old string. */
        free(environ[i]);
        environ[i] = entry;
        return 0;
    }

    /* Append: ensure capacity. */
    if (s_env_count + 2 > s_env_cap) {
        size_t   new_cap = s_env_cap ? s_env_cap * 2 : ENV_INIT_CAP;
        char   **new_arr = malloc(new_cap * sizeof(char *));
        if (!new_arr) {
            free(entry);
            return -1;
        }
        if (environ) {
            memcpy(new_arr, environ, (s_env_count + 1) * sizeof(char *));
            free(environ);
        }
        environ   = new_arr;
        s_env_cap = new_cap;
    }
    environ[s_env_count++] = entry;
    environ[s_env_count]   = NULL;
    return 0;
}

int Unsetenv(const char *name) {
    if (!EnvNameValid(name))
        return -1;
    long i = EnvFind(name);
    if (i < 0)
        return 0; /* not set: success, nothing to do */
    free(environ[i]);
    /* Shift the tail (including the NULL terminator). */
    for (size_t j = (size_t)i; j < s_env_count; j++)
        environ[j] = environ[j + 1];
    s_env_count--;
    return 0;
}

int Putenv(char *string) {
    if (!string)
        return -1;
    char *eq = strchr(string, '=');
    if (!eq || eq == string)
        return -1; /* must contain '=' and a non-empty NAME */

    /* NAME = [string, eq).  Temporarily split for the lookup. */
    char saved = *eq;
    *eq        = '\0';
    long i     = EnvFind(string);
    *eq        = saved;

    if (i >= 0) {
        free(environ[i]);
        environ[i] = string; /* caller-owned, not copied (POSIX) */
        return 0;
    }

    /* Append (same growth path as setenv). */
    if (s_env_count + 2 > s_env_cap) {
        size_t   new_cap = s_env_cap ? s_env_cap * 2 : ENV_INIT_CAP;
        char   **new_arr = malloc(new_cap * sizeof(char *));
        if (!new_arr)
            return -1;
        if (environ) {
            memcpy(new_arr, environ, (s_env_count + 1) * sizeof(char *));
            free(environ);
        }
        environ   = new_arr;
        s_env_cap = new_cap;
    }
    environ[s_env_count++] = string;
    environ[s_env_count]   = NULL;
    return 0;
}

int System(const char *string) {
    /* No shell execution in v0.1.  Per C11: if string is NULL,
     * return 0 (no command processor available). */
    (void)string;
    return 0;
}

/* ====================================================================
 * aligned_alloc (C11 §7.22.3.1)
 * ==================================================================== */

void *aligned_alloc(size_t alignment, size_t size) {
    /* malloc already returns 16-byte-aligned payloads. */
    if (alignment <= 16)
        return malloc(size);

    /* Over-allocate by alignment + header and adjust. */
    size_t total = size + alignment + sizeof(void *);
    void  *raw   = malloc(total);
    if (!raw)
        return NULL;

    /* Align past the header pointer. */
    uintptr_t addr = (uintptr_t)raw + sizeof(void *);
    addr           = (addr + alignment - 1) & ~(uintptr_t)(alignment - 1);

    /* Store the raw pointer just before the aligned address. */
    ((void **)addr)[-1] = raw;
    return (void *)addr;
}
