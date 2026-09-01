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
 * wm_demo.c - Desktop demo for the v0.4 window manager
 * Copyright (c) 2026 OpSys Project
 *
 * A sample desktop client: creates three windows through libwm
 * (Terminal / Files / Settings), fills their body content, starts the
 * desktop session, and lets the wm's input routing drive the demo
 * (1-9 focus, hjkl move, q quits — the wm handles the keys, this app
 * only observes and exits when the session ends).
 *
 * Run from the shell: exec wm_demo
 */

#include <libc/stdio.h>
#include <libc/string.h>
#include <stdint.h>
#include <libos/syscalls.h>
#include <libwm/wm.h>

static const struct {
    const char *title;
    uint32_t    x, y, w, h;
    const char *body[WM_CONTENT_ROWS];
} s_wins[3] = {
    {"Terminal", 2, 4, 36, 9,
     {"opsys$ ls", "/Users", "a.txt", "p2vfs.bin", "", "", "", ""}},
    {"Files", 40, 4, 36, 9,
     {"Volumes", "System (ro)", "Users (rw)", "Disk (rw)", "", "", "", ""}},
    {"Settings", 78, 4, 33, 9,
     {"User: admin (OWNER)", "Theme: default", "Keyboard: US", "Window manager: wm", "", "", "", ""}},
};

int main(void) {
    printf("[wm-demo] starting\n");

    if (WmPortGet() < 0) {
        printf("[wm-demo] wm service unavailable\n");
        return 1;
    }

    uint32_t ids[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        int r = WmCreate(s_wins[i].title, s_wins[i].x, s_wins[i].y,
                          s_wins[i].w, s_wins[i].h, &ids[i]);
        if (r < 0) {
            printf("[wm-demo] WmCreate(%s) failed (%d)\n", s_wins[i].title, r);
            return 1;
        }
        for (int row = 0; row < WM_CONTENT_ROWS; row++) {
            if (s_wins[i].body[row][0] != '\0')
                (void)WmWrite(ids[i], (uint32_t)row, s_wins[i].body[row]);
        }
        printf("[wm-demo] window '%s' id=%u at (%u,%u)\n",
               s_wins[i].title, ids[i], s_wins[i].x, s_wins[i].y);
    }

    /* Verify the registry reflects the three windows. */
    uint32_t n = 0;
    if (WmList(NULL, &n) == 0)
        printf("[wm-demo] registry has %u window(s)\n", n);

    int r = WmActivate();
    if (r < 0) {
        printf("[wm-demo] wm_activate failed (%d)\n", r);
        return 1;
    }
    printf("[wm-demo] desktop session started - 1-9 focus, hjkl move, q quit\n");

    /* Wait for the session to end (user pressed q in the wm), then
     * clean up: destroy our windows and exit. */
    for (;;) {
        uint32_t active = 0;
        if (WmGetState(&active, NULL, NULL) < 0)
            break;
        if (!active)
            break;
        Sleep(25);
    }

    for (int i = 0; i < 3; i++)
        (void)WmDestroy(ids[i]);
    printf("[wm-demo] session ended, windows destroyed, exiting\n");
    return 0;
}
