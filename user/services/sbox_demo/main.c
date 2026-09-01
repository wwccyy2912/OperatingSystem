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
 * main.c - Sandbox demo: manifest-driven capability flow
 * Copyright (c) 2026 OpSys Project
 *
 * Demonstrates docs/ops_format.md §5 (沙盒授权流) / §8.4-8.5 (验收):
 * the app is installed via the pkg port (shell: pkg install sbox_demo
 * --perms=...), reports APP_READY through libpkg so pkg-manager signs
 * the manifest atoms into OUR kernel cap table, then exercises the
 * ATOM_SYS_SET_TIME-gated OsSetTime() syscall (syscall.c:818).
 *
 * The same ELF is installed twice — once with perms=sys.set_time
 * (expect set_time OK) and once with no permissions (expect DENIED) —
 * so this app never hardcodes an expectation: it only reports the real
 * OsSetTime() return value.  It also attempts a self-grant via
 * CapCreateAtom(), which must fail with ERR_NOCAP (the syscall is
 * gated on ATOM_CAP_GRANT_SELF, a management atom no app may hold).
 *
 * It is embedded into kernel.elf as a blob (build/sbox_demo_blob.o)
 * and spawned by pkg-manager via SYS_PROCESS_CREATE — not by manager.
 * Its process name is set to the app_id chosen at .ops install time.
 */

#include <libc/stdio.h>
#include <libos/syscalls.h>
#include <libpkg/pkg.h>

int main(void) {
    int r = PkgReady();
    printf("sbox_demo: PkgReady() = %d\n", r);
    Sleep(50);

    /* A valid wall-clock value (2026). */
    rtc_time_t t;
    t.year   = 2026;
    t.month  = 8;
    t.day    = 15;
    t.hour   = 12;
    t.minute = 0;
    t.second = 0;

    int ret = OsSetTime(&t);
    if (ret == 0)
        printf("set_time OK\n");
    else if (ret == ERR_NOCAP)
        printf("set_time DENIED (no permission)\n");
    else
        printf("set_time failed: %d\n", ret);
    Sleep(50);

    /* Self-grant attempt: cap_create_atom is gated on
     * ATOM_CAP_GRANT_SELF (docs/ops_format.md §6), which no app can
     * declare or hold — this must return ERR_NOCAP. */
    int h = CapCreateAtom(ATOM_SYS_SET_TIME, RIGHT_ALL, 0, 0, 0);
    printf("self-grant attempt = %d (expect ERR_NOCAP)\n", h);
    Sleep(50);

    return 0;
}
