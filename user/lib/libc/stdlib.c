/*
 * stdlib.c - Standard library utilities
 * Copyright (c) 2026 OpSys Project
 *
 * Numeric conversion, random numbers, searching, sorting.
 * String/memory functions now live in string.c.
 */

/* Limits for strtol/strtoll — defined here because GCC freestanding
 * limits.h can't chain to the system version via #include_next.
 */
#ifndef LONG_MIN
#define LONG_MIN        (-2147483647L - 1L)
#endif
#ifndef LONG_MAX
#define LONG_MAX        2147483647L
#endif
#ifndef LLONG_MIN
#define LLONG_MIN       (-9223372036854775807LL - 1LL)
#endif
#ifndef LLONG_MAX
#define LLONG_MAX       9223372036854775807LL
#endif

#include "stdlib.h"
#include "stdio.h"     /* for __assert_fail */
#include "../libos/syscalls.h"

/* ====================================================================
 * Numeric conversion
 * ==================================================================== */

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s)
{
    return strtol(s, NULL, 10);
}

long long atoll(const char *s)
{
    return strtoll(s, NULL, 10);
}

/*
 * Core integer parser: parse a string in the given base (0 or 2-36).
 * If base == 0, auto-detect: 0x → 16, 0 → 8, else → 10.
 * Skips leading whitespace and handles an optional +/- sign.
 */
static unsigned long long parse_int(const char *s, char **endptr,
                                     int base, int *neg)
{
    *neg = 0;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\v' ||
           *s == '\f' || *s == '\r')
        s++;

    /* Sign */
    if (*s == '-') { *neg = 1; s++; }
    else if (*s == '+') s++;

    /* Auto-detect base */
    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
            else                        { base = 8; }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        /* Skip optional 0x/0X prefix */
        if (*s == '0' && (*(s+1) == 'x' || *(s+1) == 'X'))
            s += 2;
    }

    unsigned long long acc = 0;
    const char *start = s;

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

long strtol(const char *s, char **endptr, int base)
{
    int neg;
    unsigned long long val = parse_int(s, endptr, base, &neg);

    /* Clamp to LONG_MAX/LONG_MIN on overflow */
    if (val > (unsigned long long)(neg ? -(unsigned long long)LONG_MIN : LONG_MAX)) {
        val = neg ? -(unsigned long long)LONG_MIN : (unsigned long long)LONG_MAX;
    }

    return neg ? -(long)val : (long)val;
}

unsigned long strtoul(const char *s, char **endptr, int base)
{
    int neg;
    unsigned long long val = parse_int(s, endptr, base, &neg);
    if (neg) val = -val;
    return (unsigned long)val;
}

long long strtoll(const char *s, char **endptr, int base)
{
    int neg;
    unsigned long long val = parse_int(s, endptr, base, &neg);

    if (val > (unsigned long long)(neg ? -((unsigned long long)LLONG_MIN) : LLONG_MAX))
        val = neg ? -((unsigned long long)LLONG_MIN) : (unsigned long long)LLONG_MAX;

    return neg ? -(long long)val : (long long)val;
}

unsigned long long strtoull(const char *s, char **endptr, int base)
{
    int neg;
    unsigned long long val = parse_int(s, endptr, base, &neg);
    if (neg) val = -val;
    return val;
}

/* ====================================================================
 * Absolute value
 * ==================================================================== */

int abs(int x)
{
    return x < 0 ? -x : x;
}

long labs(long x)
{
    return x < 0 ? -x : x;
}

long long llabs(long long x)
{
    return x < 0 ? -x : x;
}

/* ====================================================================
 * Pseudo-random number generator (LCG, BSD-style)
 * ==================================================================== */

static unsigned long s_rand_next = 1;

int rand(void)
{
    s_rand_next = s_rand_next * 1103515245UL + 12345UL;
    return (int)((s_rand_next / 65536UL) % 32768UL);
}

void srand(unsigned int seed)
{
    s_rand_next = seed;
}

/* ====================================================================
 * Binary search
 * ==================================================================== */

void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*compar)(const void *, const void *))
{
    const char *p = (const char *)base;
    size_t lo = 0, hi = nmemb;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void *midp = p + mid * size;
        int c = compar(key, midp);
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

static void swap(char *a, char *b, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        char t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

static void qsort_range(char *base, size_t lo, size_t hi,
                        size_t size, int (*compar)(const void *, const void *))
{
    if (lo >= hi) return;

    /* Hoare partition with middle pivot */
    size_t pivot = lo + (hi - lo) / 2;
    char *pivot_ptr = base + pivot * size;

    size_t i = lo, j = hi;
    while (1) {
        while (compar(base + i * size, pivot_ptr) < 0) i++;
        while (compar(base + j * size, pivot_ptr) > 0) j--;
        if (i >= j) break;
        swap(base + i * size, base + j * size, size);
        /* If we swapped the pivot, update pivot_ptr */
        if (i == pivot) pivot_ptr = base + j * size;
        else if (j == pivot) pivot_ptr = base + i * size;
        i++;
        j--;
    }

    if (j > lo) qsort_range(base, lo, j, size, compar);
    if (i < hi) qsort_range(base, i, hi, size, compar);
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    if (nmemb <= 1) return;
    qsort_range((char *)base, 0, nmemb - 1, size, compar);
}

/* ====================================================================
 * assert() support — __assert_fail
 * ==================================================================== */

void __assert_fail(const char *expr, const char *file, int line)
{
    printf("ASSERTION FAILED: %s (%s:%d)\n", expr, file, line);
    /* TODO: call abort() when we have signal support */
    for (;;)
        thread_yield();
}
