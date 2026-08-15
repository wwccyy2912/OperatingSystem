/*
 * signal.h - POSIX-style signal definitions for user-space
 * Copyright (c) 2026 OpSys Project
 *
 * User-space signal handling interface.  Mirrors kernel/include/kernel/signal.h
 * but with user-facing types and helper functions.
 */

#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>

/* ====================================================================
 * Signal numbers (POSIX subset)
 * ==================================================================== */

#define SIGKILL 9  /* Kill signal (uncatchable, unignorable) */
#define SIGUSR1 10 /* User-defined signal 1 */
#define SIGSEGV 11 /* Segmentation fault */
#define SIGUSR2 12 /* User-defined signal 2 */
#define SIGPIPE 13 /* Broken pipe */
#define SIGALRM 14 /* Alarm clock */
#define SIGTERM 15 /* Termination signal */
#define SIGCHLD 17 /* Child status changed */
#define SIGCONT 18 /* Continue */
#define SIGSTOP 19 /* Stop (unignorable) */

/* ---- Handler sentinels ---- */
#define SIG_DFL ((sighandler_t)0)    /* Default action */
#define SIG_IGN ((sighandler_t)1)    /* Ignore signal */
#define SIG_ERR ((sighandler_t) - 1) /* Error return from signal() */

/* ====================================================================
 * Signal handler type
 * ==================================================================== */

typedef void (*sighandler_t)(int);

/* ====================================================================
 * Signal handling functions (from libos/syscalls.h)
 * ==================================================================== */

/**
 * Register a signal handler or query the current handler.
 *
 * @param signum  Signal number (1..NSIG-1)
 * @param handler SIG_DFL, SIG_IGN, or a user function pointer
 * @return        Previous handler on success, SIG_ERR on failure
 *
 * Signal delivery is process-wide and lazy (checkpoint-based).
 * For SIGKILL and SIGSTOP, registration fails (POSIX-compliant).
 */
sighandler_t signal(int signum, sighandler_t handler);

/**
 * Send a signal to a process.
 *
 * @param pid    Destination process ID
 * @param signum Signal number (1..NSIG-1)
 * @return       0 on success, negative error code on failure
 *               - ERR_NOENT: process not found
 *               - ERR_INVAL: invalid signal number
 *
 * The signal is delivered lazily at the next checkpoint on any thread
 * of the target process (syscall return or interrupt return).
 */
int kill(int pid, int signum);

/* ====================================================================
 * Signal handling best practices (comments for user code)
 * ==================================================================== */

/*
 * Signal-safe functions:
 *   - write()
 *   - signal(), kill()
 *   - exit(), _exit()
 *   - Async-safe list per POSIX 1003.1-2004
 *
 * NOT signal-safe:
 *   - malloc(), free(), realloc()  (v0.1)
 *   - printf()
 *   - any libc function with internal locks
 *   - pthread_* functions
 *
 * To safely allocate in a signal handler (v0.1):
 *   1. Pre-allocate a buffer in main()
 *   2. Use it in the handler without malloc
 *   OR
 *   3. Set a flag and return; handle in main loop
 */

/*
 * Default actions for each signal:
 *
 *   SIGKILL      - Terminate (always, cannot be caught)
 *   SIGUSR1      - Terminate
 *   SIGSEGV      - Terminate
 *   SIGUSR2      - Terminate
 *   SIGPIPE      - Terminate
 *   SIGALRM      - Terminate
 *   SIGTERM      - Terminate
 *   SIGCHLD      - Ignore
 *   SIGCONT      - Continue (unblocked by SIGSTOP)
 *   SIGSTOP      - Stop process (uncatchable, unignorable)
 */

/*
 * Signal masks (v1.0+ feature):
 *
 *   sigprocmask()    - Block/unblock signals (per-thread in v1.0)
 *   sigemptyset()    - Clear a signal set
 *   sigfillset()     - Fill a signal set
 *   sigaddset()      - Add signal to set
 *   sigdelset()      - Remove signal from set
 *   sigismember()    - Test membership
 *   sigpending()     - Query pending signals
 *   sigsuspend()     - Sleep until signal
 *
 * Not implemented in v0.1 (process-wide only).
 */

#endif /* SIGNAL_H */
