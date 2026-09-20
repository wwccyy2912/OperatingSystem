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
 * signal_user.c - Ring 3 signal semantics (kernel_roadmap.md D4/P2)
 * Copyright (c) 2026 OpSys Project
 *
 * The kernel keeps only the delivery MECHANISM: at a checkpoint it
 * snapshots the interrupted context into a sigframe_t on the user
 * stack and diverts execution here (__sig_dispatcher) with
 * RDI = sigframe base.  Everything else -- the per-process handler
 * table, SIG_IGN/SIG_DFL policy and the default-action list -- lives
 * in this file, in user memory, invisible to the kernel TCB.
 *
 * Dispatcher contract (entered like the old kernel-delivered handler):
 *   RIP = __sig_dispatcher, RDI = frame_base, RSP = frame_base - 8,
 *   [frame_base - 8] = 0 -> NEVER return; exit via SYS_SIGRETURN
 *   (context restore) or exit() (default terminate).
 *
 * Signal() is pure user space: swapping table slots touches no
 * supervisor state, so no syscall is involved.  Nothing in this file
 * adds a syscall either: the only two it uses already exist
 * (SYS_SIGNAL once at startup, SYS_SIGRETURN to resume, plus
 * SYS_GET_PID/SYS_KILL to re-raise a merged delivery).
 *
 * ------------------------------------------------------------------
 * Structure (signal_user):
 *   sigframe_t (kernel ABI) -> s_handlers[NSIG] (SIG_DFL/SIG_IGN/
 *   handler) -> s_in_handler / s_deferred bitmasks ->
 *   __sig_dispatcher -> SYS_SIGRETURN | exit().
 * How it works:
 *   The kernel parks a pending bit per signal and diverts the next user
 *   return into the dispatcher with the interrupted context in a
 *   sigframe.  The dispatcher validates the signal number, applies the
 *   table entry (ignore, default action, or handler) and restores the
 *   context with SYS_SIGRETURN.  A signal that arrives while its own
 *   handler runs is merged into one deferred bit and re-raised through
 *   Kill() after the handler returns, so handlers never nest on
 *   themselves and repeated deliveries coalesce.
 * Purpose:
 *   Deliver kernel-raised signals (SIGSEGV/SIGTERM/...) into user
 *   handlers while keeping all policy in Ring 3.
 * Caveats:
 *   - One delivery per signal per checkpoint (the kernel latches a bit,
 *     it does not queue): N rapid SIGUSR1 deliveries are observed as one
 *     or more handler runs, never as a counted queue.
 *   - There is no signal mask API yet (v1.0 sigprocmask): the only
 *     blocking that exists is the implicit "same signal while its own
 *     handler runs" merging above.
 *   - Handler state is process-wide (single global table), matching the
 *     process-wide pending bitmask in the kernel.
 *   - Handlers must not block on kernel IPC that re-enters the signal
 *     path, and (see signal.h) malloc/free/printf are not signal-safe.
 * ------------------------------------------------------------------
 */

#include <runtime.h>         /* exit() */
#include <errno.h>           /* EINVAL */
#include <libos/syscalls.h>  /* sys_call, SYS_*, sighandler_t, NSIG, Kill, GetPid */

/*
 * sigframe_t mirrors kernel/include/kernel/signal.h (ABI between the
 * kernel delivery core and this dispatcher).  Keep the two in sync.
 */
typedef struct {
    unsigned long gprs[15]; /* r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax */
    unsigned long rip;      /* interrupted instruction pointer */
    unsigned long rflags;   /* interrupted RFLAGS */
    unsigned long rsp;      /* interrupted user stack pointer */
    unsigned long signum;   /* signal number delivered */
} sigframe_t;

/* Per-process handler table: SIG_DFL (0), SIG_IGN (1) or handler addr.
 * Lives in user memory -- the kernel has no knowledge of it.  Slot 0 is
 * never used (signal numbers start at 1) and every access is bounds
 * checked against NSIG, so a corrupt signum can only fall back to
 * SIG_DFL. */
static sighandler_t s_handlers[NSIG];

/* Signals whose handler is currently executing.  POSIX blocks a signal
 * while its own handler runs; the kernel knows nothing about handlers
 * and would deliver again, so the dispatcher performs that part itself:
 * a repeat delivery is merged into s_deferred (one bit -- repeats
 * coalesce) and re-raised once the handler has returned. */
static volatile unsigned long s_in_handler = 0;
static volatile unsigned long s_deferred   = 0;

/* POSIX default action for a signal: terminate (SIGSEGV/SIGPIPE/
 * SIGALRM/SIGTERM) or ignore.  SIGKILL never reaches the dispatcher:
 * the kernel force-exits before delivery (process lifecycle). */
static int SDefaultTerminates(int signum) {
    switch (signum) {
    case SIGSEGV:
    case SIGPIPE:
    case SIGALRM:
    case SIGTERM:
        return 1;
    default:
        return 0;
    }
}

/* Restore the interrupted context from the sigframe at frame_base.
 * The kernel rebuilds the return frame and never comes back. */
static void __attribute__((noreturn)) s_sig_return(unsigned long frame_base) {
    sys_call(SYS_SIGRETURN, (long)frame_base, 0, 0, 0, 0);
    for (;;)
        ; /* unreachable: kernel never returns from SYS_SIGRETURN */
}

/*
 * Delivery entry point.  The kernel registers this address once at
 * process startup (see s_sig_install_dispatcher below); every pending
 * signal lands here with RDI = sigframe base.
 *
 * Never returns: the return address slot below the sigframe is zeroed
 * by the kernel, so every path ends in s_sig_return() (context restore)
 * or exit() (default action = terminate).
 */
void __attribute__((noreturn)) __sig_dispatcher(unsigned long frame_base) {
    const sigframe_t *sf     = (const sigframe_t *)frame_base;
    int               signum = (int)sf->signum;

    /* A signal number outside the table cannot be acted upon: treat it
     * as "ignore" and resume the interrupted context rather than
     * indexing the handler table out of bounds. */
    if (signum <= 0 || signum >= NSIG)
        s_sig_return(frame_base);

    /* Uncathable / reserved signals: the kernel never routes them here
     * (SIGKILL is force-exited kernel-side, SIGSTOP is a documented
     * no-op), but if one arrives the only sane policy is the POSIX one:
     * SIGKILL terminates, SIGSTOP has no observable effect. */
    if (signum == SIGKILL)
        exit(128 + SIGKILL);
    if (signum == SIGSTOP)
        s_sig_return(frame_base);

    unsigned long bit = 1UL << signum;

    /* Snapshot the slot: the handler may call Signal() and change it. */
    sighandler_t handler = s_handlers[signum];

    if (handler == SIG_IGN)
        s_sig_return(frame_base);

    if (handler == SIG_DFL) {
        if (SDefaultTerminates(signum))
            exit(128 + signum); /* runs atexit handlers, then dies */
        s_sig_return(frame_base); /* default action = ignore */
    }

    /* Re-entrancy protection: this signal is already being handled.
     * Merge the new delivery into the deferred bit (repeats coalesce)
     * and go back to the interrupted code instead of nesting. */
    if (s_in_handler & bit) {
        s_deferred |= bit;
        s_sig_return(frame_base);
    }

    s_in_handler |= bit;
    handler(signum);
    s_in_handler &= ~bit;

    /* Re-raise a merged delivery now that this handler has returned.
     * Kill() only latches the kernel's pending bit (no new syscall
     * surface), and the very next checkpoint -- the SYS_SIGRETURN
     * below -- delivers it. */
    if (s_deferred & bit) {
        s_deferred &= ~bit;
        (void)Kill(GetPid(), signum);
    }

    s_sig_return(frame_base);
}

/*
 * Register a handler for signum.  Pure user space: swap the table
 * slot and report the previous value.
 *
 * @param signum   Signal number (1 .. NSIG-1).
 * @param handler  SIG_DFL, SIG_IGN or a handler address.
 * @return Previous handler (SIG_DFL if never set), or SIG_ERR when
 *         signum is invalid or uncatchable (SIGKILL/SIGSTOP) or when
 *         handler itself is SIG_ERR.  errno is set to EINVAL on
 *         failure (POSIX signal() semantics); the stored table entry is
 *         left untouched, so a rejected call cannot disable a handler
 *         that was already installed.
 */
sighandler_t Signal(int signum, sighandler_t handler) {
    if (signum <= 0 || signum >= NSIG || signum == SIGKILL || signum == SIGSTOP ||
        handler == SIG_ERR) {
        errno = EINVAL;
        return SIG_ERR;
    }

    sighandler_t old   = s_handlers[signum];
    s_handlers[signum] = handler;
    return old;
}

/* Install the dispatcher once at process startup (.init_array), so
 * any kill() can be delivered even before main() runs.  A signal that
 * arrives before this constructor has run is not lost: the kernel keeps
 * the pending bit and retries at the next checkpoint (signal.c,
 * SignalCheckCommon: "without a registered dispatcher the signal cannot
 * be delivered yet"). */
__attribute__((constructor)) static void s_sig_install_dispatcher(void) {
    sys_call(SYS_SIGNAL, (long)(uintptr_t)__sig_dispatcher, 0, 0, 0, 0);
}
