// pthreads, semaphores and thread-local storage for guest code.
// Guest synchronisation objects are only 4 bytes wide (32-bit bionic); the word
// holds an index into a table of host objects, created lazily so that static
// initialisers (PTHREAD_MUTEX_INITIALIZER etc.) work.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

#include <atomic>
#include <mutex>

#include "guest_types.h"
#include "jni_bridge.h"
#include "thunk.h"

namespace a64b {

namespace {

HandleTable<pthread_mutex_t> g_mutexes;
HandleTable<pthread_cond_t> g_conds;
HandleTable<pthread_rwlock_t> g_rwlocks;
HandleTable<sem_t> g_sems;

std::atomic<u32>* word(u32 addr) { return reinterpret_cast<std::atomic<u32>*>(g2h<u32>(addr)); }

// Guest word encodings: bit0 set => (index << 1) | 1, otherwise a static initialiser.
pthread_mutex_t* get_mutex(u32 addr) {
    auto* w = word(addr);
    for (;;) {
        u32 v = w->load(std::memory_order_acquire);
        if (v & 1) {
            if (auto* m = g_mutexes.get(v >> 1)) return m;
            v = 0;  // stale/destroyed: recreate as a plain mutex
        }
        auto* m = new pthread_mutex_t;
        pthread_mutexattr_t at;
        pthread_mutexattr_init(&at);
        int type = (v >> 14) & 3;  // bionic: 0 normal, 1 recursive, 2 errorcheck
        pthread_mutexattr_settype(&at, type == 1 ? PTHREAD_MUTEX_RECURSIVE : type == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
        pthread_mutex_init(m, &at);
        pthread_mutexattr_destroy(&at);
        u32 idx = g_mutexes.put(m);
        u32 expect = w->load();
        if (((expect & 1) == 0 || !g_mutexes.get(expect >> 1)) && w->compare_exchange_strong(expect, (idx << 1) | 1)) return m;
        g_mutexes.take(idx);
        pthread_mutex_destroy(m);
        delete m;
    }
}

pthread_cond_t* get_cond(u32 addr) {
    auto* w = word(addr);
    for (;;) {
        u32 v = w->load(std::memory_order_acquire);
        if (v & 1) {
            if (auto* cv = g_conds.get(v >> 1)) return cv;
            v = 0;
        }
        auto* cv = new pthread_cond_t;
        pthread_condattr_t at;
        pthread_condattr_init(&at);
        if (v & 2) pthread_condattr_setclock(&at, CLOCK_MONOTONIC);
        pthread_cond_init(cv, &at);
        pthread_condattr_destroy(&at);
        u32 idx = g_conds.put(cv);
        u32 expect = w->load();
        // Remember the clock in bit 1 of the index word.
        if (((expect & 1) == 0 || !g_conds.get(expect >> 1)) && w->compare_exchange_strong(expect, (idx << 1) | 1)) return cv;
        g_conds.take(idx);
        pthread_cond_destroy(cv);
        delete cv;
    }
}

pthread_rwlock_t* get_rwlock(u32 addr) {
    auto* w = word(addr);
    for (;;) {
        u32 v = w->load(std::memory_order_acquire);
        if (v & 1) {
            if (auto* l = g_rwlocks.get(v >> 1)) return l;
        }
        auto* l = new pthread_rwlock_t;
        pthread_rwlock_init(l, nullptr);
        u32 idx = g_rwlocks.put(l);
        u32 expect = w->load();
        if (((expect & 1) == 0 || !g_rwlocks.get(expect >> 1)) && w->compare_exchange_strong(expect, (idx << 1) | 1)) return l;
        g_rwlocks.take(idx);
        pthread_rwlock_destroy(l);
        delete l;
    }
}

sem_t* get_sem(u32 addr, bool create, u32 initial = 0) {
    auto* w = word(addr);
    u32 v = w->load();
    if (!create && (v & 1)) {
        if (auto* s = g_sems.get(v >> 1)) return s;
    }
    auto* s = new sem_t;
    sem_init(s, 0, create ? initial : v >> 1);
    w->store((g_sems.put(s) << 1) | 1);
    return s;
}

timespec abs_from_guest(const GTimespec* g) { return from_guest(g); }

timespec realtime_after(const GTimespec* rel) {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    now.tv_sec += rel->tv_sec;
    now.tv_nsec += rel->tv_nsec;
    if (now.tv_nsec >= 1000000000) {
        now.tv_sec++;
        now.tv_nsec -= 1000000000;
    }
    return now;
}

// Converts an absolute CLOCK_MONOTONIC deadline to CLOCK_REALTIME.
timespec mono_to_real(const GTimespec* abs_mono) {
    timespec mono, real;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    i64 ns = ((i64)abs_mono->tv_sec - mono.tv_sec) * 1000000000ll + (abs_mono->tv_nsec - mono.tv_nsec);
    if (ns < 0) ns = 0;
    ns += (i64)real.tv_sec * 1000000000ll + real.tv_nsec;
    timespec out;
    out.tv_sec = ns / 1000000000ll;
    out.tv_nsec = ns % 1000000000ll;
    return out;
}

// ---- threads ----
struct ThreadRec {
    pthread_t host;
    u32 fn = 0, arg = 0;
    u32 stack_size = 0;
    std::atomic<u32> retval{0};
    bool detached = false;
    std::atomic<Cpu*> cpu{nullptr};  // while the thread runs: where signals for it go
};
HandleTable<ThreadRec> g_threads;

// ---- thread-specific keys ----
constexpr int kMaxKeys = 256;
std::mutex g_key_mu;
struct KeySlot {
    bool used;
    u32 dtor;
} g_keys[kMaxKeys];

void run_key_destructors(Cpu& c) {
    for (int round = 0; round < 4; round++) {
        bool any = false;
        for (int k = 0; k < kMaxKeys; k++) {
            u32 v = c.tls_values[k], d;
            {
                std::lock_guard<std::mutex> lk(g_key_mu);
                d = g_keys[k].used ? g_keys[k].dtor : 0;
            }
            if (v && d) {
                c.tls_values[k] = 0;
                c.call(d, {v});
                any = true;
            }
        }
        if (!any) break;
    }
}

void* thread_main(void* p) {
    u32 id = (u32)(uintptr_t)p;
    ThreadRec* t = g_threads.get(id);
    Cpu& c = Cpu::current();
    c.thread_id = id;
    c.guest_thread = true;
    t->cpu.store(&c);
    g_guest_threads.fetch_add(1);
    u32 ret = 0;
    try {
        ret = (u32)c.call(t->fn, {t->arg});
    } catch (const ThreadExit& e) {
        ret = e.retval;
    }
    c.depth = 0;
    try {
        run_key_destructors(c);
    } catch (const ThreadExit&) {
    }
    jni_thread_exit(c);
    t->cpu.store(nullptr);
    t->retval.store(ret);
    g_guest_threads.fetch_sub(1);
    return nullptr;
}

u32 self_id() {
    Cpu& c = Cpu::current();
    if (!c.thread_id) {
        auto* t = new ThreadRec;
        t->host = pthread_self();
        t->detached = true;
        t->cpu.store(&c);
        c.thread_id = g_threads.put(t);
    }
    return c.thread_id;
}

// Guest pthread_attr_t (bionic, 32-bit): flags, stack_base, stack_size, guard_size, sched_policy, sched_priority
struct GAttr {
    u32 flags, stack_base, stack_size, guard_size;
    i32 sched_policy, sched_priority;
};

std::recursive_mutex g_once_mu;

}  // namespace

#define RAW(name) reg_raw(#name, [](Cpu& c)
#define A32() a.next32()

void register_pthread_thunks() {
    // ---- threads ----
    RAW(pthread_create) {
        ArgReader a(c);
        u32 out = A32(), attr = A32(), fn = A32(), arg = A32();
        auto* t = new ThreadRec;
        t->fn = fn;
        t->arg = arg;
        if (attr) t->detached = g2h<GAttr>(attr)->flags & 1;
        u32 id = g_threads.put(t);
        pthread_attr_t ha;
        pthread_attr_init(&ha);
        pthread_attr_setstacksize(&ha, 1024 * 1024);  // host side only runs the emulator loop
        if (t->detached) pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
        if (out) *g2h<u32>(out) = id;
        int r = pthread_create(&t->host, &ha, thread_main, (void*)(uintptr_t)id);
        pthread_attr_destroy(&ha);
        if (r != 0) g_threads.take(id);
        c.set_ret32((u32)r);
    });
    reg("pthread_self", +[]() -> u32 { return self_id(); });
    reg("pthread_equal", +[](u32 x, u32 y) -> int { return x == y; });
    reg("pthread_join", +[](u32 id, u32* ret) -> int {
        ThreadRec* t = g_threads.get(id);
        if (!t) return ESRCH;
        if (pthread_equal(t->host, pthread_self())) {
            // A thread joining itself (seen when a network callback destroys the object that owns its
            // thread). Android of the time did not report this; today it is EDEADLK, which such games
            // turn into an uncaught C++ exception. Treat it as a detach and let the thread finish.
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 4) LOGW("pthread_join: thread %u joins itself, treated as detach", id);
            pthread_detach(t->host);
            t->detached = true;
            if (ret) *ret = 0;
            return 0;
        }
        BlockingCall blocking;
        int r = pthread_join(t->host, nullptr);
        if (r == 0) {
            if (ret) *ret = t->retval.load();
            g_threads.take(id);
            delete t;
        }
        return r;
    });
    reg("pthread_detach", +[](u32 id) -> int {
        ThreadRec* t = g_threads.get(id);
        return t ? pthread_detach(t->host) : ESRCH;
    });
    RAW(pthread_exit) {
        ArgReader a(c);
        throw ThreadExit{A32()};
    });
    reg("pthread_kill", +[](u32 id, int sig) -> int {
        ThreadRec* t = g_threads.get(id);
        if (!t) return ESRCH;
        return guest_signal_send(t->cpu, t->host, sig);
    });
    reg("pthread_setname_np", +[](u32, const char*) -> int { return 0; });
    reg("pthread_getname_np", +[](u32, char* b, size_t n) -> int { if (n) b[0] = 0; return 0; });
    reg("pthread_setschedparam", +[](u32, int, void*) -> int { return 0; });
    reg("pthread_getschedparam", +[](u32, int* pol, i32* param) -> int { if (pol) *pol = 0; if (param) *param = 0; return 0; });
    reg("pthread_setcancelstate", +[](int, int*) -> int { return 0; });
    reg("pthread_setcanceltype", +[](int, int*) -> int { return 0; });
    reg("pthread_cancel", +[](u32) -> int { return 0; });
    reg("pthread_testcancel", +[]() {});
    reg("pthread_getcpuclockid", +[](u32, int* clk) -> int { *clk = CLOCK_THREAD_CPUTIME_ID; return 0; });
    reg("pthread_gettid_np", +[](u32) -> int { return (int)gettid(); });
    reg("__pthread_cleanup_push", +[](u32*, u32, u32) {});
    reg("__pthread_cleanup_pop", +[](u32*, int) {});

    // ---- attributes ----
    reg("pthread_attr_init", +[](GAttr* at) -> int { memset(at, 0, sizeof(*at)); at->stack_size = 1024 * 1024; at->guard_size = 4096; return 0; });
    reg("pthread_attr_destroy", +[](GAttr*) -> int { return 0; });
    reg("pthread_attr_setdetachstate", +[](GAttr* at, int st) -> int { at->flags = (at->flags & ~1u) | (st ? 1 : 0); return 0; });
    reg("pthread_attr_getdetachstate", +[](const GAttr* at, int* st) -> int { *st = at->flags & 1; return 0; });
    reg("pthread_attr_setstacksize", +[](GAttr* at, u32 sz) -> int { at->stack_size = sz; return 0; });
    reg("pthread_attr_getstacksize", +[](const GAttr* at, u32* sz) -> int { *sz = at->stack_size; return 0; });
    reg("pthread_attr_setstack", +[](GAttr* at, u32 base, u32 sz) -> int { at->stack_base = base; at->stack_size = sz; return 0; });
    reg("pthread_attr_getstack", +[](const GAttr*, u32* base, u32* sz) -> int {
        Cpu& c = Cpu::current();
        *base = c.stack_base;
        *sz = c.stack_size;
        return 0;
    });
    reg("pthread_attr_setguardsize", +[](GAttr* at, u32 sz) -> int { at->guard_size = sz; return 0; });
    reg("pthread_attr_getguardsize", +[](const GAttr* at, u32* sz) -> int { *sz = at->guard_size; return 0; });
    reg("pthread_attr_setschedpolicy", +[](GAttr* at, int p) -> int { at->sched_policy = p; return 0; });
    reg("pthread_attr_getschedpolicy", +[](const GAttr* at, int* p) -> int { *p = at->sched_policy; return 0; });
    reg("pthread_attr_setschedparam", +[](GAttr* at, const i32* p) -> int { at->sched_priority = *p; return 0; });
    reg("pthread_attr_getschedparam", +[](const GAttr* at, i32* p) -> int { *p = at->sched_priority; return 0; });
    reg("pthread_attr_setscope", +[](GAttr*, int) -> int { return 0; });
    reg("pthread_attr_setinheritsched", +[](GAttr*, int) -> int { return 0; });
    reg("pthread_getattr_np", +[](u32, GAttr* at) -> int {
        Cpu& c = Cpu::current();
        memset(at, 0, sizeof(*at));
        at->stack_base = c.stack_base;
        at->stack_size = c.stack_size;
        return 0;
    });

    // ---- mutexes ----
    reg("pthread_mutex_init", +[](u32 m, const u32* attr) -> int {
        u32 type = attr ? (*attr & 3) : 0;
        *g2h<u32>(m) = type << 14;
        return 0;
    });
    reg("pthread_mutex_destroy", +[](u32 m) -> int {
        u32 v = *g2h<u32>(m);
        if (v & 1) {
            if (auto* h = g_mutexes.take(v >> 1)) {
                pthread_mutex_destroy(h);
                delete h;
            }
        }
        *g2h<u32>(m) = 0;
        return 0;
    });
    reg("pthread_mutex_lock", +[](u32 m) -> int {
        pthread_mutex_t* h = get_mutex(m);
        int r = pthread_mutex_trylock(h);
        if (r != EBUSY) return r;
        BlockingCall blocking;
        return pthread_mutex_lock(h);
    });
    reg("pthread_mutex_trylock", +[](u32 m) -> int { return pthread_mutex_trylock(get_mutex(m)); });
    reg("pthread_mutex_unlock", +[](u32 m) -> int { return pthread_mutex_unlock(get_mutex(m)); });
    reg("pthread_mutex_timedlock", +[](u32 m, const GTimespec* ts) -> int {
        timespec h = abs_from_guest(ts);
        return pthread_mutex_timedlock(get_mutex(m), &h);
    });
    reg("pthread_mutex_lock_timeout_np", +[](u32 m, u32 ms) -> int {
        GTimespec rel{(i32)(ms / 1000), (i32)(ms % 1000) * 1000000};
        timespec h = realtime_after(&rel);
        return pthread_mutex_timedlock(get_mutex(m), &h);
    });
    reg("pthread_mutexattr_init", +[](u32* at) -> int { *at = 0; return 0; });
    reg("pthread_mutexattr_destroy", +[](u32*) -> int { return 0; });
    reg("pthread_mutexattr_settype", +[](u32* at, int type) -> int { *at = (*at & ~3u) | (type & 3); return 0; });
    reg("pthread_mutexattr_gettype", +[](const u32* at, int* type) -> int { *type = *at & 3; return 0; });
    reg("pthread_mutexattr_setpshared", +[](u32*, int) -> int { return 0; });
    reg("pthread_mutexattr_getpshared", +[](const u32*, int* v) -> int { *v = 0; return 0; });

    // ---- condition variables ----
    reg("pthread_cond_init", +[](u32 cv, const u32* attr) -> int {
        *g2h<u32>(cv) = (attr && (*attr & 2)) ? 2 : 0;
        return 0;
    });
    reg("pthread_cond_destroy", +[](u32 cv) -> int {
        u32 v = *g2h<u32>(cv);
        if (v & 1) {
            if (auto* h = g_conds.take(v >> 1)) {
                pthread_cond_destroy(h);
                delete h;
            }
        }
        *g2h<u32>(cv) = 0;
        return 0;
    });
    reg("pthread_cond_signal", +[](u32 cv) -> int { return pthread_cond_signal(get_cond(cv)); });
    reg("pthread_cond_broadcast", +[](u32 cv) -> int { return pthread_cond_broadcast(get_cond(cv)); });
    reg("pthread_cond_wait", +[](u32 cv, u32 m) -> int {
        BlockingCall blocking;
        return pthread_cond_wait(get_cond(cv), get_mutex(m));
    });
    reg("pthread_cond_timedwait", +[](u32 cv, u32 m, const GTimespec* ts) -> int {
        BlockingCall blocking;
        timespec h = abs_from_guest(ts);
        return pthread_cond_timedwait(get_cond(cv), get_mutex(m), &h);
    });
    auto mono_wait = +[](u32 cv, u32 m, const GTimespec* ts) -> int {
        // Only valid for condition variables that use the realtime clock (the default).
        BlockingCall blocking;
        timespec h = mono_to_real(ts);
        return pthread_cond_timedwait(get_cond(cv), get_mutex(m), &h);
    };
    reg("pthread_cond_timedwait_monotonic_np", mono_wait);
    reg("pthread_cond_timedwait_monotonic", mono_wait);
    reg("pthread_cond_timedwait_relative_np", +[](u32 cv, u32 m, const GTimespec* rel) -> int {
        BlockingCall blocking;
        timespec h = realtime_after(rel);
        return pthread_cond_timedwait(get_cond(cv), get_mutex(m), &h);
    });
    reg("pthread_cond_timeout_np", +[](u32 cv, u32 m, u32 ms) -> int {
        BlockingCall blocking;
        GTimespec rel{(i32)(ms / 1000), (i32)(ms % 1000) * 1000000};
        timespec h = realtime_after(&rel);
        return pthread_cond_timedwait(get_cond(cv), get_mutex(m), &h);
    });
    reg("pthread_condattr_init", +[](u32* at) -> int { *at = 0; return 0; });
    reg("pthread_condattr_destroy", +[](u32*) -> int { return 0; });
    reg("pthread_condattr_setclock", +[](u32* at, int clk) -> int { *at = (*at & ~2u) | (clk == CLOCK_MONOTONIC ? 2 : 0); return 0; });
    reg("pthread_condattr_getclock", +[](const u32* at, int* clk) -> int { *clk = (*at & 2) ? CLOCK_MONOTONIC : CLOCK_REALTIME; return 0; });
    reg("pthread_condattr_setpshared", +[](u32*, int) -> int { return 0; });

    // ---- rwlocks ----
    reg("pthread_rwlock_init", +[](u32 l, void*) -> int { *g2h<u32>(l) = 0; return 0; });
    reg("pthread_rwlock_destroy", +[](u32 l) -> int {
        u32 v = *g2h<u32>(l);
        if (v & 1) {
            if (auto* h = g_rwlocks.take(v >> 1)) {
                pthread_rwlock_destroy(h);
                delete h;
            }
        }
        *g2h<u32>(l) = 0;
        return 0;
    });
    reg("pthread_rwlock_rdlock", +[](u32 l) -> int {
        pthread_rwlock_t* h = get_rwlock(l);
        if (pthread_rwlock_tryrdlock(h) == 0) return 0;
        BlockingCall blocking;
        return pthread_rwlock_rdlock(h);
    });
    reg("pthread_rwlock_tryrdlock", +[](u32 l) -> int { return pthread_rwlock_tryrdlock(get_rwlock(l)); });
    reg("pthread_rwlock_wrlock", +[](u32 l) -> int {
        pthread_rwlock_t* h = get_rwlock(l);
        if (pthread_rwlock_trywrlock(h) == 0) return 0;
        BlockingCall blocking;
        return pthread_rwlock_wrlock(h);
    });
    reg("pthread_rwlock_trywrlock", +[](u32 l) -> int { return pthread_rwlock_trywrlock(get_rwlock(l)); });
    reg("pthread_rwlock_unlock", +[](u32 l) -> int { return pthread_rwlock_unlock(get_rwlock(l)); });
    reg("pthread_rwlockattr_init", +[](u32* at) -> int { *at = 0; return 0; });
    reg("pthread_rwlockattr_destroy", +[](u32*) -> int { return 0; });

    // ---- once ----
    RAW(pthread_once) {
        ArgReader a(c);
        u32 ctl = A32(), fn = A32();
        std::lock_guard<std::recursive_mutex> lk(g_once_mu);
        i32* state = g2h<i32>(ctl);
        if (*state == 0) {
            *state = 1;
            c.call(fn, {});
            *state = 2;
        }
        c.set_ret32(0);
    });

    // ---- thread-specific data ----
    reg("pthread_key_create", +[](i32* key, u32 dtor) -> int {
        std::lock_guard<std::mutex> lk(g_key_mu);
        for (int k = 1; k < kMaxKeys; k++) {
            if (!g_keys[k].used) {
                g_keys[k] = KeySlot{true, dtor};
                *key = k;
                return 0;
            }
        }
        return EAGAIN;
    });
    reg("pthread_key_delete", +[](i32 key) -> int {
        if (key <= 0 || key >= kMaxKeys) return EINVAL;
        std::lock_guard<std::mutex> lk(g_key_mu);
        g_keys[key] = KeySlot{false, 0};
        return 0;
    });
    reg("pthread_getspecific", +[](i32 key) -> u32 {
        if (key <= 0 || key >= kMaxKeys) return 0;
        return Cpu::current().tls_values[key];
    });
    reg("pthread_setspecific", +[](i32 key, u32 value) -> int {
        if (key <= 0 || key >= kMaxKeys) return EINVAL;
        Cpu::current().tls_values[key] = value;
        return 0;
    });

    // ---- semaphores ----
    reg("sem_init", +[](u32 s, int, u32 value) -> int { *g2h<u32>(s) = 0; get_sem(s, true, value); return 0; });
    reg("sem_destroy", +[](u32 s) -> int {
        u32 v = *g2h<u32>(s);
        if (v & 1) {
            if (auto* h = g_sems.take(v >> 1)) {
                sem_destroy(h);
                delete h;
            }
        }
        *g2h<u32>(s) = 0;
        return 0;
    });
    reg("sem_wait", +[](u32 s) -> int {
        sem_t* h = get_sem(s, false);
        if (sem_trywait(h) == 0) return 0;
        BlockingCall blocking;
        return sem_wait(h);
    });
    reg("sem_trywait", +[](u32 s) -> int { return sem_trywait(get_sem(s, false)); });
    reg("sem_post", +[](u32 s) -> int { return sem_post(get_sem(s, false)); });
    reg("sem_timedwait", +[](u32 s, const GTimespec* ts) -> int {
        BlockingCall blocking;
        timespec h = abs_from_guest(ts);
        return sem_timedwait(get_sem(s, false), &h);
    });
    reg("sem_getvalue", +[](u32 s, int* out) -> int { return sem_getvalue(get_sem(s, false), out); });

    // ---- C++ static initialisation guards (32-bit guard word, bit 0 = initialised) ----
    reg("__cxa_guard_acquire", +[](u32* guard) -> int {
        if (*guard & 1) return 0;
        g_once_mu.lock();
        if (*guard & 1) {
            g_once_mu.unlock();
            return 0;
        }
        return 1;  // lock stays held until release/abort
    });
    reg("__cxa_guard_release", +[](u32* guard) {
        *guard |= 1;
        g_once_mu.unlock();
    });
    reg("__cxa_guard_abort", +[](u32*) { g_once_mu.unlock(); });
}

}  // namespace a64b
