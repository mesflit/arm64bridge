// libm
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>

#include "thunk.h"

namespace a64b {

void register_math_thunks() {
#define D1(n) reg(#n, +[](double x) -> double { return ::n(x); });
#define F1(n) reg(#n, +[](float x) -> float { return ::n(x); });
#define D2(n) reg(#n, +[](double x, double y) -> double { return ::n(x, y); });
#define F2(n) reg(#n, +[](float x, float y) -> float { return ::n(x, y); });
    D1(sin) D1(cos) D1(tan) D1(asin) D1(acos) D1(atan) D1(sinh) D1(cosh) D1(tanh) D1(asinh) D1(acosh) D1(atanh)
    D1(exp) D1(exp2) D1(expm1) D1(log) D1(log2) D1(log10) D1(log1p) D1(logb) D1(sqrt) D1(cbrt) D1(fabs) D1(ceil)
    D1(floor) D1(round) D1(trunc) D1(rint) D1(nearbyint) D1(erf) D1(erfc) D1(tgamma) D1(lgamma)
    F1(sinf) F1(cosf) F1(tanf) F1(asinf) F1(acosf) F1(atanf) F1(sinhf) F1(coshf) F1(tanhf) F1(asinhf) F1(acoshf)
    F1(atanhf) F1(expf) F1(exp2f) F1(expm1f) F1(logf) F1(log2f) F1(log10f) F1(log1pf) F1(logbf) F1(sqrtf) F1(cbrtf)
    F1(fabsf) F1(ceilf) F1(floorf) F1(roundf) F1(truncf) F1(rintf) F1(nearbyintf) F1(erff) F1(erfcf) F1(tgammaf)
    F1(lgammaf)
    D2(pow) D2(atan2) D2(fmod) D2(hypot) D2(fmin) D2(fmax) D2(fdim) D2(copysign) D2(remainder) D2(nextafter)
    F2(powf) F2(atan2f) F2(fmodf) F2(hypotf) F2(fminf) F2(fmaxf) F2(fdimf) F2(copysignf) F2(remainderf)
    F2(nextafterf)
#undef D1
#undef F1
#undef D2
#undef F2
    // long double is the same as double on 32-bit ARM
    reg("sinl", +[](double x) -> double { return sin(x); });
    reg("cosl", +[](double x) -> double { return cos(x); });
    reg("sqrtl", +[](double x) -> double { return sqrt(x); });
    reg("fabsl", +[](double x) -> double { return fabs(x); });
    reg("floorl", +[](double x) -> double { return floor(x); });
    reg("ceill", +[](double x) -> double { return ceil(x); });
    reg("powl", +[](double x, double y) -> double { return pow(x, y); });
    reg("fmodl", +[](double x, double y) -> double { return fmod(x, y); });
    reg("ldexpl", +[](double x, int e) -> double { return ldexp(x, e); });
    reg("frexpl", +[](double x, int* e) -> double { return frexp(x, e); });
    reg("modfl", +[](double x, double* ip) -> double { return modf(x, ip); });

    reg("ldexp", +[](double x, int e) -> double { return ldexp(x, e); });
    reg("ldexpf", +[](float x, int e) -> float { return ldexpf(x, e); });
    reg("scalbn", +[](double x, int e) -> double { return scalbn(x, e); });
    reg("scalbnf", +[](float x, int e) -> float { return scalbnf(x, e); });
    reg("scalbln", +[](double x, int e) -> double { return scalbn(x, e); });
    reg("frexp", +[](double x, int* e) -> double { return frexp(x, e); });
    reg("frexpf", +[](float x, int* e) -> float { return frexpf(x, e); });
    reg("modf", +[](double x, double* ip) -> double { return modf(x, ip); });
    reg("modff", +[](float x, float* ip) -> float { return modff(x, ip); });
    reg("fma", +[](double x, double y, double z) -> double { return fma(x, y, z); });
    reg("fmaf", +[](float x, float y, float z) -> float { return fmaf(x, y, z); });
    reg("remquo", +[](double x, double y, int* q) -> double { return remquo(x, y, q); });
    reg("remquof", +[](float x, float y, int* q) -> float { return remquof(x, y, q); });
    reg("ilogb", +[](double x) -> int { return ilogb(x); });
    reg("ilogbf", +[](float x) -> int { return ilogbf(x); });
    reg("lrint", +[](double x) -> int { return (int)lrint(x); });
    reg("lrintf", +[](float x) -> int { return (int)lrintf(x); });
    reg("lround", +[](double x) -> int { return (int)lround(x); });
    reg("lroundf", +[](float x) -> int { return (int)lroundf(x); });
    reg("llrint", +[](double x) -> long long { return llrint(x); });
    reg("llrintf", +[](float x) -> long long { return llrintf(x); });
    reg("llround", +[](double x) -> long long { return llround(x); });
    reg("llroundf", +[](float x) -> long long { return llroundf(x); });
    reg("sincos", +[](double x, double* s, double* c) { *s = sin(x); *c = cos(x); });
    reg("sincosf", +[](float x, float* s, float* c) { *s = sinf(x); *c = cosf(x); });
    reg("nan", +[](const char*) -> double { return NAN; });
    reg("nanf", +[](const char*) -> float { return NAN; });
    reg("__isnan", +[](double x) -> int { return std::isnan(x); });
    reg("__isnanf", +[](float x) -> int { return std::isnan(x); });
    reg("isnan", +[](double x) -> int { return std::isnan(x); });
    reg("isnanf", +[](float x) -> int { return std::isnan(x); });
    reg("__isinf", +[](double x) -> int { return std::isinf(x); });
    reg("__isinff", +[](float x) -> int { return std::isinf(x); });
    reg("isinf", +[](double x) -> int { return std::isinf(x); });
    reg("__isfinite", +[](double x) -> int { return std::isfinite(x); });
    reg("__isfinitef", +[](float x) -> int { return std::isfinite(x); });
    reg("finite", +[](double x) -> int { return std::isfinite(x); });
    reg("finitef", +[](float x) -> int { return std::isfinite(x); });
    reg("__fpclassifyd", +[](double x) -> int { return std::fpclassify(x); });
    reg("__fpclassifyf", +[](float x) -> int { return std::fpclassify(x); });
    reg("__signbit", +[](double x) -> int { return std::signbit(x); });
    reg("__signbitf", +[](float x) -> int { return std::signbit(x); });
}

}  // namespace a64b
