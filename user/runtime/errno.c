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
 * errno.c - Error number storage and the POSIX accessor
 * Copyright (c) 2026 OpSys Project
 *
 * PROCESS-GLOBAL, NOT THREAD-LOCAL: there is no user-space TLS in
 * v0.1 — Ring 3 runs with GS.base = 0 (kernel/arch/x86_64/
 * syscall_entry.S:318, kernel/sched/sched.c:187) and the thread library
 * documents "slots are process-wide (not per-thread) until TLS support"
 * (user/lib/libc/threads.c:334).  All threads of a process therefore
 * share the single __errno object below; a multi-threaded program may
 * only use errno as a hint (POSIX requires per-thread storage, so this
 * is a documented v0.1 limitation, not a silent one).
 *
 * Both names in <errno.h> resolve to the same storage:
 *
 *     #define errno __errno          -> the plain global below
 *     int *__errno_location(void)     -> its address
 *
 * so code compiled against either idiom reads and writes one object.
 *
 * ------------------------------------------------------------------
 * Structure (errno):
 *   __errno (one int in .bss) <- errno macro / __errno_location().
 * How it works:
 *   The accessor returns the address of the global; a function that can
 *   fail assigns the POSIX error code through it (errno = EINVAL), and
 *   nothing else clears it — errno is only meaningful immediately after
 *   a call that reported failure.
 * Purpose:
 *   Give libc and user code the standard POSIX error-reporting channel
 *   without a TLS slot.
 * Caveats:
 *   - Shared by every thread of the process (see above).
 *   - errno is zero-initialised by the image (.bss) and is not reset by
 *     successful calls, exactly as POSIX specifies.
 *   - A compiler may cache the plain global across calls within one
 *     function; the accessor __errno_location() exists so code that
 *     re-reads errno around calls can take its address instead.
 * ------------------------------------------------------------------
 */

#include <errno.h>

/* Process-global errno storage (zero at startup: .bss). */
int __errno = 0;

/* Standard POSIX accessor: the address of this process's errno storage.
 * Kept as a real function (not a macro) so shared code can take the
 * address and so a future TLS-backed implementation only has to change
 * this body. */
int *__errno_location(void) {
    return &__errno;
}
