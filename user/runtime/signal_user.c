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
 * supervisor state, so no syscall is involved.
 
 *
 * ------------------------------------------------------------------
 * Structure (signal_user):
 *   syscall/interrupt signal frames -> SignalRaise/SignalRestore ->
 *   user dispatcher trampoline on the user stack.
 * How it works:
 *   The kernel parks pending signals; the user stub saves context to
 *   a sigframe, runs the handler, then restores via syscall.
 * Purpose:
 *   Deliver kernel-raised signals (SIGSEGV etc.) into user handlers.
 * Caveats:
 *   One delivery per signal per check point (no queuing); handlers
 *   must not block on kernel IPC that re-enters the signal path.
 * ------------------------------------------------------------------
 */

#include <runtime.h>         /* exit() */
#include <libos/syscalls.h>  /* sys_call, SYS_*, sighandler_t, NSIG */

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
 * Lives in user memory -- the kernel has no knowledge of it. */
static sighandler_t s_handlers[NSIG];

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
 */
void __sig_dispatcher(unsigned long frame_base) {
    const sigframe_t *sf     = (const sigframe_t *)frame_base;
    int               signum = (int)sf->signum;

    sighandler_t h = (signum > 0 && signum < NSIG) ? s_handlers[signum] : SIG_DFL;

    if (h == SIG_IGN) {
        s_sig_return(frame_base);
    } else if (h == SIG_DFL) {
        if (SDefaultTerminates(signum))
            exit(128 + signum); /* runs atexit handlers, then dies */
        s_sig_return(frame_base); /* default = ignore */
    } else {
        h(signum);
        s_sig_return(frame_base);
    }
}

/*
 * Register a handler for signum.  Pure user space: swap the table
 * slot and report the previous value.
 *
 * @return Previous handler (SIG_DFL if never set), or SIG_ERR when
 *         signum is invalid or uncatchable (SIGKILL/SIGSTOP).
 */
sighandler_t Signal(int signum, sighandler_t handler) {
    if (signum <= 0 || signum >= NSIG || signum == SIGKILL || signum == SIGSTOP)
        return SIG_ERR;
    sighandler_t old   = s_handlers[signum];
    s_handlers[signum] = handler;
    return old;
}

/* Install the dispatcher once at process startup (.init_array), so
 * any kill() can be delivered even before main() runs. */
__attribute__((constructor)) static void s_sig_install_dispatcher(void) {
    sys_call(SYS_SIGNAL, (long)(uintptr_t)__sig_dispatcher, 0, 0, 0, 0);
}
