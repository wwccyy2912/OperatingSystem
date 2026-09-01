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
 * ipc.h - Inter-Process Communication
 * Copyright (c) 2026 OpSys Project
 *
 * Synchronous message passing via ports.
 * Small messages (<=64 bytes) passed via registers.
 * Larger messages via shared memory.
 */

#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include <kernel/types.h>
#include <kernel/thread.h>

/* IPC message header */
typedef struct {
    port_t dest_port;
    port_t src_port;
    u32    msg_type;
    u32    msg_len;
    u64    caps[4]; /* Capability handles transferred */
    u32    cap_count;
} ipc_header_t;

/* IPC message buffer */
typedef struct {
    ipc_header_t header;
    u8           data[MAX_MSG_SIZE];
} ipc_msg_t;

/* Port structure */
typedef struct {
    port_t    port_id;
    tid_t     owner_tid;  /* Owning thread */
    pid_t     owner_pid;  /* Owning process */
    thread_t *wait_queue; /* Threads waiting on this port */
    bool      in_use;
} port_entry_t;

/**
 * Initialize the IPC subsystem.
 */
void IpcInit(void);

/**
 * Create a new IPC port owned by the current thread.
 * @return Port ID, or negative error code.
 */
port_t IpcPortCreate(void);

/**
 * Destroy an IPC port.
 * @param port  Port to destroy.
 */
void IpcPortDestroy(port_t port);

/**
 * Tear down every IPC resource owned by a dying process: destroy its
 * ports (waking all blocked peers with ERR_NOENT) and drop its
 * registry names.  Called from ProcessReap().
 * @param pid  PID of the dying process.
 */
void IpcCleanupProcess(pid_t pid);

/**
 * Send a message to a port (blocking if port has no receiver).
 * @param port  Destination port.
 * @param msg   Message data.
 * @param len   Message length in bytes.
 * @return OK or error.
 */
error_t IpcSend(port_t port, const void *msg, u32 len);

/**
 * Receive a message from a port (blocking).
 * @param port    Source port.
 * @param buf     Buffer to receive into.
 * @param len     In: buffer size, Out: actual message size.
 * @param tok_ptr Out: reply token if the message was a call (0 otherwise).
 *                The token must be passed to IpcReply() unchanged.
 * @return OK or error.
 */
error_t IpcRecv(port_t port, void *buf, u32 *len, u32 *tok_ptr);

/**
 * Receive with sender identity (P0 地基, docs permission_model.md §三).
 * Identical to IpcRecv() — including the existing 4-arg semantics —
 * PLUS: when sender_subject != NULL, the kernel-filled, unforgeable
 * subject of the sender's process is written to it.  sender_subject
 * == NULL is tolerated and behaves exactly like IpcRecv().
 * @param port            Source port.
 * @param buf             Buffer to receive into.
 * @param len             In: buffer size, Out: actual message size.
 * @param tok_ptr         Out: reply token if the message was a call.
 * @param sender_subject  Out: sender's process subject (may be NULL).
 * @return OK or error.
 */
error_t IpcRecvFrom(port_t port, void *buf, u32 *len, u32 *tok_ptr, subject_id_t *sender_subject);

/**
 * Synchronous call: send request and wait for response.
 * @param port     Target port.
 * @param req      Request data.
 * @param req_len  Request length.
 * @param resp     Response buffer.
 * @param resp_len In: buffer size, Out: response size.
 * @return OK or error.
 */
error_t IpcCall(port_t port, const void *req, u32 req_len, void *resp, u32 *resp_len);

/**
 * Reply to a pending call (send response to caller).
 * The token is an opaque handle returned by IpcRecv() for call
 * messages.  It uniquely identifies one pending caller, so concurrent
 * callers to the same port no longer share a single active-call slot.
 * @param token  Reply token from IpcRecv() (must be non-zero).
 * @param msg    Response data.
 * @param len    Response length.
 * @return OK, ERR_NOENT (bad/stale token), ERR_BUSY (already replied),
 *         or ERR_INVAL.
 */
error_t IpcReply(u32 token, const void *msg, u32 len);

/**
 * Get or create a well-known port by name.
 * @param name  Port name string.
 * @return Port ID, or negative error.
 */
port_t IpcGetPort(const char *name);

/**
 * Register a well-known port name.
 * @param name  Name to register.
 * @param port  Port ID to associate.
 * @return OK or error.
 */
error_t IpcRegisterPort(const char *name, port_t port);

/**
 * Abort a thread's IPC wait (called from the signal kill path when
 * force-terminating a blocked thread).  Unlinks the thread from
 * whatever IPC structure holds it (the port recv FIFO queue, or the
 * pending/reply-wait list holding its call message) and records
 * ERR_INTERRUPTED so the woken syscall returns an error instead of
 * re-blocking.  The blocked thread's IpcCall() remains the only code
 * that frees its pending message.  A late IpcReply() for a freed
 * slot fails token validation (in_use + generation), so it can never
 * touch a reused slot.
 * @param t  Thread blocked in IpcRecv()/IpcCall() (no-op otherwise).
 */
void IpcAbortWait(thread_t *t);

#endif /* KERNEL_IPC_H */
