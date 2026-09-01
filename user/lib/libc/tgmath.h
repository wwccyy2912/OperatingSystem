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
 * tgmath.h - Type-generic math (C11 §7.22)
 * Copyright (c) 2026 OpSys Project
 *
 * Type-generic macros that dispatch to the float / double / long double
 * (and complex) variant of each math function based on the argument
 * type.  Pulls in <math.h> and <complex.h>.
 *
 * On GCC 10+ the dispatch is folded by __builtin_tgmath into a single
 * call; otherwise a portable _Generic-based fallback is used.  Both
 * paths conform to C11 §7.22.
 */

#ifndef LIBC_TGMATH_H
#define LIBC_TGMATH_H

#include "math.h"
#include "complex.h"

/* ====================================================================
 * Dispatch implementation
 *
 * Two strategies are provided:
 *
 *  (1) __builtin_tgmath (GCC 10+) — pass the float/double/long double
 *      function pointers (plus the complex variants where applicable)
 *      followed by the arguments; the compiler selects the target.
 *
 *  (2) _Generic fallback — selects the function by the type of the
 *      (first) argument.  Integer arguments fall through to the double
 *      variant via the `default:` clause, matching C11 §7.22p3.
 * ==================================================================== */

#if defined(__GNUC__) && (__GNUC__ >= 10) && !defined(__clang__)

/* --- __builtin_tgmath helpers ------------------------------------- */

/* Real + complex, F is the real basename (e.g. acos -> cacos).
 * The complex variants are built by pasting `c` + F + suffix. */
#define __TGMATH_1rc(F, x)    __builtin_tgmath(F##f, F, F##l, c##F##f, c##F, c##F##l, x)
#define __TGMATH_2rc(F, x, y) __builtin_tgmath(F##f, F, F##l, c##F##f, c##F, c##F##l, x, y)

/* Real-only. */
#define __TGMATH_1r(F, x)       __builtin_tgmath(F##f, F, F##l, x)
#define __TGMATH_2r(F, x, y)    __builtin_tgmath(F##f, F, F##l, x, y)
#define __TGMATH_3r(F, x, y, z) __builtin_tgmath(F##f, F, F##l, x, y, z)

/* Complex-only — F already carries the `c` prefix (e.g. carg). */
#define __TGMATH_1c(F, x)    __builtin_tgmath(F##f, F, F##l, x)
#define __TGMATH_2c(F, x, y) __builtin_tgmath(F##f, F, F##l, x, y)

#else /* !__builtin_tgmath — _Generic fallback */

/* Real + complex, 1 arg.  Integer types hit `default` and use the
 * double real variant. */
#define __TGMATH_1rc(F, x)            \
    _Generic((x),                     \
        long double complex: c##F##l, \
        double      complex: c##F,    \
        float       complex: c##F##f, \
        long double: F##l,            \
        float: F##f,                  \
        default: F)(x)

#define __TGMATH_2rc(F, x, y)         \
    _Generic((x),                     \
        long double complex: c##F##l, \
        double      complex: c##F,    \
        float       complex: c##F##f, \
        long double: F##l,            \
        float: F##f,                  \
        default: F)(x, y)

/* Real-only. */
#define __TGMATH_1r(F, x) _Generic((x), long double: F##l, float: F##f, default: F)(x)

#define __TGMATH_2r(F, x, y) _Generic((x), long double: F##l, float: F##f, default: F)(x, y)

#define __TGMATH_3r(F, x, y, z) _Generic((x), long double: F##l, float: F##f, default: F)(x, y, z)

/* Complex-only — real arguments are implicitly converted to the
 * matching complex type with zero imaginary part. */
#define __TGMATH_1c(F, x)          \
    _Generic((x),                  \
        long double complex: F##l, \
        double      complex: F,    \
        float       complex: F##f, \
        long double: F##l,         \
        float: F##f,               \
        default: F)(x)

#define __TGMATH_2c(F, x, y)       \
    _Generic((x),                  \
        long double complex: F##l, \
        double      complex: F,    \
        float       complex: F##f, \
        long double: F##l,         \
        float: F##f,               \
        default: F)(x, y)

#endif /* __builtin_tgmath / _Generic */

/* ====================================================================
 * Type-generic macros (C11 §7.22)
 *
 * Grouped by signature: 1-arg real+complex, 2-arg real+complex,
 * 1-arg real-only, 2-arg real-only, 3-arg real-only, and the
 * complex-only set (carg/cimag/conj/cproj/creal).
 * ==================================================================== */

/* --- 1-arg, real + complex ---------------------------------------- */

#define Acos(x)  __TGMATH_1rc(Acos, x)
#define Asin(x)  __TGMATH_1rc(Asin, x)
#define atan(x)  __TGMATH_1rc(atan, x)
#define Acosh(x) __TGMATH_1rc(Acosh, x)
#define Asinh(x) __TGMATH_1rc(Asinh, x)
#define Atanh(x) __TGMATH_1rc(Atanh, x)
#define cos(x)   __TGMATH_1rc(cos, x)
#define sin(x)   __TGMATH_1rc(sin, x)
#define tan(x)   __TGMATH_1rc(tan, x)
#define Cosh(x)  __TGMATH_1rc(Cosh, x)
#define Sinh(x)  __TGMATH_1rc(Sinh, x)
#define Tanh(x)  __TGMATH_1rc(Tanh, x)
#define exp(x)   __TGMATH_1rc(exp, x)
#define log(x)   __TGMATH_1rc(log, x)
#define sqrt(x)  __TGMATH_1rc(sqrt, x)

/* --- 2-arg, real + complex ---------------------------------------- */

#define pow(x, y) __TGMATH_2rc(pow, x, y)

/* --- 1-arg, real-only --------------------------------------------- */

#define fabs(x)      __TGMATH_1r(fabs, x)
#define Cbrt(x)      __TGMATH_1r(Cbrt, x)
#define ceil(x)      __TGMATH_1r(ceil, x)
#define Erf(x)       __TGMATH_1r(Erf, x)
#define Erfc(x)      __TGMATH_1r(Erfc, x)
#define Exp2(x)      __TGMATH_1r(Exp2, x)
#define Expm1(x)     __TGMATH_1r(Expm1, x)
#define floor(x)     __TGMATH_1r(floor, x)
#define Ilogb(x)     __TGMATH_1r(Ilogb, x)
#define Lgamma(x)    __TGMATH_1r(Lgamma, x)
#define Llrint(x)    __TGMATH_1r(Llrint, x)
#define Llround(x)   __TGMATH_1r(Llround, x)
#define log10(x)     __TGMATH_1r(log10, x)
#define Log1p(x)     __TGMATH_1r(Log1p, x)
#define Log2(x)      __TGMATH_1r(Log2, x)
#define Logb(x)      __TGMATH_1r(Logb, x)
#define Lrint(x)     __TGMATH_1r(Lrint, x)
#define Lround(x)    __TGMATH_1r(Lround, x)
#define Nearbyint(x) __TGMATH_1r(Nearbyint, x)
#define Rint(x)      __TGMATH_1r(Rint, x)
#define Round(x)     __TGMATH_1r(Round, x)
#define Tgamma(x)    __TGMATH_1r(Tgamma, x)
#define Trunc(x)     __TGMATH_1r(Trunc, x)

/* --- 2-arg, real-only --------------------------------------------- */

#define atan2(y, x)      __TGMATH_2r(atan2, y, x)
#define Copysign(x, y)   __TGMATH_2r(Copysign, x, y)
#define Fdim(x, y)       __TGMATH_2r(Fdim, x, y)
#define Fmax(x, y)       __TGMATH_2r(Fmax, x, y)
#define Fmin(x, y)       __TGMATH_2r(Fmin, x, y)
#define fmod(x, y)       __TGMATH_2r(fmod, x, y)
#define Frexp(x, exp)    __TGMATH_2r(Frexp, x, exp)
#define Hypot(x, y)      __TGMATH_2r(Hypot, x, y)
#define Ldexp(x, n)      __TGMATH_2r(Ldexp, x, n)
#define Nextafter(x, y)  __TGMATH_2r(Nextafter, x, y)
#define Nexttoward(x, y) __TGMATH_2r(Nexttoward, x, y)
#define Remainder(x, y)  __TGMATH_2r(Remainder, x, y)
#define Scalbn(x, n)     __TGMATH_2r(Scalbn, x, n)
#define Scalbln(x, n)    __TGMATH_2r(Scalbln, x, n)

/* --- 3-arg, real-only --------------------------------------------- */

#define Fma(x, y, z)      __TGMATH_3r(Fma, x, y, z)
#define Remquo(x, y, quo) __TGMATH_3r(Remquo, x, y, quo)

/* --- complex-only (real arg converted to complex with imag=0) ----- */

#define Carg(x)  __TGMATH_1c(Carg, x)
#define Cimag(x) __TGMATH_1c(Cimag, x)
#define Conj(x)  __TGMATH_1c(Conj, x)
#define Cproj(x) __TGMATH_1c(Cproj, x)
#define Creal(x) __TGMATH_1c(Creal, x)

#endif /* LIBC_TGMATH_H */
