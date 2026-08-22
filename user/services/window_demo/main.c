/*
 * window_demo.c - Minimal windowing demo (v0.4 图形最小闭环)
 * Copyright (c) 2026 OpSys Project
 *
 * A simple "desktop" rendered through the term service (the display
 * owner): three windows (boxes with titles) + a status bar.  The demo
 * takes the keyboard focus and lets the user switch the focused
 * window with 1/2/3; 'q' quits and releases the focus.
 *
 * This is the v0.4 windowing closed loop on the EXISTING secure
 * architecture: rendering goes through term's IPC (never direct
 * framebuffer access — term is the atom-gated display owner), input
 * through the keyboard service's focus routing (only the focus owner
 * receives keys).  A future real window manager replaces the demo's
 * hardcoded layout with a window registry + compositor.
 *
 * Run from the shell: exec window_demo
 */

#include <libc/stdio.h>
#include <libc/string.h>
#include <stdint.h>
#include <libos/syscalls.h>
#include <libtui/tui.h>

#define WIN_COUNT 3

/* Window geometry (term cell grid: 113 x 38) */
static const struct {
    uint32_t         x, y, w, h;
    const char *title;
    const char *body[3];
} s_windows[WIN_COUNT] = {
    {2, 4, 33, 9, "Window 1", {"Terminal", "Shell + apps", "Focus: [1]"}},
    {38, 4, 33, 9, "Window 2", {"Files", "VFS browser", "Focus: [2]"}},
    {74, 4, 33, 9, "Window 3", {"Settings", "System config", "Focus: [3]"}},
};

static int s_focus = 0;

/* Redraw one window; the focused one carries a '*' marker and is drawn
 * last so it reads as "on top". */
static void draw_window(int idx, int focused) {
    char title[48];
    snprintf(title, sizeof(title), "%c %s", focused ? '*' : ' ', s_windows[idx].title);
    tui_render_box(s_windows[idx].x, s_windows[idx].y, s_windows[idx].w, s_windows[idx].h, title);
    for (int i = 0; i < 3; i++)
        tui_render_line_at(s_windows[idx].x + 2,
                           s_windows[idx].y + 2 + (uint32_t)i,
                           s_windows[idx].body[i],
                           (uint32_t)strlen(s_windows[idx].body[i]));
}

static void redraw(void) {
    for (int i = 0; i < WIN_COUNT; i++)
        draw_window(i, i == s_focus);
    char st[64];
    snprintf(st, sizeof(st), "window-demo: Focus = Window %d (1/2/3 switch, q quit)",
             s_focus + 1);
    tui_render_status("System", st);
}

/* Keyboard protocol (user/services/keyboard/keyboard.c) */
#define KBD_OP_READ_BLOCK    2
#define KBD_OP_TAKE_FOCUS    3
#define KBD_OP_RELEASE_FOCUS 4

static int kbd_req(uint32_t op, uint8_t *key) {
    static uint8_t s_req[8];
    static uint8_t s_resp[8];
    uint32_t      *h = (uint32_t *)s_req;
    h[0]        = op;
    h[1]        = 1; /* max data bytes */
    int resp_len = (int)sizeof(s_resp);
    int port     = port_get("keyboard");
    if (port < 0 || ipc_call(port, s_req, 8, s_resp, &resp_len) < 0 || resp_len < 4)
        return -1;
    if (key)
        *key = s_resp[4];
    return *(int *)s_resp;
}

int main(void) {
    printf("[window-demo] starting\n");

    if (tui_port_get() < 0) {
        printf("[window-demo] term service unavailable\n");
        return 1;
    }
    if (port_get("keyboard") < 0) {
        printf("[window-demo] keyboard service unavailable\n");
        return 1;
    }

    tui_clear();
    redraw();
    printf("[window-demo] desktop rendered, focus=1\n");

    /* Take the keyboard: while held, only this process receives keys. */
    if (kbd_req(KBD_OP_TAKE_FOCUS, NULL) < 0) {
        printf("[window-demo] TAKE_FOCUS failed\n");
        return 1;
    }

    for (;;) {
        uint8_t key = 0;
        if (kbd_req(KBD_OP_READ_BLOCK, &key) < 0)
            break;
        if (key >= '1' && key <= '3') {
            s_focus = key - '1';
            redraw();
            printf("[window-demo] focus=%d\n", s_focus + 1);
        } else if (key == 'q' || key == 'Q') {
            break;
        }
    }

    (void)kbd_req(KBD_OP_RELEASE_FOCUS, NULL);
    tui_render_status("System", "window-demo quit");
    printf("[window-demo] quit\n");
    return 0;
}
