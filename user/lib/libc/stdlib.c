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
 *   atoi/strtol family -> ParseNumber; strtod/atof -> ParseDouble
 *   (utf8-free, digit-accumulating); malloc/calloc/realloc/free and
 *   posix_memalign -> runtime heap (malloc.c); getenv/setenv -> static
 *   environ table; mbstowcs/wcstombs -> utf8.c + wchar.c helpers.
 * How it works:
 *   Numeric parsers delegate to a shared digit reader; memory calls
 *   forward to the runtime allocator; environ is a fixed-size array
 *   copied from the process image.
 * Purpose:
 *   C standard-library basics for user services.
 * Caveats:
 *   environ has a fixed cap; strtol overflow follows C99 (clamped,
 *   errno-style); strtod truncates past 19 significant digits and
 *   long double is aliased to double (see the section comment).
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
#include "math.h"   /* for HUGE_VAL / isinf / Ldexp */
#include "utf8.h"   /* for Utf8Decode (mbstowcs) */
#include "wchar.h"  /* for wcrtomb (wcstombs) */
#include <errno.h>  /* for ERANGE / EINVAL */
#include <malloc.h> /* for malloc (aligned_alloc) */
#include <stdint.h> /* for uintptr_t */
#include "../libos/syscalls.h"

/* Unsigned 64-bit limits — defined here for the same reason as the
 * signed ones above (freestanding GCC headers do not chain). */
#ifndef ULLONG_MAX
#define ULLONG_MAX 18446744073709551615ULL
#endif

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
 * Integer division (C11 §7.22.6)
 *
 * C99/C11 integer division truncates toward zero, so quot/rem satisfy
 * quot * denom + rem == numer exactly as the standard requires; the
 * remainder therefore keeps the sign of the numerator.  Divisor 0 (and
 * INT_MIN / -1) is undefined behaviour, as in C.
 * ==================================================================== */

div_t div(int numer, int denom) {
    div_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

lldiv_t lldiv(long long numer, long long denom) {
    lldiv_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
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
 * aligned_alloc (C11 §7.22.3.1) / posix_memalign (POSIX)
 *
 * Both functions are implemented by the runtime heap, next to the rest
 * of the allocator (user/runtime/malloc.c): only that code can hand out
 * over-aligned blocks that the same free()/realloc() can release again.
 * <stdlib.h> declares them because that is where C programs look for
 * them, and every user binary links the runtime, so the symbols are
 * always resolved.  They are deliberately NOT defined here — an
 * interior-pointer fallback in libc would break free() for any caller
 * that asked for alignment > 16.
 * ==================================================================== */

/* ====================================================================
 * Floating-point / string conversion (C11 §7.22.1.3)
 *
 * strtod() parses the C11 "subject sequence": optional whitespace, an
 * optional sign, then one of
 *     digits [. digits] [(e|E) [sign] digits]           decimal
 *     0x hexdigits [. hexdigits] [(p|P)[sign] digits]   hex float
 *     0x . hexdigits (p|P) [sign] digits                hex, no int part
 *     inf | infinity | nan | nan(n-char-sequence)
 * and sets *endptr to the first unconsumed byte (== s when nothing at
 * all converted).  atof/strtof/strtold are wrappers; long double is
 * aliased to double everywhere in v0.9 (see math.c), so strtold()
 * widens the double result instead of using an 80-bit x87 path.
 *
 * Caveats (deliberate, kept small enough to audit):
 *   - At most 19 significant digits are kept (64-bit mantissa); digits
 *     past that only move the exponent, so results are truncated, not
 *     rounded (at most ~1 ulp low).
 *   - The decimal scaler multiplies by exact powers of ten (1e0..1e22)
 *     in steps, which can add one extra rounding step.
 *   - Overflow sets ERANGE and returns ±HUGE_VAL; underflow (a nonzero
 *     value that flushes to zero) sets ERANGE and returns ±0.
 *   - No locale: the decimal point is always '.'.
 * ==================================================================== */

/* Exact powers of ten representable without rounding. */
static const double s_pow10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

/* mant * 10^exp.  The exponent is clamped first: a mantissa below 2^64
 * cannot survive ±350 decades, and clamping keeps the loop short. */
static double ScalePow10(double mant, int exp) {
    if (exp > 350)
        return HUGE_VAL;
    if (exp < -350)
        return 0.0;
    while (exp > 22) {
        mant *= s_pow10[22];
        exp -= 22;
    }
    while (exp < -22) {
        mant /= s_pow10[22];
        exp += 22;
    }
    if (exp > 0)
        mant *= s_pow10[exp];
    else if (exp < 0)
        mant /= s_pow10[-exp];
    return mant;
}

/* mant * 2^exp through math.c's Ldexp (x87 FSCALE).  The exponent is
 * clamped because FSCALE is only defined for a modest exponent range. */
static double ScalePow2(double mant, int exp) {
    if (exp > 1100)
        return HUGE_VAL;
    if (exp < -1200)
        return 0.0;
    return Ldexp(mant, exp);
}

/* Value of a digit byte, or -1 when it is not one. */
static int FloatDigitValue(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Case-insensitive word match at *p; on success advances *p past it. */
static int MatchWordCI(const char **p, const char *word) {
    const char *s = *p;
    const char *w = word;
    while (*w) {
        int c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';
        if (c != *w)
            return 0;
        s++;
        w++;
    }
    *p = s;
    return 1;
}

/* Parse "[sign] digits" after an exponent marker ('e'/'E'/'p'/'P').
 * Returns 1 and advances *pp when at least one digit follows, else 0
 * (the marker itself is then not part of the number). */
static int ParseExponent(const char **pp, int *out) {
    const char *r    = *pp + 1;
    int         sign = 1;
    if (*r == '+' || *r == '-') {
        if (*r == '-')
            sign = -1;
        r++;
    }
    if (*r < '0' || *r > '9')
        return 0;
    int val = 0;
    while (*r >= '0' && *r <= '9') {
        if (val < 100000) /* clamp: the scalers saturate anyway */
            val = val * 10 + (*r - '0');
        r++;
    }
    *out += sign * val;
    *pp = r;
    return 1;
}

static double ParseDouble(const char *s, char **endptr) {
    const char *p = s;

    /* Leading whitespace (isspace() in the "C" locale). */
    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;

    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }

    /* inf / infinity / nan — no digits are involved. */
    const char *q = p;
    if (MatchWordCI(&q, "infinity") || MatchWordCI(&q, "inf")) {
        if (endptr)
            *endptr = (char *)q;
        return neg ? -HUGE_VAL : HUGE_VAL;
    }
    if (MatchWordCI(&q, "nan")) {
        const char *r = q;
        if (*r == '(') {
            const char *t = r + 1;
            while ((*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'z') ||
                   (*t >= 'A' && *t <= 'Z') || *t == '_')
                t++;
            if (*t == ')')
                q = t + 1; /* full nan(n-char-sequence) form */
        }
        if (endptr)
            *endptr = (char *)q;
        return neg ? -__builtin_nan("") : __builtin_nan("");
    }

    /* Hex prefix.  It only counts as part of the number when at least
     * one hex digit follows, hence hex_rewind. */
    int         hex        = 0;
    const char *hex_rewind = NULL;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        hex        = 1;
        hex_rewind = p + 1; /* just past the leading '0' */
        p += 2;
    }

    int                base  = hex ? 16 : 10;
    unsigned long long mant  = 0;
    int                exp10 = 0;
    int                exp2  = 0;
    int                any   = 0;

    /* Integer part. */
    for (;;) {
        int d = FloatDigitValue((unsigned char)*p);
        if (d < 0 || d >= base)
            break;
        any = 1;
        if (mant <= (ULLONG_MAX - (unsigned long long)d) / (unsigned long long)base)
            mant = mant * (unsigned long long)base + (unsigned long long)d;
        else if (hex)
            exp2 += 4; /* dropped digit: shift the mantissa instead */
        else
            exp10++;
        p++;
    }

    /* Fractional part. */
    if (*p == '.') {
        p++;
        for (;;) {
            int d = FloatDigitValue((unsigned char)*p);
            if (d < 0 || d >= base)
                break;
            any = 1;
            if (mant <= (ULLONG_MAX - (unsigned long long)d) / (unsigned long long)base) {
                mant = mant * (unsigned long long)base + (unsigned long long)d;
                if (hex)
                    exp2 -= 4;
                else
                    exp10--;
            }
            p++;
        }
    }

    if (hex && !any) {
        /* "0x" with no hex digit: the subject sequence is the leading
         * '0' alone and 'x' stays unconsumed (C11 §7.22.1.3). */
        hex  = 0;
        p    = hex_rewind;
        any  = 1;
        mant = 0;
    }
    if (!any) {
        if (endptr)
            *endptr = (char *)s; /* nothing converted at all */
        return 0.0;
    }

    if (hex) {
        if (*p == 'p' || *p == 'P')
            (void)ParseExponent(&p, &exp2);
    } else if (*p == 'e' || *p == 'E') {
        (void)ParseExponent(&p, &exp10);
    }

    double val = hex ? ScalePow2((double)mant, exp2) : ScalePow10((double)mant, exp10);
    if (neg)
        val = -val;

    if (isinf(val)) {
        errno = ERANGE; /* overflow: ±HUGE_VAL per C11 */
        val   = neg ? -HUGE_VAL : HUGE_VAL;
    } else if (val == 0.0 && mant != 0) {
        errno = ERANGE; /* underflow: a nonzero value flushed to zero */
    }

    if (endptr)
        *endptr = (char *)p;
    return val;
}

/**
 * @brief Convert a string to double (C11 §7.22.1.3).
 * @param s       Subject string (never modified).
 * @param endptr  Optional; receives the first unconsumed byte.
 * @return The parsed value, 0.0 when nothing converted.
 */
double strtod(const char *s, char **endptr) {
    if (!s) {
        if (endptr)
            *endptr = NULL;
        return 0.0;
    }
    return ParseDouble(s, endptr);
}

double atof(const char *s) {
    return strtod(s, NULL);
}

float strtof(const char *s, char **endptr) {
    return (float)strtod(s, endptr);
}

long double strtold(const char *s, char **endptr) {
    return (long double)strtod(s, endptr);
}

/* ====================================================================
 * Multibyte <-> wide character conversion (C11 §7.22.8)
 *
 * UTF-8 is the only execution encoding (docs/i18n_design.md), so the
 * two conversions are thin wrappers over the shared helpers: decoding
 * uses Utf8Decode() from utf8.c, encoding uses wcrtomb() from wchar.c.
 *
 * Invalid input — a stray continuation byte, a truncated sequence, an
 * overlong encoding, a UTF-16 surrogate (U+D800..U+DFFF) or a code
 * point above U+10FFFF — makes the function stop, return (size_t)-1
 * and set errno.  The standard wants EILSEQ here; this tree's errno.h
 * has no EILSEQ yet, so the code reported is EINVAL.  Whatever was
 * already converted stays in the destination buffer (the caller must
 * ignore it).  A NULL @p src / @p dest is treated as "no string" and
 * yields 0 rather than trapping.
 * ==================================================================== */

/* Reject the sequences UTF-8 forbids but a naive decoder accepts. */
static int WideFromUtf8(uint32_t cp, int len) {
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0; /* UTF-16 surrogate: not a Unicode scalar value */
    if (len == 2 && cp < 0x80)
        return 0; /* overlong */
    if (len == 3 && cp < 0x800)
        return 0; /* overlong */
    if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))
        return 0; /* overlong / out of range */
    return 1;
}

/**
 * @brief Convert a UTF-8 string to wide characters (C11 §7.22.8.1).
 *
 * Stores at most @p n wide characters; the terminating L'\0' counts
 * against @p n and is written only when the whole string was
 * converted.  Nothing is written when @p dest is NULL (length query).
 *
 * @param dest  Destination array, or NULL to measure only.
 * @param src   NUL-terminated UTF-8 string, or NULL (= empty).
 * @param n     Capacity of @p dest in wide characters.
 * @return Number of wide characters stored (excluding the L'\0'), or
 *         (size_t)-1 on an invalid multibyte sequence (errno = EINVAL).
 */
size_t mbstowcs(wchar_t *dest, const char *src, size_t n) {
    if (!src)
        return 0;
    size_t      count = 0;
    const char *p     = src;

    while (*p) {
        uint32_t cp  = 0;
        int      len = Utf8Decode(p, &cp);
        if (len <= 0 || !WideFromUtf8(cp, len)) {
            errno = EINVAL;
            return (size_t)-1;
        }
        if (dest) {
            if (count >= n)
                break; /* capacity reached (n counts the terminator too) */
            dest[count] = (wchar_t)cp;
        }
        count++;
        p += len;
    }
    if (dest && count < n)
        dest[count] = L'\0';
    return count;
}

/**
 * @brief Convert a wide string to UTF-8 (C11 §7.22.8.2).
 *
 * Stores at most @p n bytes; the terminating NUL counts against @p n
 * and is written only when the whole string was converted.  Nothing is
 * written when @p dest is NULL (length query).
 *
 * @param dest  Destination buffer, or NULL to measure only.
 * @param src   NUL-terminated wide string, or NULL (= empty).
 * @param n     Capacity of @p dest in bytes.
 * @return Number of bytes stored (excluding the NUL), or (size_t)-1 on
 *         a surrogate or out-of-range code point (errno = EINVAL).
 */
size_t wcstombs(char *dest, const wchar_t *src, size_t n) {
    if (!src)
        return 0;
    size_t total = 0;

    for (const wchar_t *p = src; *p; p++) {
        char   tmp[4];
        size_t len = wcrtomb(tmp, *p, NULL);
        if (len == (size_t)-1) {
            errno = EINVAL;
            return (size_t)-1;
        }
        if (dest) {
            if (total + len > n)
                break; /* would not fit: stop without the terminator */
            memcpy(dest + total, tmp, len);
        }
        total += len;
    }
    if (dest && total < n)
        dest[total] = '\0';
    return total;
}

