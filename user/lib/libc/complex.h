/*
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
double complex cacos(double complex z);
double complex casin(double complex z);
double complex catan(double complex z);
double complex ccos(double complex z);
double complex csin(double complex z);
double complex ctan(double complex z);

/* Hyperbolic (§7.3.6) */
double complex cacosh(double complex z);
double complex casinh(double complex z);
double complex catanh(double complex z);
double complex ccosh(double complex z);
double complex csinh(double complex z);
double complex ctanh(double complex z);

/* Exponential and logarithmic (§7.3.7) */
double complex cexp(double complex z);
double complex clog(double complex z);

/* Power and absolute value (§7.3.8) */
double         cabs(double complex z);
double complex cpow(double complex x, double complex z);
double complex csqrt(double complex z);

/* Manipulation (§7.3.9) — these return real or complex results. */
double         carg(double complex z);
double         cimag(double complex z);
double complex conj(double complex z);
double complex cproj(double complex z);
double         creal(double complex z);

/* ====================================================================
 * Functions — float complex
 * ==================================================================== */

/* Trigonometric */
float complex cacosf(float complex z);
float complex casinf(float complex z);
float complex catanf(float complex z);
float complex ccosf(float complex z);
float complex csinf(float complex z);
float complex ctanf(float complex z);

/* Hyperbolic */
float complex cacoshf(float complex z);
float complex casinhf(float complex z);
float complex catanhf(float complex z);
float complex ccoshf(float complex z);
float complex csinhf(float complex z);
float complex ctanhf(float complex z);

/* Exponential and logarithmic */
float complex cexpf(float complex z);
float complex clogf(float complex z);

/* Power and absolute value */
float         cabsf(float complex z);
float complex cpowf(float complex x, float complex z);
float complex csqrtf(float complex z);

/* Manipulation */
float         cargf(float complex z);
float         cimagf(float complex z);
float complex conjf(float complex z);
float complex cprojf(float complex z);
float         crealf(float complex z);

/* ====================================================================
 * Functions — long double complex
 * ==================================================================== */

/* Trigonometric */
long double complex cacosl(long double complex z);
long double complex casinl(long double complex z);
long double complex catanl(long double complex z);
long double complex ccosl(long double complex z);
long double complex csinl(long double complex z);
long double complex ctanl(long double complex z);

/* Hyperbolic */
long double complex cacoshl(long double complex z);
long double complex casinhl(long double complex z);
long double complex catanhl(long double complex z);
long double complex ccoshl(long double complex z);
long double complex csinhl(long double complex z);
long double complex ctanhl(long double complex z);

/* Exponential and logarithmic */
long double complex cexpl(long double complex z);
long double complex clogl(long double complex z);

/* Power and absolute value */
long double         cabsl(long double complex z);
long double complex cpowl(long double complex x, long double complex z);
long double complex csqrtl(long double complex z);

/* Manipulation */
long double         cargl(long double complex z);
long double         cimagl(long double complex z);
long double complex conjl(long double complex z);
long double complex cprojl(long double complex z);
long double         creall(long double complex z);

#endif /* LIBC_COMPLEX_H */
