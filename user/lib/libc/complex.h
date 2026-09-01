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
 * complex.h - Complex arithmetic (C11 §7.3)
 * Copyright (c) 2026 OpSys Project
 *
 * Provides the complex/imaginary type macros, the imaginary unit I,
 * the CMPLX/CMPLXF/CMPLXL constructors and the standard set of complex
 * math functions in double, float and long double variants.
 *
 * The function declarations here match the prototypes in C11 §7.3.5-7.3.9.
 * Implementations live in libc/complex.c (planned); the header itself is
 * declarations only so user code can rely on the prototypes today.
 */

#ifndef LIBC_COMPLEX_H
#define LIBC_COMPLEX_H

/* ====================================================================
 * Type macros (C11 §7.3.1)
 * ==================================================================== */

/* complex expands to the _Complex keyword (a type specifier). */
#define complex _Complex

/* imaginary expands to the _Imaginary keyword only when the compiler
 * actually supports imaginary types; otherwise the macro is left
 * undefined per C11 §6.10.8/§7.3.1.  GCC does not define
 * __STDC_IEC_559_COMPLEX__ on most targets, so `imaginary` is usually
 * absent — code should prefer `_Complex`. */
#ifdef __STDC_IEC_559_COMPLEX__
#define imaginary _Imaginary
#endif

/* The imaginary unit, as a compile-time constant.  Using
 * __builtin_complex keeps the value an exact +1.0i and lets the
 * compiler fold it. */
#define _Complex_I __builtin_complex(0.0F, 1.0F)
#define I          _Complex_I

/* CMPLX/CMPLXF/CMPLXL constructors (C11 §7.3.9.3) — build a complex
 * value from its real and imaginary parts.  __builtin_complex is used
 * so that an argument of NaN/Inf is preserved in the imaginary part
 * (a plain cast to _Complex would zero it out). */
#define CMPLX(x, y)  __builtin_complex((double)(x), (double)(y))
#define CMPLXF(x, y) __builtin_complex((float)(x), (float)(y))
#define CMPLXL(x, y) __builtin_complex((long double)(x), (long double)(y))

/* ====================================================================
 * Functions — double complex (C11 §7.3.5-7.3.9)
 * ==================================================================== */

/* Trigonometric (§7.3.5) */
double complex Cacos(double complex z);
double complex Casin(double complex z);
double complex Catan(double complex z);
double complex Ccos(double complex z);
double complex Csin(double complex z);
double complex Ctan(double complex z);

/* Hyperbolic (§7.3.6) */
double complex Cacosh(double complex z);
double complex Casinh(double complex z);
double complex Catanh(double complex z);
double complex Ccosh(double complex z);
double complex Csinh(double complex z);
double complex Ctanh(double complex z);

/* Exponential and logarithmic (§7.3.7) */
double complex Cexp(double complex z);
double complex Clog(double complex z);

/* Power and absolute value (§7.3.8) */
double         Cabs(double complex z);
double complex Cpow(double complex x, double complex z);
double complex Csqrt(double complex z);

/* Manipulation (§7.3.9) — these return real or complex results. */
double         Carg(double complex z);
double         Cimag(double complex z);
double complex Conj(double complex z);
double complex Cproj(double complex z);
double         Creal(double complex z);

/* ====================================================================
 * Functions — float complex
 * ==================================================================== */

/* Trigonometric */
float complex Cacosf(float complex z);
float complex Casinf(float complex z);
float complex Catanf(float complex z);
float complex Ccosf(float complex z);
float complex Csinf(float complex z);
float complex Ctanf(float complex z);

/* Hyperbolic */
float complex Cacoshf(float complex z);
float complex Casinhf(float complex z);
float complex Catanhf(float complex z);
float complex Ccoshf(float complex z);
float complex Csinhf(float complex z);
float complex Ctanhf(float complex z);

/* Exponential and logarithmic */
float complex Cexpf(float complex z);
float complex Clogf(float complex z);

/* Power and absolute value */
float         Cabsf(float complex z);
float complex Cpowf(float complex x, float complex z);
float complex Csqrtf(float complex z);

/* Manipulation */
float         Cargf(float complex z);
float         Cimagf(float complex z);
float complex Conjf(float complex z);
float complex Cprojf(float complex z);
float         Crealf(float complex z);

/* ====================================================================
 * Functions — long double complex
 * ==================================================================== */

/* Trigonometric */
long double complex Cacosl(long double complex z);
long double complex Casinl(long double complex z);
long double complex Catanl(long double complex z);
long double complex Ccosl(long double complex z);
long double complex Csinl(long double complex z);
long double complex Ctanl(long double complex z);

/* Hyperbolic */
long double complex Cacoshl(long double complex z);
long double complex Casinhl(long double complex z);
long double complex Catanhl(long double complex z);
long double complex Ccoshl(long double complex z);
long double complex Csinhl(long double complex z);
long double complex Ctanhl(long double complex z);

/* Exponential and logarithmic */
long double complex Cexpl(long double complex z);
long double complex Clogl(long double complex z);

/* Power and absolute value */
long double         Cabsl(long double complex z);
long double complex Cpowl(long double complex x, long double complex z);
long double complex Csqrtl(long double complex z);

/* Manipulation */
long double         Cargl(long double complex z);
long double         Cimagl(long double complex z);
long double complex Conjl(long double complex z);
long double complex Cprojl(long double complex z);
long double         Creall(long double complex z);

#endif /* LIBC_COMPLEX_H */
