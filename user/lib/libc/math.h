/*
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

#define HUGE_VAL        (__builtin_huge_val())
#define HUGE_VALF       (__builtin_huge_valf())
#define HUGE_VALL       (__builtin_huge_vall())
#define INFINITY        (__builtin_inff())
#define NAN             (__builtin_nanf(""))

/* floating-point classification macros */
#define FP_NAN          0
#define FP_INFINITE     1
#define FP_SUBNORMAL    2
#define FP_ZERO         3
#define FP_NORMAL       4

#define fpclassify(x)   __builtin_fpclassify(FP_NAN, FP_INFINITE, \
                        FP_NORMAL, FP_SUBNORMAL, FP_ZERO, (x))

#define isfinite(x)     __builtin_isfinite(x)
#define isinf(x)        __builtin_isinf(x)
#define isnan(x)        __builtin_isnan(x)
#define isnormal(x)     __builtin_isnormal(x)
#define signbit(x)      __builtin_signbit(x)

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

#define MATH_ERRNO      1
#define MATH_ERREXCEPT  2
#define math_errhandling 0       /* no errno/exceptions in v0.1 */

/* ====================================================================
 * Functions — double
 * ==================================================================== */

double      acos(double x);
double      asin(double x);
double      atan(double x);
double      atan2(double y, double x);
double      cos(double x);
double      sin(double x);
double      tan(double x);
double      cosh(double x);
double      sinh(double x);
double      tanh(double x);
double      acosh(double x);
double      asinh(double x);
double      atanh(double x);
double      exp(double x);
double      exp2(double x);
double      expm1(double x);
double      frexp(double value, int *exp);
double      ldexp(double x, int exp);
double      log(double x);
double      log10(double x);
double      log2(double x);
double      log1p(double x);
int         ilogb(double x);
double      logb(double x);
double      modf(double value, double *iptr);
double      scalbn(double x, int n);
double      scalbln(double x, long n);
double      cbrt(double x);
double      fabs(double x);
double      hypot(double x, double y);
double      pow(double x, double y);
double      sqrt(double x);
double      erf(double x);
double      erfc(double x);
double      lgamma(double x);
double      tgamma(double x);
double      ceil(double x);
double      floor(double x);
double      nearbyint(double x);
double      rint(double x);
long        lrint(double x);
long long   llrint(double x);
double      round(double x);
long        lround(double x);
long long   llround(double x);
double      trunc(double x);
double      fmod(double x, double y);
double      remainder(double x, double y);
double      remquo(double x, double y, int *quo);
double      copysign(double x, double y);
double      nan(const char *tagp);
double      nextafter(double x, double y);
double      nexttoward(double x, long double y);
double      fdim(double x, double y);
double      fmax(double x, double y);
double      fmin(double x, double y);
double      fma(double x, double y, double z);

/* ====================================================================
 * Functions — float
 * ==================================================================== */

float       acosf(float x);
float       asinf(float x);
float       atanf(float x);
float       atan2f(float y, float x);
float       cosf(float x);
float       sinf(float x);
float       tanf(float x);
float       coshf(float x);
float       sinhf(float x);
float       tanhf(float x);
float       acoshf(float x);
float       asinhf(float x);
float       atanhf(float x);
float       expf(float x);
float       exp2f(float x);
float       expm1f(float x);
float       frexpf(float value, int *exp);
float       ldexpf(float x, int exp);
float       logf(float x);
float       log10f(float x);
float       log2f(float x);
float       log1pf(float x);
int         ilogbf(float x);
float       logbf(float x);
float       modff(float value, float *iptr);
float       scalbnf(float x, int n);
float       scalblnf(float x, long n);
float       cbrtf(float x);
float       fabsf(float x);
float       hypotf(float x, float y);
float       powf(float x, float y);
float       sqrtf(float x);
float       erff(float x);
float       erfcf(float x);
float       lgammaf(float x);
float       tgammaf(float x);
float       ceilf(float x);
float       floorf(float x);
float       nearbyintf(float x);
float       rintf(float x);
long        lrintf(float x);
long long   llrintf(float x);
float       roundf(float x);
long        lroundf(float x);
long long   llroundf(float x);
float       truncf(float x);
float       fmodf(float x, float y);
float       remainderf(float x, float y);
float       remquof(float x, float y, int *quo);
float       copysignf(float x, float y);
float       nanf(const char *tagp);
float       nextafterf(float x, float y);
float       nexttowardf(float x, long double y);
float       fdimf(float x, float y);
float       fmaxf(float x, float y);
float       fminf(float x, float y);
float       fmaf(float x, float y, float z);

/* ====================================================================
 * Functions — long double (aliased to double in v0.1)
 * ==================================================================== */

long double acosl(long double x);
long double asinl(long double x);
long double atanl(long double x);
long double atan2l(long double y, long double x);
long double cosl(long double x);
long double sinl(long double x);
long double tanl(long double x);
long double coshl(long double x);
long double sinhl(long double x);
long double tanhl(long double x);
long double acoshl(long double x);
long double asinhl(long double x);
long double atanhl(long double x);
long double expl(long double x);
long double exp2l(long double x);
long double expm1l(long double x);
long double frexpl(long double value, int *exp);
long double ldexpl(long double x, int exp);
long double logl(long double x);
long double log10l(long double x);
long double log2l(long double x);
long double log1pl(long double x);
int         ilogbl(long double x);
long double logbl(long double x);
long double modfl(long double value, long double *iptr);
long double scalbnl(long double x, int n);
long double scalblnl(long double x, long n);
long double cbrtl(long double x);
long double fabsl(long double x);
long double hypotl(long double x, long double y);
long double powl(long double x, long double y);
long double sqrtl(long double x);
long double erfl(long double x);
long double erfcl(long double x);
long double lgammal(long double x);
long double tgammal(long double x);
long double ceill(long double x);
long double floorl(long double x);
long double nearbyintl(long double x);
long double rintl(long double x);
long        lrintl(long double x);
long long   llrintl(long double x);
long double roundl(long double x);
long        lroundl(long double x);
long long   llroundl(long double x);
long double truncl(long double x);
long double fmodl(long double x, long double y);
long double remainderl(long double x, long double y);
long double remquol(long double x, long double y, int *quo);
long double copysignl(long double x, long double y);
long double nanl(const char *tagp);
long double nextafterl(long double x, long double y);
long double nexttowardl(long double x, long double y);
long double fdiml(long double x, long double y);
long double fmaxl(long double x, long double y);
long double fminl(long double x, long double y);
long double fmal(long double x, long double y, long double z);

/* ====================================================================
 * Useful constants
 * ==================================================================== */

#define M_E            2.71828182845904523536
#define M_LOG2E        1.44269504088896340736
#define M_LOG10E       0.43429448190325182765
#define M_LN2          0.69314718055994530942
#define M_LN10         2.30258509299404568402
#define M_PI           3.14159265358979323846
#define M_PI_2         1.57079632679489661923
#define M_PI_4         0.78539816339744830962
#define M_1_PI         0.31830988618379067154
#define M_2_PI         0.63661977236758134308
#define M_2_SQRTPI     1.12837916709551257390
#define M_SQRT2        1.41421356237309504880
#define M_SQRT1_2      0.70710678118654752440

#endif /* LIBC_MATH_H */
