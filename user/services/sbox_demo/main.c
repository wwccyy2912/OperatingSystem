/*
 * main.c - Sandbox demo: manifest-driven capability flow
 * Copyright (c) 2026 OpSys Project
 *
 * Demonstrates docs/ops_format.md §5 (沙盒授权流) / §8.4-8.5 (验收):
 * the app is installed via the pkg port (shell: pkg install sbox_demo
 * --perms=...), reports APP_READY through libpkg so pkg-manager signs
 * the manifest atoms into OUR kernel cap table, then exercises the
 * ATOM_SYS_SET_TIME-gated os_set_time() syscall (syscall.c:780).
 *
 * The same ELF is installed twice — once with perms=sys.set_time
 * (expect set_time OK) and once with no permissions (expect DENIED) —
 * so this app never hardcodes an expectation: it only reports the real
 * os_set_time() return value.  It also attempts a self-grant via
 * cap_create_atom(), which must fail with ERR_NOCAP (the syscall is
 * gated on ATOM_CAP_GRANT_SELF, a management atom no app may hold).
 *
 * It is embedded into kernel.elf as a blob (build/sbox_demo_blob.o)
 * and spawned by pkg-manager via SYS_PROCESS_CREATE — not by manager.
 * Its process name is set to the app_id chosen at .ops install time.
 */

#include <libc/stdio.h>
#include <libos/syscalls.h>
#include <libpkg/pkg.h>

int main(void)
{
        int r = pkg_ready();
        printf("sbox_demo: pkg_ready() = %d\n", r);
        sleep(50);

        /* A valid wall-clock value (2026). */
        rtc_time_t t;
        t.year = 2026;
        t.month = 8;
        t.day = 15;
        t.hour = 12;
        t.minute = 0;
        t.second = 0;

        int ret = os_set_time(&t);
        if (ret == 0)
                printf("set_time OK\n");
        else if (ret == ERR_NOCAP)
                printf("set_time DENIED (no permission)\n");
        else
                printf("set_time failed: %d\n", ret);
        sleep(50);

        /* Self-grant attempt: cap_create_atom is gated on
         * ATOM_CAP_GRANT_SELF (docs/ops_format.md §6), which no app can
         * declare or hold — this must return ERR_NOCAP. */
        int h = cap_create_atom(ATOM_SYS_SET_TIME, RIGHT_ALL, 0, 0, 0);
        printf("self-grant attempt = %d (expect ERR_NOCAP)\n", h);
        sleep(50);

        return 0;
}
