/*
 * thread_ctx.c - SYS_THREAD_SET_CTX: overwrite a thread's saved context
 * Copyright (c) 2026 OpSys Project
 *
 * seL4 TCB_WriteRegisters equivalent (roadmap P2): replace the target
 * thread's saved register frame with values supplied by the caller.
 * The saved-context slot (thread_t fields rsp..rip) is consumed only by
 * the next context_switch INTO that thread, so a caller that rewrites a
 * ready/blocked thread's state controls exactly where and how the thread
 * resumes.  User-space signal trampolines use this to rewrite a thread's
 * ucontext-style frame before handing it back to the scheduler.
 *
 * This file implements only the primitive plus validation; the trampoline
 * itself lives in user space and is out of scope here.
 */

#include <kernel/thread_ctx.h>
#include <kernel/thread.h>
#include <kernel/process.h>
#include <kernel/vmm.h>
#include <kernel/string.h>

/* Compile-time lock: thread_ctx_t must exactly span thread_t's saved
 * register region (fields rsp..rip).  Both the struct above and the
 * RSP_OFFSET..RIP_OFFSET constants in context_switch.S are pinned to
 * this layout, so if either side drifts the build fails here instead of
 * corrupting a resumed thread at runtime. */
_Static_assert(offsetof(thread_t, rsp) + sizeof(thread_ctx_t) ==
                   offsetof(thread_t, rip) + sizeof(((thread_t *)0)->rip),
               "thread_ctx_t must exactly span the thread_t saved-context region");

/*
 * SYS_THREAD_SET_CTX — overwrite a target thread's saved register context.
 * a1 = tid, a2 = user pointer to a thread_ctx_t, a3 = ctx_size.
 *
 * Errors:
 *   ERR_INVAL — tid is negative, or ctx_size != sizeof(thread_ctx_t)
 *   ERR_NOENT — no thread with this TID, or it belongs to another process
 *   ERR_FAULT — ctx pointer range is not fully mapped in the caller
 *
 * Running-thread note: setting the context of the currently RUNNING
 * thread is allowed and is a harmless no-op in effect.  The saved-context
 * slot is only consumed by the next context_switch INTO that thread; a
 * running thread is on the CPU, never switched to.  The next time it
 * switches away, context_switch.S overwrites the slot with the live
 * registers before anything can observe the values written here, so the
 * write is dead data — safe to permit.  (It also spares the trampoline
 * from having to special-case "target == caller".)
 *
 * Atomicity: the int 0x80 syscall entry is an interrupt gate, so IF=0 for
 * the whole handler; the PIT can neither preempt us mid-copy nor switch
 * the target in while we write its slot.
 */
i64 sc_sys_thread_set_ctx(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    (void)a4;
    (void)a5;

    tid_t tid      = (tid_t)a1;
    u64   ctx_ptr  = a2;
    u64   ctx_size = a3;

    /* A negative TID cannot exist (the thread table is indexed by TID). */
    if (tid < 0)
        return (i64)ERR_INVAL;

    /* The caller must hand us exactly one thread_ctx_t. */
    if (ctx_size != sizeof(thread_ctx_t))
        return (i64)ERR_INVAL;

    /* Resolve the target: it must be a thread of the CALLING process.
     * A thread that exists but belongs to another process is reported
     * like an unknown TID so no existence information leaks. */
    process_t *proc = process_current();
    if (!proc)
        return (i64)ERR_FAULT;
    thread_t *target = thread_get(tid);
    if (!target || target->pid != proc->pid)
        return (i64)ERR_NOENT;

    /* Validate the user range before touching it: every page must be
     * mapped (the kernel reads the buffer, so need_write=false; a
     * present-but-unwritable page is fine).  This mirrors validate_user_ptr
     * in syscall.c, which wraps vmm_validate_user_range the same way, and
     * prevents an unmapped window from #PF-ing the kernel on the memcpy. */
    if (!vmm_validate_user_range(proc->addr_space, ctx_ptr, sizeof(thread_ctx_t), false))
        return (i64)ERR_FAULT;

    /* Copy into a kernel-local struct, then overwrite the target's
     * saved-context slot.  The slot is the thread_t region at
     * &target->rsp, exactly sizeof(thread_ctx_t) bytes wide (see the
     * _Static_assert above).  rax..r11 are NOT part of the frame:
     * context_switch.S only saves/restores rsp, rbx, rbp, r12..r15,
     * rflags and rip, so there is nothing else to preserve. */
    thread_ctx_t ctx;
    memcpy(&ctx, (const void *)(uptr)ctx_ptr, sizeof(ctx));
    memcpy(&target->rsp, &ctx, sizeof(ctx));

    return (i64)OK;
}
