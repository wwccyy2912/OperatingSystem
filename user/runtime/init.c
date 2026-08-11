/*
 * init.c - C runtime initialization
 * Copyright (c) 2026 OpSys Project
 *
 * Iterates .init_array (global constructors) on startup.
 * Works with crt0.S which calls _init() before main().
 * The linker script defines __init_array_start/End symbols
 * from the .init_array section.
 */

#include <runtime.h>

void _init(void)
{
    for (init_func_t *p = __init_array_start; p < __init_array_end; p++) {
        if (*p)
            (*p)();
    }
}
