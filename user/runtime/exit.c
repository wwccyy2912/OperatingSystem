/*
 * exit.c - Process termination, atexit, and global destructors
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
static int s_atexit_count = 0;

int atexit(atexit_func_t func)
{
        if (s_atexit_count >= ATEXIT_MAX)
                return -1;
        s_atexit[s_atexit_count++] = func;
        return 0;
}

void exit(int code)
{
        /* Call registered atexit handlers in reverse order */
        for (int i = s_atexit_count - 1; i >= 0; i--) {
                if (s_atexit[i])
                        s_atexit[i]();
        }

        /* Run global destructors (.fini_array) */
        _fini();

        _exit(code);
}

void _exit(int code)
{
        (void)code;
        thread_exit(code);
        __builtin_unreachable();
}
