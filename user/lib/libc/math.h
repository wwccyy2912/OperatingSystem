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
 * math.h - Mathematics (C11 §7.12)
 * Copyright (c) 2026 OpSys Project
 *
 * Standard floating-point math functions.  Implemented in math.c using
 * a mix of x87 FPU instructions (inline asm) and software algorithms.
 *
 * Note: the build uses -mno-sse so all FP ops go through x87 FPU.
 */

#ifndef LIBC_MATH_H
#define LIBC_MATH_H

/* ====================================================================
 * Types and macros (C11 §7.12, §7.12.3, §7.12.4)
 * ==================================================================== */

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

/* floating-point classification macros */
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_SUBNORMAL 2
#define FP_ZERO      3
#define FP_NORMAL    4

#define fpclassify(x) \
    __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, (x))

#define isfinite(x) __builtin_isfinite(x)
#define isinf(x)    __builtin_isinf(x)
#define isnan(x)    __builtin_isnan(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x)  __builtin_signbit(x)

/* sign macros */
#define isgreater(x, y)      __builtin_isgreater(x, y)
#define isgreaterequal(x, y) __builtin_isgreaterequal(x, y)
#define isless(x, y)         __builtin_isless(x, y)
#define islessequal(x, y)    __builtin_islessequal(x, y)
#define islessgreater(x, y)  __builtin_islessgreater(x, y)
#define isunordered(x, y)    __builtin_isunordered(x, y)

/* ====================================================================
 * Error handling
 * ==================================================================== */

#define MATH_ERRNO       1
#define MATH_ERREXCEPT   2
#define math_errhandling 0 /* no errno/exceptions in v0.1 */

/* ====================================================================
 * Functions — double
 * ==================================================================== */

double    Acos(double x);
double    Asin(double x);
double    atan(double x);
double    atan2(double y, double x);
double    cos(double x);
double    sin(double x);
double    tan(double x);
double    Cosh(double x);
double    Sinh(double x);
double    Tanh(double x);
double    Acosh(double x);
double    Asinh(double x);
double    Atanh(double x);
double    exp(double x);
double    Exp2(double x);
double    Expm1(double x);
double    Frexp(double value, int *exp);
double    Ldexp(double x, int exp);
double    log(double x);
double    log10(double x);
double    Log2(double x);
double    Log1p(double x);
int       Ilogb(double x);
double    Logb(double x);
double    Modf(double value, double *iptr);
double    Scalbn(double x, int n);
double    Scalbln(double x, long n);
double    Cbrt(double x);
double    fabs(double x);
double    Hypot(double x, double y);
double    pow(double x, double y);
double    sqrt(double x);
double    Erf(double x);
double    Erfc(double x);
double    Lgamma(double x);
double    Tgamma(double x);
double    ceil(double x);
double    floor(double x);
double    Nearbyint(double x);
double    Rint(double x);
long      Lrint(double x);
long long Llrint(double x);
double    Round(double x);
long      Lround(double x);
long long Llround(double x);
double    Trunc(double x);
double    fmod(double x, double y);
double    Remainder(double x, double y);
double    Remquo(double x, double y, int *quo);
double    Copysign(double x, double y);
double    Nan(const char *tagp);
double    Nextafter(double x, double y);
double    Nexttoward(double x, long double y);
double    Fdim(double x, double y);
double    Fmax(double x, double y);
double    Fmin(double x, double y);
double    Fma(double x, double y, double z);

/* ====================================================================
 * Functions — float
 * ==================================================================== */

float     Acosf(float x);
float     Asinf(float x);
float     Atanf(float x);
float     Atan2f(float y, float x);
float     Cosf(float x);
float     Sinf(float x);
float     Tanf(float x);
float     Coshf(float x);
float     Sinhf(float x);
float     Tanhf(float x);
float     Acoshf(float x);
float     Asinhf(float x);
float     Atanhf(float x);
float     Expf(float x);
float     Exp2f(float x);
float     Expm1f(float x);
float     Frexpf(float value, int *exp);
float     Ldexpf(float x, int exp);
float     Logf(float x);
float     Log10f(float x);
float     Log2f(float x);
float     Log1pf(float x);
int       Ilogbf(float x);
float     Logbf(float x);
float     Modff(float value, float *iptr);
float     Scalbnf(float x, int n);
float     Scalblnf(float x, long n);
float     Cbrtf(float x);
float     Fabsf(float x);
float     Hypotf(float x, float y);
float     Powf(float x, float y);
float     Sqrtf(float x);
float     Erff(float x);
float     Erfcf(float x);
float     Lgammaf(float x);
float     Tgammaf(float x);
float     Ceilf(float x);
float     Floorf(float x);
float     Nearbyintf(float x);
float     Rintf(float x);
long      Lrintf(float x);
long long Llrintf(float x);
float     Roundf(float x);
long      Lroundf(float x);
long long Llroundf(float x);
float     Truncf(float x);
float     Fmodf(float x, float y);
float     Remainderf(float x, float y);
float     Remquof(float x, float y, int *quo);
float     Copysignf(float x, float y);
float     Nanf(const char *tagp);
float     Nextafterf(float x, float y);
float     Nexttowardf(float x, long double y);
float     Fdimf(float x, float y);
float     Fmaxf(float x, float y);
float     Fminf(float x, float y);
float     Fmaf(float x, float y, float z);

/* ====================================================================
 * Functions — long double (aliased to double in v0.1)
 * ==================================================================== */

long double Acosl(long double x);
long double Asinl(long double x);
long double Atanl(long double x);
long double Atan2l(long double y, long double x);
long double Cosl(long double x);
long double Sinl(long double x);
long double Tanl(long double x);
long double Coshl(long double x);
long double Sinhl(long double x);
long double Tanhl(long double x);
long double Acoshl(long double x);
long double Asinhl(long double x);
long double Atanhl(long double x);
long double Expl(long double x);
long double Exp2l(long double x);
long double Expm1l(long double x);
long double Frexpl(long double value, int *exp);
long double Ldexpl(long double x, int exp);
long double Logl(long double x);
long double Log10l(long double x);
long double Log2l(long double x);
long double Log1pl(long double x);
int         Ilogbl(long double x);
long double Logbl(long double x);
long double Modfl(long double value, long double *iptr);
long double Scalbnl(long double x, int n);
long double Scalblnl(long double x, long n);
long double Cbrtl(long double x);
long double Fabsl(long double x);
long double Hypotl(long double x, long double y);
long double Powl(long double x, long double y);
long double Sqrtl(long double x);
long double Erfl(long double x);
long double Erfcl(long double x);
long double Lgammal(long double x);
long double Tgammal(long double x);
long double Ceill(long double x);
long double Floorl(long double x);
long double Nearbyintl(long double x);
long double Rintl(long double x);
long        Lrintl(long double x);
long long   Llrintl(long double x);
long double Roundl(long double x);
long        Lroundl(long double x);
long long   Llroundl(long double x);
long double Truncl(long double x);
long double Fmodl(long double x, long double y);
long double Remainderl(long double x, long double y);
long double Remquol(long double x, long double y, int *quo);
long double Copysignl(long double x, long double y);
long double Nanl(const char *tagp);
long double Nextafterl(long double x, long double y);
long double Nexttowardl(long double x, long double y);
long double Fdiml(long double x, long double y);
long double Fmaxl(long double x, long double y);
long double Fminl(long double x, long double y);
long double Fmal(long double x, long double y, long double z);

/* ====================================================================
 * Useful constants
 * ==================================================================== */

#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#endif /* LIBC_MATH_H */
