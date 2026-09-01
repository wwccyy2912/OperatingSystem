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
 * main.c - Flaky demo service (independent process)
 * Copyright (c) 2026 OpSys Project
 *
 * Exists ONLY to prove the manager's monitor/restart loop: sleeps a few
 * ticks then exits with code 7 (a simulated crash).  Deliberately given
 * no caps and no IRQ bindings.
 *
 * Runs as its OWN process, spawned by the manager via
 * SYS_PROCESS_CREATE (manager fetches this image as the "flaky" blob).
 * main() returning 7 -> crt0 -> exit(7) -> ThreadExit(7); because this
 * is the process's only thread, the kernel marks the process ZOMBIE
 * with exit code 7 and wakes the manager's SYS_PROCESS_WAIT caller —
 * which then applies the restart policy.
 */

#include <libos/syscalls.h>

int main(void) {
    Sleep(5); /* ~0.05 s at 100 Hz — 4 runs ≈ 0.2 s */
    return 7;
}
