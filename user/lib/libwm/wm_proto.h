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
 * wm_proto.h - Window Manager protocol (v0.4)
 * Copyright (c) 2026 OpSys Project
 *
 * Protocol structs/ops shared between the wm service and its clients.
 * The service's canonical copy is user/services/wm/wm.h; this thin
 * shim keeps the client library self-contained (libwm is compiled
 * without knowledge of the service directory layout).
 */

#ifndef LIBWM_WM_PROTO_H
#define LIBWM_WM_PROTO_H

#include <stdint.h>

#define WM_PORT_NAME "wm"

/* Registry limits */
#define WM_MAX_WINDOWS 16
#define WM_TITLE_MAX   32
#define WM_CONTENT_ROWS 8  /* body rows per window  */
#define WM_CONTENT_COLS 44 /* body columns per row  */

/* Ops */
#define WM_OP_CREATE     1 /* create a window        -> {win_id}       */
#define WM_OP_DESTROY    2 /* destroy a window       -> {ret}          */
#define WM_OP_LIST       3 /* list registry          -> {count,lines}  */
#define WM_OP_FOCUS      4 /* set keyboard focus     -> {ret}          */
#define WM_OP_MOVE       5 /* move a window          -> {ret}          */
#define WM_OP_WRITE      6 /* write a content row    -> {ret}          */
#define WM_OP_ACTIVATE   7 /* start desktop session  -> {ret}          */
#define WM_OP_DEACTIVATE 8 /* end desktop session    -> {ret}          */
#define WM_OP_GET_STATE  9 /* query {active,focus,n} -> {ret}          */

typedef struct {
    uint32_t op;
    char     title[WM_TITLE_MAX];
    uint32_t x, y, w, h;
    uint32_t win_id;
    uint32_t mx, my;
    uint32_t row;
    char     text[WM_CONTENT_COLS];
} wm_req_t;

typedef struct {
    int32_t  ret;
    uint32_t win_id;
    uint32_t count;
    uint32_t active;
    uint32_t focus;
    char     lines[WM_MAX_WINDOWS][WM_TITLE_MAX + 24];
} wm_resp_t;

#endif /* LIBWM_WM_PROTO_H */
