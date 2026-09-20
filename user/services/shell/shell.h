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
 * shell.h - Shell command registration API
 * Copyright (c) 2026 OpSys Project
 *
 * The shell's command set is a runtime-registered singly-linked list,
 * not a compile-time table.  ShellMain() registers its 12 built-ins
 * itself; any other service (e.g. the manager) may add its own commands
 * via ShellRegisterCommand() BEFORE the shell starts reading input.
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
int ShellRegisterCommand(const char *name, const char *help, cmd_func_t func);

/* ====================================================================
 * Helpers exported for the shell's command modules (v0.9)
 *
 * The shell is a single process: its terminal/keyboard ports, current
 * working directory and line editor live in shell.c.  The command
 * modules (cmd_fs.c / cmd_disk.c / cmd_power.c / cmd_net.c) use these
 * accessors instead of duplicating that state.  All output goes to the
 * "term" port — never to the serial debug channel.
 * ==================================================================== */

#include <stddef.h>

void ShellWrite(const char *s);         /* raw string                       */
void ShellPutc(char c);                 /* single character                 */
void ShellPrintf(const char *fmt, ...); /* formatted (same subset as printf) */

/* Read one line from the console with the shell's line editor (history,
 * UTF-8, IME).  Echo is on for ShellReadLine and masked for
 * ShellReadLineMasked (passwords).  CR/LF is stripped.  Returns the
 * line length, or a negative error (ERR_INTERRUPTED on Ctrl-C,
 * ERR_NOENT on Ctrl-D at an empty line). */
int ShellReadLine(char *buf, int maxlen);
int ShellReadLineMasked(char *buf, int maxlen);

/* Resolve a user-supplied path against the shell's cwd into a VFS URL
 * (handles "", ".", "..", absolute paths and the optional /Volumes
 * prefix).  Returns 0 on success, negative on error. */
int ShellResolvePath(const char *path, char *out, size_t outsz);

/* The shell's current working directory (always an absolute VFS URL). */
const char *ShellCwd(void);

#endif /* SHELL_H */
