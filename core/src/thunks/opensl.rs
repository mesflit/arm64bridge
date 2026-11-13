//! OpenSL ES (native audio). The guest gets its own objects and interfaces whose
//! method tables point at host stubs; each one wraps the real 64-bit OpenSL ES
//! object. Buffer queue callbacks run the guest's callback on the audio thread.
// SPDX-License-Identifier: GPL-2.0-or-later
use crate::thunk::reg;

#[cfg(not(target_os = "android"))]
pub fn register_opensl_thunks() {
    reg("slCreateEngine", |_out: u32, _a: u32, _b: u32, _c: u32, _d: u32, _e: u32| -> u32 { 12 /* SL_RESULT_FEATURE_UNSUPPORTED */ });
}

#[cfg(target_os = "android")]
pub use sl::register_opensl_thunks;

#[cfg(target_os = "android")]
mod sl {
    use std::collections::{BTreeMap, HashMap};
    use std::os::raw::c_void;
    use std::sync::Mutex;

    use super::reg;
    use crate::cpu::{catch_thread_exit, trap_create, Cpu, TrapFn};
    use crate::mem::*;
    use crate::thunk::{thunk_register_data, IntoThunk};
    use crate::util::lock;

    type P = *mut c_void;
    type Iid = *const c_void;

    const SL_RESULT_SUCCESS: u32 = 0;
    const SL_RESULT_PARAMETER_INVALID: u32 = 2;
    const SL_RESULT_CONTENT_UNSUPPORTED: u32 = 9;
    const SL_RESULT_FEATURE_UNSUPPORTED: u32 = 12;
    const SL_DATALOCATOR_OUTPUTMIX: u32 = 4;
    const SL_DATALOCATOR_BUFFERQUEUE: u32 = 6;
    const SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE: u32 = 0x8000_07BD;
    const SL_DATAFORMAT_PCM: u32 = 2;

    #[link(name = "OpenSLES")]
    extern "C" {
        static SL_IID_ENGINE: Iid;
        static SL_IID_PLAY: Iid;
        static SL_IID_BUFFERQUEUE: Iid;
        static SL_IID_ANDROIDSIMPLEBUFFERQUEUE: Iid;
        static SL_IID_VOLUME: Iid;
        static SL_IID_OUTPUTMIX: Iid;
        static SL_IID_SEEK: Iid;
        static SL_IID_ENVIRONMENTALREVERB: Iid;
        static SL_IID_EFFECTSEND: Iid;
        static SL_IID_MUTESOLO: Iid;
        static SL_IID_PLAYBACKRATE: Iid;
        fn slCreateEngine(out: *mut P, num_options: u32, options: P, num_itf: u32, ids: *const Iid, req: *const u32) -> u32;
    }

    // Host structures (64-bit layouts).
    #[repr(C)]
    struct HLocatorBq {
        ty: u32,
        num_buffers: u32,
    }
    #[repr(C)]
    struct HDataSource {
        locator: *const c_void,
        format: *const c_void,
    }
    #[repr(C)]
    struct HLocatorMix {
        ty: u32,
        mix: P,
    }

    /// Method `slot` of a host interface (a pointer to a pointer to a table of functions).
    unsafe fn method<T: Copy>(itf: P, slot: usize) -> T {
        debug_assert!(std::mem::size_of::<T>() == std::mem::size_of::<usize>());
        let table = *(itf as *const *const usize);
        std::mem::transmute_copy(&*table.add(slot))
    }

    #[derive(Clone, Copy, PartialEq, Eq)]
    enum Kind {
        Object = 0,
        Engine = 1,
        Play = 2,
        Queue = 3,
        Volume = 4,
    }

    struct Wrap {
        kind: Kind,
        guest: u32,                 // guest address of the interface (a pointer to the method table)
        host: P,                    // SLObjectItf / SLEngineItf / ...
        itfs: BTreeMap<u32, usize>, // interfaces handed out by this object, by guest IID
        cb: u32,                    // guest buffer queue callback
        cb_ctx: u32,
    }

    struct State {
        wraps: HashMap<u32, usize>, // guest interface -> Wrap
        iids: HashMap<u32, usize>,  // guest IID pointer -> host IID
        vt: [u32; 5],
        iid_engine: u32,
        iid_play: u32,
        iid_bq: u32,
        iid_asbq: u32,
        iid_volume: u32,
    }
    static STATE: Mutex<Option<State>> = Mutex::new(None);
    fn st<R>(f: impl FnOnce(&mut State) -> R) -> R {
        f(lock(&STATE).get_or_insert_with(|| State {
            wraps: HashMap::new(),
            iids: HashMap::new(),
            vt: [0; 5],
            iid_engine: 0,
            iid_play: 0,
            iid_bq: 0,
            iid_asbq: 0,
            iid_volume: 0,
        }))
    }

    fn find(guest: u32) -> *mut Wrap {
        st(|s| s.wraps.get(&guest).copied().unwrap_or(0)) as *mut Wrap
    }
    fn find_kind(guest: u32, kind: Kind) -> P {
        let w = find(guest);
        unsafe {
            if !w.is_null() && (*w).kind == kind {
                (*w).host
            } else {
                std::ptr::null_mut()
            }
        }
    }

    fn make(kind: Kind, host: P) -> *mut Wrap {
        let guest = g_calloc(1, 8);
        let w = Box::into_raw(Box::new(Wrap { kind, guest, host, itfs: BTreeMap::new(), cb: 0, cb_ctx: 0 }));
        st(|s| {
            unsafe { wr::<u32>(guest, s.vt[kind as usize]) };
            s.wraps.insert(guest, w as usize);
        });
        w
    }

    unsafe fn drop_wrap(w: *mut Wrap) {
        st(|s| s.wraps.remove(&(*w).guest));
        g_free((*w).guest);
        drop(Box::from_raw(w));
    }

    fn stub<M, F: IntoThunk<M>>(name: &str, f: F) -> u32 {
        trap_create(name, Box::leak(f.into_thunk()))
    }
    fn raw_stub(name: &str, f: Box<TrapFn>) -> u32 {
        trap_create(name, Box::leak(f))
    }
    fn unsupported(name: &'static str) -> u32 {
        raw_stub(
            name,
            Box::new(move |c: &mut Cpu| {
                logw!("OpenSL ES: {} is not bridged", name);
                c.set_ret32(SL_RESULT_FEATURE_UNSUPPORTED);
            }),
        )
    }
    fn ok_stub(name: &str) -> u32 {
        raw_stub(name, Box::new(|c: &mut Cpu| c.set_ret32(SL_RESULT_SUCCESS)))
    }

    unsafe fn export_iid(name: &str, host: Iid) -> u32 {
        let guid = g_calloc(1, 16);
        std::ptr::copy_nonoverlapping(host as *const u8, g2h::<u8>(guid), 16);
        let var = g_calloc(1, 4);
        wr::<u32>(var, guid);
        thunk_register_data(name, var);
        st(|s| s.iids.insert(guid, host as usize));
        guid
    }

    // Host buffer queue callback (audio thread) -> guest callback(queue interface, context).
    unsafe extern "C" fn queue_callback(_q: P, ctx: P) {
        let w = &*(ctx as *const Wrap);
        if w.cb != 0 {
            let _ = catch_thread_exit(|| Cpu::current().call(w.cb, &[w.guest, w.cb_ctx]));
        }
    }

    // ---- SLObjectItf ----
    fn object_realize(this: u32, _async: u32) -> u32 {
        let o = find_kind(this, Kind::Object);
        if o.is_null() {
            return SL_RESULT_PARAMETER_INVALID;
        }
        unsafe { method::<unsafe extern "C" fn(P, u32) -> u32>(o, 0)(o, 0) }
    }
    fn object_resume(this: u32, _async: u32) -> u32 {
        let o = find_kind(this, Kind::Object);
        if o.is_null() {
            return SL_RESULT_PARAMETER_INVALID;
        }
        unsafe { method::<unsafe extern "C" fn(P, u32) -> u32>(o, 1)(o, 0) }
    }
    fn object_get_state(this: u32, state: *mut u32) -> u32 {
        let o = find_kind(this, Kind::Object);
        if o.is_null() {
            return SL_RESULT_PARAMETER_INVALID;
        }
        unsafe { method::<unsafe extern "C" fn(P, *mut u32) -> u32>(o, 2)(o, state) }
    }
    fn object_get_interface(this: u32, iid: u32, out: *mut u32) -> u32 {
        let w = find(this);
        unsafe {
            if w.is_null() || (*w).kind != Kind::Object || out.is_null() {
                return SL_RESULT_PARAMETER_INVALID;
            }
            *out = 0;
            if let Some(&known) = (*w).itfs.get(&iid) {
                *out = (*(known as *const Wrap)).guest;
                return SL_RESULT_SUCCESS;
            }
            let (kind, host_iid) = st(|s| {
                let kind = if iid == s.iid_engine {
                    Some(Kind::Engine)
                } else if iid == s.iid_play {
                    Some(Kind::Play)
                } else if iid == s.iid_bq || iid == s.iid_asbq {
                    Some(Kind::Queue)
                } else if iid == s.iid_volume {
                    Some(Kind::Volume)
                } else {
                    None
                };
                (kind, s.iids.get(&iid).copied().unwrap_or(0) as Iid)
            });
            let Some(kind) = kind else {
                logw!("OpenSL ES: GetInterface for an interface that is not bridged");
                return SL_RESULT_FEATURE_UNSUPPORTED;
            };
            let o = (*w).host;
            let mut itf: P = std::ptr::null_mut();
            let r = method::<unsafe extern "C" fn(P, Iid, *mut P) -> u32>(o, 3)(o, host_iid, &mut itf);
            if r != SL_RESULT_SUCCESS {
                return r;
            }
            let iw = make(kind, itf);
            (*w).itfs.insert(iid, iw as usize);
            *out = (*iw).guest;
            SL_RESULT_SUCCESS
        }
    }
    fn object_destroy(this: u32) {
        let w = find(this);
        unsafe {
            if w.is_null() || (*w).kind != Kind::Object {
                return;
            }
            let o = (*w).host;
            method::<unsafe extern "C" fn(P)>(o, 6)(o); // waits for running callbacks
            for &iw in (*w).itfs.values() {
                drop_wrap(iw as *mut Wrap);
            }
            drop_wrap(w);
        }
    }

    // ---- SLEngineItf ----
    fn engine_create_output_mix(this: u32, out: *mut u32, _n: u32, _ids: u32, _req: u32) -> u32 {
        let e = find_kind(this, Kind::Engine);
        if e.is_null() || out.is_null() {
            return SL_RESULT_PARAMETER_INVALID;
        }
        unsafe {
            let mut mix: P = std::ptr::null_mut();
            let r = method::<unsafe extern "C" fn(P, *mut P, u32, *const Iid, *const u32) -> u32>(e, 7)(e, &mut mix, 0, std::ptr::null(), std::ptr::null());
            *out = if r == SL_RESULT_SUCCESS { (*make(Kind::Object, mix)).guest } else { 0 };
            r
        }
    }

    fn engine_create_audio_player(this: u32, out: *mut u32, src: *const u32, snk: *const u32, n: u32, ids: *const u32, req: *const u32) -> u32 {
        let e = find_kind(this, Kind::Engine);
        if e.is_null() || out.is_null() || src.is_null() || snk.is_null() {
            return SL_RESULT_PARAMETER_INVALID;
        }
        unsafe {
            *out = 0;
            // Source: only PCM buffer queues (what game sound engines use) are bridged.
            let (gloc, gfmt) = (*src, *src.add(1));
            let loc_ty = if gloc != 0 { rd::<u32>(gloc) } else { 0 };
            let fmt: [u32; 7] = if gfmt != 0 { rd(gfmt) } else { [0; 7] };
            if gloc == 0 || gfmt == 0 || (loc_ty != SL_DATALOCATOR_BUFFERQUEUE && loc_ty != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE) || fmt[0] != SL_DATAFORMAT_PCM {
                logw!("OpenSL ES: audio player with locator 0x{:x} / format 0x{:x} is not bridged", loc_ty, fmt[0]);
                return SL_RESULT_CONTENT_UNSUPPORTED;
            }
            let hloc = HLocatorBq { ty: SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, num_buffers: rd::<u32>(gloc + 4) };
            let hsrc = HDataSource { locator: &hloc as *const _ as *const c_void, format: fmt.as_ptr() as *const c_void };
            // Sink: { SL_DATALOCATOR_OUTPUTMIX, output mix object }
            let gsloc = *snk;
            let mix = if gsloc != 0 { find_kind(rd::<u32>(gsloc + 4), Kind::Object) } else { std::ptr::null_mut() };
            if mix.is_null() {
                return SL_RESULT_PARAMETER_INVALID;
            }
            let hsloc = HLocatorMix { ty: SL_DATALOCATOR_OUTPUTMIX, mix };
            let hsnk = HDataSource { locator: &hsloc as *const _ as *const c_void, format: std::ptr::null() };

            let mut hids: Vec<Iid> = Vec::new();
            let mut hreq: Vec<u32> = Vec::new();
            if !ids.is_null() {
                st(|s| {
                    for i in 0..n as usize {
                        let g = *ids.add(i);
                        if g != s.iid_play && g != s.iid_bq && g != s.iid_asbq && g != s.iid_volume {
                            continue;
                        }
                        // A buffer queue source is driven through the Android simple buffer queue interface.
                        hids.push(if g == s.iid_bq { SL_IID_ANDROIDSIMPLEBUFFERQUEUE } else { s.iids.get(&g).copied().unwrap_or(0) as Iid });
                        hreq.push(if req.is_null() { 1 } else { *req.add(i) });
                    }
                });
            }
            let mut player: P = std::ptr::null_mut();
            let create = method::<unsafe extern "C" fn(P, *mut P, *const HDataSource, *const HDataSource, u32, *const Iid, *const u32) -> u32>(e, 2);
            let r = create(e, &mut player, &hsrc, &hsnk, hids.len() as u32, hids.as_ptr(), hreq.as_ptr());
            logi!("OpenSL ES: audio player {} ch, {} Hz, {} bit -> {}", fmt[1], fmt[2] / 1000, fmt[3], if r == SL_RESULT_SUCCESS { "ok" } else { "failed" });
            if r == SL_RESULT_SUCCESS {
                *out = (*make(Kind::Object, player)).guest;
            }
            r
        }
    }

    // Calls method `slot` of the interface `this` (of the given kind) with one extra argument.
    fn call1<A: Copy>(this: u32, kind: Kind, slot: usize, a: A) -> u32 {
        let p = find_kind(this, kind);
        if p.is_null() {
            return SL_RESULT_PARAMETER_INVALID;
        }
        unsafe { method::<unsafe extern "C" fn(P, A) -> u32>(p, slot)(p, a) }
    }

    fn queue_register_callback(this: u32, cb: u32, ctx: u32) -> u32 {
        let w = find(this);
        unsafe {
            if w.is_null() || (*w).kind != Kind::Queue {
                return SL_RESULT_PARAMETER_INVALID;
            }
            let q = (*w).host;
            (*w).cb = cb;
            (*w).cb_ctx = ctx;
            let f: Option<unsafe extern "C" fn(P, P)> = if cb != 0 { Some(queue_callback) } else { None };
            method::<unsafe extern "C" fn(P, Option<unsafe extern "C" fn(P, P)>, P) -> u32>(q, 3)(q, f, w as P)
        }
    }

    fn create_engine(out: *mut u32, _a: u32, _b: u32, _c: u32, _d: u32, _e: u32) -> u32 {
        if out.is_null() {
            return SL_RESULT_PARAMETER_INVALID;
        }
        unsafe {
            let mut e: P = std::ptr::null_mut();
            let r = slCreateEngine(&mut e, 0, std::ptr::null_mut(), 0, std::ptr::null(), std::ptr::null());
            logi!("OpenSL ES: engine {}", if r == SL_RESULT_SUCCESS { "created" } else { "could not be created" });
            *out = if r == SL_RESULT_SUCCESS { (*make(Kind::Object, e)).guest } else { 0 };
            r
        }
    }

    fn table(entries: &[u32]) -> u32 {
        let t = g_calloc(entries.len() as u32, 4);
        for (i, &e) in entries.iter().enumerate() {
            unsafe { wr::<u32>(t + i as u32 * 4, e) };
        }
        t
    }

    pub fn register_opensl_thunks() {
        unsafe {
            let (engine, play, bq, asbq, volume) = (
                export_iid("SL_IID_ENGINE", SL_IID_ENGINE),
                export_iid("SL_IID_PLAY", SL_IID_PLAY),
                export_iid("SL_IID_BUFFERQUEUE", SL_IID_BUFFERQUEUE),
                export_iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", SL_IID_ANDROIDSIMPLEBUFFERQUEUE),
                export_iid("SL_IID_VOLUME", SL_IID_VOLUME),
            );
            st(|s| {
                s.iid_engine = engine;
                s.iid_play = play;
                s.iid_bq = bq;
                s.iid_asbq = asbq;
                s.iid_volume = volume;
            });
            // Known to exist but not bridged: GetInterface reports them as unsupported.
            export_iid("SL_IID_OUTPUTMIX", SL_IID_OUTPUTMIX);
            export_iid("SL_IID_SEEK", SL_IID_SEEK);
            export_iid("SL_IID_ENVIRONMENTALREVERB", SL_IID_ENVIRONMENTALREVERB);
            export_iid("SL_IID_EFFECTSEND", SL_IID_EFFECTSEND);
            export_iid("SL_IID_MUTESOLO", SL_IID_MUTESOLO);
            export_iid("SL_IID_PLAYBACKRATE", SL_IID_PLAYBACKRATE);
        }

        let object = table(&[
            stub("SLObject:Realize", object_realize),
            stub("SLObject:Resume", object_resume),
            stub("SLObject:GetState", object_get_state),
            stub("SLObject:GetInterface", object_get_interface),
            ok_stub("SLObject:RegisterCallback"),
            ok_stub("SLObject:AbortAsyncOperation"),
            stub("SLObject:Destroy", object_destroy),
            ok_stub("SLObject:SetPriority"),
            unsupported("SLObject:GetPriority"),
            ok_stub("SLObject:SetLossOfControlInterfaces"),
        ]);
        let engine = table(&[
            unsupported("SLEngine:CreateLEDDevice"),
            unsupported("SLEngine:CreateVibraDevice"),
            stub("SLEngine:CreateAudioPlayer", engine_create_audio_player),
            unsupported("SLEngine:CreateAudioRecorder"),
            unsupported("SLEngine:CreateMidiPlayer"),
            unsupported("SLEngine:CreateListener"),
            unsupported("SLEngine:Create3DGroup"),
            stub("SLEngine:CreateOutputMix", engine_create_output_mix),
            unsupported("SLEngine:CreateMetadataExtractor"),
            unsupported("SLEngine:CreateExtensionObject"),
            unsupported("SLEngine:QueryNumSupportedInterfaces"),
            unsupported("SLEngine:QuerySupportedInterfaces"),
            unsupported("SLEngine:QueryNumSupportedExtensions"),
            unsupported("SLEngine:QuerySupportedExtension"),
            unsupported("SLEngine:IsExtensionSupported"),
        ]);
        let play = table(&[
            stub("SLPlay:SetPlayState", |this: u32, state: u32| -> u32 { call1(this, Kind::Play, 0, state) }),
            stub("SLPlay:GetPlayState", |this: u32, state: *mut u32| -> u32 { call1(this, Kind::Play, 1, state) }),
            stub("SLPlay:GetDuration", |this: u32, ms: *mut u32| -> u32 { call1(this, Kind::Play, 2, ms) }),
            stub("SLPlay:GetPosition", |this: u32, ms: *mut u32| -> u32 { call1(this, Kind::Play, 3, ms) }),
            ok_stub("SLPlay:RegisterCallback"),
            ok_stub("SLPlay:SetCallbackEventsMask"),
            unsupported("SLPlay:GetCallbackEventsMask"),
            ok_stub("SLPlay:SetMarkerPosition"),
            ok_stub("SLPlay:ClearMarkerPosition"),
            unsupported("SLPlay:GetMarkerPosition"),
            ok_stub("SLPlay:SetPositionUpdatePeriod"),
            unsupported("SLPlay:GetPositionUpdatePeriod"),
        ]);
        let queue = table(&[
            stub("SLBufferQueue:Enqueue", |this: u32, buf: *const c_void, size: u32| -> u32 {
                let q = find_kind(this, Kind::Queue);
                if q.is_null() {
                    return SL_RESULT_PARAMETER_INVALID;
                }
                unsafe { method::<unsafe extern "C" fn(P, *const c_void, u32) -> u32>(q, 0)(q, buf, size) }
            }),
            stub("SLBufferQueue:Clear", |this: u32| -> u32 {
                let q = find_kind(this, Kind::Queue);
                if q.is_null() {
                    return SL_RESULT_PARAMETER_INVALID;
                }
                unsafe { method::<unsafe extern "C" fn(P) -> u32>(q, 1)(q) }
            }),
            // SLAndroidSimpleBufferQueueState is two 32-bit counters on every ABI.
            stub("SLBufferQueue:GetState", |this: u32, state: *mut u32| -> u32 { call1(this, Kind::Queue, 2, state) }),
            stub("SLBufferQueue:RegisterCallback", queue_register_callback),
        ]);
        let volume = table(&[
            stub("SLVolume:SetVolumeLevel", |this: u32, mb: i32| -> u32 { call1(this, Kind::Volume, 0, mb as i16 as i32) }),
            stub("SLVolume:GetVolumeLevel", |this: u32, mb: *mut i16| -> u32 { call1(this, Kind::Volume, 1, mb) }),
            stub("SLVolume:GetMaxVolumeLevel", |this: u32, mb: *mut i16| -> u32 { call1(this, Kind::Volume, 2, mb) }),
            stub("SLVolume:SetMute", |this: u32, mute: u32| -> u32 { call1(this, Kind::Volume, 3, (mute != 0) as u32) }),
            stub("SLVolume:GetMute", |this: u32, mute: *mut u32| -> u32 { call1(this, Kind::Volume, 4, mute) }),
            unsupported("SLVolume:EnableStereoPosition"),
            unsupported("SLVolume:IsEnabledStereoPosition"),
            unsupported("SLVolume:SetStereoPosition"),
            unsupported("SLVolume:GetStereoPosition"),
        ]);
        st(|s| s.vt = [object, engine, play, queue, volume]);
        reg("slCreateEngine", create_engine);
    }
}
