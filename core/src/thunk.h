// Argument marshalling between the 32-bit guest ABI (AAPCS, softfp) and host
// functions, plus the registry of host implementations of guest imports.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <pthread.h>

#include <cerrno>
#include <string>
#include <tuple>
#include <type_traits>

#include "common.h"

#include "cpu.h"
#include "guest_mem.h"

namespace a64b {

// Reads successive arguments of a guest call.
struct ArgReader {
    Cpu& c;
    int reg = 0;      // next core register (0..4)
    u32 stack = 0;    // offset into the stack argument area
    explicit ArgReader(Cpu& cpu) : c(cpu) {}

    u32 next32() {
        if (reg < 4) return c.r[reg++];
        u32 v = *g2h<u32>(c.r[13] + stack);
        stack += 4;
        return v;
    }
    u64 next64() {
        if (reg < 4) reg = (reg + 1) & ~1;
        if (reg < 4) {
            u64 v = (u64)c.r[reg] | ((u64)c.r[reg + 1] << 32);
            reg += 2;
            return v;
        }
        stack = (stack + 7) & ~7u;
        u64 v;
        memcpy(&v, g2h(c.r[13] + stack), 8);
        stack += 8;
        return v;
    }
};

// A source of variadic arguments: either the rest of a call's arguments or a guest va_list.
struct VaSource {
    ArgReader* ar = nullptr;
    u32 ap = 0;
    explicit VaSource(ArgReader& a) : ar(&a) {}
    explicit VaSource(u32 va_list_ptr) : ap(va_list_ptr) {}
    u32 next32() {
        if (ar) return ar->next32();
        u32 v = *g2h<u32>(ap);
        ap += 4;
        return v;
    }
    u64 next64() {
        if (ar) return ar->next64();
        ap = (ap + 7) & ~7u;
        u64 v;
        memcpy(&v, g2h(ap), 8);
        ap += 8;
        return v;
    }
};

// Marshalling policies. In the guest "long" is 32 bits wide.
struct LibcPolicy {
    static constexpr bool long_is_64 = false;
    template <class T>
    static T* get_ptr(ArgReader& a) { return g2h<T>(a.next32()); }
    template <class T>
    static void ret_ptr(Cpu& c, T* v) { c.set_ret32(h2g(v)); }
};

template <class P, class T, class = void>
struct Marshal {
    // Fallback: 32-bit or smaller integral / enum types.
    static T get(ArgReader& a) {
        static_assert((std::is_integral_v<T> || std::is_enum_v<T>) && sizeof(T) <= 4, "unsupported argument type");
        return static_cast<T>(a.next32());
    }
    static void ret(Cpu& c, T v) {
        static_assert((std::is_integral_v<T> || std::is_enum_v<T>) && sizeof(T) <= 4, "unsupported return type");
        c.set_ret32(static_cast<u32>(static_cast<i64>(v)));
    }
};

template <class P, class T>
struct Marshal<P, T*, void> {
    static T* get(ArgReader& a) { return P::template get_ptr<T>(a); }
    static void ret(Cpu& c, T* v) { P::template ret_ptr<T>(c, v); }
};

template <class P>
struct Marshal<P, float, void> {
    static float get(ArgReader& a) {
        u32 v = a.next32();
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    static void ret(Cpu& c, float f) {
        u32 v;
        memcpy(&v, &f, 4);
        c.set_ret32(v);
    }
};

template <class P>
struct Marshal<P, double, void> {
    static double get(ArgReader& a) {
        u64 v = a.next64();
        double d;
        memcpy(&d, &v, 8);
        return d;
    }
    static void ret(Cpu& c, double d) {
        u64 v;
        memcpy(&v, &d, 8);
        c.set_ret64(v);
    }
};

// 64-bit integers. "long"/"unsigned long" are 8 bytes on the host but 4 in the guest,
// unless the policy says the API really means a 64-bit quantity (JNI's jlong).
template <class P, class T>
struct Marshal<P, T, std::enable_if_t<std::is_integral_v<T> && sizeof(T) == 8>> {
    static constexpr bool is_long = std::is_same_v<T, long> || std::is_same_v<T, unsigned long>;
    static T get(ArgReader& a) {
        if (is_long && !P::long_is_64) {
            u32 v = a.next32();
            return std::is_signed_v<T> ? static_cast<T>(static_cast<i32>(v)) : static_cast<T>(v);
        }
        return static_cast<T>(a.next64());
    }
    static void ret(Cpu& c, T v) {
        if (is_long && !P::long_is_64)
            c.set_ret32(static_cast<u32>(v));
        else
            c.set_ret64(static_cast<u64>(v));
    }
};

template <class P, class R, class... A>
struct Invoker {
    static void call(Cpu& c, void* ud) {
        auto fn = reinterpret_cast<R (*)(A...)>(ud);
        ArgReader ar(c);
        // Braced initialisation guarantees left-to-right evaluation.
        std::tuple<std::decay_t<A>...> args{Marshal<P, std::decay_t<A>>::get(ar)...};
        errno = *g2h<i32>(c.errno_addr);
        if constexpr (std::is_void_v<R>) {
            std::apply(fn, args);
        } else {
            R rv = std::apply(fn, args);
            Marshal<P, std::decay_t<R>>::ret(c, rv);
        }
        c.sync_errno();
    }
};

// ----- registry of host-implemented guest symbols -----
void thunk_register(const char* name, TrapFn fn, void* ud);
void thunk_register_data(const char* name, u32 guest_addr);
// Returns the guest address for an imported symbol or 0 if the bridge does not provide it.
u32 thunk_resolve(const std::string& name);
// Every symbol the bridge implements, with its guest address (creates all call stubs).
std::vector<std::pair<std::string, u32>> thunk_resolve_all();
bool thunk_exists(const std::string& name);
// Stub for an unknown import: logs when called and returns 0.
u32 thunk_missing(const std::string& name);

template <class P, class R, class... A>
void reg_fn(const char* name, R (*fn)(A...)) {
    thunk_register(name, &Invoker<P, R, A...>::call, reinterpret_cast<void*>(fn));
}
template <class R, class... A>
void reg(const char* name, R (*fn)(A...)) {
    reg_fn<LibcPolicy, R, A...>(name, fn);
}
// Raw handler that does its own marshalling.
inline void reg_raw(const char* name, void (*fn)(Cpu&)) {
    thunk_register(name,
                   [](Cpu& c, void* ud) {
                       errno = *g2h<i32>(c.errno_addr);
                       reinterpret_cast<void (*)(Cpu&)>(ud)(c);
                       c.sync_errno();
                   },
                   reinterpret_cast<void*>(fn));
}

// Each thunk source file defines one of these; they are called once at start-up.
void register_libc_thunks();
void register_stdio_thunks();
void register_math_thunks();
void register_pthread_thunks();
void register_misc_thunks();
void register_dl_thunks();
void register_old_bionic_thunks();
void register_signal_thunks();
// Sends a guest signal to a guest thread (guest_signals.cpp). Returns 0 or an errno value.
int guest_signal_send(Cpu* target, pthread_t host_thread, int sig);
void register_android_thunks();
void register_gles_thunks();
void register_data_symbols();
void register_zlib_thunks();
void register_opensl_thunks();
void register_posix_extra_thunks();
// Host-owned strings handed to the guest (cached copies in guest memory).
u32 guest_intern_string(const char* s);
void thunks_init();

// printf-style formatting with guest arguments.
std::string guest_format(const char* fmt, VaSource& va);

}  // namespace a64b
