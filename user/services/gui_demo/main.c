/*
 * main.c - GUI demo application (v0.7.1, GUI round)
 * Copyright (c) 2026 OpSys Project
 *
 * Spawned by the shell's `gui` command.  Activates the compositor,
 * opens three windows and exercises the event path:
 *   - "Keys"    echoes typed characters (one line, wraps)
 *   - "Canvas"  paints a colored square wherever the left button clicks
 *   - "Info"    live pointer position + button state
 * Press 'q' (or Esc) to quit: windows are destroyed, the compositor is
 * deactivated (term redraws its text screen) and the process exits.
 */

#include "../gui/gui.h"

#include <libc/stdio.h>
#include <libc/string.h>
#include <libos/syscalls.h>

static int s_port = -1;

static int gui_call(u32 op, const void *payload, u32 payload_len, void *resp, u32 resp_cap) {
    static gui_req_t req;
    memset(&req, 0, sizeof(req));
    req.op  = op;
    req.len = payload_len;
    if (payload_len > 0 && payload)
        memcpy(req.data, payload, payload_len);
    int rlen = (int)resp_cap;
    return ipc_call(s_port, &req, (int)(8 + payload_len), resp, &rlen);
}

static int gui_activate(void) {
    gui_resp_t r;
    memset(&r, 0, sizeof(r));
    return gui_call(GUI_OP_ACTIVATE, NULL, 0, &r, sizeof(r));
}

static int gui_deactivate(void) {
    gui_resp_t r;
    memset(&r, 0, sizeof(r));
    return gui_call(GUI_OP_DEACTIVATE, NULL, 0, &r, sizeof(r));
}

static int gui_create(const char *title, int w, int h) {
    gui_req_create_t c;
    memset(&c, 0, sizeof(c));
    strncpy(c.title, title, sizeof(c.title) - 1);
    c.w = w;
    c.h = h;
    gui_resp_t r;
    memset(&r, 0, sizeof(r));
    if (gui_call(GUI_OP_CREATE, &c, sizeof(c), &r, sizeof(r)) < 0 || r.ret < 0)
        return -1;
    return r.ret;
}

static int gui_destroy(int id) {
    gui_resp_t r;
    memset(&r, 0, sizeof(r));
    return gui_call(GUI_OP_DESTROY, &id, 4, &r, sizeof(r));
}

static int gui_move(int id, int x, int y) {
    i32        a[3] = {id, x, y};
    gui_resp_t r;
    memset(&r, 0, sizeof(r));
    return gui_call(GUI_OP_MOVE, a, sizeof(a), &r, sizeof(r));
}

static int gui_fill(int id, int x, int y, int w, int h, u32 color) {
    gui_req_fill_t f;
    memset(&f, 0, sizeof(f));
    f.id = id;
    f.x  = x;
    f.y  = y;
    f.w  = w;
    f.h  = h;
    f.color = color;
    gui_resp_t r;
    memset(&r, 0, sizeof(r));
    return gui_call(GUI_OP_FILL, &f, sizeof(f), &r, sizeof(r));
}

static int gui_text(int id, int x, int y, const char *s, u32 fg, u32 bg) {
    gui_req_text_t t;
    memset(&t, 0, sizeof(t));
    t.id = id;
    t.x  = x;
    t.y  = y;
    t.fg = fg;
    t.bg = bg;
    strncpy(t.text, s, sizeof(t.text) - 1);
    gui_resp_t r;
    memset(&r, 0, sizeof(r));
    return gui_call(GUI_OP_TEXT, &t, (u32)(20 + strlen(t.text) + 1), &r, sizeof(r));
}

static int gui_poll(gui_resp_poll_t *ev, int *count) {
    memset(ev, 0, sizeof(*ev));
    if (gui_call(GUI_OP_POLL, NULL, 0, ev, sizeof(*ev)) < 0)
        return -1;
    *count = (int)ev->count;
    return 0;
}

/* ---- demo state ---- */

#define WIN_W 300
#define WIN_H 180

static const u32 s_colors[] = {
    0x00FF4040, 0x0040FF40, 0x004040FF, 0x00FFFF40,
    0x0040FFFF, 0x00FF40FF, 0x00FFFFFF,
};

int main(void) {
    printf("gui_demo: starting\n");

    printf("gui_demo: resolving gui port\n");
    s_port = port_get(GUI_PORT_NAME);
    if (s_port < 0) {
        printf("gui_demo: 'gui' port unavailable (%d)\n", s_port);
        return 1;
    }

    printf("gui_demo: activating\n");
    if (gui_activate() < 0) {
        printf("gui_demo: ACTIVATE failed\n");
        return 1;
    }

    printf("gui_demo: creating windows\n");
    int w1 = gui_create("Keys (type; q quits)", WIN_W, WIN_H);
    int w2 = gui_create("Canvas (click to paint)", WIN_W, WIN_H);
    int w3 = gui_create("Info", 240, 96);
    printf("gui_demo: windows %d %d %d\n", w1, w2, w3);
    if (w1 < 0 || w2 < 0 || w3 < 0) {
        printf("gui_demo: window create failed (%d %d %d)\n", w1, w2, w3);
        gui_deactivate();
        return 1;
    }
    gui_move(w2, 60, 60);
    gui_move(w3, 400, 300);

    /* Window-local coordinates for click hit-conversion. */
    int wx2 = 60, wy2 = 60;

    /* Initial content. */
    gui_fill(w2, 0, 0, WIN_W, WIN_H, 0x00101010);
    gui_text(w1, 4, 4, "OpSys GUI demo", 0x00FFFF80, 0);
    gui_text(w1, 4, 24, "Type keys below - they echo here.", 0x00C0C0C0, 0);
    gui_text(w2, 8, 8, "Click anywhere to paint.", 0x00C0C0C0, 0);

    char    line[64];
    int     lpos = 0;
    int     lrow = 44; /* next echo row */
    int     color_idx = 0;

    for (;;) {
        gui_resp_poll_t ev;
        int             n = 0;
        if (gui_poll(&ev, &n) < 0)
            break;
        for (int i = 0; i < n; i++) {
            gui_event_t *e = &ev.events[i];
            if (e->type == GUI_EV_KEY) {
                u8 ch = (u8)e->code;
                if (ch == 'q' || ch == 'Q' || ch == 27)
                    goto done; /* quit */
                if (ch == '\r' || ch == '\n') {
                    lrow += 20;
                    lpos = 0;
                    if (lrow > WIN_H - 20) {
                        gui_fill(w1, 0, 0, WIN_W, WIN_H, 0x00101018);
                        lrow = 44;
                    }
                } else if (ch >= ' ' && ch < 0x7F) {
                    if (lpos < (int)sizeof(line) - 2) {
                        line[lpos++] = (char)ch;
                        line[lpos]   = '\0';
                        gui_text(w1, 4, lrow, line, 0x00FFFFFF, 0x00101018);
                    }
                }
            } else if (e->type == GUI_EV_BUTTON && e->code == 1) {
                /* Press: paint a square in the Canvas window. */
                int cx = e->x - wx2 - 1;
                int cy = e->y - wy2 - GUI_TITLE_H - 1;
                if (cx >= 0 && cy >= 0 && cx + 12 <= WIN_W && cy + 12 <= WIN_H) {
                    gui_fill(w2, cx, cy, 12, 12, s_colors[color_idx % 7]);
                    color_idx++;
                }
            } else if (e->type == GUI_EV_MOUSEMOVE) {
                /* Info window: pointer position. */
                char info[64];
                snprintf(info, sizeof(info), "ptr %d,%d", e->x, e->y);
                gui_fill(w3, 4, 4, 200, 20, 0x00101010);
                gui_text(w3, 4, 4, info, 0x00FFFFFF, 0);
            }
        }
        (void)sleep(1);
    }

done:
    gui_destroy(w1);
    gui_destroy(w2);
    gui_destroy(w3);
    gui_deactivate();
    printf("gui_demo: exited cleanly\n");
    return 0;
}
