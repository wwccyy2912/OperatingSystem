/*
 * main.c - Flaky demo service (independent process)
 * Copyright (c) 2026 OpSys Project
 *
 * Exists ONLY to prove the manager's monitor/restart loop: sleeps a few
 * ticks then exits with code 7 (a simulated crash).  Deliberately given
 * no caps and no IRQ bindings.
 *
 * Runs as its OWN process, spawned by the manager via
 * SYS_PROCESS_CREATE (manager fetches this image as the "flaky" blob).
 * main() returning 7 -> crt0 -> exit(7) -> thread_exit(7); because this
 * is the process's only thread, the kernel marks the process ZOMBIE
 * with exit code 7 and wakes the manager's SYS_PROCESS_WAIT caller —
 * which then applies the restart policy.
 */

#include <libos/syscalls.h>

int main(void)
{
    sleep(5);               /* ~0.05 s at 100 Hz — 4 runs ≈ 0.2 s */
    return 7;
}
