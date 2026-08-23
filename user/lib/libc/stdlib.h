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
int            atexit(void (*func)(void));
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
int setenv(const char *name, const char *value, int overwrite);

/* Remove NAME from the environment.  Returns 0 on success (also when
 * NAME was not set), -1 on invalid NAME. */
int unsetenv(const char *name);

/* Install "NAME=value" (string must remain valid; not copied).
 * Replaces any existing NAME entry.  Returns 0 on success. */
int putenv(char *string);

int system(const char *string);

/* ====================================================================
 * Memory (C11 §7.22.3)
 * ==================================================================== */

/* aligned_alloc is C11: allocate `size` bytes at `alignment` boundary.
 * The existing malloc already returns 16-byte-aligned payloads, so for
 * alignment <= 16 we delegate to malloc.  Larger alignments are served
 * by over-allocating and adjusting. */
void *aligned_alloc(size_t alignment, size_t size);

#endif /* LIBC_STDLIB_H */
