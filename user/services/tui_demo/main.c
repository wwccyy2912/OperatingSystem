/*
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

static void demo_main(void) {
    printf("[tui-demo] Starting TUI demonstration\n");

    /* Verify terminal is available */
    int port = tui_port_get();
    if (port < 0) {
        printf("[tui-demo] Terminal service unavailable (%d)\n", port);
        return;
    }
    printf("[tui-demo] Terminal port: %d\n", port);

    /* Clear the screen */
    int ret = tui_clear();
    if (ret < 0) {
        printf("[tui-demo] tui_clear failed (%d)\n", ret);
        return;
    }

    /* Draw a title box at the top */
    tui_render_box(2, 1, 70, 5, "OpSys TUI Demo v1.0");

    /* Render welcome message inside the box */
    tui_render_line_at(4, 3, "Welcome to the TUI demonstration!", 34);
    tui_render_line_at(4, 4, "Using the new framebuffer terminal service", 43);

    /* Move cursor and write some text */
    tui_set_cursor(2, 7);
    tui_write_str("Status: ");
    tui_write_str("[OK]\n");

    tui_write_str("Features demonstrated:\n");
    tui_write_str("  - Status bar rendering\n");
    tui_write_str("  - Box drawing with titles\n");
    tui_write_str("  - Cursor control\n");
    tui_write_str("  - Formatted output\n");

    /* Render another box: input area */
    tui_render_box(2, 13, 70, 5, "Input Area");
    tui_render_line_at(4, 14, "You can now interact with the system.", 37);
    tui_render_line_at(4, 15, "Type commands in the shell to control the TUI.", 47);

    /* Render status bar at the bottom */
    tui_render_status("System", "TUI Demo Ready - Use shell commands");

    printf("[tui-demo] Demo rendering complete\n");

    /* Keep running for a bit so the display persists */
    sleep(50);

    printf("[tui-demo] Demo finished\n");
}

int main(void) {
    demo_main();
    return 0;
}
