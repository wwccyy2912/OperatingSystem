/*
 * signal.c - POSIX-style signal delivery
 * Copyright (c) 2026 OpSys Project
 *
 * Lazy delivery via checkpoints patched into the return paths:
 *
 *   - signal_check_syscall()    syscall_entry.S, after syscall_dispatch
 *                               returns, before the GPR pops + IRETQ
 *   - signal_check_interrupt()  isr_handler (idt.c), on every
 *                               interrupt/IRQ return to user mode
 *
 * Both run with IF=0 (interrupt gates) on the current thread's kernel
 * stack, with no spinlocks held by the caller.  They may re-enter the
 * scheduler (thread_exit / sched_enqueue) but never block.
 *
 * Semantics (Ring 3 migration, kernel_roadmap.md D4/P2):
 *   - kill(SIGKILL)            -> signal_kill_process(): force_exit on
 *                                 every thread + wake blocked ones;
 *                                 each thread dies at its next checkpoint.
 *   - kill(other)              -> set the pending bit; at the next
 *                                 checkpoint on some thread of the
 *                                 process, snapshot the interrupted
 *                                 context into a sigframe on the user
 *                                 stack and divert RIP/RDI/RSP into
 *                                 the Ring 3 dispatcher registered by
 *                                 the C runtime (see signal.h ABI).
 *                                 Handler lookup, ignore/default
 *                                 policy and default actions live in
 *                                 user/runtime/signal_user.c.
 *   - SYS_SIGRETURN            -> signal_restore(): copy the sigframe
 *                                 back into the current syscall frame.
 */

#include <kernel/signal.h>
#include <kernel/gdt.h>
#include <kernel/process.h>
#include <kernel/thread.h>
#include <kernel/sched.h>
#include <kernel/vmm.h>
#include <kernel/ipc.h>
#include <kernel/mutex.h>

/*
 * Syscall frame layout (syscall_entry.S): 15 GPR pushes (r15..rax)
 * followed by the 5-qword CPU frame (RIP, CS, RFLAGS, RSP, SS).
 * Indexes must match the SF_* constants in syscall_entry.S:
 *
 *   SF_RDI   72 -> idx 9
 *   SF_RIP  120 -> idx 15
 *   SF_CS   128 -> idx 16
 *   SF_RFLAGS 136 -> idx 17
 *   SF_RSP  144 -> idx 18
 *   SF_SS   152 -> idx 19
 */
#define SYSCALL_FRAME_SIZE 160
#define SF_RDI_IDX         9
#define SF_RIP_IDX         15
#define SF_CS_IDX          16
#define SF_RFLAGS_IDX      17
#define SF_RSP_IDX         18
#define SF_SS_IDX          19

/* ------------------------------------------------------------------ */

/*
 * Shared delivery core.  Called by both checkpoints with the ORIGINAL
 * interrupted context (gprs/rip/rsp/rflags of the user-mode frame).
 *
 * Returns true when the frame was rewritten to enter the Ring 3
 * dispatcher; false when nothing was delivered.
 */
static bool signal_check_common(process_t *proc, u64 *gprs, u64 *rip, u64 *rsp, u64 *rflags) {
    if (!proc || proc->sig_pending == 0)
        return false;

    u64 pending = proc->sig_pending;

    for (int signum = 1; signum < NSIG; signum++) {
        if (!(pending & (1ULL << signum)))
            continue;

        /* Single delivery, no queuing: consume the bit now */
        proc->sig_pending &= ~(1ULL << signum);

        /* Ring 3 migration: the kernel knows nothing about handlers.
         * Without a registered dispatcher the signal cannot be
         * delivered yet -- keep it pending and retry at the next
         * checkpoint. */
        if (proc->sig_dispatcher == 0) {
            proc->sig_pending |= (1ULL << signum);
            return false;
        }

        /* ---- Build the sigframe on the user stack ----
         *
         * Layout (signal.h): sigframe_t at [base, base+152), zeroed
         * 8-byte slot just below at [base-8, base); the dispatcher
         * enters with RSP = base-8 (≡ 8 mod 16, SysV ABI entry
         * alignment), RDI = sigframe base, RIP = dispatcher. */
        u64 base = (*rsp - SIGFRAME_TOTAL) & ~0xFULL;

        if (base < 0x1000 ||
            !vmm_validate_user_range(proc->addr_space, base - 8, SIGFRAME_TOTAL, true)) {
            /* Stack exhausted: cannot deliver now.  Keep the signal
             * pending and retry at the next checkpoint. */
            proc->sig_pending |= (1ULL << signum);
            return false;
        }

        /* Snapshot the interrupted user context (all values still
         * original here -- the rewrites happen below). */
        sigframe_t *sf = (sigframe_t *)(uintptr_t)base;
        for (int i = 0; i < 15; i++)
            sf->gprs[i] = gprs[i];
        sf->rip    = *rip;
        sf->rflags = *rflags;
        sf->rsp    = *rsp;
        sf->signum = (u64)signum;

        *(u64 *)(uintptr_t)(base - 8) = 0;

        /* Divert the return path into the Ring 3 dispatcher */
        gprs[9] = base; /* RDI = sigframe base */
        *rip    = proc->sig_dispatcher;
        *rsp    = base - 8;

        return true;
    }

    return false;
}

/*
 * Syscall-return checkpoint (syscall_entry.S).  `frame` points at the
 * 160-byte syscall frame at the top of the current thread's kernel
 * stack (RSP == frame at the call site).
 */
bool signal_check_syscall(u64 *frame) {
    thread_t *cur = thread_current();
    if (!cur)
        return false;

    /* Only user-mode frames can be diverted into handlers */
    if (frame[SF_CS_IDX] != GDT_SEL_UCODE)
        return false;

    /* Force-kill: terminate now with the recorded exit code */
    if (cur->force_exit) {
        thread_exit(cur->exit_code);
        /* never returns */
    }

    process_t *proc = process_current();
    if (!proc)
        return false;

    return signal_check_common(
        proc, frame, &frame[SF_RIP_IDX], &frame[SF_RSP_IDX], &frame[SF_RFLAGS_IDX]);
}

/*
 * Interrupt/IRQ-return checkpoint (idt.c isr_handler tail and IRQ
 * branch).  Called with the isr_common_stub interrupt frame.
 */
bool signal_check_interrupt(interrupt_frame_t *frame) {
    thread_t *cur = thread_current();
    if (!cur)
        return false;

    /* Only user-mode frames can be diverted into handlers */
    if ((frame->cs & 3) != 3)
        return false;

    /* Force-kill: terminate now with the recorded exit code */
    if (cur->force_exit) {
        thread_exit(cur->exit_code);
        /* never returns */
    }

    process_t *proc = process_current();
    if (!proc)
        return false;

    /* interrupt_frame_t is packed, so taking the address of its members
     * may produce unaligned pointers.  Copy the frame into a local,
     * naturally-aligned buffer using direct member access (the GPRs are
     * saved in the same order as the syscall frame: r15 first, rax last,
     * RDI = index 9). */
    u64 gprs[15];
    u64 rip, rsp, rflags;

    gprs[0]  = frame->r15;
    gprs[1]  = frame->r14;
    gprs[2]  = frame->r13;
    gprs[3]  = frame->r12;
    gprs[4]  = frame->r11;
    gprs[5]  = frame->r10;
    gprs[6]  = frame->r9;
    gprs[7]  = frame->r8;
    gprs[8]  = frame->rbp;
    gprs[9]  = frame->rdi;
    gprs[10] = frame->rsi;
    gprs[11] = frame->rdx;
    gprs[12] = frame->rcx;
    gprs[13] = frame->rbx;
    gprs[14] = frame->rax;
    rip      = frame->rip;
    rsp      = frame->rsp;
    rflags   = frame->rflags;

    bool delivered = signal_check_common(proc, gprs, &rip, &rsp, &rflags);

    /* Write back only what the common core may have rewritten
     * (handler entry point + RSP + RDI); RFLAGS stays interrupted. */
    if (delivered) {
        frame->r15 = gprs[0];
        frame->r14 = gprs[1];
        frame->r13 = gprs[2];
        frame->r12 = gprs[3];
        frame->r11 = gprs[4];
        frame->r10 = gprs[5];
        frame->r9  = gprs[6];
        frame->r8  = gprs[7];
        frame->rbp = gprs[8];
        frame->rdi = gprs[9];
        frame->rsi = gprs[10];
        frame->rdx = gprs[11];
        frame->rcx = gprs[12];
        frame->rbx = gprs[13];
        frame->rax = gprs[14];
        frame->rip = rip;
        frame->rsp = rsp;
    }

    return delivered;
}

/*
 * SYS_SIGRETURN implementation.  Restores the user context from the
 * sigframe at `frame_ptr` (user address) into the CURRENT syscall
 * frame -- the one syscall_dispatch is returning through, located at
 * the executing thread's kstack_top - SYSCALL_FRAME_SIZE.
 *
 * Returns the restored user RAX (becomes the value the interrupted
 * code sees the syscall returning), or an error code (< 0).
 */
i64 signal_restore(u64 frame_ptr) {
    thread_t *cur = thread_current();
    if (!cur)
        return (i64)ERR_FAULT;
    if (frame_ptr >= USER_PTR_MAX)
        return (i64)ERR_FAULT;

    process_t *proc = process_current();
    if (!proc || !proc->addr_space)
        return (i64)ERR_FAULT;
    if (!vmm_validate_user_range(proc->addr_space, frame_ptr, sizeof(sigframe_t), false))
        return (i64)ERR_FAULT;

    /* Read the sigframe from user memory (same address space: the
     * target process IS the current one). */
    const sigframe_t *usf = (const sigframe_t *)(uintptr_t)frame_ptr;
    sigframe_t        sf;
    for (int i = 0; i < 15; i++)
        sf.gprs[i] = usf->gprs[i];
    sf.rip    = usf->rip;
    sf.rflags = usf->rflags;
    sf.rsp    = usf->rsp;
    sf.signum = usf->signum;

    /* Rebuild the syscall frame with the interrupted user context.
     * cs/ss are rewritten defensively to the user segments. */
    u64 *frame = (u64 *)(uintptr_t)(cur->kstack_top - SYSCALL_FRAME_SIZE);
    for (int i = 0; i < 15; i++)
        frame[i] = sf.gprs[i];
    frame[SF_RIP_IDX]    = sf.rip;
    frame[SF_CS_IDX]     = GDT_SEL_UCODE;
    frame[SF_RFLAGS_IDX] = sf.rflags;
    frame[SF_RSP_IDX]    = sf.rsp;
    frame[SF_SS_IDX]     = GDT_SEL_UDATA;

    /* The restored RAX is what the interrupted code sees the syscall
     * returning (the entry stub re-stores it from the return value). */
    return (i64)sf.gprs[14];
}

/*
 * Force-terminate a process: mark every thread with force_exit (they
 * die at their next checkpoint) and wake blocked ones so the process
 * cannot linger.  Blocked threads are first unlinked from whatever
 * wait structure holds them, then re-enqueued as READY; when they run
 * their interrupted syscall returns an error and the checkpoint
 * thread_exit()s them.
 *
 * The CURRENT thread (if part of the target process) is only marked --
 * its checkpoint (or the direct thread_exit in the default-action
 * path) performs the exit.
 */
void signal_kill_process(process_t *proc, int exit_code) {
    if (!proc)
        return;

    for (tid_t tid = 0; tid < MAX_THREADS; tid++) {
        thread_t *t = thread_get(tid);
        if (!t || t->pid != proc->pid)
            continue;

        t->force_exit = true;
        t->exit_code  = exit_code;

        if (t->state == THREAD_STATE_BLOCKED) {
            /* Unlink from any wait structure, then wake.  Each helper
             * is a no-op when the thread is not blocked on it.
             *
             * A killed sleeper must be unlinked from the sleep list:
             * between the kill and the sleep deadline it would sit
             * simultaneously on the sleep list and in the ready tree,
             * violating the scheduler's single-structure invariant.
             * sched_unsleep() is a no-op for non-sleepers.  notify
             * (wait_mask) and join (joiner_tid) have no queue structure
             * to corrupt; process_wait's waiting_tid is cleared by
             * process_thread_exited() after the wake. */
            if (t->blocked_port != PORT_NULL)
                ipc_abort_wait(t);
            mutex_abort_wait(t);

            sched_unsleep(t);

            t->state = THREAD_STATE_READY;
            sched_enqueue(t);
        }
    }
}
