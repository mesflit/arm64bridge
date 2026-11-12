// Registry of guest-visible symbols implemented by the bridge.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <mutex>
#include <unordered_map>
#include <vector>

#include "thunk.h"

namespace a64b {

namespace {
struct Impl {
    TrapFn fn;
    void* ud;
};
std::unordered_map<std::string, Impl>& impls() {
    static auto* m = new std::unordered_map<std::string, Impl>();
    return *m;
}
std::unordered_map<std::string, u32>& resolved() {
    static auto* m = new std::unordered_map<std::string, u32>();
    return *m;
}
std::mutex g_mu;

void missing_handler(Cpu& c, void* ud) {
    const char* name = static_cast<const char*>(ud);
    LOGE("UNIMPLEMENTED function called by guest: %s (returning 0)", name);
    c.set_ret64(0);
}
}  // namespace

void thunk_register(const char* name, TrapFn fn, void* ud) { impls()[name] = Impl{fn, ud}; }

void thunk_register_data(const char* name, u32 guest_addr) {
    std::lock_guard<std::mutex> lk(g_mu);
    resolved()[name] = guest_addr;
}

bool thunk_exists(const std::string& name) {
    thunks_init();
    std::lock_guard<std::mutex> lk(g_mu);
    return impls().count(name) || resolved().count(name);
}

u32 thunk_resolve(const std::string& name) {
    thunks_init();
    std::lock_guard<std::mutex> lk(g_mu);
    auto r = resolved().find(name);
    if (r != resolved().end()) return r->second;
    auto it = impls().find(name);
    if (it == impls().end()) return 0;
    u32 addr = trap_create(name.c_str(), it->second.fn, it->second.ud);
    resolved()[name] = addr;
    return addr;
}

std::vector<std::pair<std::string, u32>> thunk_resolve_all() {
    thunks_init();
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& kv : impls()) {
        if (resolved().count(kv.first)) continue;
        resolved()[kv.first] = trap_create(kv.first.c_str(), kv.second.fn, kv.second.ud);
    }
    std::vector<std::pair<std::string, u32>> out;
    for (auto& kv : resolved())
        if (!kv.first.empty() && kv.first[0] != '?') out.emplace_back(kv.first, kv.second);
    return out;
}

u32 thunk_missing(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string key = "?" + name;
    auto r = resolved().find(key);
    if (r != resolved().end()) return r->second;
    u32 addr = trap_create(name.c_str(), missing_handler, strdup(name.c_str()));
    resolved()[key] = addr;
    return addr;
}

void thunks_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!cpu_global_init()) fatal("bridge initialisation failed");
        register_libc_thunks();
        register_stdio_thunks();
        register_math_thunks();
        register_pthread_thunks();
        register_misc_thunks();
        register_dl_thunks();
        register_old_bionic_thunks();
        register_signal_thunks();
        register_android_thunks();
        register_gles_thunks();
        register_data_symbols();
        register_zlib_thunks();
        register_posix_extra_thunks();
    });
}

}  // namespace a64b
