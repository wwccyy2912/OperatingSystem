/*
 * math.c - Mathematics functions (C11 §7.12)
 * Copyright (c) 2026 OpSys Project
 *
 * Uses x87 FPU instructions (inline asm) for hardware-accelerated
 * operations (sqrt, fabs, sin, cos, etc.) and software algorithms
 * for the rest.  Float and long-double variants delegate to double.
 
 *
 * ------------------------------------------------------------------
 * Structure (math):
 *   MATH_FN(name) macro instantiates fabs/sqrt/floor/ceil/pow/... as
 *   soft-float or SSE-scalar helpers per target flags.
 * How it works:
 *   Each standard function is a tiny wrapper; unsupported operations
 *   degrade to approximations or the FPU instruction.
 * Purpose:
 *   Minimal libm for the user runtime.
 * Caveats:
 *   Precision is not IEEE-754-perfect; errno/domain errors are not
 *   fully modelled.
 * ------------------------------------------------------------------
 */

#include "math.h"

/* ====================================================================
 * x87 FPU helpers (inline assembly)
 * ==================================================================== */

static inline double FpuFabs(double x) {
    __asm__ volatile("fabs" : "+t"(x));
    return x;
}

static inline double FpuSqrt(double x) {
    __asm__ volatile("fsqrt" : "+t"(x));
    return x;
}

static inline double FpuFchsn(double x) {
    __asm__ volatile("fchs" : "+t"(x));
    return x;
}

static inline double FpuSin(double x) {
    __asm__ volatile("fsin" : "+t"(x));
    return x;
}

static inline double FpuCos(double x) {
    __asm__ volatile("fcos" : "+t"(x));
    return x;
}

static inline double FpuFrndint(double x) {
    __asm__ volatile("frndint" : "+t"(x));
    return x;
}

static inline double FpuScale(double x, double s) {
    __asm__ volatile("fscale" : "+t"(x) : "u"(s));
    return x;
}

static inline double FpuF2xm1(double x) {
    __asm__ volatile("f2xm1" : "+t"(x));
    return x;
}

static inline double FpuFyl2x(double x, double y) {
    double result;
    __asm__ volatile("fyl2x" : "=t"(result) : "0"(x), "u"(y) : "st(1)");
    return result;
}

static inline double FpuFpatan(double x, double y) {
    double result;
    __asm__ volatile("fpatan" : "=t"(result) : "0"(x), "u"(y) : "st(1)");
    return result;
}

/* ====================================================================
 * Basic functions
 * ==================================================================== */

double fabs(double x) {
    return FpuFabs(x);
}
double sqrt(double x) {
    return x < 0.0 ? NAN : FpuSqrt(x);
}

double ceil(double x) {
    double r = FpuFrndint(x);
    return r < x ? r + 1.0 : r;
}

double floor(double x) {
    double r = FpuFrndint(x);
    return r > x ? r - 1.0 : r;
}

double Trunc(double x) {
    return x >= 0.0 ? floor(x) : ceil(x);
}

double Round(double x) {
    return x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5);
}

long Lround(double x) {
    return (long)Round(x);
}
long long Llround(double x) {
    return (long long)Round(x);
}

double Nearbyint(double x) {
    return FpuFrndint(x);
}
double Rint(double x) {
    return FpuFrndint(x);
}
long Lrint(double x) {
    return (long)FpuFrndint(x);
}
long long Llrint(double x) {
    return (long long)FpuFrndint(x);
}

double fmod(double x, double y) {
    if (y == 0.0)
        return NAN;
    long long q = (long long)(x / y);
    return x - (double)q * y;
}

double Remainder(double x, double y) {
    if (y == 0.0)
        return NAN;
    double r = x - y * Round(x / y);
    return r;
}

double Remquo(double x, double y, int *quo) {
    if (quo)
        *quo = (int)Trunc(x / y) & 7;
    return Remainder(x, y);
}

double Copysign(double x, double y) {
    double ax = FpuFabs(x);
    return y >= 0.0 ? ax : FpuFchsn(ax);
}

double Fdim(double x, double y) {
    return x > y ? x - y : 0.0;
}

double Fmax(double x, double y) {
    return x > y ? x : y;
}
double Fmin(double x, double y) {
    return x < y ? x : y;
}

double Fma(double x, double y, double z) {
    return x * y + z;
}

double Hypot(double x, double y) {
    x = FpuFabs(x);
    y = FpuFabs(y);
    if (x < y) {
        double t = x;
        x        = y;
        y        = t;
    }
    if (x == 0.0)
        return 0.0;
    double r = y / x;
    return x * FpuSqrt(1.0 + r * r);
}

double Nan(const char *tagp) {
    (void)tagp;
    return NAN;
}

double Nextafter(double x, double y) {
    if (isnan(x) || isnan(y))
        return NAN;
    if (x == y)
        return y;
    /* Approximate via bit manipulation through union. */
    union {
        double             d;
        unsigned long long u;
    } u;
    u.d = x;
    if (x < y)
        u.u = u.u + (u.u >> 63 | 1);
    else
        u.u = u.u - (~u.u >> 63 | 1);
    return u.d;
}

double Nexttoward(double x, long double y) {
    return Nextafter(x, (double)y);
}

double Scalbn(double x, int n) {
    return FpuScale(x, (double)n);
}

double Scalbln(double x, long n) {
    return FpuScale(x, (double)n);
}

double Ldexp(double x, int exp) {
    return FpuScale(x, (double)exp);
}

double Frexp(double value, int *exp) {
    if (value == 0.0) {
        if (exp)
            *exp = 0;
        return 0.0;
    }
    union {
        double             d;
        unsigned long long u;
    } u;
    u.d   = value;
    int e = (int)((u.u >> 52) & 0x7FF) - 1022;
    u.u   = (u.u & 0x800FFFFFFFFFFFFFULL) | (0x3FEULL << 52);
    if (exp)
        *exp = e;
    return u.d;
}

double Modf(double value, double *iptr) {
    double i = Trunc(value);
    if (iptr)
        *iptr = i;
    return value - i;
}

/* ====================================================================
 * Trigonometric functions (x87 hardware)
 * ==================================================================== */

double sin(double x) {
    return FpuSin(x);
}
double cos(double x) {
    return FpuCos(x);
}

double tan(double x) {
    /* x87 FPTAN pushes 1.0 then tan; we must pop the 1.0. */
    double r;
    __asm__ volatile("fptan\n\t"
                     "fstp %%st(0)"
                     : "=t"(r)
                     : "0"(x));
    return r;
}

double Asin(double x) {
    if (x < -1.0 || x > 1.0)
        return NAN;
    if (x == 0.0)
        return 0.0;
    /* Asin(x) = atan(x / sqrt(1 - x^2)) */
    return FpuFpatan(x, FpuSqrt(1.0 - x * x));
}

double Acos(double x) {
    if (x < -1.0 || x > 1.0)
        return NAN;
    /* Acos(x) = atan2(sqrt(1-x^2), x) */
    return FpuFpatan(FpuSqrt(1.0 - x * x), x);
}

double atan(double x) {
    return FpuFpatan(x, 1.0);
}

double atan2(double y, double x) {
    return FpuFpatan(y, x);
}

/* ====================================================================
 * Hyperbolic functions (software)
 * ==================================================================== */

double Sinh(double x) {
    if (isinf(x))
        return x;
    double ex = exp(x);
    return (ex - 1.0 / ex) / 2.0;
}

double Cosh(double x) {
    if (isinf(x))
        return FpuFabs(x);
    double ex = exp(x);
    return (ex + 1.0 / ex) / 2.0;
}

double Tanh(double x) {
    if (isinf(x))
        return x > 0 ? 1.0 : -1.0;
    double ex = exp(2.0 * x);
    return (ex - 1.0) / (ex + 1.0);
}

/* ====================================================================
 * Exponential and logarithmic functions
 * ==================================================================== */

double exp(double x) {
    if (isnan(x))
        return NAN;
    if (isinf(x))
        return x < 0 ? 0.0 : x;
    if (x == 0.0)
        return 1.0;

    /* exp(x) = 2^(x * Log2(e))
     *        = 2^k * 2^r   where k = Round(x*Log2(e)), r = fractional */
    double t = x * M_LOG2E;
    double k = Round(t);
    double r = t - k;
    /* 2^r - 1 via f2xm1, then scale by 2^k */
    return FpuScale(FpuF2xm1(r) + 1.0, k);
}

double Exp2(double x) {
    if (x == 0.0)
        return 1.0;
    double k = Round(x);
    double r = x - k;
    return FpuScale(FpuF2xm1(r) + 1.0, k);
}

double Expm1(double x) {
    if (x == 0.0)
        return 0.0;
    return exp(x) - 1.0;
}

double log(double x) {
    if (isnan(x))
        return NAN;
    if (x < 0.0)
        return NAN;
    if (x == 0.0)
        return -INFINITY;
    if (isinf(x))
        return INFINITY;
    /* log(x) = Log2(x) / Log2(e) = Log2(x) * ln(2) */
    return FpuFyl2x(x, M_LN2);
}

double Log2(double x) {
    if (x <= 0.0)
        return (x == 0.0) ? -INFINITY : NAN;
    return FpuFyl2x(x, 1.0);
}

double log10(double x) {
    if (x <= 0.0)
        return (x == 0.0) ? -INFINITY : NAN;
    return FpuFyl2x(x, M_LN10 / M_LN2);
}

double Log1p(double x) {
    if (x == 0.0)
        return 0.0;
    return log(1.0 + x);
}

/* ====================================================================
 * Power functions
 * ==================================================================== */

double pow(double x, double y) {
    if (y == 0.0)
        return 1.0;
    if (x == 0.0)
        return y > 0 ? 0.0 : INFINITY;
    if (x < 0.0) {
        /* Integer exponent only for negative base. */
        long long yi = (long long)y;
        if ((double)yi == y) {
            double r = exp(y * log(FpuFabs(x)));
            return (yi & 1) ? -r : r;
        }
        return NAN;
    }
    /* x^y = exp(y * log(x)) = 2^(y * Log2(x)) */
    return exp(y * log(x));
}

double Cbrt(double x) {
    if (x == 0.0)
        return 0.0;
    double ax = FpuFabs(x);
    double r  = exp(log(ax) / 3.0);
    return x < 0.0 ? -r : r;
}

/* ====================================================================
 * Special functions (simplified)
 * ==================================================================== */

double Erf(double x) {
    /* Abramowitz & Stegun approximation 7.1.26 */
    double ax = FpuFabs(x);
    double t  = 1.0 / (1.0 + 0.3275911 * ax);
    double y  = 1.0 -
                (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t +
                 0.254829592) *
                    t * exp(-ax * ax);
    return x < 0.0 ? -y : y;
}

double Erfc(double x) {
    return 1.0 - Erf(x);
}

double Tgamma(double x) {
    /* Lanczos approximation. */
    static const double g[] = {6.938676328e-11,
                               3.7021161e-10,
                               -3.332589375e-9,
                               1.567399362e-8,
                               -6.548369353e-8,
                               2.516822038e-7,
                               -8.914688249e-7,
                               3.102704959e-6,
                               -1.074678818e-5,
                               3.777844696e-5,
                               -1.368417876e-4,
                               5.169655123e-4,
                               -2.068770617e-3,
                               8.682176637e-3,
                               -3.87532468e-2,
                               1.995902491e-1,
                               1.0};
    if (x <= 0.0)
        return NAN;
    double sum = 0.0;
    for (int i = 0; i < 17; i++)
        sum = sum * x + g[i];
    return pow(x / 2.718281828459045, x) * sum;
}

double Lgamma(double x) {
    /* Stirling series (good for x > 0.5). */
    if (x <= 0.0)
        return INFINITY;
    if (x < 0.5)
        return log(M_PI / sin(M_PI * x)) - Lgamma(1.0 - x);
    x -= 1.0;
    double r = 0.99999999999980993 + 676.5203681218851 / (x + 1) - 1259.1392167224028 / (x + 2) +
               771.32342877765313 / (x + 3) - 176.61502916214059 / (x + 4) +
               12.507343278686905 / (x + 5) - 0.13857109526572012 / (x + 6) +
               9.9843695780195716e-6 / (x + 7) + 1.5056327351493116e-7 / (x + 8);
    return log(2.5066282746310002 * r) + (x + 0.5) * log(x + 7.5) - (x + 7.5);
}

/* ====================================================================
 * Float variants (delegate to double)
 * ==================================================================== */

#define F2D(name)                              \
    float name(float x) {                      \
        return (float)name##d_to_d((double)x); \
    }

/* Simple delegation pattern for float functions. */
float Acosf(float x) {
    return (float)Acos((double)x);
}
float Asinf(float x) {
    return (float)Asin((double)x);
}
float Atanf(float x) {
    return (float)atan((double)x);
}
float Atan2f(float y, float x) {
    return (float)atan2((double)y, (double)x);
}
float Cosf(float x) {
    return (float)cos((double)x);
}
float Sinf(float x) {
    return (float)sin((double)x);
}
float Tanf(float x) {
    return (float)tan((double)x);
}
float Coshf(float x) {
    return (float)Cosh((double)x);
}
float Sinhf(float x) {
    return (float)Sinh((double)x);
}
float Tanhf(float x) {
    return (float)Tanh((double)x);
}
float Expf(float x) {
    return (float)exp((double)x);
}
float Exp2f(float x) {
    return (float)Exp2((double)x);
}
float Expm1f(float x) {
    return (float)Expm1((double)x);
}
float Frexpf(float v, int *e) {
    double r = Frexp((double)v, e);
    return (float)r;
}
float Ldexpf(float x, int e) {
    return (float)Ldexp((double)x, e);
}
float Logf(float x) {
    return (float)log((double)x);
}
float Log10f(float x) {
    return (float)log10((double)x);
}
float Log2f(float x) {
    return (float)Log2((double)x);
}
float Log1pf(float x) {
    return (float)Log1p((double)x);
}
float Modff(float v, float *i) {
    double d, r = Modf((double)v, &d);
    *i = (float)d;
    return (float)r;
}
float Scalbnf(float x, int n) {
    return (float)Scalbn((double)x, n);
}
float Scalblnf(float x, long n) {
    return (float)Scalbln((double)x, n);
}
float Cbrtf(float x) {
    return (float)Cbrt((double)x);
}
float Fabsf(float x) {
    return (float)fabs((double)x);
}
float Hypotf(float x, float y) {
    return (float)Hypot((double)x, (double)y);
}
float Powf(float x, float y) {
    return (float)pow((double)x, (double)y);
}
float Sqrtf(float x) {
    return (float)sqrt((double)x);
}
float Erff(float x) {
    return (float)Erf((double)x);
}
float Erfcf(float x) {
    return (float)Erfc((double)x);
}
float Lgammaf(float x) {
    return (float)Lgamma((double)x);
}
float Tgammaf(float x) {
    return (float)Tgamma((double)x);
}
float Ceilf(float x) {
    return (float)ceil((double)x);
}
float Floorf(float x) {
    return (float)floor((double)x);
}
float Nearbyintf(float x) {
    return (float)Nearbyint((double)x);
}
float Rintf(float x) {
    return (float)Rint((double)x);
}
long Lrintf(float x) {
    return Lrint((double)x);
}
long long Llrintf(float x) {
    return Llrint((double)x);
}
float Roundf(float x) {
    return (float)Round((double)x);
}
long Lroundf(float x) {
    return Lround((double)x);
}
long long Llroundf(float x) {
    return Llround((double)x);
}
float Truncf(float x) {
    return (float)Trunc((double)x);
}
float Fmodf(float x, float y) {
    return (float)fmod((double)x, (double)y);
}
float Remainderf(float x, float y) {
    return (float)Remainder((double)x, (double)y);
}
float Remquof(float x, float y, int *q) {
    double r = Remquo((double)x, (double)y, q);
    return (float)r;
}
float Copysignf(float x, float y) {
    return (float)Copysign((double)x, (double)y);
}
float Nanf(const char *t) {
    (void)t;
    return NAN;
}
float Nextafterf(float x, float y) {
    return (float)Nextafter((double)x, (double)y);
}
float Nexttowardf(float x, long double y) {
    return (float)Nextafter((double)x, (double)y);
}
float Fdimf(float x, float y) {
    return (float)Fdim((double)x, (double)y);
}
float Fmaxf(float x, float y) {
    return (float)Fmax((double)x, (double)y);
}
float Fminf(float x, float y) {
    return (float)Fmin((double)x, (double)y);
}
float Fmaf(float x, float y, float z) {
    return (float)Fma((double)x, (double)y, (double)z);
}

/* ====================================================================
 * long double variants (alias to double in v0.1)
 * ==================================================================== */

long double Acosl(long double x) {
    return (long double)Acos((double)x);
}
long double Asinl(long double x) {
    return (long double)Asin((double)x);
}
long double Atanl(long double x) {
    return (long double)atan((double)x);
}
long double Atan2l(long double y, long double x) {
    return (long double)atan2((double)y, (double)x);
}
long double Cosl(long double x) {
    return (long double)cos((double)x);
}
long double Sinl(long double x) {
    return (long double)sin((double)x);
}
long double Tanl(long double x) {
    return (long double)tan((double)x);
}
long double Coshl(long double x) {
    return (long double)Cosh((double)x);
}
long double Sinhl(long double x) {
    return (long double)Sinh((double)x);
}
long double Tanhl(long double x) {
    return (long double)Tanh((double)x);
}
long double Expl(long double x) {
    return (long double)exp((double)x);
}
long double Exp2l(long double x) {
    return (long double)Exp2((double)x);
}
long double Expm1l(long double x) {
    return (long double)Expm1((double)x);
}
long double Frexpl(long double v, int *e) {
    double r = Frexp((double)v, e);
    return (long double)r;
}
long double Ldexpl(long double x, int e) {
    return (long double)Ldexp((double)x, e);
}
long double Logl(long double x) {
    return (long double)log((double)x);
}
long double Log10l(long double x) {
    return (long double)log10((double)x);
}
long double Log2l(long double x) {
    return (long double)Log2((double)x);
}
long double Log1pl(long double x) {
    return (long double)Log1p((double)x);
}
long double Modfl(long double v, long double *i) {
    double d, r = Modf((double)v, &d);
    *i = (long double)d;
    return (long double)r;
}
long double Scalbnl(long double x, int n) {
    return (long double)Scalbn((double)x, n);
}
long double Scalblnl(long double x, long n) {
    return (long double)Scalbln((double)x, n);
}
long double Cbrtl(long double x) {
    return (long double)Cbrt((double)x);
}
long double Fabsl(long double x) {
    return (long double)fabs((double)x);
}
long double Hypotl(long double x, long double y) {
    return (long double)Hypot((double)x, (double)y);
}
long double Powl(long double x, long double y) {
    return (long double)pow((double)x, (double)y);
}
long double Sqrtl(long double x) {
    return (long double)sqrt((double)x);
}
long double Erfl(long double x) {
    return (long double)Erf((double)x);
}
long double Erfcl(long double x) {
    return (long double)Erfc((double)x);
}
long double Lgammal(long double x) {
    return (long double)Lgamma((double)x);
}
long double Tgammal(long double x) {
    return (long double)Tgamma((double)x);
}
long double Ceill(long double x) {
    return (long double)ceil((double)x);
}
long double Floorl(long double x) {
    return (long double)floor((double)x);
}
long double Nearbyintl(long double x) {
    return (long double)Nearbyint((double)x);
}
long double Rintl(long double x) {
    return (long double)Rint((double)x);
}
long Lrintl(long double x) {
    return Lrint((double)x);
}
long long Llrintl(long double x) {
    return Llrint((double)x);
}
long double Roundl(long double x) {
    return (long double)Round((double)x);
}
long Lroundl(long double x) {
    return Lround((double)x);
}
long long Llroundl(long double x) {
    return Llround((double)x);
}
long double Truncl(long double x) {
    return (long double)Trunc((double)x);
}
long double Fmodl(long double x, long double y) {
    return (long double)fmod((double)x, (double)y);
}
long double Remainderl(long double x, long double y) {
    return (long double)Remainder((double)x, (double)y);
}
long double Remquol(long double x, long double y, int *q) {
    double r = Remquo((double)x, (double)y, q);
    return (long double)r;
}
long double Copysignl(long double x, long double y) {
    return (long double)Copysign((double)x, (double)y);
}
long double Nanl(const char *t) {
    (void)t;
    return NAN;
}
long double Nextafterl(long double x, long double y) {
    return (long double)Nextafter((double)x, (double)y);
}
long double Nexttowardl(long double x, long double y) {
    return (long double)Nextafter((double)x, (double)y);
}
long double Fdiml(long double x, long double y) {
    return (long double)Fdim((double)x, (double)y);
}
long double Fmaxl(long double x, long double y) {
    return (long double)Fmax((double)x, (double)y);
}
long double Fminl(long double x, long double y) {
    return (long double)Fmin((double)x, (double)y);
}
long double Fmal(long double x, long double y, long double z) {
    return (long double)Fma((double)x, (double)y, (double)z);
}
