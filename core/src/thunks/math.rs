//! libm
// SPDX-License-Identifier: GPL-2.0-or-later
use std::os::raw::{c_int, c_long, c_longlong};

use crate::thunk::reg;

macro_rules! libm {
    (d1: $($d1:ident)*; f1: $($f1:ident)*; d2: $($d2:ident)*; f2: $($f2:ident)*;) => {
        extern "C" {
            $(fn $d1(x: f64) -> f64;)*
            $(fn $f1(x: f32) -> f32;)*
            $(fn $d2(x: f64, y: f64) -> f64;)*
            $(fn $f2(x: f32, y: f32) -> f32;)*
        }
        fn reg_simple() {
            $(reg(stringify!($d1), |x: f64| -> f64 { unsafe { $d1(x) } });)*
            $(reg(stringify!($f1), |x: f32| -> f32 { unsafe { $f1(x) } });)*
            $(reg(stringify!($d2), |x: f64, y: f64| -> f64 { unsafe { $d2(x, y) } });)*
            $(reg(stringify!($f2), |x: f32, y: f32| -> f32 { unsafe { $f2(x, y) } });)*
        }
    };
}
libm! {
    d1: sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh exp exp2 expm1 log log2 log10 log1p logb sqrt cbrt fabs ceil
        floor round trunc rint nearbyint erf erfc tgamma lgamma;
    f1: sinf cosf tanf asinf acosf atanf sinhf coshf tanhf asinhf acoshf atanhf expf exp2f expm1f logf log2f log10f log1pf logbf
        sqrtf cbrtf fabsf ceilf floorf roundf truncf rintf nearbyintf erff erfcf tgammaf lgammaf;
    d2: pow atan2 fmod hypot fmin fmax fdim copysign remainder nextafter;
    f2: powf atan2f fmodf hypotf fminf fmaxf fdimf copysignf remainderf nextafterf;
}

extern "C" {
    fn ldexp(x: f64, e: c_int) -> f64;
    fn ldexpf(x: f32, e: c_int) -> f32;
    fn scalbn(x: f64, e: c_int) -> f64;
    fn scalbnf(x: f32, e: c_int) -> f32;
    fn frexp(x: f64, e: *mut c_int) -> f64;
    fn frexpf(x: f32, e: *mut c_int) -> f32;
    fn modf(x: f64, ip: *mut f64) -> f64;
    fn modff(x: f32, ip: *mut f32) -> f32;
    fn fma(x: f64, y: f64, z: f64) -> f64;
    fn fmaf(x: f32, y: f32, z: f32) -> f32;
    fn remquo(x: f64, y: f64, q: *mut c_int) -> f64;
    fn remquof(x: f32, y: f32, q: *mut c_int) -> f32;
    fn ilogb(x: f64) -> c_int;
    fn ilogbf(x: f32) -> c_int;
    fn lrint(x: f64) -> c_long;
    fn lrintf(x: f32) -> c_long;
    fn lround(x: f64) -> c_long;
    fn lroundf(x: f32) -> c_long;
    fn llrint(x: f64) -> c_longlong;
    fn llrintf(x: f32) -> c_longlong;
    fn llround(x: f64) -> c_longlong;
    fn llroundf(x: f32) -> c_longlong;
}

// bionic's FP_* classes
fn classify(c: std::num::FpCategory) -> i32 {
    match c {
        std::num::FpCategory::Infinite => 1,
        std::num::FpCategory::Nan => 2,
        std::num::FpCategory::Normal => 4,
        std::num::FpCategory::Subnormal => 8,
        std::num::FpCategory::Zero => 0x10,
    }
}

pub fn register_math_thunks() {
    reg_simple();
    // long double is the same as double on 32-bit ARM
    reg("sinl", |x: f64| -> f64 { unsafe { sin(x) } });
    reg("cosl", |x: f64| -> f64 { unsafe { cos(x) } });
    reg("sqrtl", |x: f64| -> f64 { unsafe { sqrt(x) } });
    reg("fabsl", |x: f64| -> f64 { x.abs() });
    reg("floorl", |x: f64| -> f64 { x.floor() });
    reg("ceill", |x: f64| -> f64 { x.ceil() });
    reg("powl", |x: f64, y: f64| -> f64 { unsafe { pow(x, y) } });
    reg("fmodl", |x: f64, y: f64| -> f64 { unsafe { fmod(x, y) } });
    for n in ["ldexp", "ldexpl"] {
        reg(n, |x: f64, e: i32| -> f64 { unsafe { ldexp(x, e) } });
    }
    reg("scalbln", |x: f64, e: i32| -> f64 { unsafe { scalbn(x, e) } });
    reg("scalbn", |x: f64, e: i32| -> f64 { unsafe { scalbn(x, e) } });
    reg("ldexpf", |x: f32, e: i32| -> f32 { unsafe { ldexpf(x, e) } });
    reg("scalbnf", |x: f32, e: i32| -> f32 { unsafe { scalbnf(x, e) } });
    for n in ["frexp", "frexpl"] {
        reg(n, |x: f64, e: *mut i32| -> f64 { unsafe { frexp(x, e) } });
    }
    reg("frexpf", |x: f32, e: *mut i32| -> f32 { unsafe { frexpf(x, e) } });
    for n in ["modf", "modfl"] {
        reg(n, |x: f64, ip: *mut f64| -> f64 {
            // The guest pointer may be unaligned for the host.
            let mut i = 0f64;
            let r = unsafe { modf(x, &mut i) };
            unsafe { ip.write_unaligned(i) };
            r
        });
    }
    reg("modff", |x: f32, ip: *mut f32| -> f32 {
        let mut i = 0f32;
        let r = unsafe { modff(x, &mut i) };
        unsafe { ip.write_unaligned(i) };
        r
    });
    reg("fma", |x: f64, y: f64, z: f64| -> f64 { unsafe { fma(x, y, z) } });
    reg("fmaf", |x: f32, y: f32, z: f32| -> f32 { unsafe { fmaf(x, y, z) } });
    reg("remquo", |x: f64, y: f64, q: *mut i32| -> f64 { unsafe { remquo(x, y, q) } });
    reg("remquof", |x: f32, y: f32, q: *mut i32| -> f32 { unsafe { remquof(x, y, q) } });
    reg("ilogb", |x: f64| -> i32 { unsafe { ilogb(x) } });
    reg("ilogbf", |x: f32| -> i32 { unsafe { ilogbf(x) } });
    reg("lrint", |x: f64| -> i32 { unsafe { lrint(x) as i32 } });
    reg("lrintf", |x: f32| -> i32 { unsafe { lrintf(x) as i32 } });
    reg("lround", |x: f64| -> i32 { unsafe { lround(x) as i32 } });
    reg("lroundf", |x: f32| -> i32 { unsafe { lroundf(x) as i32 } });
    reg("llrint", |x: f64| -> i64 { unsafe { llrint(x) } });
    reg("llrintf", |x: f32| -> i64 { unsafe { llrintf(x) } });
    reg("llround", |x: f64| -> i64 { unsafe { llround(x) } });
    reg("llroundf", |x: f32| -> i64 { unsafe { llroundf(x) } });
    reg("sincos", |x: f64, s: *mut f64, c: *mut f64| unsafe {
        s.write_unaligned(sin(x));
        c.write_unaligned(cos(x));
    });
    reg("sincosf", |x: f32, s: *mut f32, c: *mut f32| unsafe {
        s.write_unaligned(sinf(x));
        c.write_unaligned(cosf(x));
    });
    reg("nan", |_tag: u32| -> f64 { f64::NAN });
    reg("nanf", |_tag: u32| -> f32 { f32::NAN });
    for n in ["__isnan", "isnan"] {
        reg(n, |x: f64| -> i32 { x.is_nan() as i32 });
    }
    for n in ["__isnanf", "isnanf"] {
        reg(n, |x: f32| -> i32 { x.is_nan() as i32 });
    }
    for n in ["__isinf", "isinf"] {
        reg(n, |x: f64| -> i32 { x.is_infinite() as i32 });
    }
    reg("__isinff", |x: f32| -> i32 { x.is_infinite() as i32 });
    for n in ["__isfinite", "finite"] {
        reg(n, |x: f64| -> i32 { x.is_finite() as i32 });
    }
    for n in ["__isfinitef", "finitef"] {
        reg(n, |x: f32| -> i32 { x.is_finite() as i32 });
    }
    reg("__fpclassifyd", |x: f64| -> i32 { classify(x.classify()) });
    reg("__fpclassifyf", |x: f32| -> i32 { classify(x.classify()) });
    reg("__signbit", |x: f64| -> i32 { x.is_sign_negative() as i32 });
    reg("__signbitf", |x: f32| -> i32 { x.is_sign_negative() as i32 });
}
