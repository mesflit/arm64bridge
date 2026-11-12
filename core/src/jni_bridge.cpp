// SPDX-License-Identifier: GPL-2.0-or-later
#include "jni_bridge.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "guest_mem.h"
#include "thunk.h"

extern "C" {
extern const char a64b_tramp_pool[];
uint64_t a64b_tramp_dispatch(uint32_t index, uint64_t* saved, uint64_t* stack);
}

namespace a64b {

using Iface = std::remove_const_t<std::remove_pointer_t<decltype(std::declval<JNIEnv>().functions)>>;

namespace {

JavaVM* g_vm = nullptr;
u32 g_guest_vm = 0;       // guest JavaVM (pointer to a pointer to the invoke table)
u32 g_guest_iface = 0;    // guest JNINativeInterface table
constexpr u32 kIfaceSlots = 240;

// ---- reference tokens ------------------------------------------------------
constexpr u32 kLocalTag = 0x40000000, kGlobalTag = 0x80000000, kTagMask = 0xC0000000;

std::mutex g_global_mu;
std::vector<jobject> g_globals;
std::vector<u32> g_global_free;

u32 new_global_token(jobject o) {
    if (!o) return 0;
    std::lock_guard<std::mutex> lk(g_global_mu);
    u32 idx;
    if (!g_global_free.empty()) {
        idx = g_global_free.back();
        g_global_free.pop_back();
        g_globals[idx] = o;
    } else {
        idx = (u32)g_globals.size();
        g_globals.push_back(o);
    }
    return kGlobalTag | idx;
}

jobject take_global_token(u32 tok) {
    std::lock_guard<std::mutex> lk(g_global_mu);
    u32 idx = tok & ~kTagMask;
    if ((tok & kTagMask) != kGlobalTag || idx >= g_globals.size()) return nullptr;
    jobject o = g_globals[idx];
    g_globals[idx] = nullptr;
    if (o) g_global_free.push_back(idx);
    return o;
}

// ---- method / field IDs ----------------------------------------------------
struct IdInfo {
    void* host;
    std::string shorty;  // empty for fields
};
std::mutex g_id_mu;
std::vector<IdInfo*> g_ids;
std::unordered_map<void*, u32> g_id_lookup;

u32 id_token(void* host, const std::string& shorty) {
    if (!host) return 0;
    std::lock_guard<std::mutex> lk(g_id_mu);
    auto it = g_id_lookup.find(host);
    if (it != g_id_lookup.end()) {
        IdInfo* info = g_ids[it->second - 1];
        if (info->shorty.empty() && !shorty.empty()) info->shorty = shorty;
        return it->second;
    }
    g_ids.push_back(new IdInfo{host, shorty});
    u32 tok = (u32)g_ids.size();
    g_id_lookup[host] = tok;
    return tok;
}

IdInfo* id_info(u32 tok) {
    std::lock_guard<std::mutex> lk(g_id_mu);
    if (tok == 0 || tok > g_ids.size()) {
        LOGE("guest passed an invalid JNI method/field ID 0x%x", tok);
        return nullptr;
    }
    return g_ids[tok - 1];
}

}  // namespace

JavaVM* jni_host_vm() { return g_vm; }

void jni_thread_exit(Cpu& c) {
    if (!g_vm) return;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK || !env) return;
    for (void* o : c.locals)
        if (o) env->DeleteLocalRef(static_cast<jobject>(o));
    c.locals.clear();
    c.local_serials.clear();
    for (auto& kv : c.kept_last) env->DeleteGlobalRef(static_cast<jobject>(kv.second));
    c.kept_last.clear();
    c.kept_refs.clear();
    c.kept_unique.clear();
    c.kept_alias.clear();
    g_vm->DetachCurrentThread();
    c.host_env = nullptr;
}

JNIEnv* jni_host_env(Cpu& c) {
    if (!c.host_env && g_vm) {
        JNIEnv* env = nullptr;
        if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) c.host_env = env;
    }
    if (!c.host_env) fatal("guest code used JNI on a thread that is not attached to the Java VM");
    return static_cast<JNIEnv*>(c.host_env);
}

namespace {
constexpr size_t kMaxKeptRefs = 4096;    // distinct argument objects kept alive per thread
constexpr size_t kMaxKeptAliases = 1024;

// Index of a local token in the thread's table, or -1.
int local_index(Cpu& c, u32 serial) {
    auto it = std::lower_bound(c.local_serials.begin(), c.local_serials.end(), serial);
    if (it == c.local_serials.end() || *it != serial) return -1;
    return (int)(it - c.local_serials.begin());
}
}  // namespace

u32 jni_new_local(Cpu& c, jobject obj) {
    if (!obj) return 0;
    u32 serial = c.next_local_serial++;
    if (c.next_local_serial >= kLocalTag) {  // wrapped: start over (stale tokens become invalid)
        c.next_local_serial = 1;
        c.locals.clear();
        c.local_serials.clear();
        serial = c.next_local_serial++;
    }
    c.locals.push_back(obj);
    c.local_serials.push_back(serial);
    return kLocalTag | serial;
}

jobject jni_to_host(Cpu& c, u32 tok) {
    if (!tok) return nullptr;
    u32 idx = tok & ~kTagMask;
    if ((tok & kTagMask) == kLocalTag) {
        int i = local_index(c, idx);
        if (i >= 0 && c.locals[i]) return static_cast<jobject>(c.locals[i]);
        auto kept = c.kept_refs.find(idx);
        if (kept != c.kept_refs.end()) return static_cast<jobject>(kept->second);
    } else if ((tok & kTagMask) == kGlobalTag) {
        std::lock_guard<std::mutex> lk(g_global_mu);
        if (idx < g_globals.size()) return g_globals[idx];
    }
    LOGE("guest passed an invalid JNI reference 0x%08x", tok);
    return nullptr;
}

JniFrame::JniFrame(Cpu& cpu, JNIEnv* env) : c(cpu), base(cpu.locals.size()), saved_env(cpu.host_env) { c.host_env = env; }
JniFrame::~JniFrame() {
    JNIEnv* env = static_cast<JNIEnv*>(c.host_env);
    for (size_t i = base; env && i < base + keep && i < c.locals.size(); i++) {
        jobject local = static_cast<jobject>(c.locals[i]);
        if (!local) continue;
        u32 serial = c.local_serials[i];
        u64 key = ((u64)keep_key << 8) | (i - base);
        auto last = c.kept_last.find(key);
        if (last != c.kept_last.end() && env->IsSameObject(local, static_cast<jobject>(last->second))) {
            // The same object as in the previous call (typically "this"): share its reference.
            c.kept_refs[serial] = last->second;
            c.kept_alias.push_back(serial);
            continue;
        }
        jobject g = env->NewGlobalRef(local);
        if (!g) continue;
        c.kept_refs[serial] = g;
        c.kept_unique.push_back(serial);
        c.kept_last[key] = g;
    }
    while (c.kept_alias.size() > kMaxKeptAliases) {
        c.kept_refs.erase(c.kept_alias.front());
        c.kept_alias.pop_front();
    }
    while (env && c.kept_unique.size() > kMaxKeptRefs) {
        auto it = c.kept_refs.find(c.kept_unique.front());
        c.kept_unique.pop_front();
        if (it == c.kept_refs.end()) continue;
        void* g = it->second;
        c.kept_refs.erase(it);
        for (u32 alias : c.kept_alias) {
            auto a = c.kept_refs.find(alias);
            if (a != c.kept_refs.end() && a->second == g) c.kept_refs.erase(a);
        }
        for (auto l = c.kept_last.begin(); l != c.kept_last.end();) l = l->second == g ? c.kept_last.erase(l) : std::next(l);
        env->DeleteGlobalRef(static_cast<jobject>(g));
    }
    if (c.locals.size() > base) {
        c.locals.resize(base);
        c.local_serials.resize(base);
    }
    c.host_env = saved_env;
}

std::string jni_shorty_from_signature(const char* sig) {
    std::string args, ret = "V";
    const char* p = sig;
    if (*p == '(') p++;
    bool in_ret = false;
    while (*p) {
        if (*p == ')') {
            in_ret = true;
            p++;
            continue;
        }
        char ch = *p;
        if (ch == '[') {
            while (*p == '[') p++;
            if (*p == 'L') while (*p && *p != ';') p++;
            if (*p) p++;
            ch = 'L';
        } else if (ch == 'L') {
            while (*p && *p != ';') p++;
            if (*p) p++;
        } else {
            p++;
        }
        if (in_ret) {
            ret = std::string(1, ch);
            break;
        }
        args.push_back(ch);
    }
    return ret + args;
}

// ---------------------------------------------------------------------------
// Marshalling policy for JNI functions.
// ---------------------------------------------------------------------------
struct JniPolicy {
    static constexpr bool long_is_64 = true;  // jlong
    template <class T>
    static T* get_ptr(ArgReader& a) {
        u32 v = a.next32();
        if constexpr (std::is_same_v<T*, jmethodID> || std::is_same_v<T*, jfieldID>) {
            IdInfo* info = id_info(v);
            return info ? static_cast<T*>(info->host) : nullptr;
        } else if constexpr (std::is_base_of_v<_jobject, T>) {
            return static_cast<T*>(jni_to_host(a.c, v));
        } else {
            return g2h<T>(v);
        }
    }
    template <class T>
    static void ret_ptr(Cpu& c, T* v) {
        if constexpr (std::is_same_v<T*, jfieldID>) {
            c.set_ret32(id_token(v, ""));
        } else if constexpr (std::is_same_v<T*, jmethodID>) {
            c.set_ret32(id_token(v, ""));
        } else if constexpr (std::is_base_of_v<_jobject, T>) {
            c.set_ret32(jni_new_local(c, v));
        } else {
            c.set_ret32(h2g(v));
        }
    }
};

namespace {

template <class M>
struct JniCall;
template <class R, class... A>
struct JniCall<R (*Iface::*)(JNIEnv*, A...)> {
    using Member = R (*Iface::*)(JNIEnv*, A...);
    // The member pointer is stored out of line because it cannot travel through a void*.
    static void call(Cpu& c, void* ud) {
        Member m = *static_cast<Member*>(ud);
        JNIEnv* env = jni_host_env(c);
        ArgReader ar(c);
        ar.next32();  // guest JNIEnv*
        std::tuple<std::decay_t<A>...> args{Marshal<JniPolicy, std::decay_t<A>>::get(ar)...};
        auto fn = env->functions->*m;
        if constexpr (std::is_void_v<R>) {
            std::apply([&](auto... x) { fn(env, x...); }, args);
        } else {
            R rv = std::apply([&](auto... x) { return fn(env, x...); }, args);
            Marshal<JniPolicy, std::decay_t<R>>::ret(c, rv);
        }
    }
};

// The shorty of a java.lang.reflect.Method or Constructor, so that a method ID from
// FromReflectedMethod can be called like one from GetMethodID (Unity's AndroidJavaObject does this).
std::string reflected_shorty(JNIEnv* env, jobject member) {
    auto type_char = [env](jclass type) -> char {
        jclass cc = env->GetObjectClass(type);
        jmethodID get_name = env->GetMethodID(cc, "getName", "()Ljava/lang/String;");
        env->DeleteLocalRef(cc);
        jstring js = (jstring)env->CallObjectMethod(type, get_name);
        const char* chars = env->GetStringUTFChars(js, nullptr);
        std::string n = chars;
        env->ReleaseStringUTFChars(js, chars);
        env->DeleteLocalRef(js);
        static const std::pair<const char*, char> prims[] = {{"int", 'I'}, {"boolean", 'Z'}, {"byte", 'B'}, {"char", 'C'}, {"short", 'S'},
                                                              {"long", 'J'}, {"float", 'F'}, {"double", 'D'}, {"void", 'V'}};
        for (auto& p : prims)
            if (n == p.first) return p.second;
        return 'L';
    };
    if (!member) return "";
    jclass mc = env->GetObjectClass(member);
    jmethodID get_params = env->GetMethodID(mc, "getParameterTypes", "()[Ljava/lang/Class;");
    jmethodID get_ret = env->GetMethodID(mc, "getReturnType", "()Ljava/lang/Class;");
    if (env->ExceptionCheck()) env->ExceptionClear();  // a Constructor has no getReturnType
    env->DeleteLocalRef(mc);
    if (!get_params) return "";
    std::string shorty = "V";
    if (get_ret) {
        jclass rt = (jclass)env->CallObjectMethod(member, get_ret);
        shorty[0] = type_char(rt);
        env->DeleteLocalRef(rt);
    }
    jobjectArray params = (jobjectArray)env->CallObjectMethod(member, get_params);
    jsize np = params ? env->GetArrayLength(params) : 0;
    for (jsize i = 0; i < np; i++) {
        jclass pt = (jclass)env->GetObjectArrayElement(params, i);
        shorty += type_char(pt);
        env->DeleteLocalRef(pt);
    }
    if (params) env->DeleteLocalRef(params);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return "";
    }
    return shorty;
}

// A Java exception left pending by guest code.
//
// The JNI rules allow only a few functions (ExceptionCheck, Release*, Delete*Ref...) while an exception
// is pending; ART aborts the process for any other. Old games often ignore exceptions that their
// Android version never threw: Samurai vs Zombies reads the IMEI (getDeviceId throws a
// SecurityException since Android 10) and calls FindClass right after. As an old Android would have
// let the game go on, the exception is logged and cleared before such a call.
void clear_ignored_exception(JNIEnv* env, const char* call) {
    jthrowable t = env->ExceptionOccurred();
    env->ExceptionClear();
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) >= 20 || !t) {
        if (t) env->DeleteLocalRef(t);
        return;
    }
    std::string what = "?";
    jclass tc = env->GetObjectClass(t);
    jmethodID to_string = env->GetMethodID(tc, "toString", "()Ljava/lang/String;");
    jstring js = to_string ? (jstring)env->CallObjectMethod(t, to_string) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (js) {
        const char* chars = env->GetStringUTFChars(js, nullptr);
        if (chars) what = chars;
        env->ReleaseStringUTFChars(js, chars);
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(tc);
    env->DeleteLocalRef(t);
    LOGW("guest code called %s with a pending Java exception it did not handle; cleared it: %s", call, what.c_str());
}

bool allowed_with_pending_exception(const char* name) {
    static const char* const kAllowed[] = {"Exception", "Release", "Delete", "MonitorExit", "PushLocalFrame", "PopLocalFrame", "Throw"};
    for (const char* a : kAllowed)
        if (strstr(name, a)) return true;
    return false;
}

struct Slot {
    TrapFn fn;
    void* ud;
    const char* name;
};

void checked_slot(Cpu& c, void* ud) {
    Slot* s = static_cast<Slot*>(ud);
    JNIEnv* env = jni_host_env(c);
    if (env->ExceptionCheck()) clear_ignored_exception(env, s->name);
    s->fn(c, s->ud);
}

void set_slot(size_t index, const char* name, TrapFn fn, void* ud) {
    if (index >= kIfaceSlots) fatal("JNI slot out of range");
    if (!allowed_with_pending_exception(name)) {
        ud = new Slot{fn, ud, name};
        fn = checked_slot;
    }
    g2h<u32>(g_guest_iface)[index] = trap_create(name, fn, ud);
}

template <class M>
void auto_slot(size_t offset, const char* name, M member) {
    set_slot(offset / sizeof(void*), name, &JniCall<M>::call, new M(member));
}

#define AUTO(name) auto_slot(offsetof(Iface, name), "JNI:" #name, &Iface::name)
#define HAND(name, ...) \
    set_slot(offsetof(Iface, name) / sizeof(void*), "JNI:" #name, [](Cpu& c, void*) { JNIEnv* env = jni_host_env(c); ArgReader a(c); a.next32(); (void)env; __VA_ARGS__ }, nullptr)
#define OBJ() jni_to_host(c, a.next32())

void unsupported_slot(Cpu& c, void* ud) {
    LOGE("guest called unsupported JNI function #%u", (unsigned)(uintptr_t)ud);
    c.set_ret64(0);
}

// ---- Call<Type>Method{,V,A} families ---------------------------------------
enum { kVirtual = 0, kNonvirtual = 1, kStatic = 2, kNew = 3 };
enum { kDots = 0, kVaList = 1, kArray = 2 };

void call_family(Cpu& c, void* ud) {
    unsigned code = (unsigned)(uintptr_t)ud;
    char ret = (char)(code & 0xff);
    int kind = (code >> 8) & 3, va = (code >> 10) & 3;
    JNIEnv* env = jni_host_env(c);
    ArgReader a(c);
    a.next32();
    jobject obj = jni_to_host(c, a.next32());
    jclass cls = kind == kNonvirtual ? (jclass)jni_to_host(c, a.next32()) : nullptr;
    IdInfo* info = id_info(a.next32());
    if (!info || info->shorty.empty()) {
        LOGE("JNI call with an unknown method ID");
        c.set_ret64(0);
        return;
    }
    jmethodID mid = (jmethodID)info->host;
    const std::string& sh = info->shorty;
    std::vector<jvalue> args(sh.size());
    if (va == kArray) {
        const u8* p = g2h<u8>(a.next32());
        for (size_t i = 1; i < sh.size(); i++, p += 8) {
            jvalue& v = args[i - 1];
            v.j = 0;
            switch (sh[i]) {
                case 'J': case 'D': memcpy(&v.j, p, 8); break;
                case 'L': { u32 t; memcpy(&t, p, 4); v.l = jni_to_host(c, t); break; }
                default: memcpy(&v.i, p, 4); break;
            }
        }
    } else {
        u32 ap = va == kVaList ? a.next32() : 0;
        VaSource src = va == kVaList ? VaSource(ap) : VaSource(a);
        for (size_t i = 1; i < sh.size(); i++) {
            jvalue& v = args[i - 1];
            v.j = 0;
            switch (sh[i]) {
                case 'J': v.j = (jlong)src.next64(); break;
                case 'D': { u64 b = src.next64(); memcpy(&v.d, &b, 8); break; }
                case 'F': { u64 b = src.next64(); double d; memcpy(&d, &b, 8); v.f = (jfloat)d; break; }  // promoted to double
                case 'L': v.l = jni_to_host(c, src.next32()); break;
                default: v.i = (jint)src.next32(); break;
            }
        }
    }
    const jvalue* av = args.data();
    if (kind == kNew) {
        c.set_ret32(jni_new_local(c, env->NewObjectA((jclass)obj, mid, av)));
        return;
    }
#define DISPATCH(T)                                                                  \
    (kind == kStatic ? env->CallStatic##T##MethodA((jclass)obj, mid, av)             \
                     : kind == kNonvirtual ? env->CallNonvirtual##T##MethodA(obj, cls, mid, av) \
                                           : env->Call##T##MethodA(obj, mid, av))
    switch (ret) {
        case 'V':
            if (kind == kStatic) env->CallStaticVoidMethodA((jclass)obj, mid, av);
            else if (kind == kNonvirtual) env->CallNonvirtualVoidMethodA(obj, cls, mid, av);
            else env->CallVoidMethodA(obj, mid, av);
            break;
        case 'L': c.set_ret32(jni_new_local(c, DISPATCH(Object))); break;
        case 'Z': c.set_ret32(DISPATCH(Boolean)); break;
        case 'B': c.set_ret32((u32)(i32)DISPATCH(Byte)); break;
        case 'C': c.set_ret32(DISPATCH(Char)); break;
        case 'S': c.set_ret32((u32)(i32)DISPATCH(Short)); break;
        case 'I': c.set_ret32((u32)DISPATCH(Int)); break;
        case 'J': c.set_ret64((u64)DISPATCH(Long)); break;
        case 'F': { jfloat f = DISPATCH(Float); u32 b; memcpy(&b, &f, 4); c.set_ret32(b); break; }
        case 'D': { jdouble d = DISPATCH(Double); u64 b; memcpy(&b, &d, 8); c.set_ret64(b); break; }
    }
#undef DISPATCH
}

#define CALLSLOTS(T, ch)                                                                                              \
    set_slot(offsetof(Iface, Call##T##Method) / sizeof(void*), "JNI:Call" #T "Method", call_family, (void*)(uintptr_t)(ch | (kVirtual << 8) | (kDots << 10)));      \
    set_slot(offsetof(Iface, Call##T##MethodV) / sizeof(void*), "JNI:Call" #T "MethodV", call_family, (void*)(uintptr_t)(ch | (kVirtual << 8) | (kVaList << 10)));  \
    set_slot(offsetof(Iface, Call##T##MethodA) / sizeof(void*), "JNI:Call" #T "MethodA", call_family, (void*)(uintptr_t)(ch | (kVirtual << 8) | (kArray << 10)));   \
    set_slot(offsetof(Iface, CallNonvirtual##T##Method) / sizeof(void*), "JNI:CallNonvirtual" #T "Method", call_family, (void*)(uintptr_t)(ch | (kNonvirtual << 8) | (kDots << 10)));     \
    set_slot(offsetof(Iface, CallNonvirtual##T##MethodV) / sizeof(void*), "JNI:CallNonvirtual" #T "MethodV", call_family, (void*)(uintptr_t)(ch | (kNonvirtual << 8) | (kVaList << 10))); \
    set_slot(offsetof(Iface, CallNonvirtual##T##MethodA) / sizeof(void*), "JNI:CallNonvirtual" #T "MethodA", call_family, (void*)(uintptr_t)(ch | (kNonvirtual << 8) | (kArray << 10)));  \
    set_slot(offsetof(Iface, CallStatic##T##Method) / sizeof(void*), "JNI:CallStatic" #T "Method", call_family, (void*)(uintptr_t)(ch | (kStatic << 8) | (kDots << 10)));     \
    set_slot(offsetof(Iface, CallStatic##T##MethodV) / sizeof(void*), "JNI:CallStatic" #T "MethodV", call_family, (void*)(uintptr_t)(ch | (kStatic << 8) | (kVaList << 10))); \
    set_slot(offsetof(Iface, CallStatic##T##MethodA) / sizeof(void*), "JNI:CallStatic" #T "MethodA", call_family, (void*)(uintptr_t)(ch | (kStatic << 8) | (kArray << 10)));

// ---- primitive arrays -------------------------------------------------------
// Element storage handed to the guest is a copy in guest memory.
struct ArrayKind { const char* desc; u32 size; };
const ArrayKind kArrayKinds[8] = {{"[Z", 1}, {"[B", 1}, {"[C", 2}, {"[S", 2}, {"[I", 4}, {"[J", 8}, {"[F", 4}, {"[D", 8}};
jclass g_array_classes[8];

void array_region(JNIEnv* env, jarray arr, int kind, jsize len, void* buf, bool set) {
    switch (kind) {
#define K(i, T, t) case i: if (set) env->Set##T##ArrayRegion((t##Array)arr, 0, len, (const t*)buf); else env->Get##T##ArrayRegion((t##Array)arr, 0, len, (t*)buf); break;
        K(0, Boolean, jboolean) K(1, Byte, jbyte) K(2, Char, jchar) K(3, Short, jshort)
        K(4, Int, jint) K(5, Long, jlong) K(6, Float, jfloat) K(7, Double, jdouble)
#undef K
    }
}

int array_kind_of(JNIEnv* env, jarray arr) {
    for (int i = 0; i < 8; i++)
        if (env->IsInstanceOf(arr, g_array_classes[i])) return i;
    return 1;
}

u32 get_elements(Cpu& c, JNIEnv* env, jarray arr, int kind, u32 is_copy) {
    if (!arr) return 0;
    if (kind < 0) kind = array_kind_of(env, arr);
    jsize len = env->GetArrayLength(arr);
    u32 buf = g_malloc((u32)len * kArrayKinds[kind].size + 8);
    if (!buf) return 0;
    array_region(env, arr, kind, len, g2h(buf), false);
    if (is_copy) *g2h<u8>(is_copy) = JNI_TRUE;
    (void)c;
    return buf;
}

void release_elements(JNIEnv* env, jarray arr, int kind, u32 buf, jint mode) {
    if (!arr || !buf) return;
    if (kind < 0) kind = array_kind_of(env, arr);
    if (mode != JNI_ABORT) array_region(env, arr, kind, env->GetArrayLength(arr), g2h(buf), true);
    if (mode != JNI_COMMIT) g_free(buf);
}

// ---- native method records --------------------------------------------------
struct NativeMethod {
    u32 guest_fn;
    std::string shorty;
    std::string name;
    std::atomic<u64> calls{0};
    u64 reported = 0;
};
constexpr u32 kMaxTrampolines = 4096;
#if defined(__aarch64__)
constexpr u32 kTrampStride = 8, kGprCount = 8;
#else
constexpr u32 kTrampStride = 16, kGprCount = 6;
#endif
NativeMethod* g_methods[kMaxTrampolines];
std::atomic<u32> g_method_count{0};
std::mutex g_method_mu;
std::unordered_map<std::string, void*> g_method_cache;

// ---- guest JavaVM -------------------------------------------------------------
u32 guest_env_for(Cpu& c);
void vm_get_env(Cpu& c, void*) {
    ArgReader a(c);
    a.next32();
    u32 penv = a.next32();
    jint version = (jint)a.next32();
    JNIEnv* env = nullptr;
    jint rc = g_vm->GetEnv((void**)&env, version > 0 ? version : JNI_VERSION_1_6);
    if (rc == JNI_OK) {
        c.host_env = env;
        if (penv) *g2h<u32>(penv) = guest_env_for(c);
    } else if (penv) {
        *g2h<u32>(penv) = 0;
    }
    c.set_ret32((u32)rc);
}

void vm_attach(Cpu& c, void* ud) {
    ArgReader a(c);
    a.next32();
    u32 penv = a.next32(), gargs = a.next32();
    JavaVMAttachArgs args = {JNI_VERSION_1_6, nullptr, nullptr};
    if (gargs) {
        const u32* g = g2h<u32>(gargs);
        args.version = (jint)g[0];
        args.name = g[1] ? g2h<char>(g[1]) : nullptr;
        args.group = jni_to_host(c, g[2]);
    }
    JNIEnv* env = nullptr;
#ifdef __ANDROID__
    jint rc = ud ? g_vm->AttachCurrentThreadAsDaemon(&env, &args) : g_vm->AttachCurrentThread(&env, &args);
#else
    jint rc = ud ? g_vm->AttachCurrentThreadAsDaemon((void**)&env, &args) : g_vm->AttachCurrentThread((void**)&env, &args);
#endif
    if (rc == JNI_OK) {
        c.host_env = env;
        c.detach_deferred = false;
        if (penv) *g2h<u32>(penv) = guest_env_for(c);
    }
    c.set_ret32((u32)rc);
}

void vm_detach(Cpu& c, void*) {
    if (c.guest_thread && c.host_env) {
        // Games often attach and detach a thread around every Java call (several times per frame).
        // Attaching creates a java.lang.Thread each time, so keep guest threads attached until they
        // end, and only release what a real detach would release: the thread's local references.
        JNIEnv* env = static_cast<JNIEnv*>(c.host_env);
        for (void* o : c.locals)
            if (o) env->DeleteLocalRef(static_cast<jobject>(o));
        c.locals.clear();
        c.local_serials.clear();
        c.detach_deferred = true;
        c.set_ret32(JNI_OK);
        return;
    }
    jint rc = g_vm->DetachCurrentThread();
    c.host_env = nullptr;
    c.locals.clear();
    c.local_serials.clear();
    c.set_ret32((u32)rc);
}

void build_tables() {
    g_guest_iface = g_calloc(kIfaceSlots, 4);
    for (u32 i = 0; i < kIfaceSlots; i++)
        g2h<u32>(g_guest_iface)[i] = trap_create("JNI:<unsupported>", unsupported_slot, (void*)(uintptr_t)i);

    // -- straight pass-through with automatic marshalling --
    AUTO(GetVersion); AUTO(DefineClass); AUTO(FindClass); AUTO(FromReflectedField); AUTO(ToReflectedMethod);
    AUTO(GetSuperclass); AUTO(IsAssignableFrom); AUTO(ToReflectedField); AUTO(Throw); AUTO(ThrowNew);
    AUTO(ExceptionOccurred); AUTO(ExceptionDescribe); AUTO(ExceptionClear); AUTO(FatalError); AUTO(PushLocalFrame);
    AUTO(PopLocalFrame); AUTO(NewLocalRef); AUTO(IsSameObject); AUTO(EnsureLocalCapacity); AUTO(AllocObject);
    AUTO(GetObjectClass); AUTO(IsInstanceOf); AUTO(GetFieldID); AUTO(GetStaticFieldID);
#define FIELDS(T) AUTO(Get##T##Field); AUTO(Set##T##Field); AUTO(GetStatic##T##Field); AUTO(SetStatic##T##Field);
    FIELDS(Object) FIELDS(Boolean) FIELDS(Byte) FIELDS(Char) FIELDS(Short) FIELDS(Int) FIELDS(Long) FIELDS(Float) FIELDS(Double)
#undef FIELDS
    AUTO(NewString); AUTO(GetStringLength); AUTO(NewStringUTF); AUTO(GetStringUTFLength); AUTO(GetArrayLength);
    AUTO(NewObjectArray); AUTO(GetObjectArrayElement); AUTO(SetObjectArrayElement);
#define ARRAYS(T) AUTO(New##T##Array); AUTO(Get##T##ArrayRegion); AUTO(Set##T##ArrayRegion);
    ARRAYS(Boolean) ARRAYS(Byte) ARRAYS(Char) ARRAYS(Short) ARRAYS(Int) ARRAYS(Long) ARRAYS(Float) ARRAYS(Double)
#undef ARRAYS
    AUTO(UnregisterNatives); AUTO(MonitorEnter); AUTO(MonitorExit); AUTO(GetStringRegion); AUTO(GetStringUTFRegion);
    AUTO(ExceptionCheck); AUTO(NewDirectByteBuffer); AUTO(GetDirectBufferCapacity); AUTO(GetObjectRefType);

    // -- method calls --
    CALLSLOTS(Object, 'L') CALLSLOTS(Boolean, 'Z') CALLSLOTS(Byte, 'B') CALLSLOTS(Char, 'C') CALLSLOTS(Short, 'S')
    CALLSLOTS(Int, 'I') CALLSLOTS(Long, 'J') CALLSLOTS(Float, 'F') CALLSLOTS(Double, 'D') CALLSLOTS(Void, 'V')
    set_slot(offsetof(Iface, NewObject) / sizeof(void*), "JNI:NewObject", call_family, (void*)(uintptr_t)('L' | (kNew << 8) | (kDots << 10)));
    set_slot(offsetof(Iface, NewObjectV) / sizeof(void*), "JNI:NewObjectV", call_family, (void*)(uintptr_t)('L' | (kNew << 8) | (kVaList << 10)));
    set_slot(offsetof(Iface, NewObjectA) / sizeof(void*), "JNI:NewObjectA", call_family, (void*)(uintptr_t)('L' | (kNew << 8) | (kArray << 10)));

    HAND(GetMethodID, {
        jclass cls = (jclass)OBJ();
        const char* name = g2h<char>(a.next32());
        const char* sig = g2h<char>(a.next32());
        c.set_ret32(id_token(env->GetMethodID(cls, name, sig), jni_shorty_from_signature(sig)));
    });
    HAND(GetStaticMethodID, {
        jclass cls = (jclass)OBJ();
        const char* name = g2h<char>(a.next32());
        const char* sig = g2h<char>(a.next32());
        c.set_ret32(id_token(env->GetStaticMethodID(cls, name, sig), jni_shorty_from_signature(sig)));
    });
    HAND(FromReflectedMethod, {
        jobject member = OBJ();
        std::string shorty = reflected_shorty(env, member);
        if (shorty.empty()) LOGW("FromReflectedMethod: could not read the method's parameter types");
        c.set_ret32(id_token(env->FromReflectedMethod(member), shorty));
    });

    // -- references --
    HAND(NewGlobalRef, { c.set_ret32(new_global_token(env->NewGlobalRef(OBJ()))); });
    HAND(NewWeakGlobalRef, { c.set_ret32(new_global_token(env->NewWeakGlobalRef(OBJ()))); });
    HAND(DeleteGlobalRef, { if (jobject o = take_global_token(a.next32())) env->DeleteGlobalRef(o); });
    HAND(DeleteWeakGlobalRef, { if (jobject o = take_global_token(a.next32())) env->DeleteWeakGlobalRef((jweak)o); });
    HAND(DeleteLocalRef, {
        u32 tok = a.next32();
        int i = (tok & kTagMask) == kLocalTag ? local_index(c, tok & ~kTagMask) : -1;
        if (i >= 0 && c.locals[i]) {
            env->DeleteLocalRef((jobject)c.locals[i]);
            if ((size_t)i + 1 == c.locals.size()) {
                c.locals.pop_back();
                c.local_serials.pop_back();
            } else {
                c.locals[i] = nullptr;
            }
        }
    });

    // -- strings --
    HAND(GetStringUTFChars, {
        jstring s = (jstring)OBJ();
        u32 is_copy = a.next32();
        if (!s) { c.set_ret32(0); return; }
        const char* h = env->GetStringUTFChars(s, nullptr);
        u32 g = h ? g_strdup(h) : 0;
        if (h) env->ReleaseStringUTFChars(s, h);
        if (is_copy) *g2h<u8>(is_copy) = JNI_TRUE;
        c.set_ret32(g);
    });
    HAND(ReleaseStringUTFChars, { a.next32(); g_free(a.next32()); });
    auto get_chars = [](Cpu& c, void*) {
        JNIEnv* env = jni_host_env(c);
        ArgReader a(c);
        a.next32();
        jstring s = (jstring)OBJ();
        u32 is_copy = a.next32();
        if (!s) { c.set_ret32(0); return; }
        jsize len = env->GetStringLength(s);
        u32 g = g_calloc((u32)len + 1, 2);
        if (g) env->GetStringRegion(s, 0, len, g2h<jchar>(g));
        if (is_copy) *g2h<u8>(is_copy) = JNI_TRUE;
        c.set_ret32(g);
    };
    auto release_chars = [](Cpu& c, void*) { ArgReader a(c); a.next32(); a.next32(); g_free(a.next32()); };
    set_slot(offsetof(Iface, GetStringChars) / sizeof(void*), "JNI:GetStringChars", get_chars, nullptr);
    set_slot(offsetof(Iface, GetStringCritical) / sizeof(void*), "JNI:GetStringCritical", get_chars, nullptr);
    set_slot(offsetof(Iface, ReleaseStringChars) / sizeof(void*), "JNI:ReleaseStringChars", release_chars, nullptr);
    set_slot(offsetof(Iface, ReleaseStringCritical) / sizeof(void*), "JNI:ReleaseStringCritical", release_chars, nullptr);

    // -- primitive array element access --
    auto get_elems = [](Cpu& c, void* ud) {
        JNIEnv* env = jni_host_env(c);
        ArgReader a(c);
        a.next32();
        jarray arr = (jarray)OBJ();
        c.set_ret32(get_elements(c, env, arr, (int)(intptr_t)ud, a.next32()));
    };
    auto release_elems = [](Cpu& c, void* ud) {
        JNIEnv* env = jni_host_env(c);
        ArgReader a(c);
        a.next32();
        jarray arr = (jarray)OBJ();
        u32 buf = a.next32();
        release_elements(env, arr, (int)(intptr_t)ud, buf, (jint)a.next32());
    };
#define ELEMS(i, T)                                                                                                    \
    set_slot(offsetof(Iface, Get##T##ArrayElements) / sizeof(void*), "JNI:Get" #T "ArrayElements", get_elems, (void*)(intptr_t)i); \
    set_slot(offsetof(Iface, Release##T##ArrayElements) / sizeof(void*), "JNI:Release" #T "ArrayElements", release_elems, (void*)(intptr_t)i);
    ELEMS(0, Boolean) ELEMS(1, Byte) ELEMS(2, Char) ELEMS(3, Short) ELEMS(4, Int) ELEMS(5, Long) ELEMS(6, Float) ELEMS(7, Double)
#undef ELEMS
    set_slot(offsetof(Iface, GetPrimitiveArrayCritical) / sizeof(void*), "JNI:GetPrimitiveArrayCritical", get_elems, (void*)(intptr_t)-1);
    set_slot(offsetof(Iface, ReleasePrimitiveArrayCritical) / sizeof(void*), "JNI:ReleasePrimitiveArrayCritical", release_elems, (void*)(intptr_t)-1);

    HAND(GetDirectBufferAddress, {
        void* p = env->GetDirectBufferAddress(OBJ());
        if (p && !mem_contains((u64)(uintptr_t)p)) {
            LOGW("GetDirectBufferAddress: buffer %p lives outside the 32-bit guest address space", p);
            p = nullptr;
        }
        c.set_ret32((u32)(uintptr_t)p);
    });
    HAND(GetJavaVM, {
        u32 out = a.next32();
        if (out) *g2h<u32>(out) = g_guest_vm;
        c.set_ret32(0);
    });
    HAND(RegisterNatives, {
        jclass cls = (jclass)OBJ();
        u32 gm = a.next32();
        jint n = (jint)a.next32();
        std::vector<JNINativeMethod> hm((size_t)(n > 0 ? n : 0));
        bool ok = true;
        for (jint i = 0; i < n; i++) {
            const u32* e = g2h<u32>(gm + (u32)i * 12);
            hm[i].name = g2h<char>(e[0]);
            hm[i].signature = g2h<char>(e[1]);
            hm[i].fnPtr = jni_make_trampoline(e[2], jni_shorty_from_signature(hm[i].signature), hm[i].name);
            if (!hm[i].fnPtr) ok = false;
        }
        c.set_ret32(ok ? (u32)env->RegisterNatives(cls, hm.data(), n) : (u32)JNI_ERR);
    });

    // -- JavaVM --
    u32 invoke = g_calloc(8, 4);
    u32* iv = g2h<u32>(invoke);
    iv[3] = trap_create("JavaVM:DestroyJavaVM", [](Cpu& c, void*) { c.set_ret32((u32)JNI_ERR); }, nullptr);
    iv[4] = trap_create("JavaVM:AttachCurrentThread", vm_attach, nullptr);
    iv[5] = trap_create("JavaVM:DetachCurrentThread", vm_detach, nullptr);
    iv[6] = trap_create("JavaVM:GetEnv", vm_get_env, nullptr);
    iv[7] = trap_create("JavaVM:AttachCurrentThreadAsDaemon", vm_attach, (void*)1);
    g_guest_vm = g_calloc(1, 4);
    *g2h<u32>(g_guest_vm) = invoke;
}

u32 guest_env_for(Cpu& c) {
    if (!c.guest_env) {
        c.guest_env = g_calloc(1, 8);
        *g2h<u32>(c.guest_env) = g_guest_iface;
    }
    return c.guest_env;
}

}  // namespace

bool jni_bridge_init(JavaVM* vm) {
    static std::once_flag once;
    std::call_once(once, [vm] {
        thunks_init();
        g_vm = vm;
        build_tables();
        JNIEnv* env = nullptr;
        if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK && env) {
            for (int i = 0; i < 8; i++) {
                jclass cls = env->FindClass(kArrayKinds[i].desc);
                g_array_classes[i] = (jclass)env->NewGlobalRef(cls);
                env->DeleteLocalRef(cls);
            }
        }
    });
    return g_vm != nullptr;
}

void* jni_make_trampoline(u32 guest_fn, const std::string& shorty, const std::string& name) {
    std::lock_guard<std::mutex> lk(g_method_mu);
    std::string key = strfmt("%08x:", guest_fn) + shorty;
    auto it = g_method_cache.find(key);
    if (it != g_method_cache.end()) return it->second;
    u32 idx = g_method_count.load();
    if (idx >= kMaxTrampolines) {
        LOGE("too many native methods (limit %u)", kMaxTrampolines);
        return nullptr;
    }
    auto* nm = new NativeMethod();
    nm->guest_fn = guest_fn;
    nm->shorty = shorty;
    nm->name = name;
    g_methods[idx] = nm;
    g_method_count.store(idx + 1);
    void* p = (void*)(a64b_tramp_pool + (size_t)idx * kTrampStride);
    g_method_cache[key] = p;
    return p;
}

}  // namespace a64b

// Called by the assembly stubs: marshals a host native-method call into the guest.
uint64_t a64b_tramp_dispatch(uint32_t index, uint64_t* saved, uint64_t* stack) {
    using namespace a64b;
    NativeMethod* m = index < kMaxTrampolines ? g_methods[index] : nullptr;
    if (!m) fatal("native method trampoline %u is not bound", index);
    m->calls.fetch_add(1, std::memory_order_relaxed);
    u32 ngpr = 0, nfpr = 0, nstack = 0;
    uint64_t* fprs = saved + kGprCount;
    auto next_int = [&]() -> u64 { return ngpr < kGprCount ? saved[ngpr++] : stack[nstack++]; };
    auto next_fp = [&]() -> u64 { return nfpr < 8 ? fprs[nfpr++] : stack[nstack++]; };

    JNIEnv* env = (JNIEnv*)next_int();
    jobject self = (jobject)next_int();
    Cpu& c = Cpu::current();
    JniFrame frame(c, env);

    std::vector<u32> w;
    w.reserve(m->shorty.size() * 2 + 2);
    w.push_back(guest_env_for(c));
    w.push_back(jni_new_local(c, self));
    frame.keep = 1;
    frame.keep_key = index + 1;
    for (size_t i = 1; i < m->shorty.size(); i++) {
        switch (m->shorty[i]) {
            case 'J': case 'D': {
                u64 v = m->shorty[i] == 'J' ? next_int() : next_fp();
                if (w.size() & 1) w.push_back(0);  // 64-bit values are 8-byte aligned
                w.push_back((u32)v);
                w.push_back((u32)(v >> 32));
                break;
            }
            case 'F': w.push_back((u32)next_fp()); break;
            case 'L':
                w.push_back(jni_new_local(c, (jobject)next_int()));
                frame.keep = c.locals.size() - frame.base;
                break;
            // The 64-bit caller leaves the upper bits of sub-word values undefined; the 32-bit
            // callee expects them zero/sign extended.
            case 'Z': w.push_back((u32)(next_int() & 0xff ? 1 : 0)); break;
            case 'B': w.push_back((u32)(i32)(i8)next_int()); break;
            case 'C': w.push_back((u32)(u16)next_int()); break;
            case 'S': w.push_back((u32)(i32)(i16)next_int()); break;
            default: w.push_back((u32)next_int()); break;
        }
    }
    u64 r = c.call(m->guest_fn, w.data(), w.size());
    switch (m->shorty[0]) {
        case 'V': return 0;
        case 'L': return (u64)(uintptr_t)jni_to_host(c, (u32)r);
        case 'Z': return r & 0xff ? 1 : 0;
        case 'B': return (u64)(i64)(i8)r;
        case 'C': return r & 0xffff;
        case 'S': return (u64)(i64)(i16)r;
        case 'J': case 'D': return r;
        default: return (u32)r;  // I, F
    }
}

namespace a64b {

// ---------------------------------------------------------------------------
// Binding of exported Java_* functions.
// ---------------------------------------------------------------------------
namespace {

std::string jni_mangle(const std::string& s) {
    std::string out;
    for (unsigned char ch : s) {
        if (isalnum(ch)) out.push_back((char)ch);
        else if (ch == '/' || ch == '.') out.push_back('_');
        else if (ch == '_') out += "_1";
        else if (ch == ';') out += "_2";
        else if (ch == '[') out += "_3";
        else out += strfmt("_0%04x", ch);
    }
    return out;
}

// Splits "Java_com_foo_Bar_method[__args]" into class ("com/foo/Bar") and method name.
bool decode_symbol(const std::string& sym, std::string& cls, std::string& method) {
    std::string body = sym.substr(5);
    size_t dbl = body.find("__");
    if (dbl != std::string::npos) body = body.substr(0, dbl);
    std::string path;
    for (size_t i = 0; i < body.size(); i++) {
        if (body[i] != '_') {
            path.push_back(body[i]);
        } else if (i + 1 < body.size() && body[i + 1] == '1') {
            path.push_back('_');
            i++;
        } else if (i + 5 < body.size() && body[i + 1] == '0') {
            path.push_back((char)strtol(body.substr(i + 2, 4).c_str(), nullptr, 16));
            i += 5;
        } else {
            path.push_back('/');
        }
    }
    size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= path.size()) return false;
    cls = path.substr(0, slash);
    method = path.substr(slash + 1);
    return true;
}

std::string descriptor_of(JNIEnv* env, jclass type, jmethodID get_name) {
    jstring js = (jstring)env->CallObjectMethod(type, get_name);
    const char* chars = env->GetStringUTFChars(js, nullptr);
    std::string n = chars;
    env->ReleaseStringUTFChars(js, chars);
    env->DeleteLocalRef(js);
    static const std::pair<const char*, const char*> prims[] = {{"int", "I"}, {"boolean", "Z"}, {"byte", "B"}, {"char", "C"}, {"short", "S"},
                                                                 {"long", "J"}, {"float", "F"}, {"double", "D"}, {"void", "V"}};
    for (auto& p : prims)
        if (n == p.first) return p.second;
    for (char& ch : n)
        if (ch == '.') ch = '/';
    return n[0] == '[' ? n : "L" + n + ";";
}

bool clear_exception(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

jobject g_app_loader = nullptr;

// Finds a class without initialising it. FindClass would run the static initialiser of every
// class that has a Java_* export, at a moment the app does not expect (during System.loadLibrary).
jclass find_class_noinit(JNIEnv* env, const std::string& slash_name) {
    if (g_app_loader) {
        std::string dotted = slash_name;
        for (char& ch : dotted)
            if (ch == '/') ch = '.';
        jclass class_cls = env->FindClass("java/lang/Class");
        jmethodID for_name = env->GetStaticMethodID(class_cls, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
        jstring jn = env->NewStringUTF(dotted.c_str());
        jclass cls = (jclass)env->CallStaticObjectMethod(class_cls, for_name, jn, JNI_FALSE, g_app_loader);
        env->DeleteLocalRef(jn);
        env->DeleteLocalRef(class_cls);
        if (!clear_exception(env) && cls) return cls;
    }
    jclass cls = env->FindClass(slash_name.c_str());
    if (clear_exception(env)) return nullptr;
    return cls;
}

}  // namespace

void jni_set_app_class_loader(JNIEnv* env, jobject loader) {
    if (!loader || g_app_loader) return;
    g_app_loader = env->NewGlobalRef(loader);
}

int jni_bind_java_exports(JNIEnv* env, Module* m) {
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> by_class;  // class -> (method, symbol)
    for (auto& e : m->exports) {
        if (e.first.compare(0, 5, "Java_") != 0) continue;
        std::string cls, method;
        if (decode_symbol(e.first, cls, method)) by_class[cls].emplace_back(method, e.first);
    }
    if (by_class.empty()) return 0;

    jclass class_cls = env->FindClass("java/lang/Class");
    jclass method_cls = env->FindClass("java/lang/reflect/Method");
    jmethodID get_methods = env->GetMethodID(class_cls, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    jmethodID class_name = env->GetMethodID(class_cls, "getName", "()Ljava/lang/String;");
    jmethodID m_name = env->GetMethodID(method_cls, "getName", "()Ljava/lang/String;");
    jmethodID m_mods = env->GetMethodID(method_cls, "getModifiers", "()I");
    jmethodID m_params = env->GetMethodID(method_cls, "getParameterTypes", "()[Ljava/lang/Class;");
    jmethodID m_ret = env->GetMethodID(method_cls, "getReturnType", "()Ljava/lang/Class;");
    int bound = 0;

    for (auto& kv : by_class) {
        jclass cls = find_class_noinit(env, kv.first);
        if (!cls) {
            LOGW("class %s not found; its %zu native method(s) stay unbound", kv.first.c_str(), kv.second.size());
            continue;
        }
        jobjectArray methods = (jobjectArray)env->CallObjectMethod(cls, get_methods);
        if (clear_exception(env) || !methods) {
            env->DeleteLocalRef(cls);
            continue;
        }
        jsize count = env->GetArrayLength(methods);
        std::string mangled_class = jni_mangle(kv.first);
        for (jsize i = 0; i < count; i++) {
            jobject method = env->GetObjectArrayElement(methods, i);
            if (!(env->CallIntMethod(method, m_mods) & 0x100)) {  // Modifier.NATIVE
                env->DeleteLocalRef(method);
                continue;
            }
            jstring jname = (jstring)env->CallObjectMethod(method, m_name);
            const char* chars = env->GetStringUTFChars(jname, nullptr);
            std::string name = chars;
            env->ReleaseStringUTFChars(jname, chars);
            env->DeleteLocalRef(jname);

            std::string args;
            jobjectArray params = (jobjectArray)env->CallObjectMethod(method, m_params);
            jsize np = env->GetArrayLength(params);
            for (jsize p = 0; p < np; p++) {
                jclass pt = (jclass)env->GetObjectArrayElement(params, p);
                args += descriptor_of(env, pt, class_name);
                env->DeleteLocalRef(pt);
            }
            env->DeleteLocalRef(params);
            jclass rt = (jclass)env->CallObjectMethod(method, m_ret);
            std::string sig = "(" + args + ")" + descriptor_of(env, rt, class_name);
            env->DeleteLocalRef(rt);
            env->DeleteLocalRef(method);

            std::string short_sym = "Java_" + mangled_class + "_" + jni_mangle(name);
            std::string long_sym = short_sym + "__" + jni_mangle(args);
            u32 fn = m->find(long_sym);
            if (!fn) fn = m->find(short_sym);
            if (!fn) {
                LOGW("no implementation for native method %s.%s%s", kv.first.c_str(), name.c_str(), sig.c_str());
                continue;
            }
            void* tramp = jni_make_trampoline(fn, jni_shorty_from_signature(sig.c_str()), name);
            if (!tramp) continue;
            JNINativeMethod nm = {const_cast<char*>(name.c_str()), const_cast<char*>(sig.c_str()), tramp};
            if (env->RegisterNatives(cls, &nm, 1) == JNI_OK) {
                bound++;
            } else {
                clear_exception(env);
                LOGW("could not register %s.%s%s", kv.first.c_str(), name.c_str(), sig.c_str());
            }
        }
        env->DeleteLocalRef(methods);
        env->DeleteLocalRef(cls);
    }
    env->DeleteLocalRef(class_cls);
    env->DeleteLocalRef(method_cls);
    LOGI("%s: bound %d Java_* native methods", m->name.c_str(), bound);
    return bound;
}

std::string jni_call_stats() {
    std::vector<std::pair<u64, std::string>> v;
    u32 n = g_method_count.load();
    for (u32 i = 0; i < n; i++) {
        NativeMethod* m = g_methods[i];
        if (!m) continue;
        u64 now = m->calls.load(std::memory_order_relaxed);
        if (now != m->reported) v.emplace_back(now - m->reported, m->name);
        m->reported = now;
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string out;
    for (size_t i = 0; i < v.size() && i < 12; i++) out += strfmt("%s%s x%llu", i ? ", " : "", v[i].second.c_str(), (unsigned long long)v[i].first);
    return out.empty() ? "none" : out;
}

// Games that present their frames from Java (GLSurfaceView) never call eglSwapBuffers in native code.
// Their frame rate is taken from the native method the renderer calls once per frame: the busiest
// method whose name says so.
u64 jni_frame_method_calls() {
    static const char* const words[] = {"update", "render", "draw", "step", "frame", "tick"};
    u64 best = 0;
    u32 n = g_method_count.load();
    for (u32 i = 0; i < n; i++) {
        NativeMethod* m = g_methods[i];
        if (!m) continue;
        std::string name = m->name;
        for (char& ch : name) ch = (char)tolower((unsigned char)ch);
        bool match = false;
        for (const char* w : words) match = match || name.find(w) != std::string::npos;
        if (!match) continue;
        best = std::max<u64>(best, m->calls.load(std::memory_order_relaxed));
    }
    return best;
}

jint jni_call_onload(JNIEnv* env, Module* m) {
    u32 fn = m->find("JNI_OnLoad");
    if (!fn) return JNI_VERSION_1_6;
    Cpu& c = Cpu::current();
    JniFrame frame(c, env);
    guest_env_for(c);
    jint v = (jint)c.call(fn, {g_guest_vm, 0u});
    LOGI("%s: JNI_OnLoad returned 0x%x", m->name.c_str(), v);
    return v;
}

jint jni_load_library(JNIEnv* env, const std::string& name, std::string* error) {
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    if (!jni_bridge_init(vm)) {
        if (error) *error = "bridge initialisation failed";
        return JNI_ERR;
    }
    Cpu& c = Cpu::current();
    Module* m = loader_find(name);
    bool fresh = m == nullptr;
    {
        JniFrame frame(c, env);  // constructors may already use JNI through a cached JavaVM
        guest_env_for(c);
        m = loader_load(name, error);
    }
    if (!m) return JNI_ERR;
    if (!fresh) return JNI_VERSION_1_6;
    jni_bind_java_exports(env, m);
    return jni_call_onload(env, m);
}

}  // namespace a64b
