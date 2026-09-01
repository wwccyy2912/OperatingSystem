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
 * ipc.c - Inter-Process Communication
 * Copyright (c) 2026 OpSys Project
 *
 * Synchronous message passing via ports. Threads block on send/recv
 * until the other side is ready. Supports call/reply for RPC.
 *
 * v0.2: Single-CPU, cli/sti spinlock (s_ipc_lock) protects every queue
 * manipulation.  A per-port reply-wait list replaces the old single-slot
 * s_active_call: each call gets an opaque reply token (pool index + 1
 * generation), so concurrent callers to one port no longer clobber each
 * other, and a destroyed port can wake every waiter with an explicit
 * error.
 *
 * v0.3: Reply tokens carry a per-slot GENERATION counter.  A token is
 * only valid while its slot is in use AND its generation matches, so a
 * stale token (slot freed, reallocated to a different call) is rejected
 * instead of replying to the wrong caller (ABA reuse).
 *
 * Message ownership: the blocked thread owns its pending message and is
 * the ONLY one that frees it.  Receivers/repliers/destroy only copy data
 * or record an error and wake the owner -- they never free.  This makes
 * the wakeup path free of use-after-free by construction.
 *
 * Lock discipline (hard rule): NEVER call thread_yield /
 * sched_reschedule / sched_sleep while holding s_ipc_lock -- blocking
 * with IF=0 is a guaranteed deadlock.  Every block path releases the
 * lock first, then yields, then re-acquires on wakeup.
 *
 * ------------------------------------------------------------------
 * Structure (IPC):
 *
 *   sender (IpcSend/IpcCall)            receiver (IpcRecvFrom)
 *       |  send/call                          |  recv
 *       v                                     v
 *   +------------+  pending queue  +---------------------+
 *   |  port      | <=============> | wait_queue          |
 *   +------------+  (blocked msgs) +---------------------+
 *       |  call: parked on s_reply_wait[port], token handed to recv
 *       v
 *   IpcReply(token) copies resp, wakes the caller (owner frees msg)
 *
 * How it works:
 *   Senders and receivers rendezvous on a port.  If a receiver is
 *   already blocked, the message is bounced through a kernel buffer
 *   and copied into the receiver's user space under ITS CR3; otherwise
 *   the sender blocks with its message parked on the port's pending
 *   queue.  Calls park on a reply-wait list and give the receiver an
 *   opaque reply token (pool slot + generation).
 * Purpose:
 *   Synchronous port-based messaging between threads with call/reply
 *   for RPC, plus a name registry (IpcRegisterPort/IpcGetPort) for
 *   well-known services.
 * Caveats:
 *   The blocked thread owns its pending message and is the only one
 *   that frees it.  Never yield/sleep while holding s_ipc_lock (IF=0
 *   deadlock); reply tokens carry a generation so stale tokens are
 *   rejected instead of replying to the wrong caller.
 * ------------------------------------------------------------------
 */

#include <kernel/ipc.h>
#include <kernel/string.h> /* memcpy: qword-optimized (shared) */
#include <kernel/serial.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/vmm.h>
#include <kernel/process.h> /* process_current(): sender subject (P0) */

extern thread_t *thread_current(void);
extern void      SchedEnqueue(thread_t *t);


#define PENDING_POOL_SIZE  128
#define PORT_REGISTRY_SIZE 64

typedef struct ipc_pending_msg {
    struct ipc_pending_msg *next;
    tid_t                   sender_tid;
    /* P0 地基: kernel-filled sender subject (unforgeable, docs
     * permission_model.md §三).  Filled at enqueue time from the
     * sender's process subject; userland cannot modify it. */
    subject_id_t sender_subject;
    port_t       port; /* port the message was sent/called on */
    u32          msg_len;
    u8           msg_data[MAX_MSG_SIZE];
    u8           resp_data[MAX_MSG_SIZE];
    u32          resp_len;
    error_t      err; /* OK, or ERR_NOENT when the port died */
    bool         is_call;
    bool         replied; /* IpcReply() already delivered */
    bool         in_use;
    u16          generation; /* bumped on every alloc: makes stale
                              * tokens (slot reused by another call)
                              * fail validation */
} ipc_pending_msg_t;

typedef struct {
    void        *buf;
    u32         *len_ptr;
    u32          token;
    subject_id_t sender_subject; /* P0: filled by deliver_to_waiter */
    error_t      err;
} recv_slot_t;

static spinlock_t         s_ipc_lock = SPINLOCK_INIT;
static port_entry_t       s_port_table[MAX_PORTS];
static ipc_pending_msg_t  s_pending_pool[PENDING_POOL_SIZE];
static ipc_pending_msg_t *s_free_list;
static ipc_pending_msg_t *s_pending_head[MAX_PORTS];
static ipc_pending_msg_t *s_reply_wait[MAX_PORTS];
static recv_slot_t        s_recv[MAX_THREADS];
static struct {
    char   name[64];
    port_t port;
} s_port_registry[PORT_REGISTRY_SIZE];
static u32 s_port_registry_count;

/* ---- Pool helpers (caller holds s_ipc_lock) ---- */

/*
 * Reply tokens are opaque to userland but encode slot + generation:
 *   token = (generation << 16) | (pool_index + 1)
 * The generation is bumped on every allocation, so a token survives
 * only while its slot holds the SAME call.  If the slot is freed and
 * reused by a different call, the stale token fails validation in
 * token_to_msg() instead of replying to the wrong caller.
 */
#define IPC_TOKEN_GEN_SHIFT 16
#define IPC_TOKEN_SLOT_MASK 0xFFFFu

static u32 TokenOf(ipc_pending_msg_t *m) {
    u32 slot = (u32)(m - s_pending_pool) + 1;
    return ((u32)m->generation << IPC_TOKEN_GEN_SHIFT) | slot;
}

static ipc_pending_msg_t *token_to_msg(u32 token) {
    u32 slot = token & IPC_TOKEN_SLOT_MASK;
    u16 gen  = (u16)(token >> IPC_TOKEN_GEN_SHIFT);
    if (slot == 0 || slot > PENDING_POOL_SIZE)
        return NULL;
    ipc_pending_msg_t *m = &s_pending_pool[slot - 1];
    if (!m->in_use)
        return NULL;
    if (m->generation != gen)
        return NULL; /* stale token: slot reused by another call */
    return m;
}

static ipc_pending_msg_t *pending_alloc(void) {
    if (!s_free_list)
        return NULL;
    ipc_pending_msg_t *m = s_free_list;
    s_free_list          = m->next;
    m->next              = NULL;
    m->in_use            = true;
    /* Keep the generation inside 15 bits: a reply token must never
     * carry the sign bit.  Userland treats tokens as opaque but uses
     * the `s_pending_token >= 0` idiom with a -1 sentinel (keyboard.c
     * and serial.c parked-read slots); once the generation overflows
     * 0x7FFF (after ~32k calls on a slot), tokens turn negative and
     * those checks silently fail — the parked blocking read is never
     * completed and the client freezes forever.  The wrap window is
     * far wider than any call's lifetime, and a slot held by a pending
     * call is never reallocated, so stale-token validation is
     * unaffected. */
    m->generation = (u16)((m->generation + 1) & 0x7FFF);
    return m;
}

static void PendingFree(ipc_pending_msg_t *m) {
    if (!m)
        return;
    m->in_use     = false;
    m->sender_tid = -1;
    m->next       = s_free_list;
    s_free_list   = m;
}

static port_entry_t *port_lookup(port_t p) {
    if (p == PORT_NULL || p > MAX_PORTS)
        return NULL;
    return s_port_table[p - 1].in_use ? &s_port_table[p - 1] : NULL;
}

/* Wake a thread only if it is still blocked (avoids double-insert into
 * the CFS tree -- SchedEnqueue() sets state=READY unconditionally). */
static void WakeThread(thread_t *t) {
    if (t && t->state == THREAD_STATE_BLOCKED) {
        t->state = THREAD_STATE_READY;
        SchedEnqueue(t);
        /* A woken thread is no longer blocked on any port. */
        t->blocked_port = PORT_NULL;
    }
}

/* P0 地基: resolve the current thread's process subject (0 = System/
 * no process).  The kernel fills sender_subject from this at enqueue
 * time — a user message cannot forge it. */
static subject_id_t IpcSenderSubject(void) {
    process_t *proc = process_current();
    return proc ? proc->subject_id : 0;
}

/* Copy msg (a KERNEL buffer -- callers must have bounced the sender's
 * user data into m->msg_data first) into a recv-blocked thread's USER
 * buffer, record the call token if any, wake it.  Caller holds
 * s_ipc_lock.
 *
 * Cross-address-space delivery: rs->buf is a user pointer valid in the
 * RECEIVER's address space, but we run under the SENDER's CR3.  A plain
 * copy would write through the sender's page tables (silently hitting
 * whatever that virtual address maps to in the sender -- usually its own
 * BSS) and the receiver would never see the message.  Switch CR3 to the
 * receiver's addr_space for the copy.  This is safe: the kernel-half is
 * mapped in every PML4 (so our own stack/heap stay valid) and IF=0 while
 * s_ipc_lock is held (no timer IRQ can fire mid-switch). */
static void DeliverToWaiter(port_entry_t *p,
                              thread_t     *recv,
                              const void   *msg,
                              u32           len,
                              u32           token,
                              subject_id_t  sender_subject) {
    recv_slot_t *rs = &s_recv[recv->tid];
    if (rs->buf) {
        u32           n     = (len < *rs->len_ptr) ? len : *rs->len_ptr;
        addr_space_t *saved = thread_current()->addr_space;
        VmmSwitchAddrSpace(recv->addr_space);
        memcpy(rs->buf, msg, n);
        *rs->len_ptr = len;
        VmmSwitchAddrSpace(saved);
    }
    if (token != 0)
        rs->token = token;
    rs->sender_subject = sender_subject;
    thread_t **pp      = &p->wait_queue;
    while (*pp) {
        if (*pp == recv) {
            *pp        = recv->next;
            recv->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }
    WakeThread(recv);
}

/* Link m into the port's pending queue and block the current thread.
 * Caller holds s_ipc_lock; the lock is released before yielding and
 * re-acquired before returning.  On return m->err carries the outcome
 * and the caller still owns m (must pending_free it). */
static error_t BlockWithPending(port_t port, ipc_pending_msg_t *m, const void *msg, u32 len) {
    thread_t *cur = thread_current();
    m->msg_len    = len;
    if (msg && len > 0)
        memcpy(m->msg_data, msg, len);
    m->next                  = s_pending_head[port - 1];
    s_pending_head[port - 1] = m;
    /* Lock held (IF=0): the state change + ready-tree dequeue are
     * atomic w.r.t. the timer IRQ.  A BLOCKED thread must not stay in
     * the ready tree or Reschedule() can force-pick it as `next`. */
    cur->state        = THREAD_STATE_BLOCKED;
    cur->blocked_port = port;
    SchedDequeue(cur);
    SpinUnlock(&s_ipc_lock);
    ThreadYield();
    SpinLock(&s_ipc_lock);
    return m->err;
}

void IpcInit(void) {
    /* All static data is BSS-zeroed on first boot; build the free list */
    for (i32 i = PENDING_POOL_SIZE - 1; i >= 0; i--) {
        s_pending_pool[i].next = s_free_list;
        s_free_list            = &s_pending_pool[i];
    }
}

port_t IpcPortCreate(void) {
    SpinLock(&s_ipc_lock);
    thread_t *cur = thread_current();
    for (u32 i = 0; i < MAX_PORTS; i++) {
        if (!s_port_table[i].in_use) {
            port_entry_t *p = &s_port_table[i];
            p->port_id      = (port_t)(i + 1);
            p->owner_tid    = cur->tid;
            p->owner_pid    = cur->pid;
            p->wait_queue   = NULL;
            p->in_use       = true;
            SpinUnlock(&s_ipc_lock);
            return p->port_id;
        }
    }
    SpinUnlock(&s_ipc_lock);
    return (port_t)ERR_NOMEM;
}

void IpcPortDestroy(port_t port) {
    SpinLock(&s_ipc_lock);
    port_entry_t *p = port_lookup(port);
    if (!p) {
        SpinUnlock(&s_ipc_lock);
        return;
    }

    /* 1. Threads blocked in ipc_recv: wake with ERR_NOENT via slot */
    thread_t *t = p->wait_queue;
    while (t) {
        thread_t *n        = t->next;
        s_recv[t->tid].err = ERR_NOENT;
        WakeThread(t);
        t = n;
    }
    p->wait_queue = NULL;

    /* 2. Pending messages (not yet received): mark ERR_NOENT, wake the
     *    owner.  The owner frees its own message. */
    ipc_pending_msg_t **pp = &s_pending_head[port - 1];
    while (*pp) {
        ipc_pending_msg_t *m = *pp;
        *pp                  = m->next;
        m->next              = NULL;
        m->err               = ERR_NOENT;
        WakeThread(thread_get(m->sender_tid));
    }

    /* 3. Call messages awaiting reply: same treatment */
    pp = &s_reply_wait[port - 1];
    while (*pp) {
        ipc_pending_msg_t *m = *pp;
        *pp                  = m->next;
        m->next              = NULL;
        m->err               = ERR_NOENT;
        WakeThread(thread_get(m->sender_tid));
    }

    p->in_use = false;
    SpinUnlock(&s_ipc_lock);
}

/*
 * Tear down every IPC resource owned by a dying process:
 *   1. Destroy all its ports — ipc_port_destroy wakes every blocked
 *      receiver / pending sender / awaiting-reply caller with
 *      ERR_NOENT, so no client can hang on a dead peer.
 *   2. Drop registry names whose port belonged to the dying process,
 *      so a restarted service can re-register its well-known name
 *      (without this the name leaks and restart fails with ERR_BUSY).
 * Called from ProcessReap() (process.c) with the process already
 * marked ZOMBIE; its threads are all dead.
 */
void IpcCleanupProcess(pid_t pid) {
    for (u32 i = 0; i < MAX_PORTS; i++) {
        if (s_port_table[i].in_use && s_port_table[i].owner_pid == pid)
            IpcPortDestroy((port_t)(i + 1));
    }

    SpinLock(&s_ipc_lock);
    for (u32 i = 0; i < s_port_registry_count;) {
        port_entry_t *p = port_lookup(s_port_registry[i].port);
        if (!p || p->owner_pid == pid) {
            /* Name is dangling (port destroyed) or owned by the dead
             * process: shift the tail down and retry this slot. */
            for (u32 j = i; j + 1 < s_port_registry_count; j++)
                s_port_registry[j] = s_port_registry[j + 1];
            s_port_registry_count--;
        } else {
            i++;
        }
    }
    SpinUnlock(&s_ipc_lock);
}

error_t IpcSend(port_t port, const void *msg, u32 len) {    SpinLock(&s_ipc_lock);
    port_entry_t *p = port_lookup(port);
    if (!p) {
        SpinUnlock(&s_ipc_lock);
        return ERR_NOENT;
    }
    if (len > MAX_MSG_SIZE) {
        SpinUnlock(&s_ipc_lock);
        return ERR_INVAL;
    }

    thread_t *recv = p->wait_queue;
    if (recv) {
        /* Bounce through a kernel buffer: deliver_to_waiter copies into
         * the receiver's user space under ITS CR3, so the source must
         * not live in our (sender's) user pages. */
        ipc_pending_msg_t *m = pending_alloc();
        if (!m) {
            SpinUnlock(&s_ipc_lock);
            return ERR_NOMEM;
        }
        m->msg_len        = len;
        m->sender_subject = IpcSenderSubject();
        if (msg && len > 0)
            memcpy(m->msg_data, msg, len);
        DeliverToWaiter(p, recv, m->msg_data, len, 0, m->sender_subject);
        PendingFree(m);
        SpinUnlock(&s_ipc_lock);
        return OK;
    }

    ipc_pending_msg_t *m = pending_alloc();
    if (!m) {
        SpinUnlock(&s_ipc_lock);
        return ERR_NOMEM;
    }
    m->sender_tid     = thread_current()->tid;
    m->sender_subject = IpcSenderSubject();
    m->port           = port;
    m->is_call        = false;
    m->replied        = false;
    m->err            = OK;
    m->resp_len       = 0;

    error_t err = BlockWithPending(port, m, msg, len);
    /* Woken: delivered (OK) or port destroyed (ERR_NOENT).  We own m. */
    PendingFree(m);
    SpinUnlock(&s_ipc_lock);
    return err;
}

error_t
IpcRecvFrom(port_t port, void *buf, u32 *len, u32 *tok_ptr, subject_id_t *sender_subject) {
    SpinLock(&s_ipc_lock);
    port_entry_t *p = port_lookup(port);
    if (!p) {
        SpinUnlock(&s_ipc_lock);
        return ERR_NOENT;
    }
    if (tok_ptr)
        *tok_ptr = 0;
    if (sender_subject)
        *sender_subject = 0;

    ipc_pending_msg_t *m = s_pending_head[port - 1];
    if (m) {
        s_pending_head[port - 1] = m->next;
        m->next                  = NULL;
        if (buf) {
            u32 n = (m->msg_len < *len) ? m->msg_len : *len;
            memcpy(buf, m->msg_data, n);
        }
        *len = m->msg_len;
        /* P0 地基: expose the kernel-filled sender subject */
        if (sender_subject)
            *sender_subject = m->sender_subject;
        if (m->is_call) {
            /* Move to the reply-wait list; hand the owner a token */
            m->next                = s_reply_wait[port - 1];
            s_reply_wait[port - 1] = m;
            if (tok_ptr)
                *tok_ptr = TokenOf(m);
        } else {
            WakeThread(thread_get(m->sender_tid));
            /* sender frees m after it wakes */
        }
        SpinUnlock(&s_ipc_lock);
        return OK;
    }

    /* Block for a message */
    thread_t *cur    = thread_current();
    s_recv[cur->tid] = (recv_slot_t){buf, len, 0, 0, OK};
    /* Lock held (IF=0): state change + ready-tree dequeue are atomic
     * w.r.t. the timer IRQ.  A BLOCKED thread must not stay in the
     * ready tree or Reschedule() can force-pick it as `next`. */
    cur->state        = THREAD_STATE_BLOCKED;
    cur->blocked_port = port;
    cur->next         = p->wait_queue;
    p->wait_queue     = cur;
    SchedDequeue(cur);
    SpinUnlock(&s_ipc_lock);
    ThreadYield();
    SpinLock(&s_ipc_lock);
    recv_slot_t rs   = s_recv[cur->tid];
    s_recv[cur->tid] = (recv_slot_t){NULL, NULL, 0, 0, OK};
    if (tok_ptr)
        *tok_ptr = rs.token;
    if (sender_subject)
        *sender_subject = rs.sender_subject;
    error_t err = rs.err;
    SpinUnlock(&s_ipc_lock);
    return err;
}

error_t IpcRecv(port_t port, void *buf, u32 *len, u32 *tok_ptr) {
    /* Existing 4-arg semantics unchanged; subject out-param = NULL. */
    return IpcRecvFrom(port, buf, len, tok_ptr, NULL);
}

error_t IpcCall(port_t port, const void *req, u32 req_len, void *resp, u32 *resp_len) {
    SpinLock(&s_ipc_lock);
    port_entry_t *p = port_lookup(port);
    if (!p) {
        SpinUnlock(&s_ipc_lock);
        return ERR_NOENT;
    }
    if (req_len > MAX_MSG_SIZE) {
        SpinUnlock(&s_ipc_lock);
        return ERR_INVAL;
    }

    thread_t          *cur = thread_current();
    ipc_pending_msg_t *m   = pending_alloc();
    if (!m) {
        SpinUnlock(&s_ipc_lock);
        return ERR_NOMEM;
    }
    m->sender_tid     = cur->tid;
    m->sender_subject = IpcSenderSubject();
    m->port           = port;
    m->is_call        = true;
    m->replied        = false;
    m->err            = OK;
    m->resp_len       = 0;

    thread_t *recv = p->wait_queue;
    if (recv) {
        /* Receiver already waiting: bounce req into kernel-owned
         * m->msg_data first (deliver_to_waiter copies into the
         * receiver's user space under ITS CR3), record the token in
         * its recv slot, park m on the reply-wait list. */
        m->msg_len = req_len;
        if (req && req_len > 0)
            memcpy(m->msg_data, req, req_len);
        DeliverToWaiter(p, recv, m->msg_data, req_len, TokenOf(m), m->sender_subject);
        m->next                = s_reply_wait[port - 1];
        s_reply_wait[port - 1] = m;
    } else {
        m->msg_len = req_len;
        if (req && req_len > 0)
            memcpy(m->msg_data, req, req_len);
        m->next                  = s_pending_head[port - 1];
        s_pending_head[port - 1] = m;
    }
    /* Lock held (IF=0): state change + ready-tree dequeue are atomic
     * w.r.t. the timer IRQ.  A BLOCKED thread must not stay in the
     * ready tree or Reschedule() can force-pick it as `next`. */
    cur->state        = THREAD_STATE_BLOCKED;
    cur->blocked_port = port;
    SchedDequeue(cur);
    SpinUnlock(&s_ipc_lock);
    ThreadYield();
    SpinLock(&s_ipc_lock);
    error_t err = m->err;
    if (err == OK && resp) {
        memcpy(resp, m->resp_data, m->resp_len);
        *resp_len = m->resp_len;
    }
    /* We own m; free it.  (ipc_reply only copies + wakes.) */
    PendingFree(m);
    SpinUnlock(&s_ipc_lock);
    return err;
}

error_t IpcReply(u32 token, const void *msg, u32 len) {
    SpinLock(&s_ipc_lock);
    if (len > MAX_MSG_SIZE) {
        SpinUnlock(&s_ipc_lock);
        return ERR_INVAL;
    }

    ipc_pending_msg_t *m = token_to_msg(token);
    if (!m) {
        SpinUnlock(&s_ipc_lock);
        return ERR_NOENT;
    }
    if (!m->is_call || m->replied) {
        SpinUnlock(&s_ipc_lock);
        return ERR_BUSY;
    }

    memcpy(m->resp_data, msg, len);
    m->resp_len = len;
    m->replied  = true;

    /* Unlink from its port's reply-wait list */
    ipc_pending_msg_t **pp = &s_reply_wait[m->port - 1];
    while (*pp) {
        if (*pp == m) {
            *pp     = m->next;
            m->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }

    WakeThread(thread_get(m->sender_tid));
    SpinUnlock(&s_ipc_lock);
    return OK;
}

/*
 * Abort a thread's IPC wait (called from the signal kill path when
 * force-terminating a blocked thread).  Unlinks the thread from
 * whatever IPC structure holds it and records ERR_INTERRUPTED so the
 * woken syscall returns an error instead of re-blocking.
 *
 * Ownership rule preserved: the blocked thread's IpcCall() is the
 * only code that frees its pending message -- it does so after waking,
 * seeing the recorded error.  A late IpcReply() for a freed slot
 * fails token validation (in_use + generation), so it can never touch
 * a reused slot.  Caller must NOT hold s_ipc_lock.
 */
void IpcAbortWait(thread_t *t) {
    if (!t || t->blocked_port == PORT_NULL)
        return;

    SpinLock(&s_ipc_lock);

    port_t port = t->blocked_port;

    /* Unlink from the port's recv FIFO queue if present (harmless
   when the thread is a caller -- callers never join this queue). */
    port_entry_t *p = port_lookup(port);
    if (p) {
        thread_t **pp = &p->wait_queue;
        while (*pp) {
            if (*pp == t) {
                *pp     = t->next;
                t->next = NULL;
                break;
            }
            pp = &(*pp)->next;
        }
    }

    if (s_recv[t->tid].buf != NULL) {
        /* Recv-wait: fail the slot so IpcRecv() returns an error */
        s_recv[t->tid].err = ERR_INTERRUPTED;
    } else {
        /* Call-wait: unlink the sender's pending message from its list
   and record the error; IpcCall() frees it after waking. */
        bool                found    = false;
        ipc_pending_msg_t **lists[2] = {
            &s_pending_head[port - 1],
            &s_reply_wait[port - 1],
        };
        for (int i = 0; i < 2 && !found; i++) {
            ipc_pending_msg_t **pp = lists[i];
            while (*pp) {
                ipc_pending_msg_t *m = *pp;
                if (m->sender_tid == t->tid) {
                    *pp     = m->next;
                    m->next = NULL;
                    m->err  = ERR_INTERRUPTED;
                    found   = true;
                    break;
                }
                pp = &(*pp)->next;
            }
        }
    }

    t->blocked_port = PORT_NULL;
    SpinUnlock(&s_ipc_lock);
}

port_t IpcGetPort(const char *name) {
    SpinLock(&s_ipc_lock);
    for (u32 i = 0; i < s_port_registry_count; i++) {
        if (strcmp(s_port_registry[i].name, name) == 0) {
            port_t port = s_port_registry[i].port;
            SpinUnlock(&s_ipc_lock);
            return port;
        }
    }
    SpinUnlock(&s_ipc_lock);
    return (port_t)ERR_NOENT;
}

error_t IpcRegisterPort(const char *name, port_t port) {
    SpinLock(&s_ipc_lock);
    for (u32 i = 0; i < s_port_registry_count; i++) {
        if (strcmp(s_port_registry[i].name, name) == 0) {
            SpinUnlock(&s_ipc_lock);
            return ERR_BUSY;
        }
    }
    if (s_port_registry_count >= PORT_REGISTRY_SIZE) {
        SpinUnlock(&s_ipc_lock);
        return ERR_OVERFLOW;
    }
    if (!port_lookup(port)) {
        SpinUnlock(&s_ipc_lock);
        return ERR_NOENT;
    }
    u32   idx = s_port_registry_count;
    char *dst = s_port_registry[idx].name;
    u32   i   = 0;
    while (name[i] && i < sizeof(s_port_registry[idx].name) - 1) {
        dst[i] = name[i];
        i++;
    }
    dst[i]                    = '\0';
    s_port_registry[idx].port = port;
    s_port_registry_count++;
    SpinUnlock(&s_ipc_lock);
    return OK;
}
