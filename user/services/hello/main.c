/*
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
 * NOTE: every printf is followed by sleep(50) — the kernel rate-limits
 * SYS_DEBUG_LOG to 2048 bytes per scheduler tick (sys_debug_log in
 * syscall.c), and the self-test emits ~700 bytes total; without the
 * sleep the last lines would be silently truncated by the budget.
 *
 * The pacing assumes a stable PIT-driven clock: s_ticks advances ONLY
 * from the PIT IRQ0 (100 Hz) via sched_tick(); explicit blocking paths
 * (yield/join/wait/sleep/exit) use sched_reschedule() which does NOT
 * advance the clock.  sleep(50) therefore spans 0.5 s of wall time.
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
static void sigusr1_handler(int signum) {
    (void)signum;
    s_usr1_count++;
    printf("hello: SIGUSR1 handler ran (delivery #%d)\n", s_usr1_count);
    sleep(50);
}

int main(void) {
    int pid = get_pid();
    printf("hello: spawned process running (pid=%d)\n", pid);
    sleep(50);

    /* 1. Register a handler: signal() returns the previous handler,
     *    which must be SIG_DFL (0) since nothing was set before. */
    sighandler_t prev = signal(SIGUSR1, sigusr1_handler);
    printf("hello: signal(SIGUSR1) -> prev=0x%x (expect 0x0)\n", (unsigned int)(uintptr_t)prev);
    sleep(50);

    /* 2. Self-kill: the pending bit is latched; the Ring 3 dispatcher
     *    runs at the next delivery checkpoint (the kill syscall's own
     *    return path), calls this handler and then restores the
     *    interrupted context via SYS_SIGRETURN, so the kill() call
     *    below returns normally. */
    int ret = kill(pid, SIGUSR1);
    printf("hello: kill(self, SIGUSR1) -> %d (expect 0), count=%d (expect 1)\n", ret, s_usr1_count);
    sleep(50);

    /* 3. Deliver again: a registered handler stays installed, so the
     *    count must reach 2. */
    ret = kill(pid, SIGUSR1);
    printf("hello: kill #2 -> %d, count=%d (expect 2)\n", ret, s_usr1_count);
    sleep(50);

    /* 4. SIG_IGN: the dispatcher discards the signal at delivery, so
     *    the count must stay 2. */
    signal(SIGUSR1, SIG_IGN);
    ret = kill(pid, SIGUSR1);
    printf("hello: kill after SIG_IGN -> %d, count=%d (expect 2)\n", ret, s_usr1_count);
    sleep(50);

    /* 5. SIG_DFL: SIGUSR1's default action is ignore, so the process
     *    must survive and the count stays 2. */
    signal(SIGUSR1, SIG_DFL);
    ret = kill(pid, SIGUSR1);
    printf("hello: kill after SIG_DFL -> %d, count=%d (expect 2)\n", ret, s_usr1_count);
    sleep(50);

    /* 6. Re-register and confirm a final delivery. */
    signal(SIGUSR1, sigusr1_handler);
    kill(pid, SIGUSR1);
    printf("hello: final delivery -> count=%d (expect 3)\n", s_usr1_count);
    sleep(50);

    if (s_usr1_count == 3)
        printf("hello: signal self-test PASSED\n");
    else
        printf("hello: signal self-test FAILED (count=%d)\n", s_usr1_count);
    sleep(50);

    /* 7. Default action = terminate: SIGTERM with SIG_DFL must kill us
     *    (exit code 128+15=143) via the Ring 3 dispatcher's default
     *    action (exit(128+signum)).  Nothing after this line ever runs. */
    signal(SIGTERM, SIG_DFL);
    printf("hello: sending SIGTERM (SIG_DFL) to self - expect termination\n");
    sleep(50);
    kill(pid, SIGTERM);
    printf("hello: ERROR - survived SIGTERM!\n");

    printf("hello: exiting\n");
    return 0;
}
