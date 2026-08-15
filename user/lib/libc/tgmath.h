/*
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
#define __TGMATH_1rc(F, x)        __builtin_tgmath(F##f, F, F##l, c##F##f, c##F, c##F##l, x)
#define __TGMATH_2rc(F, x, y)     __builtin_tgmath(F##f, F, F##l, c##F##f, c##F, c##F##l, x, y)

/* Real-only. */
#define __TGMATH_1r(F, x)         __builtin_tgmath(F##f, F, F##l, x)
#define __TGMATH_2r(F, x, y)      __builtin_tgmath(F##f, F, F##l, x, y)
#define __TGMATH_3r(F, x, y, z)   __builtin_tgmath(F##f, F, F##l, x, y, z)

/* Complex-only — F already carries the `c` prefix (e.g. carg). */
#define __TGMATH_1c(F, x)         __builtin_tgmath(F##f, F, F##l, x)
#define __TGMATH_2c(F, x, y)      __builtin_tgmath(F##f, F, F##l, x, y)

#else  /* !__builtin_tgmath — _Generic fallback */

/* Real + complex, 1 arg.  Integer types hit `default` and use the
 * double real variant. */
#define __TGMATH_1rc(F, x)        \
    _Generic((x),                 \
        long double complex: c##F##l, \
        double complex:      c##F,    \
        float complex:       c##F##f, \
        long double:          F##l, \
        float:                F##f, \
        default:              F     \
    )(x)

#define __TGMATH_2rc(F, x, y)     \
    _Generic((x),                 \
        long double complex: c##F##l, \
        double complex:      c##F,    \
        float complex:       c##F##f, \
        long double:          F##l, \
        float:                F##f, \
        default:              F     \
    )(x, y)

/* Real-only. */
#define __TGMATH_1r(F, x)         \
    _Generic((x),                 \
        long double: F##l,        \
        float:       F##f,       \
        default:     F           \
    )(x)

#define __TGMATH_2r(F, x, y)      \
    _Generic((x),                 \
        long double: F##l,        \
        float:       F##f,       \
        default:     F           \
    )(x, y)

#define __TGMATH_3r(F, x, y, z)   \
    _Generic((x),                 \
        long double: F##l,        \
        float:       F##f,       \
        default:     F           \
    )(x, y, z)

/* Complex-only — real arguments are implicitly converted to the
 * matching complex type with zero imaginary part. */
#define __TGMATH_1c(F, x)         \
    _Generic((x),                 \
        long double complex: F##l, \
        double complex:      F,    \
        float complex:       F##f, \
        long double:          F##l, \
        float:                F##f, \
        default:              F     \
    )(x)

#define __TGMATH_2c(F, x, y)      \
    _Generic((x),                 \
        long double complex: F##l, \
        double complex:      F,    \
        float complex:       F##f, \
        long double:          F##l, \
        float:                F##f, \
        default:              F     \
    )(x, y)

#endif /* __builtin_tgmath / _Generic */

/* ====================================================================
 * Type-generic macros (C11 §7.22)
 *
 * Grouped by signature: 1-arg real+complex, 2-arg real+complex,
 * 1-arg real-only, 2-arg real-only, 3-arg real-only, and the
 * complex-only set (carg/cimag/conj/cproj/creal).
 * ==================================================================== */

/* --- 1-arg, real + complex ---------------------------------------- */

#define acos(x)        __TGMATH_1rc(acos, x)
#define asin(x)        __TGMATH_1rc(asin, x)
#define atan(x)        __TGMATH_1rc(atan, x)
#define acosh(x)       __TGMATH_1rc(acosh, x)
#define asinh(x)       __TGMATH_1rc(asinh, x)
#define atanh(x)       __TGMATH_1rc(atanh, x)
#define cos(x)         __TGMATH_1rc(cos, x)
#define sin(x)         __TGMATH_1rc(sin, x)
#define tan(x)         __TGMATH_1rc(tan, x)
#define cosh(x)        __TGMATH_1rc(cosh, x)
#define sinh(x)        __TGMATH_1rc(sinh, x)
#define tanh(x)        __TGMATH_1rc(tanh, x)
#define exp(x)         __TGMATH_1rc(exp, x)
#define log(x)         __TGMATH_1rc(log, x)
#define sqrt(x)        __TGMATH_1rc(sqrt, x)

/* --- 2-arg, real + complex ---------------------------------------- */

#define pow(x, y)      __TGMATH_2rc(pow, x, y)

/* --- 1-arg, real-only --------------------------------------------- */

#define fabs(x)        __TGMATH_1r(fabs, x)
#define cbrt(x)        __TGMATH_1r(cbrt, x)
#define ceil(x)        __TGMATH_1r(ceil, x)
#define erf(x)         __TGMATH_1r(erf, x)
#define erfc(x)        __TGMATH_1r(erfc, x)
#define exp2(x)        __TGMATH_1r(exp2, x)
#define expm1(x)       __TGMATH_1r(expm1, x)
#define floor(x)       __TGMATH_1r(floor, x)
#define ilogb(x)       __TGMATH_1r(ilogb, x)
#define lgamma(x)      __TGMATH_1r(lgamma, x)
#define llrint(x)      __TGMATH_1r(llrint, x)
#define llround(x)     __TGMATH_1r(llround, x)
#define log10(x)       __TGMATH_1r(log10, x)
#define log1p(x)       __TGMATH_1r(log1p, x)
#define log2(x)        __TGMATH_1r(log2, x)
#define logb(x)        __TGMATH_1r(logb, x)
#define lrint(x)       __TGMATH_1r(lrint, x)
#define lround(x)      __TGMATH_1r(lround, x)
#define nearbyint(x)   __TGMATH_1r(nearbyint, x)
#define rint(x)        __TGMATH_1r(rint, x)
#define round(x)       __TGMATH_1r(round, x)
#define tgamma(x)      __TGMATH_1r(tgamma, x)
#define trunc(x)       __TGMATH_1r(trunc, x)

/* --- 2-arg, real-only --------------------------------------------- */

#define atan2(y, x)       __TGMATH_2r(atan2, y, x)
#define copysign(x, y)    __TGMATH_2r(copysign, x, y)
#define fdim(x, y)        __TGMATH_2r(fdim, x, y)
#define fmax(x, y)        __TGMATH_2r(fmax, x, y)
#define fmin(x, y)        __TGMATH_2r(fmin, x, y)
#define fmod(x, y)        __TGMATH_2r(fmod, x, y)
#define frexp(x, exp)     __TGMATH_2r(frexp, x, exp)
#define hypot(x, y)       __TGMATH_2r(hypot, x, y)
#define ldexp(x, n)       __TGMATH_2r(ldexp, x, n)
#define nextafter(x, y)   __TGMATH_2r(nextafter, x, y)
#define nexttoward(x, y)  __TGMATH_2r(nexttoward, x, y)
#define remainder(x, y)   __TGMATH_2r(remainder, x, y)
#define scalbn(x, n)      __TGMATH_2r(scalbn, x, n)
#define scalbln(x, n)     __TGMATH_2r(scalbln, x, n)

/* --- 3-arg, real-only --------------------------------------------- */

#define fma(x, y, z)       __TGMATH_3r(fma, x, y, z)
#define remquo(x, y, quo)  __TGMATH_3r(remquo, x, y, quo)

/* --- complex-only (real arg converted to complex with imag=0) ----- */

#define carg(x)        __TGMATH_1c(carg, x)
#define cimag(x)       __TGMATH_1c(cimag, x)
#define conj(x)        __TGMATH_1c(conj, x)
#define cproj(x)       __TGMATH_1c(cproj, x)
#define creal(x)       __TGMATH_1c(creal, x)

#endif /* LIBC_TGMATH_H */
