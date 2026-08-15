/*
 * assert.h - Debug assertion macro
 * Copyright (c) 2026 OpSys Project
 *
 * If NDEBUG is defined before inclusion, assert() expands to nothing.
 * Otherwise, if the expression evaluates to false, prints a diagnostic
 * message to the debug log and calls abort (via __assert_fail).
 */

#ifndef LIBC_ASSERT_H
#define LIBC_ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else

void __assert_fail(const char *expr, const char *file, int line);

#define assert(expr) ((void)((expr) ? 0 : (__assert_fail(#expr, __FILE__, __LINE__), 0)))

#endif /* NDEBUG */

#endif /* LIBC_ASSERT_H */
