/*
 * main.c - Window compositor service (v0.7.1, GUI round)
 * Copyright (c) 2026 OpSys Project
 *
 * Owns the display framebuffer (mapped via libgui, gated on
 * ATOM_SERVICE_MANAGE) and composes an off-screen window buffer per
 * window onto it.  Clients draw into their window over IPC (FILL/TEXT);
 * the compositor re-blits on every mutation and keeps the z-order so
 * the focused window sits on top with a highlighted title bar.
 *
 * Input: an input thread polls the keyboard (KBD_OP_READ) and the PS/2
 * mouse (KBD_OP_MOUSE_READ, both served by the keyboard service),
 * maintains the pointer position, hit-tests clicks to focus windows,
 * and pushes events into a shared ring drained by GUI_OP_POLL.
 *
 * Lifecycle: the service runs from boot but stays idle (no fb writes,
 * no focus) until a client calls GUI_OP_ACTIVATE; GUI_OP_DEACTIVATE
 * restores the term text screen via TERM_OP_REDRAW and releases the
 * keyboard focus.
 *
 * Two threads (mirror of term's perm.ui split): the server thread
 * handles the "gui" port; the input thread polls devices.  Both
 * serialize the window table / events / pointer on s_lock.
 */

#include "gui.h"

#include <libc/stdio.h>
#include <libc/string.h>
#include <libgui/gui.h>
#include <libos/syscalls.h>
#include <malloc.h> /* malloc / free for window buffers */

/* Keyboard service protocol (mirror of keyboard.c — not shared) */
#define KBD_OP_READ      1
#define KBD_OP_MOUSE_READ 5

#define GUI_BG_COLOR 0x00202040 /* desktop background */
#define GUI_BORDER_COLOR  0x00A0A0A0
#define GUI_TITLE_FG      0x00FFFFFF
#define GUI_TITLE_BG      0x00204080 /* focused */
#define GUI_TITLE_BG_IDLE 0x00404040 /* unfocused */
#define GUI_FRAME_FG      0x00C0C0C0
#define GUI_PTR_COLOR     0x00FF0000

/* IPC buffers (single server thread — same pattern as term/perm). */
static u8 s_req[GUI_IPC_MAX];
static u8 s_resp[GUI_IPC_MAX];

typedef struct {
    int          in_use;
    u64          owner; /* creating subject (unforgeable) */
    int          id;
    char         title[GUI_MAX_TITLE];
    i32          x, y; /* fb position of the border */
    i32          w, h; /* content size (excludes border+title bar) */
    gui_canvas_t buf;  /* off-screen content buffer (32bpp) */
} gui_win_t;

static gui_win_t      s_wins[GUI_MAX_WINDOWS];
static gui_canvas_t   s_fb;
static int            s_active;   /* compositor on */
static int            s_focus_id; /* focused window id (0 = none) */
static int            s_next_id = 1;

static i32 s_ptr_x;
static i32 s_ptr_y;
static u8  s_ptr_buttons;

/* Title-bar dragging state (input thread).  While a window is being
 * dragged, pointer motion moves the window instead of the hit test;
 * releasing the button ends the drag. */
static int s_drag_id;    /* window being dragged (0 = none) */
static int s_drag_offx;  /* pointer offset within the window border */
static int s_drag_offy;

static gui_event_t s_events[GUI_MAX_EVENTS];
static u32         s_ev_head;
static u32         s_ev_count;

static int s_lock = -1; /* mutex: window table + events + pointer */

static int s_kbd_port  = -1;
static int s_term_port = -1;

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

static void ev_push(u32 type, u32 code, i32 x, i32 y, i32 win) {
    if (s_ev_count >= GUI_MAX_EVENTS)
        return; /* ring full: drop the oldest-free slot semantics */
    u32 idx          = (s_ev_head + s_ev_count) % GUI_MAX_EVENTS;
    s_events[idx].type = type;
    s_events[idx].code = code;
    s_events[idx].x    = x;
    s_events[idx].y    = y;
    s_events[idx].win  = win;
    s_ev_count++;
}

/* ------------------------------------------------------------------ */
/* Dirty-rectangle compositing                                         */
/*                                                                     */
/* Instead of repainting the whole 1024x768 framebuffer on every       */
/* event, every mutation records the rect it changed; gui_composite()  */
/* repaints ONLY that rect (background + the clipped parts of every    */
/* window intersecting it + the pointer).  Mouse movement therefore    */
/* redraws just a small square, which both cuts framebuffer writes and */
/* shrinks the VNC dirty region — the visible refresh-rate win.        */
/* ------------------------------------------------------------------ */

typedef struct {
    int x, y, w, h;
} gui_rect_t;

static gui_rect_t s_dirty;
static int        s_dirty_valid;

static void gui_dirty_add(int x, int y, int w, int h) {
    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    if (w <= 0 || h <= 0)
        goto out;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x >= (int)s_fb.w || y >= (int)s_fb.h || w <= 0 || h <= 0)
        goto out;
    if (x + w > (int)s_fb.w)
        w = (int)s_fb.w - x;
    if (y + h > (int)s_fb.h)
        h = (int)s_fb.h - y;
    if (!s_dirty_valid) {
        s_dirty.x = x;
        s_dirty.y = y;
        s_dirty.w = w;
        s_dirty.h = h;
        s_dirty_valid = 1;
        goto out;
    }
    /* Union into the pending rect. */
    {
        int x1 = s_dirty.x + s_dirty.w;
        int y1 = s_dirty.y + s_dirty.h;
        if (x < s_dirty.x)
            s_dirty.x = x;
        if (y < s_dirty.y)
            s_dirty.y = y;
        if (x + w > x1)
            x1 = x + w;
        if (y + h > y1)
            y1 = y + h;
        s_dirty.w = x1 - s_dirty.x;
        s_dirty.h = y1 - s_dirty.y;
    }
out:
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);
}

static int rect_intersect(gui_rect_t a, gui_rect_t b, gui_rect_t *out) {
    int x1 = (a.x + a.w < b.x + b.w) ? a.x + a.w : b.x + b.w;
    int y1 = (a.y + a.h < b.y + b.h) ? a.y + a.h : b.y + b.h;
    int x0 = (a.x > b.x) ? a.x : b.x;
    int y0 = (a.y > b.y) ? a.y : b.y;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;
    return 1;
}

static void draw_titlebar(gui_win_t *w) {
    int tx = w->x + GUI_BORDER;
    int ty = w->y + GUI_BORDER;
    gui_fill(&s_fb, tx, ty, w->w, GUI_TITLE_H,
             (w->id == s_focus_id) ? GUI_TITLE_BG : GUI_TITLE_BG_IDLE);
    /* Title text (skip background: the bar is already filled). */
    char t[GUI_MAX_TITLE + 2];
    int  n = (int)strlen(w->title);
    if (n > (int)sizeof(t) - 3)
        n = (int)sizeof(t) - 3;
    memcpy(t, w->title, (size_t)n);
    t[n++] = ' ';
    t[n++] = '*'; /* focus marker on the focused window */
    t[n]   = '\0';
    /* Center vertically in the 16px bar: font is 16px, so row 0. */
    gui_text(&s_fb, tx + 3, ty, t, GUI_TITLE_FG, 0);
}

static void gui_composite(void) {
    if (!s_active || !s_dirty_valid)
        return;
    if (s_lock >= 0)
        (void)mutex_lock(s_lock);

    gui_rect_t d = s_dirty;
    s_dirty_valid = 0;

    /* Desktop background inside the dirty rect. */
    gui_fill(&s_fb, d.x, d.y, d.w, d.h, GUI_BG_COLOR);

    /* Windows in creation order (later = on top; focus drawn last).
     * Each window repaints only its intersection with the dirty rect
     * (border + title bar are cheap whole-window draws; the content
     * blit is clipped). */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
            gui_win_t *w = &s_wins[i];
            if (!w->in_use)
                continue;
            if (pass == 0 && w->id == s_focus_id)
                continue; /* focused window goes in the top pass */
            if (pass == 1 && w->id != s_focus_id)
                continue;

            gui_rect_t wrect;
            wrect.x = w->x;
            wrect.y = w->y;
            wrect.w = w->w + 2 * GUI_BORDER;
            wrect.h = w->h + 2 * GUI_BORDER + GUI_TITLE_H;
            gui_rect_t clip;
            if (!rect_intersect(d, wrect, &clip))
                continue;

            /* Border frame + title bar (small, drawn whole). */
            gui_rect(&s_fb, w->x, w->y, w->w + 2 * GUI_BORDER,
                     w->h + 2 * GUI_BORDER + GUI_TITLE_H, GUI_BORDER_COLOR);
            draw_titlebar(w);

            /* Content blit: only the part of the window that changed. */
            int cx0 = w->x + GUI_BORDER;
            int cy0 = w->y + GUI_BORDER + GUI_TITLE_H;
            gui_rect_t cclip;
            gui_rect_t crect = {cx0, cy0, w->w, w->h};
            if (rect_intersect(d, crect, &cclip)) {
                gui_blit(&s_fb, cclip.x, cclip.y, &w->buf,
                         cclip.x - cx0, cclip.y - cy0, cclip.w, cclip.h);
            }
        }
    }

    /* Pointer: a small filled square (drawn last, on top). */
    if (s_ptr_x >= 0 && s_ptr_y >= 0) {
        gui_rect_t prect = {s_ptr_x, s_ptr_y, 5, 5};
        gui_rect_t pclip;
        if (rect_intersect(d, prect, &pclip))
            gui_fill(&s_fb, s_ptr_x, s_ptr_y, 5, 5, GUI_PTR_COLOR);
    }

    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);
}

/* ------------------------------------------------------------------ */
/* Window ops (server thread)                                          */
/* ------------------------------------------------------------------ */

static gui_win_t *win_find(int id) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (s_wins[i].in_use && s_wins[i].id == id)
            return &s_wins[i];
    return NULL;
}

static void do_create(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -2; /* ERR_INVAL */
    if (msg_len < (int)(8 + sizeof(gui_req_create_t))) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t  *req  = (gui_req_t *)s_req;
    gui_req_create_t *c = (gui_req_create_t *)req->data;
    if (c->w <= 0 || c->h <= 0 || c->w > 1024 || c->h > 768) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *slot = NULL;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!s_wins[i].in_use) {
            slot = &s_wins[i];
            break;
        }
    }
    if (!slot) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        resp->ret = -1; /* ERR_NOMEM */
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }

    /* Off-screen content buffer (32bpp). */
    size_t bytes = (size_t)c->w * c->h * 4;
    u8    *buf   = (u8 *)malloc(bytes);
    if (!buf) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        resp->ret = -1;
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->owner  = caller;
    slot->id     = s_next_id++;
    strncpy(slot->title, c->title, sizeof(slot->title) - 1);
    slot->title[sizeof(slot->title) - 1] = '\0';
    slot->w = c->w;
    slot->h = c->h;
    /* Auto-placement: walk a cascade grid and pick the first slot that
     * does NOT overlap an existing window — new windows never stack
     * their borders over an existing one by default.  (Clients may
     * still MOVE a window anywhere, including on top, and drag the
     * title bar to rearrange.) */
    {
        int ww = c->w + 2 * GUI_BORDER;
        int wh = c->h + 2 * GUI_BORDER + GUI_TITLE_H;
        int px = 20, py = 20;
        for (int attempt = 0; attempt < 400; attempt++) {
            int free = 1;
            for (int j = 0; j < GUI_MAX_WINDOWS; j++) {
                if (!s_wins[j].in_use)
                    continue;
                int ox = s_wins[j].x, oy = s_wins[j].y;
                int ow = s_wins[j].w + 2 * GUI_BORDER;
                int oh = s_wins[j].h + 2 * GUI_BORDER + GUI_TITLE_H;
                if (px < ox + ow && ox < px + ww && py < oy + oh && oy < py + wh) {
                    free = 0;
                    break;
                }
            }
            if (free)
                break;
            px += 24;
            py += 18;
            if (px + ww + 20 > (int)s_fb.w) {
                px = 20;
                py += 130;
            }
            if (py + wh > (int)s_fb.h)
                py = 20; /* wrapped: accept overlap rather than fail */
        }
        slot->x = px;
        slot->y = py;
    }
    slot->buf.w     = (u32)c->w;
    slot->buf.h     = (u32)c->h;
    slot->buf.pitch = (u32)c->w * 4;
    slot->buf.bpp   = 32;
    slot->buf.buf   = buf;
    gui_fill(&slot->buf, 0, 0, c->w, c->h, 0x00000000);

    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = slot->id;
    gui_dirty_add(slot->x, slot->y, slot->w + 2 * GUI_BORDER,
                  slot->h + 2 * GUI_BORDER + GUI_TITLE_H);
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_destroy(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -4; /* ERR_NOENT */
    if (msg_len < (int)(8 + 4)) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t *req = (gui_req_t *)s_req;
    int        id  = ((i32 *)req->data)[0];

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *w = win_find(id);
    if (!w || (w->owner != 0 && w->owner != caller)) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        resp->ret = -4;
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    /* Capture the geometry BEFORE freeing the slot (needed for the
     * dirty rect, which must be recorded outside the lock). */
    int dx = w->x, dy = w->y;
    int dw = w->w + 2 * GUI_BORDER;
    int dh = w->h + 2 * GUI_BORDER + GUI_TITLE_H;
    free(w->buf.buf);
    memset(w, 0, sizeof(*w));
    if (s_focus_id == id)
        s_focus_id = 0;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = 0;
    gui_dirty_add(dx, dy, dw, dh);
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_move(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -4;
    if (msg_len < (int)(8 + 12)) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t *req = (gui_req_t *)s_req;
    i32       *a   = (i32 *)req->data;

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *w = win_find(a[0]);
    if (!w || (w->owner != 0 && w->owner != caller)) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    int ox = w->x, oy = w->y;
    int ow = w->w + 2 * GUI_BORDER;
    int oh = w->h + 2 * GUI_BORDER + GUI_TITLE_H;
    w->x = a[1];
    w->y = a[2];
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = 0;
    gui_dirty_add(ox, oy, ow, oh); /* old spot */
    gui_dirty_add(w->x, w->y, ow, oh); /* new spot */
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_focus(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -4;
    if (msg_len < (int)(8 + 4)) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t *req = (gui_req_t *)s_req;
    int        id  = ((i32 *)req->data)[0];

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *w = win_find(id);
    if (!w || (w->owner != 0 && w->owner != caller)) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    int old_focus = s_focus_id;
    s_focus_id    = id;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = 0;
    /* Title-bar highlight changed on the old and the new focused
     * window — redraw both whole windows. */
    if (old_focus != id) {
        int ox = 0, oy = 0, ow = 0, oh = 0;
        if (s_lock >= 0)
            (void)mutex_lock(s_lock);
        gui_win_t *old = win_find(old_focus);
        if (old) {
            ox = old->x;
            oy = old->y;
            ow = old->w + 2 * GUI_BORDER;
            oh = old->h + 2 * GUI_BORDER + GUI_TITLE_H;
        }
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        if (ow > 0)
            gui_dirty_add(ox, oy, ow, oh);
        gui_dirty_add(w->x, w->y, w->w + 2 * GUI_BORDER,
                      w->h + 2 * GUI_BORDER + GUI_TITLE_H);
    }
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_fill(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -4;
    if (msg_len < (int)(8 + sizeof(gui_req_fill_t))) {
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t      *req = (gui_req_t *)s_req;
    gui_req_fill_t *f   = (gui_req_fill_t *)req->data;

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *w = win_find(f->id);
    if (!w || (w->owner != 0 && w->owner != caller)) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_fill(&w->buf, f->x, f->y, f->w, f->h, f->color);
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = 0;
    gui_dirty_add(w->x + GUI_BORDER + f->x, w->y + GUI_BORDER + GUI_TITLE_H + f->y,
                  f->w, f->h);
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_text(int token, int msg_len, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = -4;
    if (msg_len < (int)(8 + 20)) { /* header fields + at least empty text */
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_req_t      *req = (gui_req_t *)s_req;
    gui_req_text_t *t   = (gui_req_text_t *)req->data;
    t->text[sizeof(t->text) - 1] = '\0';

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    gui_win_t *w = win_find(t->id);
    if (!w || (w->owner != 0 && w->owner != caller)) {
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
        return;
    }
    gui_text(&w->buf, t->x, t->y, t->text, t->fg, t->bg);
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    resp->ret = 0;
    gui_dirty_add(w->x + GUI_BORDER + t->x, w->y + GUI_BORDER + GUI_TITLE_H + t->y,
                  (int)strlen(t->text) * 8, 16);
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_poll(int token) {
    gui_resp_poll_t *resp = (gui_resp_poll_t *)s_resp;
    resp->ret             = 0;

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    resp->count = 0;
    while (s_ev_count > 0 && resp->count < GUI_MAX_EVENTS) {
        resp->events[resp->count++] = s_events[s_ev_head];
        s_ev_head                   = (s_ev_head + 1) % GUI_MAX_EVENTS;
        s_ev_count--;
    }
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_pointer(int token) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    resp->ret        = 0;
    i32        *p    = (i32 *)resp->data;
    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    p[0] = s_ptr_x;
    p[1] = s_ptr_y;
    p[2] = (i32)s_ptr_buttons;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_activate(int token, u64 caller) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;
    (void)caller;

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    s_active      = 1;
    s_ptr_x       = (i32)s_fb.w / 2;
    s_ptr_y       = (i32)s_fb.h / 2;
    s_ptr_buttons = 0;
    s_focus_id    = 0;
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    /* Take the keyboard focus (keys now route to the GUI, not the
     * shell's parked read). */
    if (s_kbd_port >= 0) {
        u32 req[2] = {3, 0}; /* KBD_OP_TAKE_FOCUS */
        u8  rsp[4];
        int rl = (int)sizeof(rsp);
        (void)ipc_call(s_kbd_port, req, 8, rsp, &rl);
    }

    resp->ret = 0;
    gui_dirty_add(0, 0, (int)s_fb.w, (int)s_fb.h);
    gui_composite();
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_deactivate(int token) {
    gui_resp_t *resp = (gui_resp_t *)s_resp;

    /* Release the keyboard focus. */
    if (s_kbd_port >= 0) {
        u32 req[2] = {4, 0}; /* KBD_OP_RELEASE_FOCUS */
        u8  rsp[4];
        int rl = (int)sizeof(rsp);
        (void)ipc_call(s_kbd_port, req, 8, rsp, &rl);
    }

    if (s_lock >= 0)
        (void)mutex_lock(s_lock);
    s_active = 0;
    s_dirty_valid = 0; /* drop any pending rect: the screen is handed back */
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);

    /* Restore the text screen (term repaints from its cell buffer). */
    if (s_term_port >= 0) {
        u32 req[2] = {11, 0}; /* TERM_OP_REDRAW */
        u8  rsp[4];
        int rl = (int)sizeof(rsp);
        (void)ipc_call(s_term_port, req, 8, rsp, &rl);
    }

    resp->ret = 0;
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ------------------------------------------------------------------ */
/* Server thread                                                       */
/* ------------------------------------------------------------------ */

static void gui_server_loop(int port) {
    for (;;) {
        int msg_len = (int)sizeof(s_req);
        int token   = 0;
        u64 caller  = 0;
        int ret     = ipc_recv_from(port, s_req, &msg_len, &token, &caller);
        if (ret < 0) {
            printf("gui: ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }
        if (msg_len < 8) {
            gui_resp_t *resp = (gui_resp_t *)s_resp;
            resp->ret        = -2;
            (void)ipc_reply(token, resp, (int)sizeof(*resp));
            continue;
        }
        gui_req_t *req = (gui_req_t *)s_req;
        switch (req->op) {
        case GUI_OP_CREATE:    do_create(token, msg_len, caller); break;
        case GUI_OP_DESTROY:   do_destroy(token, msg_len, caller); break;
        case GUI_OP_MOVE:      do_move(token, msg_len, caller); break;
        case GUI_OP_FOCUS:     do_focus(token, msg_len, caller); break;
        case GUI_OP_FILL:      do_fill(token, msg_len, caller); break;
        case GUI_OP_TEXT:      do_text(token, msg_len, caller); break;
        case GUI_OP_POLL:      do_poll(token); break;
        case GUI_OP_POINTER:   do_pointer(token); break;
        case GUI_OP_ACTIVATE:  do_activate(token, caller); break;
        case GUI_OP_DEACTIVATE: do_deactivate(token); break;
        default: {
            gui_resp_t *resp = (gui_resp_t *)s_resp;
            resp->ret        = -2;
            (void)ipc_reply(token, resp, (int)sizeof(*resp));
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Input thread (keyboard + mouse polling)                             */
/* ------------------------------------------------------------------ */

/* Topmost window under (px, py), or 0. */
static int hit_test(i32 px, i32 py) {
    int best = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        gui_win_t *w = &s_wins[i];
        if (!w->in_use)
            continue;
        if (px >= w->x && px < w->x + w->w + 2 * GUI_BORDER &&
            py >= w->y && py < w->y + w->h + 2 * GUI_BORDER + GUI_TITLE_H)
            best = w->id;
    }
    return best;
}

static void gui_input_main(void *arg) {
    (void)arg;

    for (;;) {
        if (!s_active) {
            (void)sleep(5);
            continue;
        }

        int changed = 0;

        /* Keyboard: non-blocking READ (keys route here while focused). */
        if (s_kbd_port >= 0) {
            u32 req[2] = {KBD_OP_READ, 8};
            u8  resp[4 + 8];
            int rl = (int)sizeof(resp);
            if (ipc_call(s_kbd_port, req, 8, resp, &rl) == 0 && rl >= 4) {
                i32 n = (i32)((u32 *)resp)[0];
                if (n > 8)
                    n = 8;
                for (i32 i = 0; i < n; i++) {
                    if (s_lock >= 0)
                        (void)mutex_lock(s_lock);
                    ev_push(GUI_EV_KEY, (u8)resp[4 + i], s_ptr_x, s_ptr_y, s_focus_id);
                    if (s_lock >= 0)
                        (void)mutex_unlock(s_lock);
                }
            }
        }

        /* Mouse: accumulated deltas + buttons. */
        if (s_kbd_port >= 0) {
            u32 req[2] = {KBD_OP_MOUSE_READ, 12};
            u8  resp[4 + 12];
            int rl = (int)sizeof(resp);
            if (ipc_call(s_kbd_port, req, 8, resp, &rl) == 0 && rl >= 4 + 12) {
                i32 dx = ((i32 *)(resp + 4))[0];
                i32 dy = ((i32 *)(resp + 4))[1];
                u8  bt = (u8)((i32 *)(resp + 4))[2];
                if (dx != 0 || dy != 0) {
                    int ox, oy;
                    if (s_lock >= 0)
                        (void)mutex_lock(s_lock);
                    ox = s_ptr_x;
                    oy = s_ptr_y;
                    s_ptr_x += dx;
                    s_ptr_y -= dy; /* PS/2 Y is up-positive */
                    if (s_ptr_x < 0)
                        s_ptr_x = 0;
                    if (s_ptr_y < 0)
                        s_ptr_y = 0;
                    if (s_ptr_x >= (i32)s_fb.w)
                        s_ptr_x = (i32)s_fb.w - 1;
                    if (s_ptr_y >= (i32)s_fb.h)
                        s_ptr_y = (i32)s_fb.h - 1;
                    ev_push(GUI_EV_MOUSEMOVE, 0, s_ptr_x, s_ptr_y, hit_test(s_ptr_x, s_ptr_y));
                    /* Dragging: move the window under the pointer. */
                    if (s_drag_id != 0) {
                        gui_win_t *dw = win_find(s_drag_id);
                        if (dw) {
                            int nx = s_ptr_x - s_drag_offx;
                            int ny = s_ptr_y - s_drag_offy;
                            if (nx + dw->w + 2 * GUI_BORDER > (i32)s_fb.w)
                                nx = (i32)s_fb.w - dw->w - 2 * GUI_BORDER;
                            if (ny + dw->h + 2 * GUI_BORDER + GUI_TITLE_H >
                                (i32)s_fb.h)
                                ny = (i32)s_fb.h - dw->h - 2 * GUI_BORDER -
                                     GUI_TITLE_H;
                            if (nx < 0)
                                nx = 0;
                            if (ny < 0)
                                ny = 0;
                            if (nx != dw->x || ny != dw->y) {
                                gui_dirty_add(dw->x, dw->y,
                                              dw->w + 2 * GUI_BORDER,
                                              dw->h + 2 * GUI_BORDER +
                                                  GUI_TITLE_H);
                                dw->x = nx;
                                dw->y = ny;
                                gui_dirty_add(dw->x, dw->y,
                                              dw->w + 2 * GUI_BORDER,
                                              dw->h + 2 * GUI_BORDER +
                                                  GUI_TITLE_H);
                            }
                        }
                    }
                    if (s_lock >= 0)
                        (void)mutex_unlock(s_lock);
                    /* Redraw only the old + new pointer squares. */
                    gui_dirty_add(ox - 1, oy - 1, 7, 7);
                    gui_dirty_add(s_ptr_x - 1, s_ptr_y - 1, 7, 7);
                    changed = 1;
                }
                /* Button transitions: press focuses the hit window and
                 * starts a title-bar drag; release ends it. */
                if (bt != s_ptr_buttons) {
                    int focus_changed = 0;
                    int fx = 0, fy = 0, fw = 0, fh = 0;
                    if (s_lock >= 0)
                        (void)mutex_lock(s_lock);
                    u8 pressed = bt & ~s_ptr_buttons;
                    u8 released = s_ptr_buttons & ~bt;
                    s_ptr_buttons = bt;
                    if (pressed) {
                        int hit = hit_test(s_ptr_x, s_ptr_y);
                        if (hit != 0) {
                            /* Dragging starts when the press lands on
                             * the window's border/title-bar strip. */
                            gui_win_t *hw = win_find(hit);
                            if (hw && s_ptr_y >= hw->y &&
                                s_ptr_y < hw->y + GUI_BORDER + GUI_TITLE_H) {
                                s_drag_id  = hit;
                                s_drag_offx = s_ptr_x - hw->x;
                                s_drag_offy = s_ptr_y - hw->y;
                            }
                            if (hit != s_focus_id) {
                                gui_win_t *old = win_find(s_focus_id);
                                gui_win_t *neu = win_find(hit);
                                if (old) {
                                    fx = old->x;
                                    fy = old->y;
                                    fw = old->w + 2 * GUI_BORDER;
                                    fh = old->h + 2 * GUI_BORDER + GUI_TITLE_H;
                                }
                                if (neu) {
                                    gui_dirty_add(neu->x, neu->y,
                                                  neu->w + 2 * GUI_BORDER,
                                                  neu->h + 2 * GUI_BORDER +
                                                      GUI_TITLE_H);
                                }
                                focus_changed = 1;
                                s_focus_id    = hit;
                            } else {
                                s_focus_id = hit;
                            }
                        }
                        ev_push(GUI_EV_BUTTON, 1, s_ptr_x, s_ptr_y, hit);
                    }
                    if (released) {
                        s_drag_id = 0; /* end the drag */
                        ev_push(GUI_EV_BUTTON, 0, s_ptr_x, s_ptr_y, hit_test(s_ptr_x, s_ptr_y));
                    }
                    if (s_lock >= 0)
                        (void)mutex_unlock(s_lock);
                    if (focus_changed && fw > 0)
                        gui_dirty_add(fx, fy, fw, fh);
                    changed = 1;
                }
            }
        }

        if (changed)
            gui_composite();

        (void)sleep(1);
    }
}

/* ------------------------------------------------------------------ */
/* Entry                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("gui: starting window compositor\n");

    /* 1. Map the display framebuffer (needs ATOM_SERVICE_MANAGE via
     *    blob identity — the gui service is in the kernel seed list). */
    if (gui_fb_open(&s_fb) < 0) {
        printf("gui: framebuffer unavailable\n");
        thread_exit(1);
    }
    printf("gui: framebuffer %ux%u %ubpp buf=%p\n", s_fb.w, s_fb.h, s_fb.bpp,
           (void *)s_fb.buf);

    /* 2. Resolve the keyboard and term ports (lazily retried). */
    s_kbd_port = port_get("keyboard");
    if (s_kbd_port < 0)
        printf("gui: 'keyboard' port unresolved yet\n");
    s_term_port = port_get("term");

    /* 3. IPC port. */
    int port = ipc_port_create();
    if (port < 0) {
        printf("gui: ipc_port_create failed (%d)\n", port);
        thread_exit(1);
    }
    int ret = port_register(GUI_PORT_NAME, port);
    if (ret < 0) {
        printf("gui: port_register('%s') failed (%d)\n", GUI_PORT_NAME, ret);
        thread_exit(1);
    }
    printf("gui: port %d registered as '%s'\n", port, GUI_PORT_NAME);

    /* 4. Render lock + input thread. */
    s_lock = mutex_create();
    if (s_lock < 0)
        printf("gui: mutex_create failed (%d)\n", s_lock);
    int itid = thread_create(gui_input_main, NULL, 10);
    if (itid < 0)
        printf("gui: thread_create(input) failed (%d)\n", itid);

    /* 5. Serve clients. */
    printf("gui: serving on port %d (idle until ACTIVATE)\n", port);
    gui_server_loop(port);
    return 0;
}
