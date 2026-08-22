/*
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
void ipc_init(void);

/**
 * Create a new IPC port owned by the current thread.
 * @return Port ID, or negative error code.
 */
port_t ipc_port_create(void);

/**
 * Destroy an IPC port.
 * @param port  Port to destroy.
 */
void ipc_port_destroy(port_t port);

/**
 * Tear down every IPC resource owned by a dying process: destroy its
 * ports (waking all blocked peers with ERR_NOENT) and drop its
 * registry names.  Called from process_reap().
 * @param pid  PID of the dying process.
 */
void ipc_cleanup_process(pid_t pid);

/**
 * Send a message to a port (blocking if port has no receiver).
 * @param port  Destination port.
 * @param msg   Message data.
 * @param len   Message length in bytes.
 * @return OK or error.
 */
error_t ipc_send(port_t port, const void *msg, u32 len);

/**
 * Receive a message from a port (blocking).
 * @param port    Source port.
 * @param buf     Buffer to receive into.
 * @param len     In: buffer size, Out: actual message size.
 * @param tok_ptr Out: reply token if the message was a call (0 otherwise).
 *                The token must be passed to ipc_reply() unchanged.
 * @return OK or error.
 */
error_t ipc_recv(port_t port, void *buf, u32 *len, u32 *tok_ptr);

/**
 * Receive with sender identity (P0 地基, docs permission_model.md §三).
 * Identical to ipc_recv() — including the existing 4-arg semantics —
 * PLUS: when sender_subject != NULL, the kernel-filled, unforgeable
 * subject of the sender's process is written to it.  sender_subject
 * == NULL is tolerated and behaves exactly like ipc_recv().
 * @param port            Source port.
 * @param buf             Buffer to receive into.
 * @param len             In: buffer size, Out: actual message size.
 * @param tok_ptr         Out: reply token if the message was a call.
 * @param sender_subject  Out: sender's process subject (may be NULL).
 * @return OK or error.
 */
error_t ipc_recv_from(port_t port, void *buf, u32 *len, u32 *tok_ptr, subject_id_t *sender_subject);

/**
 * Synchronous call: send request and wait for response.
 * @param port     Target port.
 * @param req      Request data.
 * @param req_len  Request length.
 * @param resp     Response buffer.
 * @param resp_len In: buffer size, Out: response size.
 * @return OK or error.
 */
error_t ipc_call(port_t port, const void *req, u32 req_len, void *resp, u32 *resp_len);

/**
 * Reply to a pending call (send response to caller).
 * The token is an opaque handle returned by ipc_recv() for call
 * messages.  It uniquely identifies one pending caller, so concurrent
 * callers to the same port no longer share a single active-call slot.
 * @param token  Reply token from ipc_recv() (must be non-zero).
 * @param msg    Response data.
 * @param len    Response length.
 * @return OK, ERR_NOENT (bad/stale token), ERR_BUSY (already replied),
 *         or ERR_INVAL.
 */
error_t ipc_reply(u32 token, const void *msg, u32 len);

/**
 * Get or create a well-known port by name.
 * @param name  Port name string.
 * @return Port ID, or negative error.
 */
port_t ipc_get_port(const char *name);

/**
 * Register a well-known port name.
 * @param name  Name to register.
 * @param port  Port ID to associate.
 * @return OK or error.
 */
error_t ipc_register_port(const char *name, port_t port);

/**
 * Abort a thread's IPC wait (called from the signal kill path when
 * force-terminating a blocked thread).  Unlinks the thread from
 * whatever IPC structure holds it (the port recv FIFO queue, or the
 * pending/reply-wait list holding its call message) and records
 * ERR_INTERRUPTED so the woken syscall returns an error instead of
 * re-blocking.  The blocked thread's ipc_call() remains the only code
 * that frees its pending message.  A late ipc_reply() for a freed
 * slot fails token validation (in_use + generation), so it can never
 * touch a reused slot.
 * @param t  Thread blocked in ipc_recv()/ipc_call() (no-op otherwise).
 */
void ipc_abort_wait(thread_t *t);

#endif /* KERNEL_IPC_H */
