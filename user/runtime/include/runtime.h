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
 * runtime.h - Internal runtime declarations
 * Copyright (c) 2026 OpSys Project
 *
 * Internal header for the user-space C runtime. Not meant for
 * direct inclusion by user programs.
 */

#ifndef RUNTIME_H
#define RUNTIME_H

#include <stddef.h>

/* --- init/fini --- */
typedef void (*init_func_t)(void);
extern init_func_t __init_array_start[];
extern init_func_t __init_array_end[];

/* .fini_array destructors — provided by the linker script (user.ld).
 * Called in reverse order by _fini() during exit(). */
extern init_func_t __fini_array_start[];
extern init_func_t __fini_array_end[];

void _init(void);
void _fini(void);

/* --- atexit --- */
typedef void (*atexit_func_t)(void);
int Atexit(atexit_func_t func);

/* --- C++ static destructors (Itanium C++ ABI, see exit.c) --- */
/* __cxa_atexit() is called by the compiler-generated destructor
 * thunks; both signatures are fixed by the ABI, so no OpSys-specific
 * variant is allowed.  __cxa_finalize(NULL) runs every registered
 * destructor in LIFO order; exit() calls it after the Atexit() table
 * and before _fini(). */
int  __cxa_atexit(void (*func)(void *), void *arg, void *dso);
void __cxa_finalize(void *dso);

/* --- exit --- */
void exit(int code) __attribute__((noreturn));
void _exit(int code) __attribute__((noreturn));

#endif /* RUNTIME_H */
