/*
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
int atexit(atexit_func_t func);

/* --- exit --- */
void exit(int code) __attribute__((noreturn));
void _exit(int code) __attribute__((noreturn));

#endif /* RUNTIME_H */
