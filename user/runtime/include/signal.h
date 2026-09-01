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
 * signal.h - POSIX-style signal definitions for user-space
 * Copyright (c) 2026 OpSys Project
 *
 * Compatibility header.  The canonical signal API (constants, types,
 * Signal()/Kill()) lives in <libos/syscalls.h>, which mirrors
 * kernel/include/kernel/signal.h.  This header exists so code written
 * against the POSIX name <signal.h> still compiles unchanged.
 *
 * Note: only the kernel-implemented signal subset is exposed.  In
 * particular SIGCHLD/SIGCONT are NOT defined — the kernel has no
 * child-status tracking, and Kill(pid, SIGCHLD) would return ERR_INVAL.
 */

#ifndef SIGNAL_H
#define SIGNAL_H

#include <libos/syscalls.h>

/* ====================================================================
 * Signal handling best practices (comments for user code)
 * ==================================================================== */

/*
 * Signal-safe functions:
 *   - write()
 *   - Signal(), Kill()
 *   - exit(), _exit()
 *   - Async-safe list per POSIX 1003.1-2004
 *
 * NOT signal-safe:
 *   - malloc(), free(), realloc()  (v0.1)
 *   - printf()
 *   - any libc function with internal locks
 *   - pthread_* functions
 *
 * To safely allocate in a signal handler (v0.1):
 *   1. Pre-allocate a buffer in main()
 *   2. Use it in the handler without malloc
 *   OR
 *   3. Set a flag and return; handle in main loop
 */

/*
 * Default actions for each signal:
 *
 *   SIGKILL      - Terminate (always, cannot be caught)
 *   SIGUSR1      - Terminate
 *   SIGSEGV      - Terminate
 *   SIGUSR2      - Terminate
 *   SIGPIPE      - Terminate
 *   SIGALRM      - Terminate
 *   SIGTERM      - Terminate
 *   SIGSTOP      - Stop process (uncatchable, unignorable)
 */

/*
 * Signal masks (v1.0+ feature):
 *
 *   sigprocmask()    - Block/unblock signals (per-thread in v1.0)
 *   sigemptyset()    - Clear a signal set
 *   sigfillset()     - Fill a signal set
 *   sigaddset()      - Add signal to set
 *   sigdelset()      - Remove signal from set
 *   sigismember()    - Test membership
 *   sigpending()     - Query pending signals
 *   sigsuspend()     - Sleep until signal
 *
 * Not implemented in v0.1 (process-wide only).
 */

#endif /* SIGNAL_H */
