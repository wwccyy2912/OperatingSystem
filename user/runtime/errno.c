/*
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

int *__errno_location(void)
{
        return &__errno;
}
