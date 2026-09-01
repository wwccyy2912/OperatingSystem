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
 * init.c - C runtime initialization and finalization
 * Copyright (c) 2026 OpSys Project
 *
 * _init() iterates .init_array (global constructors) on startup, in
 * forward order (first registered = first called).
 * _fini() iterates .fini_array (global destructors) during exit(), in
 * reverse order (last registered = first called) — mirroring atexit
 * semantics so construction/destruction pairs are LIFO.
 *
 * Works with crt0.S which calls _init() before main(); exit.c calls
 * _fini() after atexit handlers.  The linker script (user.ld) defines
 * __init_array_start/End and __fini_array_start/End symbols from the
 * corresponding sections.
 */

#include <runtime.h>

/* Seed the per-process stack canary before ANY constructor runs (see
 * stack_chk.c): instrumented constructors read __stack_chk_guard in
 * their prologue, so the guard must be randomized first. */
extern void __stack_chk_init(void);

void _init(void) {
    __stack_chk_init();
    for (init_func_t *p = __init_array_start; p < __init_array_end; p++) {
        if (*p)
            (*p)();
    }
}

void _fini(void) {
    for (init_func_t *p = __fini_array_end; p > __fini_array_start;) {
        p--;
        if (*p)
            (*p)();
    }
}
