/*
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

int    atoi(const char *s);
long   atol(const char *s);
long long atoll(const char *s);

long           strtol(const char *s, char **endptr, int base);
unsigned long  strtoul(const char *s, char **endptr, int base);
long long      strtoll(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);

/* ====================================================================
 * Absolute value
 * ==================================================================== */

int   abs(int x);
long  labs(long x);
long long llabs(long long x);

/* ====================================================================
 * Pseudo-random number generation (linear congruential)
 * ==================================================================== */

int  rand(void);
void srand(unsigned int seed);

/* ====================================================================
 * Searching / sorting
 * ==================================================================== */

void *bsearch(const void *key, const void *base, size_t nmemb,
                            size_t size, int (*compar)(const void *, const void *));

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

/* ====================================================================
 * Process termination (C11 §7.22.4)
 * ==================================================================== */

/* Normal termination — already declared in runtime.h but also
 * accessible here for code that only includes <stdlib.h>. */
_Noreturn void abort(void);
_Noreturn void exit(int status);
_Noreturn void _Exit(int status);
int atexit(void (*func)(void));
int at_quick_exit(void (*func)(void));
_Noreturn void quick_exit(int status);

/* ====================================================================
 * Environment (C11 §7.22.4.6-7)
 * ==================================================================== */

char *getenv(const char *name);
int   system(const char *string);

/* ====================================================================
 * Memory (C11 §7.22.3)
 * ==================================================================== */

/* aligned_alloc is C11: allocate `size` bytes at `alignment` boundary.
 * The existing malloc already returns 16-byte-aligned payloads, so for
 * alignment <= 16 we delegate to malloc.  Larger alignments are served
 * by over-allocating and adjusting. */
void *aligned_alloc(size_t alignment, size_t size);

#endif /* LIBC_STDLIB_H */
