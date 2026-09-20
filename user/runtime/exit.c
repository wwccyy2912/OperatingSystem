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
 * exit() runs the C atexit handlers in reverse order (LIFO), then the
 * C++ static destructors registered through __cxa_atexit() (also LIFO),
 * then the global destructors (.fini_array via _fini()), and finally
 * calls _exit().  _exit() terminates via the SYS_THREAD_EXIT syscall.
 *
 * Ordering rationale: .init_array constructors run first and the C++
 * destructors they register are the mirror image of that construction,
 * so __cxa_finalize() runs before _fini() — a destructor always sees
 * objects that were constructed before it and destroyed objects that
 * were constructed after it.
 *
 * ------------------------------------------------------------------
 * Structure (exit):
 *   Atexit() -> s_atexit[] (32 slots, LIFO) and
 *   __cxa_atexit() -> s_cxa[] (8 slots, LIFO) -> exit() drains both,
 *   then _fini() (.fini_array) -> _exit() (SYS_THREAD_EXIT).
 * How it works:
 *   Both tables are plain static arrays; exit() snapshots/clears each
 *   slot BEFORE calling it, so a handler that re-enters exit() or
 *   registers another handler can never make one run twice or make the
 *   loop grow without bound.
 * Purpose:
 *   Deterministic process teardown for C and C++ user programs.
 * Caveats:
 *   - A full table is a hard error: Atexit()/__cxa_atexit() return
 *     non-zero and leave the existing entries untouched (they are never
 *     overwritten, so no cleanup is silently dropped).
 *   - Handlers must not depend on other threads: exit() tears the
 *     process down from the calling thread only (the kernel kills the
 *     rest when the last thread exits).
 *   - __cxa_finalize(dso) with a non-NULL handle only runs the entries
 *     registered for that handle; this runtime links static ELFs, so
 *     the handle is a grouping key and never a real shared object.
 * ------------------------------------------------------------------
 */

#include <runtime.h>
#include <libos/syscalls.h>

/* ====================================================================
 * C atexit handlers (POSIX/C11)
 * ==================================================================== */

/* Maximum number of atexit handlers */
#define ATEXIT_MAX 32

/* Registered atexit handlers (FIRST registered = LAST called) */
static atexit_func_t s_atexit[ATEXIT_MAX];
static int           s_atexit_count = 0;

int Atexit(atexit_func_t func) {
    /* Full table (or a NULL handler): fail loudly instead of dropping or
     * overwriting an entry that is already registered. */
    if (!func || s_atexit_count >= ATEXIT_MAX)
        return -1;
    s_atexit[s_atexit_count++] = func;
    return 0;
}

/* ====================================================================
 * C++ static destructors (Itanium C++ ABI)
 *
 *   int __cxa_atexit(void (*func)(void *), void *arg, void *dso);
 *   void __cxa_finalize(void *dso);
 *
 * The signature above is fixed by the ABI — GCC emits calls to it from
 * the static-destructor thunks it generates — so it must not gain an
 * OpSys-specific variant; the third argument is the "home DSO" handle
 * used by __cxa_finalize() to select a group.  A single static ELF has
 * no shared objects, so callers pass NULL (or the address of the image's
 * own base) and __cxa_finalize(NULL) means "run every entry", which is
 * exactly what exit() needs.
 * ==================================================================== */

/* Capacity of the C++ destructor table (small on purpose: GCC only
 * registers here when a translation unit has static objects with
 * destructors, and the alternative — silently running no destructor —
 * is worse than reporting the overflow). */
#define CXA_ATEXIT_MAX 8

typedef struct {
    void (*func)(void *);
    void *arg;
    void *dso;
} cxa_entry_t;

static cxa_entry_t s_cxa[CXA_ATEXIT_MAX];
static int         s_cxa_count = 0;

int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
    if (!func || s_cxa_count >= CXA_ATEXIT_MAX)
        return -1; /* non-zero = registration failed, per the ABI */
    s_cxa[s_cxa_count].func = func;
    s_cxa[s_cxa_count].arg  = arg;
    s_cxa[s_cxa_count].dso  = dso;
    s_cxa_count++;
    return 0;
}

void __cxa_finalize(void *dso) {
    /* Consume the selected entries before running any of them: a
     * destructor that registers a new handler (or calls
     * __cxa_finalize() again) must not make this loop re-run an entry. */
    cxa_entry_t run[CXA_ATEXIT_MAX];
    int         n    = 0;
    int         kept = 0;

    for (int i = 0; i < s_cxa_count; i++) {
        if (dso == NULL || s_cxa[i].dso == dso)
            run[n++] = s_cxa[i];
        else
            s_cxa[kept++] = s_cxa[i];
    }
    s_cxa_count = kept;

    /* LIFO: the mirror image of the construction order */
    for (int i = n - 1; i >= 0; i--) {
        if (run[i].func)
            run[i].func(run[i].arg);
    }
}

/* ====================================================================
 * exit / _exit
 * ==================================================================== */

/* Set while exit() is tearing the process down: a handler that calls
 * exit() again jumps straight to _exit() instead of replaying the
 * tables. */
static int s_exiting = 0;

void exit(int code) {
    if (s_exiting)
        _exit(code); /* nested exit from a handler: finish now */

    s_exiting = 1;

    /* 1. C atexit handlers, LIFO.  Each slot is cleared before its
     *    handler runs (run-once, re-entrancy safe). */
    for (int i = s_atexit_count - 1; i >= 0; i--) {
        atexit_func_t func = s_atexit[i];

        s_atexit[i] = NULL;
        if (func)
            func();
    }
    s_atexit_count = 0;

    /* 2. C++ static destructors registered through __cxa_atexit(). */
    __cxa_finalize(NULL);

    /* 3. Global destructors (.fini_array, reverse order) and 4. the
     *    kernel-side thread/process teardown. */
    _fini();

    _exit(code);
}

void _exit(int code) {
    (void)code;
    ThreadExit(code);
    __builtin_unreachable();
}
