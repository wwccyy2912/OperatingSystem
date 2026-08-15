/*
 * signal.h - POSIX-style signal delivery
 * Copyright (c) 2026 OpSys Project
 *
 * Kernel-side signal support.  Signal numbers are kept as a tiny
 * POSIX subset (SIGKILL/SIGUSR1/SIGSEGV/SIGTERM/...).  Semantics:
 *
 *   - Handlers live per-PROCESS (process_t.sig_handlers[64]).
 *     Address space is process-wide, so any thread may run the
 *     handler.  SIGKILL/SIGSTOP cannot be caught or ignored.
 *   - Pending signals are a process-wide bitmask
 *     (process_t.sig_pending).  Delivery is LAZY: the bit is set by
 *     kill(), and the handler actually runs at the next delivery
 *     checkpoint on some thread of the target process.
 *   - Checkpoints: (1) syscall return to user mode (patched into
 *     syscall_entry.S after syscall_dispatch), (2) interrupt/IRQ
 *     return to user mode (isr_handler tail).  A 100 Hz PIT
 *     guarantees any running thread hits checkpoint (2) within
 *     10 ms.  Blocked threads get their signal when they unblock
 *     and return to user mode.
 *   - SIG_DFL (0) = default action (ignore; SIGKILL terminates),
 *     SIG_IGN (1) = ignore, anything else = user handler address.
 *
 * The sigframe_t layout is the ABI between the kernel (delivery,
 * restore) and the user-space __restore_rt trampoline
 * (user/runtime/sigrestore.S).  Keep the two in sync.
 */

#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include <kernel/types.h>
#include <kernel/idt.h>

typedef struct process process_t;

/* ---- Signal numbers (small POSIX subset) ---- */
#define SIGKILL     9    /* uncatchable, unignorable terminate */
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGSTOP     19   /* reserved: cannot be caught/ignored */

/* ---- Handler sentinels ---- */
#define SIG_DFL     0    /* default action */
#define SIG_IGN     1    /* ignore */

/* Number of signal slots (1..63 valid, 0 unused) */
#define NSIG        64

/*
 * sigframe_t - user-context snapshot saved on the user stack when a
 * signal is delivered, and consumed by SYS_SIGRETURN.
 *
 * Layout on the user stack (growing DOWN from the interrupted RSP):
 *
 *   high addr  interrupted user stack (RSP = orig_rsp)
 *              [sigframe_t: 15 GPRs | rip | rflags | rsp | signum]  152 B
 *              [restorer address, 8 bytes]   <- handler entry RSP
 *   low addr   base = align_down(orig_rsp - SIGFRAME_TOTAL, 16)
 *
 * The sigframe occupies [base, base + sizeof(sigframe_t)); the
 * 8-byte restorer address (__restore_rt) sits just below it at
 * [base - 8, base).  The handler is entered with RIP = handler,
 * RDI = signum, RSP = base - 8 (≡ 8 mod 16, SysV ABI entry
 * alignment).  When the handler returns (ret), control lands on the
 * restorer address with RSP == base, which __restore_rt passes to
 * SYS_SIGRETURN as arg1 — the kernel reads the sigframe from base.
 *
 * GPR order matches the syscall/interrupt frame GPR order
 * (r15,r14,...,rbx,rax) so restore is a straight array copy.
 * Total: 15*8 + 4*8 = 152 bytes.
 */
typedef struct {
        u64 gprs[15];   /* r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax */
        u64 rip;        /* interrupted instruction pointer */
        u64 rflags;     /* interrupted RFLAGS */
        u64 rsp;        /* interrupted user stack pointer */
        u64 signum;     /* signal number delivered */
} sigframe_t;

/* Total bytes the sigframe + restorer slot occupy below orig_rsp */
#define SIGFRAME_TOTAL   (sizeof(sigframe_t) + 8)

/*
 * Delivery checkpoints.  Called with the CURRENT kernel frame on the
 * stack; both may rewrite the frame's RIP/RSP/RDI to divert the
 * return path into a user handler.  Never called for kernel-mode
 * frames (caller checks frame->cs == 0x1B / user mode).
 *
 * Returns true if the frame was rewritten to enter a user handler
 * (the return path will land in the handler).  Returns false if
 * nothing was delivered (caller resumes the original context).
 * A default-action termination (or SIGKILL) never returns.
 */
bool signal_check_syscall(u64 *frame);        /* syscall_entry.S frame (SF_* layout) */
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

/*
 * POSIX default action for a signal: true = terminate the process
 * (SIGKILL/SIGSEGV/SIGPIPE/SIGALRM/SIGTERM), false = ignore.
 * Used by both the delivery checkpoints and the kill() syscall.
 */
bool signal_default_terminates(int signum);

#endif /* KERNEL_SIGNAL_H */
