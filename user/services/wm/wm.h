/*
 * wm.h - Window Manager protocol (v0.4)
 * Copyright (c) 2026 OpSys Project
 *
 * The window manager is a Ring-3 service (independent process) that owns
 * the DESKTOP: a window registry (create/destroy/list/focus/move/write),
 * a compositor that renders every registered window through the term
 * service (the display owner — wm never touches the framebuffer directly),
 * and keyboard focus routing while a desktop session is active.
 *
 * Protocol (flat structs, raw copy, native little-endian — mirror of the
 * term/perm conventions).  Requests carry the caller's kernel-issued
 * subject via ipc_recv_from; window mutation ops (DESTROY/MOVE/WRITE)
 * are gated to the window's owner subject (or an admin caller).
 *
 * Single source of truth: the shared client header user/lib/libwm/wm_proto.h.
 */

#ifndef USER_SERVICES_WM_WM_H
#define USER_SERVICES_WM_WM_H

#include "../../lib/libwm/wm_proto.h"

#endif /* USER_SERVICES_WM_WM_H */
