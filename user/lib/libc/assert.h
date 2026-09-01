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
