/*
 * signal.h - POSIX-style signal delivery (mechanism only)
 * Copyright (c) 2026 OpSys Project
 *
 * Ring 3 migration (kernel_roadmap.md D4/P2): the kernel keeps ONLY
 * the delivery mechanism; all semantics -- handler table,
 * SIG_IGN/SIG_DFL policy, default actions -- live in the user-space
 * runtime (user/runtime/signal_user.c).
 *
 *   - Pending signals are a process-wide bitmask
 *     (process_t.sig_pending).  Delivery is LAZY: the bit is set by
 *     kill(), and delivery happens at the next checkpoint on some
 *     thread of the target process.
 *   - Checkpoints: (1) syscall return to user mode (patched into
 *     syscall_entry.S after syscall_dispatch), (2) interrupt/IRQ
 *     return to user mode (isr_handler tail).  A 100 Hz PIT
 *     guarantees any running thread hits checkpoint (2) within
 *     10 ms.  Blocked threads get their signal when they unblock
 *     and return to user mode.
 *   - Delivery snapshots the interrupted context into a sigframe_t on
 *     the user stack and diverts RIP = process_t.sig_dispatcher (the
 *     Ring 3 entry point registered once via SYS_SIGNAL by the C
 *     runtime), RDI = sigframe base.
 *   - kill(SIGKILL) force-exits kernel-side (process lifecycle, not
 *     signal semantics); SIGSTOP stays a reserved no-op.
 *
 * The sigframe_t layout is the ABI between the kernel (delivery,
 * restore) and the user-space dispatcher (user/runtime/signal_user.c).
 * Keep the two in sync.
 */

#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include <kernel/types.h>
#include <kernel/idt.h>

typedef struct process process_t;

/* ---- Signal numbers (small POSIX subset) ---- */
#define SIGKILL 9 /* uncatchable, unignorable terminate */
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTOP 19 /* reserved: cannot be caught/ignored */

/* Number of signal slots (1..63 valid, 0 unused) */
#define NSIG 64

/*
 * sigframe_t - user-context snapshot saved on the user stack when a
 * signal is delivered, and consumed by SYS_SIGRETURN.
 *
 * Layout on the user stack (growing DOWN from the interrupted RSP):
 *
 *   high addr  interrupted user stack (RSP = orig_rsp)
 *              [sigframe_t: 15 GPRs | rip | rflags | rsp | signum]  152 B
 *              [zeroed slot, 8 bytes]        <- dispatcher entry RSP
 *   low addr   base = align_down(orig_rsp - SIGFRAME_TOTAL, 16)
 *
 * The sigframe occupies [base, base + sizeof(sigframe_t)); the 8-byte
 * slot just below it at [base - 8, base) is ZEROED.  The dispatcher
 * is entered with RIP = sig_dispatcher, RDI = base, RSP = base - 8
 * (≡ 8 mod 16, SysV ABI entry alignment) and must exit via
 * SYS_SIGRETURN (context restore) or exit() -- never by returning,
 * which would jump to the zero slot and fault loudly.
 *
 * GPR order matches the syscall/interrupt frame GPR order
 * (r15,r14,...,rbx,rax) so restore is a straight array copy.
 * Total: 15*8 + 4*8 = 152 bytes.
 */
typedef struct {
    u64 gprs[15]; /* r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax */
    u64 rip;      /* interrupted instruction pointer */
    u64 rflags;   /* interrupted RFLAGS */
    u64 rsp;      /* interrupted user stack pointer */
    u64 signum;   /* signal number delivered */
} sigframe_t;

/* Total bytes the sigframe + restorer slot occupy below orig_rsp */
#define SIGFRAME_TOTAL (sizeof(sigframe_t) + 8)

/*
 * Delivery checkpoints.  Called with the CURRENT kernel frame on the
 * stack; both may rewrite the frame's RIP/RSP/RDI to divert the
 * return path into the Ring 3 dispatcher.  Never called for
 * kernel-mode frames (caller checks frame->cs == 0x1B / user mode).
 *
 * Returns true if the frame was rewritten to enter the dispatcher
 * (the return path will land in it).  Returns false if nothing was
 * delivered (caller resumes the original context).  A force-exit
 * (SIGKILL) never returns from the checkpoint itself.
 */
bool signal_check_syscall(u64 *frame);                 /* syscall_entry.S frame (SF_* layout) */
bool signal_check_interrupt(interrupt_frame_t *frame); /* isr_common_stub frame */

/*
 * SYS_SIGRETURN implementation.  Restores the user context from the
 * sigframe at `frame_ptr` (user address) into the CURRENT syscall
 * frame (the one syscall_dispatch is returning through, located at
 * the executing thread's kstack_top - 160).  Returns the restored
 * user RAX, or an error code (< 0) when the frame pointer is invalid.
 */
i64 signal_restore(u64 frame_ptr);

/*
 * Force-kill support: set force_exit (+exit_code) on every thread of
 * the target process and wake blocked ones (unlinking them from their
 * IPC/mutex wait structures first).  The delivery checkpoints
 * (signal_check_*) notice force_exit and call thread_exit() with the
 * recorded exit code, so the whole process dies within one checkpoint
 * round.  exit_code is the process exit code (e.g. 128 + signum).
 */
void signal_kill_process(process_t *proc, int exit_code);

#endif /* KERNEL_SIGNAL_H */
