/*
 * shell.h - Shell command registration API
 * Copyright (c) 2026 OpSys Project
 *
 * The shell's command set is a runtime-registered singly-linked list,
 * not a compile-time table.  shell_main() registers its 12 built-ins
 * itself; any other service (e.g. the manager) may add its own commands
 * via shell_register_command() BEFORE the shell starts reading input.
 *
 * Registration only succeeds while the shell is not yet running its
 * loop (single-writer rule, see shell.c): call this from the shell's
 * own thread or from another service before it spawns the shell.
 */

#ifndef SHELL_H
#define SHELL_H

/* Command entry point: argc/argv as parsed by the shell. */
typedef int (*cmd_func_t)(int argc, char *argv[]);

/*
 * Register a command with the shell.
 *
 *   name, help — copied (strdup'd) by the callee; caller may use
 *                string literals.
 *   func       — the command implementation.
 *
 * Returns OK (0) on success; ERR_NOMEM if any allocation fails;
 * ERR_INVAL if name/help/func is NULL or the name is already
 * registered.
 */
int shell_register_command(const char *name, const char *help, cmd_func_t func);

#endif /* SHELL_H */
