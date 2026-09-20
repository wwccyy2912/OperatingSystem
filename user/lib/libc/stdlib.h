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
 * stdlib.h - Standard library utilities
 * Copyright (c) 2026 OpSys Project
 *
 * Contains string.h for backward compatibility (string functions
 * were historically declared here).  New code should include
 * <string.h> directly.
 */

#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#include <stddef.h>

/* Pull in string.h for backward compat — existing code includes
 * <stdlib.h> for strlen/memset etc. */
#include "string.h"

/* ====================================================================
 * Numeric conversion
 * ==================================================================== */

int       atoi(const char *s);
long      atol(const char *s);
long long atoll(const char *s);

long               strtol(const char *s, char **endptr, int base);
unsigned long      strtoul(const char *s, char **endptr, int base);
long long          strtoll(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);

/* ====================================================================
 * Absolute value
 * ==================================================================== */

int       abs(int x);
long      labs(long x);
long long llabs(long long x);

/* ====================================================================
 * Pseudo-random number generation (linear congruential)
 * ==================================================================== */

int  rand(void);
void srand(unsigned int seed);

/* ====================================================================
 * Searching / sorting
 * ==================================================================== */

void *bsearch(const void *key,
              const void *base,
              size_t      nmemb,
              size_t      size,
              int (*compar)(const void *, const void *));

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

/* ====================================================================
 * Process termination (C11 §7.22.4)
 * ==================================================================== */

/* Normal termination — already declared in runtime.h but also
 * accessible here for code that only includes <stdlib.h>. */
_Noreturn void abort(void);
_Noreturn void exit(int status);
_Noreturn void _Exit(int status);
int            Atexit(void (*func)(void));
int            at_quick_exit(void (*func)(void));
_Noreturn void quick_exit(int status);

/* ====================================================================
 * Environment (C11 §7.22.4.6-7)
 * ==================================================================== */

/* The process environment: NULL-terminated array of "NAME=value"
 * strings, owned by libc.  May be NULL when the environment is empty.
 * v0.5: real environment support (was a stub returning NULL). */
extern char **environ;

/* Look up NAME in the environment; returns the value string (never
 * the "NAME=" prefix), or NULL when NAME is not set. */
char *getenv(const char *name);

/* Set NAME=value, replacing an existing entry (overwrite != 0) or
 * failing with -1 when NAME exists and overwrite == 0.  Strings are
 * copied into libc-owned storage.  Returns 0 on success. */
int Setenv(const char *name, const char *value, int overwrite);

/* Remove NAME from the environment.  Returns 0 on success (also when
 * NAME was not set), -1 on invalid NAME. */
int Unsetenv(const char *name);

/* Install "NAME=value" (string must remain valid; not copied).
 * Replaces any existing NAME entry.  Returns 0 on success. */
int Putenv(char *string);

int System(const char *string);

/* ====================================================================
 * Memory (C11 §7.22.3)
 * ==================================================================== */

/* aligned_alloc is C11: allocate `size` bytes at `alignment` boundary.
 * The existing malloc already returns 16-byte-aligned payloads, so for
 * alignment <= 16 we delegate to malloc.  Larger alignments are served
 * by over-allocating and adjusting. */
void *aligned_alloc(size_t alignment, size_t size);

/* POSIX: allocate `size` bytes at `alignment` (a power of two and a
 * multiple of sizeof(void *)); the pointer is written to *memptr.
 * Returns 0 on success, EINVAL for a bad alignment, ENOMEM on failure
 * (note: unlike malloc it reports errors through the return value, not
 * errno).  Implemented by the runtime heap (user/runtime/malloc.c). */
int posix_memalign(void **memptr, size_t alignment, size_t size);

/* ====================================================================
 * Integer division (C11 7.22.6)
 * ==================================================================== */

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

div_t   div(int numer, int denom);
ldiv_t  ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);

/* ====================================================================
 * Floating-point / string conversion (C11 7.22.1.3) — parsing only,
 * the libc has no floating-point printf.
 * ==================================================================== */

double      atof(const char *s);
double      strtod(const char *s, char **endptr);
float       strtof(const char *s, char **endptr);
long double strtold(const char *s, char **endptr);

/* ====================================================================
 * Multibyte / wide conversion (C11 7.22.8) — UTF-8 is the only
 * supported multibyte encoding.
 * ==================================================================== */

size_t mbstowcs(wchar_t *dest, const char *src, size_t n);
size_t wcstombs(char *dest, const wchar_t *src, size_t n);

#endif /* LIBC_STDLIB_H */
