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
 * exit.c - Process termination, Atexit, and global destructors
 * Copyright (c) 2026 OpSys Project
 *
 * exit() calls atexit handlers in reverse order, then runs global
 * destructors (.fini_array via _fini()), then calls _exit().
 * _exit() terminates via the SYS_THREAD_EXIT syscall.
 *
 * Ordering: atexit handlers run first (LIFO), then .fini_array
 * destructors (reverse).  This matches the common C runtime contract
 * — atexit-registered cleanups see fully-constructed global state,
 * and destructors run as the final teardown phase.
 */

#include <runtime.h>
#include <libos/syscalls.h>

/* Maximum number of atexit handlers */
#define ATEXIT_MAX 32

/* Registered atexit handlers (FIRST registered = LAST called) */
static atexit_func_t s_atexit[ATEXIT_MAX];
static int           s_atexit_count = 0;

int Atexit(atexit_func_t func) {
    if (s_atexit_count >= ATEXIT_MAX)
        return -1;
    s_atexit[s_atexit_count++] = func;
    return 0;
}

void exit(int code) {
    /* Call registered atexit handlers in reverse order */
    for (int i = s_atexit_count - 1; i >= 0; i--) {
        if (s_atexit[i])
            s_atexit[i]();
    }

    /* Run global destructors (.fini_array) */
    _fini();

    _exit(code);
}

void _exit(int code) {
    (void)code;
    ThreadExit(code);
    __builtin_unreachable();
}
