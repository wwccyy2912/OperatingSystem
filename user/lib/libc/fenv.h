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
 * fenv.h - Floating-point environment (C11 §7.6)
 * Copyright (c) 2026 OpSys Project
 *
 * Provides types and functions to inspect and control the floating-point
 * environment: exception flags, rounding modes, and the full environment
 * save/restore.  Bit values match the x87 FPU control/status layout
 * (the build uses -mno-sse, so all FP state lives in the x87 unit).
 */

#ifndef LIBC_FENV_H
#define LIBC_FENV_H

/* ====================================================================
 * Types (C11 §7.6.1)
 * ==================================================================== */

/* fenv_t — captures the x87 FPU control, status and tag words plus the
 * (unused under -mno-sse) MXCSR state.  Used by fegetenv/feholdexcept/
 * fesetenv/feupdateenv to save and restore the environment. */
typedef struct {
    unsigned short __control; /* x87 control word */
    unsigned short __status;  /* x87 status word  */
    unsigned short __tag;     /* x87 tag word     */
    unsigned int   __mxcsr;   /* SSE control/status (reserved) */
} fenv_t;

/* fexcept_t — represents the floating-point exception flags. */
typedef int fexcept_t;

/* ====================================================================
 * Exception macros (C11 §7.6.2)
 *
 * Bit values correspond to the x87 status word.
 * ==================================================================== */

#define FE_INVALID   0x0001
#define FE_DIVBYZERO 0x0004
#define FE_OVERFLOW  0x0008
#define FE_UNDERFLOW 0x0010
#define FE_INEXACT   0x0020

#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

/* ====================================================================
 * Rounding mode macros (C11 §7.6.3)
 *
 * Bit values correspond to bits 10-11 of the x87 control word.
 * ==================================================================== */

#define FE_TONEAREST  0x0000
#define FE_DOWNWARD   0x0400
#define FE_UPWARD     0x0800
#define FE_TOWARDZERO 0x0C00

/* ====================================================================
 * Default environment (C11 §7.6.4)
 * ==================================================================== */

extern const fenv_t __fe_dfl_env;
#define FE_DFL_ENV (&__fe_dfl_env)

/* ====================================================================
 * Functions — exception flags (C11 §7.6.2.1-7.6.2.5)
 * ==================================================================== */

/* Clear the given exception flags.  Returns 0 on success. */
int Feclearexcept(int excepts);

/* Store the current state of the given exception flags in *flagp. */
int Fegetexceptflag(fexcept_t *flagp, int excepts);

/* Raise the given exceptions. */
int Feraiseexcept(int excepts);

/* Set the given exception flags from *flagp (does not raise them). */
int Fesetexceptflag(const fexcept_t *flagp, int excepts);

/* Return the bitwise OR of the exception flags that are set among
 * `excepts`. */
int Fetestexcept(int excepts);

/* ====================================================================
 * Functions — rounding mode (C11 §7.6.3.1-7.6.3.2)
 * ==================================================================== */

/* Return the current rounding mode (one of the FE_* macros). */
int Fegetround(void);

/* Set the rounding mode.  Returns 0 on success. */
int Fesetround(int round);

/* ====================================================================
 * Functions — environment (C11 §7.6.4.1-7.6.4.4)
 * ==================================================================== */

/* Store the current FP environment in *envp. */
int Fegetenv(fenv_t *envp);

/* Save the current environment in *envp, clear exception flags, and
 * install a non-stop mode for all exceptions. */
int Feholdexcept(fenv_t *envp);

/* Restore the FP environment from *envp. */
int Fesetenv(const fenv_t *envp);

/* Save the current exceptions, install *envp, then raise the saved
 * exceptions. */
int Feupdateenv(const fenv_t *envp);

#endif /* LIBC_FENV_H */
