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
 * errno.c - Per-thread error number storage
 * Copyright (c) 2026 OpSys Project
 *
 * Single global __errno for v0.1 (single-threaded user runtime).
 * __errno_location() returns its address — the standard POSIX accessor
 * that lets libc and compiled code reach errno without the macro.
 * Multi-threaded user-space will require TLS-backed errno (v1.0+).
 */

#include <errno.h>

int __errno = 0;

int *__errno_location(void) {
    return &__errno;
}
