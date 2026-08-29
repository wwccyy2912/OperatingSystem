/*
 * gui.h - Window compositor protocol (v0.7.1, GUI round)
 * Copyright (c) 2026 OpSys Project
 *
 * The gui service owns the display framebuffer (mapped via libgui,
 * gated on ATOM_SERVICE_MANAGE) and composes windows onto it.  Clients
 * create windows and draw into them over IPC; the compositor keeps an
 * off-screen pixel buffer per window, re-composites on every mutation,
 * and routes keyboard + PS/2 mouse input to clients as events.
 *
 * Message layout (fits the 4 KiB IPC limit):
 *   req  = { u32 op; u32 len; u8 data[len] }
 *   resp = { i32 ret; u8 data[] }
 */

#ifndef USER_SERVICES_GUI_GUI_H
#define USER_SERVICES_GUI_GUI_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint64_t u64;

#define GUI_PORT_NAME  "gui"
#define GUI_MAX_WINDOWS 8
#define GUI_MAX_TITLE   64
#define GUI_TITLE_H     16 /* title-bar height in pixels */
#define GUI_BORDER      1  /* window border thickness */
#define GUI_IPC_MAX     4096
#define GUI_MAX_EVENTS  32

/* Ops */
enum {
    GUI_OP_CREATE = 1,  /* {title[64]; w; h}            -> {id}      */
    GUI_OP_DESTROY = 2, /* {id}                                      */
    GUI_OP_MOVE = 3,    /* {id; x; y}                               */
    GUI_OP_FOCUS = 4,   /* {id}                                      */
    GUI_OP_FILL = 5,    /* {id; x; y; w; h; color}                   */
    GUI_OP_TEXT = 6,    /* {id; x; y; fg; bg; text[N]}               */
    GUI_OP_POLL = 7,    /* -> {count; events[count]}                 */
    GUI_OP_POINTER = 8, /* -> {x; y; buttons}                        */
    GUI_OP_ACTIVATE = 9,   /* compositor on: take focus, draw        */
    GUI_OP_DEACTIVATE = 10, /* compositor off: restore term screen   */
    GUI_OP_RESIZE = 11,   /* {id; w; h} — resize a window           */
};

/* ---- request payloads (data area of gui_req_t) ---- */

typedef struct {
    char title[GUI_MAX_TITLE];
    i32  w;
    i32  h;
} gui_req_create_t;

typedef struct {
    i32 id;
} gui_req_id_t;

typedef struct {
    i32 id;
    i32 x;
    i32 y;
} gui_req_move_t;

typedef struct {
    i32  id;
    i32  x;
    i32  y;
    i32  w;
    i32  h;
    u32  color;
} gui_req_fill_t;

typedef struct {
    i32  id;
    i32  x;
    i32  y;
    u32  fg;
    u32  bg; /* 0 = transparent */
    char text[256];
} gui_req_text_t;

typedef struct {
    i32 id;
    i32 w;
    i32 h;
} gui_req_resize_t;

/* ---- events (drained by GUI_OP_POLL) ---- */

#define GUI_EV_KEY       1 /* code = scancode-derived char/control;
                            * win  = the FOCUSED window id — keyboard
                            * input follows the click-focused window */
#define GUI_EV_MOUSEMOVE 2 /* x,y = pointer position, code = 0;
                            * win  = window under the pointer (0 = none) */
#define GUI_EV_BUTTON    3 /* code = 1 left, 2 right, 3 middle; x,y;
                            * win  = window under the pointer (0 = none) */
#define GUI_EV_WHEEL     4 /* code = wheel delta (signed); x,y;
                            * win  = window under the pointer */

typedef struct {
    u32 type;
    u32 code;
    i32 x;
    i32 y;
    i32 win;  /* target window id (focus for KEY, hit for mouse) */
    u64 owner; /* window owner subject; 0 = broadcast.  The compositor
                * filters events per polling client (event isolation). */
} gui_event_t;

typedef struct {
    i32        ret;
    u32        count;
    gui_event_t events[GUI_MAX_EVENTS];
} gui_resp_poll_t;

/* ---- request/response envelopes ---- */

typedef struct {
    u32 op;
    u32 len; /* payload bytes in data[] */
    u8  data[GUI_IPC_MAX - 8];
} gui_req_t;

typedef struct {
    i32 ret;
    u8  data[GUI_IPC_MAX - 4];
} gui_resp_t;

_Static_assert(sizeof(gui_req_t) <= GUI_IPC_MAX, "gui_req_t too big");
_Static_assert(sizeof(gui_resp_t) <= GUI_IPC_MAX, "gui_resp_t too big");

#endif /* USER_SERVICES_GUI_GUI_H */
