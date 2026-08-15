/*
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

void _init(void)
{
        for (init_func_t *p = __init_array_start; p < __init_array_end; p++) {
                if (*p)
                        (*p)();
        }
}

void _fini(void)
{
        for (init_func_t *p = __fini_array_end; p > __fini_array_start; ) {
                p--;
                if (*p)
                        (*p)();
        }
}
