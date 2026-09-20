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
 * _fini() after the atexit/C++ destructor tables.  The linker script
 * (user.ld) defines __init_array_start/End and __fini_array_start/End
 * symbols from the corresponding sections.
 *
 * ------------------------------------------------------------------
 * Structure (init):
 *   _start (crt0.S) -> _init() -> { __stack_chk_init(), .init_array
 *   forward } -> main() -> exit() -> { atexit LIFO, __cxa_finalize(),
 *   _fini() reverse, _exit() }.
 * How it works:
 *   Both arrays are the linker-provided [start, end) symbol pairs; the
 *   runtime walks them directly, so no dynamic loader or C++ runtime
 *   support library is involved (constructors are plain function
 *   pointers, empty slots are skipped).
 * Purpose:
 *   Run C/C++ static constructors before main() and their destructors
 *   after it, in a fixed and documented order.
 * Caveats:
 *   - INITIALIZATION ORDER: the user heap has no init step — malloc()
 *     creates it lazily on the first call (HeapGrow() fetches the ASLR
 *     heap base from the kernel and maps the first chunk) — so a
 *     constructor may allocate, free and print safely.  What MUST run
 *     first is __stack_chk_init(): every instrumented constructor reads
 *     __stack_chk_guard in its prologue, so seeding the canary after
 *     them would compare against the pre-seeded value.
 *   - The signal dispatcher is installed by a constructor
 *     (signal_user.c) and the kernel simply keeps a signal pending until
 *     a dispatcher exists, so a signal raised before the constructors
 *     have run cannot be lost.
 *   - _init()/_fini() are one-shot: a second call is ignored, so a
 *     service that calls _init() itself cannot run constructors twice
 *     (and destructors cannot double-free global state).
 *   - Constructors run in .init_array order, which is the order the
 *     linker saw the objects; code must not depend on one translation
 *     unit's constructor running before another's.
 * ------------------------------------------------------------------
 */

#include <runtime.h>

/* Seed the per-process stack canary before ANY constructor runs (see
 * stack_chk.c): instrumented constructors read __stack_chk_guard in
 * their prologue, so the guard must be randomized first. */
extern void __stack_chk_init(void);

/* One-shot guards (see Caveats). */
static int s_initialized   = 0;
static int s_finalized     = 0;

void _init(void) {
    if (s_initialized)
        return;
    s_initialized = 1;

    /* 1. Canary first: constructors are compiled with
     *    -fstack-protector-strong and check the guard on entry. */
    __stack_chk_init();

    /* 2. Static constructors, forward order. */
    for (init_func_t *p = __init_array_start; p < __init_array_end; p++) {
        if (*p)
            (*p)();
    }
}

void _fini(void) {
    if (s_finalized)
        return;
    s_finalized = 1;

    /* Static destructors, reverse order (mirror of _init). */
    for (init_func_t *p = __fini_array_end; p > __fini_array_start;) {
        p--;
        if (*p)
            (*p)();
    }
}
