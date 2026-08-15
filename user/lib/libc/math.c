/*
 * math.c - Mathematics functions (C11 §7.12)
 * Copyright (c) 2026 OpSys Project
 *
 * Uses x87 FPU instructions (inline asm) for hardware-accelerated
 * operations (sqrt, fabs, sin, cos, etc.) and software algorithms
 * for the rest.  Float and long-double variants delegate to double.
 */

#include "math.h"

/* ====================================================================
 * x87 FPU helpers (inline assembly)
 * ==================================================================== */

static inline double fpu_fabs(double x)
{
        __asm__ volatile ("fabs" : "+t"(x));
        return x;
}

static inline double fpu_sqrt(double x)
{
        __asm__ volatile ("fsqrt" : "+t"(x));
        return x;
}

static inline double fpu_fchsn(double x)
{
        __asm__ volatile ("fchs" : "+t"(x));
        return x;
}

static inline double fpu_sin(double x)
{
        __asm__ volatile ("fsin" : "+t"(x));
        return x;
}

static inline double fpu_cos(double x)
{
        __asm__ volatile ("fcos" : "+t"(x));
        return x;
}

static inline double fpu_frndint(double x)
{
        __asm__ volatile ("frndint" : "+t"(x));
        return x;
}

static inline double fpu_scale(double x, double s)
{
        __asm__ volatile ("fscale" : "+t"(x) : "u"(s));
        return x;
}

static inline double fpu_f2xm1(double x)
{
        __asm__ volatile ("f2xm1" : "+t"(x));
        return x;
}

static inline double fpu_fyl2x(double x, double y)
{
        double result;
        __asm__ volatile ("fyl2x" : "=t"(result) : "0"(x), "u"(y) : "st(1)");
        return result;
}

static inline double fpu_fpatan(double x, double y)
{
        double result;
        __asm__ volatile ("fpatan" : "=t"(result) : "0"(x), "u"(y) : "st(1)");
        return result;
}

/* ====================================================================
 * Basic functions
 * ==================================================================== */

double fabs(double x)  { return fpu_fabs(x); }
double sqrt(double x)  { return x < 0.0 ? NAN : fpu_sqrt(x); }

double ceil(double x)
{
        double r = fpu_frndint(x);
        return r < x ? r + 1.0 : r;
}

double floor(double x)
{
        double r = fpu_frndint(x);
        return r > x ? r - 1.0 : r;
}

double trunc(double x)
{
        return x >= 0.0 ? floor(x) : ceil(x);
}

double round(double x)
{
        return x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5);
}

long lround(double x)   { return (long)round(x); }
long long llround(double x) { return (long long)round(x); }

double nearbyint(double x) { return fpu_frndint(x); }
double rint(double x)      { return fpu_frndint(x); }
long   lrint(double x)     { return (long)fpu_frndint(x); }
long long llrint(double x) { return (long long)fpu_frndint(x); }

double fmod(double x, double y)
{
        if (y == 0.0) return NAN;
        long long q = (long long)(x / y);
        return x - (double)q * y;
}

double remainder(double x, double y)
{
        if (y == 0.0) return NAN;
        double r = x - y * round(x / y);
        return r;
}

double remquo(double x, double y, int *quo)
{
        if (quo) *quo = (int)trunc(x / y) & 7;
        return remainder(x, y);
}

double copysign(double x, double y)
{
        double ax = fpu_fabs(x);
        return y >= 0.0 ? ax : fpu_fchsn(ax);
}

double fdim(double x, double y)
{
        return x > y ? x - y : 0.0;
}

double fmax(double x, double y) { return x > y ? x : y; }
double fmin(double x, double y) { return x < y ? x : y; }

double fma(double x, double y, double z) { return x * y + z; }

double hypot(double x, double y)
{
        x = fpu_fabs(x);
        y = fpu_fabs(y);
        if (x < y) { double t = x; x = y; y = t; }
        if (x == 0.0) return 0.0;
        double r = y / x;
        return x * fpu_sqrt(1.0 + r * r);
}

double nan(const char *tagp) { (void)tagp; return NAN; }

double nextafter(double x, double y)
{
        if (isnan(x) || isnan(y)) return NAN;
        if (x == y) return y;
        /* Approximate via bit manipulation through union. */
        union { double d; unsigned long long u; } u;
        u.d = x;
        if (x < y)
                u.u = u.u + (u.u >> 63 | 1);
        else
                u.u = u.u - (~u.u >> 63 | 1);
        return u.d;
}

double nexttoward(double x, long double y)
{
        return nextafter(x, (double)y);
}

double scalbn(double x, int n)
{
        return fpu_scale(x, (double)n);
}

double scalbln(double x, long n)
{
        return fpu_scale(x, (double)n);
}

double ldexp(double x, int exp)
{
        return fpu_scale(x, (double)exp);
}

double frexp(double value, int *exp)
{
        if (value == 0.0) {
                if (exp) *exp = 0;
                return 0.0;
        }
        union { double d; unsigned long long u; } u;
        u.d = value;
        int e = (int)((u.u >> 52) & 0x7FF) - 1022;
        u.u = (u.u & 0x800FFFFFFFFFFFFFULL) | (0x3FEULL << 52);
        if (exp) *exp = e;
        return u.d;
}

double modf(double value, double *iptr)
{
        double i = trunc(value);
        if (iptr) *iptr = i;
        return value - i;
}

/* ====================================================================
 * Trigonometric functions (x87 hardware)
 * ==================================================================== */

double sin(double x) { return fpu_sin(x); }
double cos(double x) { return fpu_cos(x); }

double tan(double x)
{
        /* x87 FPTAN pushes 1.0 then tan; we must pop the 1.0. */
        double r;
        __asm__ volatile (
                "fptan\n\t"
                "fstp %%st(0)"
                : "=t"(r) : "0"(x)
        );
        return r;
}

double asin(double x)
{
        if (x < -1.0 || x > 1.0) return NAN;
        if (x == 0.0) return 0.0;
        /* asin(x) = atan(x / sqrt(1 - x^2)) */
        return fpu_fpatan(x, fpu_sqrt(1.0 - x * x));
}

double acos(double x)
{
        if (x < -1.0 || x > 1.0) return NAN;
        /* acos(x) = atan2(sqrt(1-x^2), x) */
        return fpu_fpatan(fpu_sqrt(1.0 - x * x), x);
}

double atan(double x)
{
        return fpu_fpatan(x, 1.0);
}

double atan2(double y, double x)
{
        return fpu_fpatan(y, x);
}

/* ====================================================================
 * Hyperbolic functions (software)
 * ==================================================================== */

double sinh(double x)
{
        if (isinf(x)) return x;
        double ex = exp(x);
        return (ex - 1.0 / ex) / 2.0;
}

double cosh(double x)
{
        if (isinf(x)) return fpu_fabs(x);
        double ex = exp(x);
        return (ex + 1.0 / ex) / 2.0;
}

double tanh(double x)
{
        if (isinf(x)) return x > 0 ? 1.0 : -1.0;
        double ex = exp(2.0 * x);
        return (ex - 1.0) / (ex + 1.0);
}

/* ====================================================================
 * Exponential and logarithmic functions
 * ==================================================================== */

double exp(double x)
{
        if (isnan(x)) return NAN;
        if (isinf(x)) return x < 0 ? 0.0 : x;
        if (x == 0.0) return 1.0;

        /* exp(x) = 2^(x * log2(e))
         *        = 2^k * 2^r   where k = round(x*log2(e)), r = fractional */
        double t = x * M_LOG2E;
        double k = round(t);
        double r = t - k;
        /* 2^r - 1 via f2xm1, then scale by 2^k */
        return fpu_scale(fpu_f2xm1(r) + 1.0, k);
}

double exp2(double x)
{
        if (x == 0.0) return 1.0;
        double k = round(x);
        double r = x - k;
        return fpu_scale(fpu_f2xm1(r) + 1.0, k);
}

double expm1(double x)
{
        if (x == 0.0) return 0.0;
        return exp(x) - 1.0;
}

double log(double x)
{
        if (isnan(x)) return NAN;
        if (x < 0.0) return NAN;
        if (x == 0.0) return -INFINITY;
        if (isinf(x)) return INFINITY;
        /* log(x) = log2(x) / log2(e) = log2(x) * ln(2) */
        return fpu_fyl2x(x, M_LN2);
}

double log2(double x)
{
        if (x <= 0.0) return (x == 0.0) ? -INFINITY : NAN;
        return fpu_fyl2x(x, 1.0);
}

double log10(double x)
{
        if (x <= 0.0) return (x == 0.0) ? -INFINITY : NAN;
        return fpu_fyl2x(x, M_LN10 / M_LN2);
}

double log1p(double x)
{
        if (x == 0.0) return 0.0;
        return log(1.0 + x);
}

/* ====================================================================
 * Power functions
 * ==================================================================== */

double pow(double x, double y)
{
        if (y == 0.0) return 1.0;
        if (x == 0.0) return y > 0 ? 0.0 : INFINITY;
        if (x < 0.0) {
                /* Integer exponent only for negative base. */
                long long yi = (long long)y;
                if ((double)yi == y) {
                        double r = exp(y * log(fpu_fabs(x)));
                        return (yi & 1) ? -r : r;
                }
                return NAN;
        }
        /* x^y = exp(y * log(x)) = 2^(y * log2(x)) */
        return exp(y * log(x));
}

double cbrt(double x)
{
        if (x == 0.0) return 0.0;
        double ax = fpu_fabs(x);
        double r = exp(log(ax) / 3.0);
        return x < 0.0 ? -r : r;
}

/* ====================================================================
 * Special functions (simplified)
 * ==================================================================== */

double erf(double x)
{
        /* Abramowitz & Stegun approximation 7.1.26 */
        double ax = fpu_fabs(x);
        double t = 1.0 / (1.0 + 0.3275911 * ax);
        double y = 1.0 - (((((1.061405429 * t - 1.453152027) * t)
                          + 1.421413741) * t - 0.284496736) * t
                          + 0.254829592) * t * exp(-ax * ax);
        return x < 0.0 ? -y : y;
}

double erfc(double x)
{
        return 1.0 - erf(x);
}

double tgamma(double x)
{
        /* Lanczos approximation. */
        static const double g[] = {
                6.938676328e-11, 3.7021161e-10, -3.332589375e-9,
                1.567399362e-8, -6.548369353e-8, 2.516822038e-7,
                -8.914688249e-7, 3.102704959e-6, -1.074678818e-5,
                3.777844696e-5, -1.368417876e-4, 5.169655123e-4,
                -2.068770617e-3, 8.682176637e-3, -3.87532468e-2,
                1.995902491e-1, 1.0
        };
        if (x <= 0.0) return NAN;
        double sum = 0.0;
        for (int i = 0; i < 17; i++)
                sum = sum * x + g[i];
        return pow(x / 2.718281828459045, x) * sum;
}

double lgamma(double x)
{
        /* Stirling series (good for x > 0.5). */
        if (x <= 0.0) return INFINITY;
        if (x < 0.5)
                return log(M_PI / sin(M_PI * x)) - lgamma(1.0 - x);
        x -= 1.0;
        double r = 0.99999999999980993 + 676.5203681218851 / (x + 1)
                 - 1259.1392167224028 / (x + 2) + 771.32342877765313 / (x + 3)
                 - 176.61502916214059 / (x + 4) + 12.507343278686905 / (x + 5)
                 - 0.13857109526572012 / (x + 6)
                 + 9.9843695780195716e-6 / (x + 7)
                 + 1.5056327351493116e-7 / (x + 8);
        return log(2.5066282746310002 * r) + (x + 0.5) * log(x + 7.5)
               - (x + 7.5);
}

/* ====================================================================
 * Float variants (delegate to double)
 * ==================================================================== */

#define F2D(name) \
        float name(float x) { return (float)name##d_to_d((double)x); }

/* Simple delegation pattern for float functions. */
float acosf(float x)       { return (float)acos((double)x); }
float asinf(float x)       { return (float)asin((double)x); }
float atanf(float x)       { return (float)atan((double)x); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
float cosf(float x)        { return (float)cos((double)x); }
float sinf(float x)        { return (float)sin((double)x); }
float tanf(float x)        { return (float)tan((double)x); }
float coshf(float x)       { return (float)cosh((double)x); }
float sinhf(float x)       { return (float)sinh((double)x); }
float tanhf(float x)       { return (float)tanh((double)x); }
float expf(float x)        { return (float)exp((double)x); }
float exp2f(float x)       { return (float)exp2((double)x); }
float expm1f(float x)      { return (float)expm1((double)x); }
float frexpf(float v, int *e) { double r = frexp((double)v, e); return (float)r; }
float ldexpf(float x, int e) { return (float)ldexp((double)x, e); }
float logf(float x)        { return (float)log((double)x); }
float log10f(float x)      { return (float)log10((double)x); }
float log2f(float x)       { return (float)log2((double)x); }
float log1pf(float x)      { return (float)log1p((double)x); }
float modff(float v, float *i) { double d, r = modf((double)v, &d); *i = (float)d; return (float)r; }
float scalbnf(float x, int n) { return (float)scalbn((double)x, n); }
float scalblnf(float x, long n) { return (float)scalbln((double)x, n); }
float cbrtf(float x)       { return (float)cbrt((double)x); }
float fabsf(float x)       { return (float)fabs((double)x); }
float hypotf(float x, float y) { return (float)hypot((double)x, (double)y); }
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
float sqrtf(float x)       { return (float)sqrt((double)x); }
float erff(float x)        { return (float)erf((double)x); }
float erfcf(float x)       { return (float)erfc((double)x); }
float lgammaf(float x)     { return (float)lgamma((double)x); }
float tgammaf(float x)     { return (float)tgamma((double)x); }
float ceilf(float x)       { return (float)ceil((double)x); }
float floorf(float x)      { return (float)floor((double)x); }
float nearbyintf(float x)  { return (float)nearbyint((double)x); }
float rintf(float x)       { return (float)rint((double)x); }
long  lrintf(float x)      { return lrint((double)x); }
long long llrintf(float x) { return llrint((double)x); }
float roundf(float x)      { return (float)round((double)x); }
long  lroundf(float x)     { return lround((double)x); }
long long llroundf(float x){ return llround((double)x); }
float truncf(float x)      { return (float)trunc((double)x); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }
float remainderf(float x, float y) { return (float)remainder((double)x, (double)y); }
float remquof(float x, float y, int *q) { double r = remquo((double)x, (double)y, q); return (float)r; }
float copysignf(float x, float y) { return (float)copysign((double)x, (double)y); }
float nanf(const char *t)  { (void)t; return NAN; }
float nextafterf(float x, float y) { return (float)nextafter((double)x, (double)y); }
float nexttowardf(float x, long double y) { return (float)nextafter((double)x, (double)y); }
float fdimf(float x, float y) { return (float)fdim((double)x, (double)y); }
float fmaxf(float x, float y) { return (float)fmax((double)x, (double)y); }
float fminf(float x, float y) { return (float)fmin((double)x, (double)y); }
float fmaf(float x, float y, float z) { return (float)fma((double)x, (double)y, (double)z); }

/* ====================================================================
 * long double variants (alias to double in v0.1)
 * ==================================================================== */

long double acosl(long double x)       { return (long double)acos((double)x); }
long double asinl(long double x)       { return (long double)asin((double)x); }
long double atanl(long double x)       { return (long double)atan((double)x); }
long double atan2l(long double y, long double x) { return (long double)atan2((double)y, (double)x); }
long double cosl(long double x)        { return (long double)cos((double)x); }
long double sinl(long double x)        { return (long double)sin((double)x); }
long double tanl(long double x)        { return (long double)tan((double)x); }
long double coshl(long double x)       { return (long double)cosh((double)x); }
long double sinhl(long double x)       { return (long double)sinh((double)x); }
long double tanhl(long double x)       { return (long double)tanh((double)x); }
long double expl(long double x)        { return (long double)exp((double)x); }
long double exp2l(long double x)       { return (long double)exp2((double)x); }
long double expm1l(long double x)      { return (long double)expm1((double)x); }
long double frexpl(long double v, int *e) { double r = frexp((double)v, e); return (long double)r; }
long double ldexpl(long double x, int e) { return (long double)ldexp((double)x, e); }
long double logl(long double x)        { return (long double)log((double)x); }
long double log10l(long double x)      { return (long double)log10((double)x); }
long double log2l(long double x)       { return (long double)log2((double)x); }
long double log1pl(long double x)      { return (long double)log1p((double)x); }
long double modfl(long double v, long double *i) { double d, r = modf((double)v, &d); *i = (long double)d; return (long double)r; }
long double scalbnl(long double x, int n) { return (long double)scalbn((double)x, n); }
long double scalblnl(long double x, long n) { return (long double)scalbln((double)x, n); }
long double cbrtl(long double x)       { return (long double)cbrt((double)x); }
long double fabsl(long double x)       { return (long double)fabs((double)x); }
long double hypotl(long double x, long double y) { return (long double)hypot((double)x, (double)y); }
long double powl(long double x, long double y) { return (long double)pow((double)x, (double)y); }
long double sqrtl(long double x)       { return (long double)sqrt((double)x); }
long double erfl(long double x)        { return (long double)erf((double)x); }
long double erfcl(long double x)       { return (long double)erfc((double)x); }
long double lgammal(long double x)     { return (long double)lgamma((double)x); }
long double tgammal(long double x)     { return (long double)tgamma((double)x); }
long double ceill(long double x)       { return (long double)ceil((double)x); }
long double floorl(long double x)      { return (long double)floor((double)x); }
long double nearbyintl(long double x)  { return (long double)nearbyint((double)x); }
long double rintl(long double x)       { return (long double)rint((double)x); }
long   lrintl(long double x)           { return lrint((double)x); }
long long llrintl(long double x)       { return llrint((double)x); }
long double roundl(long double x)      { return (long double)round((double)x); }
long   lroundl(long double x)          { return lround((double)x); }
long long llroundl(long double x)      { return llround((double)x); }
long double truncl(long double x)      { return (long double)trunc((double)x); }
long double fmodl(long double x, long double y) { return (long double)fmod((double)x, (double)y); }
long double remainderl(long double x, long double y) { return (long double)remainder((double)x, (double)y); }
long double remquol(long double x, long double y, int *q) { double r = remquo((double)x, (double)y, q); return (long double)r; }
long double copysignl(long double x, long double y) { return (long double)copysign((double)x, (double)y); }
long double nanl(const char *t)        { (void)t; return NAN; }
long double nextafterl(long double x, long double y) { return (long double)nextafter((double)x, (double)y); }
long double nexttowardl(long double x, long double y) { return (long double)nextafter((double)x, (double)y); }
long double fdiml(long double x, long double y) { return (long double)fdim((double)x, (double)y); }
long double fmaxl(long double x, long double y) { return (long double)fmax((double)x, (double)y); }
long double fminl(long double x, long double y) { return (long double)fmin((double)x, (double)y); }
long double fmal(long double x, long double y, long double z) { return (long double)fma((double)x, (double)y, (double)z); }
