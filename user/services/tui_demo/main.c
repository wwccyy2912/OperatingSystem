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
 * tui_demo.c - TUI demonstration application
 * Copyright (c) 2026 OpSys Project
 *
 * Demonstrates the new TUI library features:
 *   - Status bar rendering
 *   - Box drawing
 *   - Cursor control
 *   - Formatted output
 *
 * Compile and embed as a service blob.
 */

#include "../lib/libc/stdio.h"
#include "../lib/libc/string.h"
#include "../lib/libos/syscalls.h"
#include "../lib/libtui/tui.h"
#include <stdint.h>

static void DemoMain(void) {
    printf("[tui-demo] Starting TUI demonstration\n");

    /* Verify terminal is available */
    int port = TuiPortGet();
    if (port < 0) {
        printf("[tui-demo] Terminal service unavailable (%d)\n", port);
        return;
    }
    printf("[tui-demo] Terminal port: %d\n", port);

    /* Clear the screen */
    int ret = TuiClear();
    if (ret < 0) {
        printf("[tui-demo] tui_clear failed (%d)\n", ret);
        return;
    }

    /* Draw a title box at the top */
    TuiRenderBox(2, 1, 70, 5, "OpSys TUI Demo v1.0");

    /* Render welcome message inside the box */
    TuiRenderLineAt(4, 3, "Welcome to the TUI demonstration!", 34);
    TuiRenderLineAt(4, 4, "Using the new framebuffer terminal service", 43);

    /* Move cursor and write some text */
    TuiSetCursor(2, 7);
    TuiWriteStr("Status: ");
    TuiWriteStr("[OK]\n");

    TuiWriteStr("Features demonstrated:\n");
    TuiWriteStr("  - Status bar rendering\n");
    TuiWriteStr("  - Box drawing with titles\n");
    TuiWriteStr("  - Cursor control\n");
    TuiWriteStr("  - Formatted output\n");

    /* Render another box: input area */
    TuiRenderBox(2, 13, 70, 5, "Input Area");
    TuiRenderLineAt(4, 14, "You can now interact with the system.", 37);
    TuiRenderLineAt(4, 15, "Type commands in the shell to control the TUI.", 47);

    /* Render status bar at the bottom */
    TuiRenderStatus("System", "TUI Demo Ready - Use shell commands");

    printf("[tui-demo] Demo rendering complete\n");

    /* Keep running for a bit so the display persists */
    Sleep(50);

    printf("[tui-demo] Demo finished\n");
}

int main(void) {
    DemoMain();
    return 0;
}
