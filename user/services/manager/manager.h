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
 * manager.h - Service-supervision protocol ("manager" port)
 * Copyright (c) 2026 OpSys Project
 *
 * The service manager owns the lifecycle of every spawned service.  The
 * shell's `svc` command family does not hold ATOM_SERVICE_MANAGE, so it
 * reaches this port through the `user` service admin proxy — exactly the
 * path `disk` and `kill` already take.  The manager re-checks the
 * *original* caller identity it receives from the proxy.
 *
 * Wire size: svc_resp_t is ~1.3 KB, well inside the 4096-byte IPC limit
 * (kernel/include/kernel/types.h MAX_MSG_SIZE), so a LIST reply travels
 * in a single message.
 */

#ifndef USER_SERVICES_MANAGER_MANAGER_H
#define USER_SERVICES_MANAGER_MANAGER_H

#include <stdint.h>

#define MANAGER_PORT_NAME "manager"
#define SVC_NAME_MAX      32
#define SVC_MAX_ENTRIES   24

enum {
    SVC_OP_LIST    = 1, /* -> every service the manager knows about */
    SVC_OP_STATUS  = 2, /* req.name -> entries[0] for that service  */
    SVC_OP_START   = 3, /* req.name -> spawn it again               */
    SVC_OP_STOP    = 4, /* req.name -> terminate it, no restart     */
    SVC_OP_RESTART = 5, /* req.name -> stop + start                 */
};

typedef struct {
    uint32_t op;
    char     name[SVC_NAME_MAX];
} svc_req_t;

typedef struct {
    char     name[SVC_NAME_MAX]; /* service / blob name            */
    int32_t  pid;                /* -1 = not running               */
    uint32_t alive;              /* 1 = a live process holds pid   */
    uint32_t monitored;          /* 1 = a monitor thread watches it */
    uint32_t restarts;           /* restart counter used so far    */
    uint32_t running;            /* 1 = spawned during boot        */
} svc_entry_t;

typedef struct {
    int32_t     ret;                 /* OK or a negative error  */
    uint32_t    count;               /* LIST: entries returned  */
    char        detail[64];          /* human-readable outcome  */
    svc_entry_t entries[SVC_MAX_ENTRIES];
} svc_resp_t;

#endif /* USER_SERVICES_MANAGER_MANAGER_H */
