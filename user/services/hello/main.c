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
 * main.c - Demo user program spawned via SYS_PROCESS_CREATE
 * Copyright (c) 2026 OpSys Project
 *
 * A minimal standalone program: prints its PID, exercises the signal
 * subsystem (register handler -> self-kill -> handler runs -> control
 * returns via the Ring 3 dispatcher's SYS_SIGRETURN), and exits.
 *
 * It is embedded into init.elf as a binary blob (build/hello_blob.o)
 * and spawned by the shell's "spawn" command to demonstrate dynamic
 * process creation from an ELF image in memory.
 *
 * NOTE: every printf is followed by Sleep(50) — the kernel rate-limits
 * SYS_DEBUG_LOG to 2048 bytes per scheduler tick (sys_debug_log in
 * syscall.c), and the self-test emits ~700 bytes total; without the
 * sleep the last lines would be silently truncated by the budget.
 *
 * The pacing assumes a stable PIT-driven clock: s_ticks advances ONLY
 * from the PIT IRQ0 (100 Hz) via sched_tick(); explicit blocking paths
 * (yield/join/wait/sleep/exit) use sched_reschedule() which does NOT
 * advance the clock.  Sleep(50) therefore spans 0.5 s of wall time.
 * The ~5 s self-test window leaves ample room for the external-kill
 * verification (spawn hello, then `kill <pid> 9` mid-run).
 */

#include <libc/stdio.h>
#include <libos/syscalls.h>

static volatile int s_usr1_count = 0; /* SIGUSR1 deliveries seen */

/*
 * Signal handler for SIGUSR1.  Runs on the interrupted thread's stack
 * below the sigframe; the Ring 3 dispatcher (user/runtime/
 * signal_user.c) calls it and restores the interrupted context via
 * SYS_SIGRETURN afterwards.  printf is safe here: it only makes a
 * debug_log syscall and uses its own stack buffer.
 */
static void Sigusr1Handler(int signum) {
    (void)signum;
    s_usr1_count++;
    printf("hello: SIGUSR1 handler ran (delivery #%d)\n", s_usr1_count);
    Sleep(50);
}

int main(void) {
    int pid = GetPid();
    printf("hello: spawned process running (pid=%d)\n", pid);
    Sleep(50);

    /* 1. Register a handler: Signal() returns the previous handler,
     *    which must be SIG_DFL (0) since nothing was set before. */
    sighandler_t prev = Signal(SIGUSR1, Sigusr1Handler);
    printf("hello: Signal(SIGUSR1) -> prev=0x%x (expect 0x0)\n", (unsigned int)(uintptr_t)prev);
    Sleep(50);

    /* 2. Self-kill: the pending bit is latched; the Ring 3 dispatcher
     *    runs at the next delivery checkpoint (the kill syscall's own
     *    return path), calls this handler and then restores the
     *    interrupted context via SYS_SIGRETURN, so the Kill() call
     *    below returns normally. */
    int ret = Kill(pid, SIGUSR1);
    printf("hello: Kill(self, SIGUSR1) -> %d (expect 0), count=%d (expect 1)\n", ret, s_usr1_count);
    Sleep(50);

    /* 3. Deliver again: a registered handler stays installed, so the
     *    count must reach 2. */
    ret = Kill(pid, SIGUSR1);
    printf("hello: kill #2 -> %d, count=%d (expect 2)\n", ret, s_usr1_count);
    Sleep(50);

    /* 4. SIG_IGN: the dispatcher discards the signal at delivery, so
     *    the count must stay 2. */
    Signal(SIGUSR1, SIG_IGN);
    ret = Kill(pid, SIGUSR1);
    printf("hello: kill after SIG_IGN -> %d, count=%d (expect 2)\n", ret, s_usr1_count);
    Sleep(50);

    /* 5. SIG_DFL: SIGUSR1's default action is ignore, so the process
     *    must survive and the count stays 2. */
    Signal(SIGUSR1, SIG_DFL);
    ret = Kill(pid, SIGUSR1);
    printf("hello: kill after SIG_DFL -> %d, count=%d (expect 2)\n", ret, s_usr1_count);
    Sleep(50);

    /* 6. Re-register and confirm a final delivery. */
    Signal(SIGUSR1, Sigusr1Handler);
    Kill(pid, SIGUSR1);
    printf("hello: final delivery -> count=%d (expect 3)\n", s_usr1_count);
    Sleep(50);

    if (s_usr1_count == 3)
        printf("hello: signal self-test PASSED\n");
    else
        printf("hello: signal self-test FAILED (count=%d)\n", s_usr1_count);
    Sleep(50);

    /* 7. Default action = terminate: SIGTERM with SIG_DFL must kill us
     *    (exit code 128+15=143) via the Ring 3 dispatcher's default
     *    action (exit(128+signum)).  Nothing after this line ever runs. */
    Signal(SIGTERM, SIG_DFL);
    printf("hello: sending SIGTERM (SIG_DFL) to self - expect termination\n");
    Sleep(50);
    Kill(pid, SIGTERM);
    printf("hello: ERROR - survived SIGTERM!\n");

    printf("hello: exiting\n");
    return 0;
}
