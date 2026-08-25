/*
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
 */

#include <kernel/ipc.h>
#include <kernel/string.h> /* memcpy: qword-optimized (shared) */
#include <kernel/serial.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/vmm.h>
#include <kernel/process.h> /* process_current(): sender subject (P0) */

extern thread_t *thread_current(void);
extern void      sched_enqueue(thread_t *t);


static i32 ipc_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (i32)(u8)*a - (i32)(u8)*b;
}

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
    bool         replied; /* ipc_reply() already delivered */
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

static u32 token_of(ipc_pending_msg_t *m) {
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

static void pending_free(ipc_pending_msg_t *m) {
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
 * the CFS tree -- sched_enqueue() sets state=READY unconditionally). */
static void wake_thread(thread_t *t) {
    if (t && t->state == THREAD_STATE_BLOCKED) {
        t->state = THREAD_STATE_READY;
        sched_enqueue(t);
        /* A woken thread is no longer blocked on any port. */
        t->blocked_port = PORT_NULL;
    }
}

/* P0 地基: resolve the current thread's process subject (0 = System/
 * no process).  The kernel fills sender_subject from this at enqueue
 * time — a user message cannot forge it. */
static subject_id_t ipc_sender_subject(void) {
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
static void deliver_to_waiter(port_entry_t *p,
                              thread_t     *recv,
                              const void   *msg,
                              u32           len,
                              u32           token,
                              subject_id_t  sender_subject) {
    recv_slot_t *rs = &s_recv[recv->tid];
    if (rs->buf) {
        u32           n     = (len < *rs->len_ptr) ? len : *rs->len_ptr;
        addr_space_t *saved = thread_current()->addr_space;
        vmm_switch_addr_space(recv->addr_space);
        memcpy(rs->buf, msg, n);
        *rs->len_ptr = len;
        vmm_switch_addr_space(saved);
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
    wake_thread(recv);
}

/* Link m into the port's pending queue and block the current thread.
 * Caller holds s_ipc_lock; the lock is released before yielding and
 * re-acquired before returning.  On return m->err carries the outcome
 * and the caller still owns m (must pending_free it). */
static error_t block_with_pending(port_t port, ipc_pending_msg_t *m, const void *msg, u32 len) {
    thread_t *cur = thread_current();
    m->msg_len    = len;
    if (msg && len > 0)
        memcpy(m->msg_data, msg, len);
    m->next                  = s_pending_head[port - 1];
    s_pending_head[port - 1] = m;
    /* Lock held (IF=0): the state change + ready-tree dequeue are
     * atomic w.r.t. the timer IRQ.  A BLOCKED thread must not stay in
     * the ready tree or reschedule() can force-pick it as `next`. */
    cur->state        = THREAD_STATE_BLOCKED;
    cur->blocked_port = port;
    sched_dequeue(cur);
    spin_unlock(&s_ipc_lock);
    thread_yield();
    spin_lock(&s_ipc_lock);
    return m->err;
}

void ipc_init(void) {
    /* All static data is BSS-zeroed on first boot; build the free list */
    for (i32 i = PENDING_POOL_SIZE - 1; i >= 0; i--) {
        s_pending_pool[i].next = s_free_list;
        s_free_list            = &s_pending_pool[i];
    }
}

port_t ipc_port_create(void) {
    spin_lock(&s_ipc_lock);
    thread_t *cur = thread_current();
    for (u32 i = 0; i < MAX_PORTS; i++) {
        if (!s_port_table[i].in_use) {
            port_entry_t *p = &s_port_table[i];
            p->port_id      = (port_t)(i + 1);
            p->owner_tid    = cur->tid;
            p->owner_pid    = cur->pid;
            p->wait_queue   = NULL;
            p->in_use       = true;
            spin_unlock(&s_ipc_lock);
            return p->port_id;
        }
    }
    spin_unlock(&s_ipc_lock);
    return (port_t)ERR_NOMEM;
}

void ipc_port_destroy(port_t port) {
    spin_lock(&s_ipc_lock);
    port_entry_t *p = port_lookup(port);
    if (!p) {
        spin_unlock(&s_ipc_lock);
        return;
    }

    /* 1. Threads blocked in ipc_recv: wake with ERR_NOENT via slot */
    thread_t *t = p->wait_queue;
    while (t) {
        thread_t *n        = t->next;
        s_recv[t->tid].err = ERR_NOENT;
        wake_thread(t);
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
        wake_thread(thread_get(m->sender_tid));
    }

    /* 3. Call messages awaiting reply: same treatment */
    pp = &s_reply_wait[port - 1];
    while (*pp) {
        ipc_pending_msg_t *m = *pp;
        *pp                  = m->next;
        m->next              = NULL;
        m->err               = ERR_NOENT;
        wake_thread(thread_get(m->sender_tid));
    }

    p->in_use = false;
    spin_unlock(&s_ipc_lock);
}

/*
 * Tear down every IPC resource owned by a dying process:
 *   1. Destroy all its ports — ipc_port_destroy wakes every blocked
 *      receiver / pending sender / awaiting-reply caller with
 *      ERR_NOENT, so no client can hang on a dead peer.
 *   2. Drop registry names whose port belonged to the dying process,
 *      so a restarted service can re-register its well-known name
 *      (without this the name leaks and restart fails with ERR_BUSY).
 * Called from process_reap() (process.c) with the process already
 * marked ZOMBIE; its threads are all dead.
 */
void ipc_cleanup_process(pid_t pid) {
    for (u32 i = 0; i < MAX_PORTS; i++) {
        if (s_port_table[i].in_use && s_port_table[i].owner_pid == pid)
            ipc_port_destroy((port_t)(i + 1));
    }

    spin_lock(&s_ipc_lock);
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
    spin_unlock(&s_ipc_lock);
}

error_t ipc_send(port_t port, const void *msg, u32 len) {    spin_lock(&s_ipc_lock);
    port_entry_t *p = port_lookup(port);
    if (!p) {
        spin_unlock(&s_ipc_lock);
        return ERR_NOENT;
    }
    if (len > MAX_MSG_SIZE) {
        spin_unlock(&s_ipc_lock);
        return ERR_INVAL;
    }

    thread_t *recv = p->wait_queue;
    if (recv) {
        /* Bounce through a kernel buffer: deliver_to_waiter copies into
         * the receiver's user space under ITS CR3, so the source must
         * not live in our (sender's) user pages. */
        ipc_pending_msg_t *m = pending_alloc();
        if (!m) {
            spin_unlock(&s_ipc_lock);
            return ERR_NOMEM;
        }
        m->msg_len        = len;
        m->sender_subject = ipc_sender_subject();
        if (msg && len > 0)
            memcpy(m->msg_data, msg, len);
        deliver_to_waiter(p, recv, m->msg_data, len, 0, m->sender_subject);
        pending_free(m);
        spin_unlock(&s_ipc_lock);
        return OK;
    }

    ipc_pending_msg_t *m = pending_alloc();
    if (!m) {
        spin_unlock(&s_ipc_lock);
        return ERR_NOMEM;
    }
    m->sender_tid     = thread_current()->tid;
    m->sender_subject = ipc_sender_subject();
    m->port           = port;
    m->is_call        = false;
    m->replied        = false;
    m->err            = OK;
    m->resp_len       = 0;

    error_t err = block_with_pending(port, m, msg, len);
    /* Woken: delivered (OK) or port destroyed (ERR_NOENT).  We own m. */
    pending_free(m);
    spin_unlock(&s_ipc_lock);
    return err;
}

error_t
ipc_recv_from(port_t port, void *buf, u32 *len, u32 *tok_ptr, subject_id_t *sender_subject) {
    spin_lock(&s_ipc_lock);
    port_entry_t *p = port_lookup(port);
    if (!p) {
        spin_unlock(&s_ipc_lock);
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
                *tok_ptr = token_of(m);
        } else {
            wake_thread(thread_get(m->sender_tid));
            /* sender frees m after it wakes */
        }
        spin_unlock(&s_ipc_lock);
        return OK;
    }

    /* Block for a message */
    thread_t *cur    = thread_current();
    s_recv[cur->tid] = (recv_slot_t){buf, len, 0, 0, OK};
    /* Lock held (IF=0): state change + ready-tree dequeue are atomic
     * w.r.t. the timer IRQ.  A BLOCKED thread must not stay in the
     * ready tree or reschedule() can force-pick it as `next`. */
    cur->state        = THREAD_STATE_BLOCKED;
    cur->blocked_port = port;
    cur->next         = p->wait_queue;
    p->wait_queue     = cur;
    sched_dequeue(cur);
    spin_unlock(&s_ipc_lock);
    thread_yield();
    spin_lock(&s_ipc_lock);
    recv_slot_t rs   = s_recv[cur->tid];
    s_recv[cur->tid] = (recv_slot_t){NULL, NULL, 0, 0, OK};
    if (tok_ptr)
        *tok_ptr = rs.token;
    if (sender_subject)
        *sender_subject = rs.sender_subject;
    error_t err = rs.err;
    spin_unlock(&s_ipc_lock);
    return err;
}

error_t ipc_recv(port_t port, void *buf, u32 *len, u32 *tok_ptr) {
    /* Existing 4-arg semantics unchanged; subject out-param = NULL. */
    return ipc_recv_from(port, buf, len, tok_ptr, NULL);
}

error_t ipc_call(port_t port, const void *req, u32 req_len, void *resp, u32 *resp_len) {
    spin_lock(&s_ipc_lock);
    port_entry_t *p = port_lookup(port);
    if (!p) {
        spin_unlock(&s_ipc_lock);
        return ERR_NOENT;
    }
    if (req_len > MAX_MSG_SIZE) {
        spin_unlock(&s_ipc_lock);
        return ERR_INVAL;
    }

    thread_t          *cur = thread_current();
    ipc_pending_msg_t *m   = pending_alloc();
    if (!m) {
        spin_unlock(&s_ipc_lock);
        return ERR_NOMEM;
    }
    m->sender_tid     = cur->tid;
    m->sender_subject = ipc_sender_subject();
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
        deliver_to_waiter(p, recv, m->msg_data, req_len, token_of(m), m->sender_subject);
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
     * ready tree or reschedule() can force-pick it as `next`. */
    cur->state        = THREAD_STATE_BLOCKED;
    cur->blocked_port = port;
    sched_dequeue(cur);
    spin_unlock(&s_ipc_lock);
    thread_yield();
    spin_lock(&s_ipc_lock);
    error_t err = m->err;
    if (err == OK && resp) {
        memcpy(resp, m->resp_data, m->resp_len);
        *resp_len = m->resp_len;
    }
    /* We own m; free it.  (ipc_reply only copies + wakes.) */
    pending_free(m);
    spin_unlock(&s_ipc_lock);
    return err;
}

error_t ipc_reply(u32 token, const void *msg, u32 len) {
    spin_lock(&s_ipc_lock);
    if (len > MAX_MSG_SIZE) {
        spin_unlock(&s_ipc_lock);
        return ERR_INVAL;
    }

    ipc_pending_msg_t *m = token_to_msg(token);
    if (!m) {
        spin_unlock(&s_ipc_lock);
        return ERR_NOENT;
    }
    if (!m->is_call || m->replied) {
        spin_unlock(&s_ipc_lock);
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

    wake_thread(thread_get(m->sender_tid));
    spin_unlock(&s_ipc_lock);
    return OK;
}

/*
 * Abort a thread's IPC wait (called from the signal kill path when
 * force-terminating a blocked thread).  Unlinks the thread from
 * whatever IPC structure holds it and records ERR_INTERRUPTED so the
 * woken syscall returns an error instead of re-blocking.
 *
 * Ownership rule preserved: the blocked thread's ipc_call() is the
 * only code that frees its pending message -- it does so after waking,
 * seeing the recorded error.  A late ipc_reply() for a freed slot
 * fails token validation (in_use + generation), so it can never touch
 * a reused slot.  Caller must NOT hold s_ipc_lock.
 */
void ipc_abort_wait(thread_t *t) {
    if (!t || t->blocked_port == PORT_NULL)
        return;

    spin_lock(&s_ipc_lock);

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
        /* Recv-wait: fail the slot so ipc_recv() returns an error */
        s_recv[t->tid].err = ERR_INTERRUPTED;
    } else {
        /* Call-wait: unlink the sender's pending message from its list
   and record the error; ipc_call() frees it after waking. */
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
    spin_unlock(&s_ipc_lock);
}

port_t ipc_get_port(const char *name) {
    spin_lock(&s_ipc_lock);
    for (u32 i = 0; i < s_port_registry_count; i++) {
        if (ipc_strcmp(s_port_registry[i].name, name) == 0) {
            port_t port = s_port_registry[i].port;
            spin_unlock(&s_ipc_lock);
            return port;
        }
    }
    spin_unlock(&s_ipc_lock);
    return (port_t)ERR_NOENT;
}

error_t ipc_register_port(const char *name, port_t port) {
    spin_lock(&s_ipc_lock);
    for (u32 i = 0; i < s_port_registry_count; i++) {
        if (ipc_strcmp(s_port_registry[i].name, name) == 0) {
            spin_unlock(&s_ipc_lock);
            return ERR_BUSY;
        }
    }
    if (s_port_registry_count >= PORT_REGISTRY_SIZE) {
        spin_unlock(&s_ipc_lock);
        return ERR_OVERFLOW;
    }
    if (!port_lookup(port)) {
        spin_unlock(&s_ipc_lock);
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
    spin_unlock(&s_ipc_lock);
    return OK;
}
