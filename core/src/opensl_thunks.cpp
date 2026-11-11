// OpenSL ES (native audio). The guest gets its own objects and interfaces whose
// method tables point at host stubs; each one wraps the real 64-bit OpenSL ES
// object. Buffer queue callbacks run the guest's callback on the audio thread.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "thunk.h"

#ifdef __ANDROID__
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace a64b {

namespace {

enum Kind { kObject, kEngine, kPlay, kQueue, kVolume };

struct Wrap {
    Kind kind;
    u32 guest = 0;          // guest address of the interface (a pointer to the method table)
    void* host = nullptr;   // SLObjectItf / SLEngineItf / ...
    Wrap* owner = nullptr;  // object an interface belongs to
    std::map<u32, Wrap*> itfs;  // interfaces handed out by this object, by guest IID
    u32 cb = 0, cb_ctx = 0;     // guest buffer queue callback
};

std::mutex g_mu;
std::unordered_map<u32, Wrap*> g_wraps;
std::unordered_map<u32, SLInterfaceID> g_iids;  // guest IID pointer -> host IID
u32 g_vt[5];
u32 g_iid_engine, g_iid_play, g_iid_bq, g_iid_asbq, g_iid_volume;

Wrap* find(u32 guest) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_wraps.find(guest);
    return it == g_wraps.end() ? nullptr : it->second;
}

Wrap* make(Kind kind, void* host, Wrap* owner) {
    auto* w = new Wrap();
    w->kind = kind;
    w->host = host;
    w->owner = owner;
    w->guest = g_calloc(1, 8);
    *g2h<u32>(w->guest) = g_vt[kind];
    std::lock_guard<std::mutex> lk(g_mu);
    g_wraps[w->guest] = w;
    return w;
}

void drop(Wrap* w) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_wraps.erase(w->guest);
    }
    g_free(w->guest);
    delete w;
}

template <class R, class... A>
u32 stub(const char* name, R (*fn)(A...)) {
    return trap_create(name, &Invoker<LibcPolicy, R, A...>::call, reinterpret_cast<void*>(fn));
}

u32 unsupported(const char* name) {
    return trap_create(name, [](Cpu& c, void* ud) {
        LOGW("OpenSL ES: %s is not bridged", static_cast<const char*>(ud));
        c.set_ret32(SL_RESULT_FEATURE_UNSUPPORTED);
    }, strdup(name));
}

u32 ok_stub(const char* name) {
    return trap_create(name, [](Cpu& c, void*) { c.set_ret32(SL_RESULT_SUCCESS); }, nullptr);
}

u32 export_iid(const char* name, SLInterfaceID host) {
    u32 guid = g_calloc(1, 16);
    memcpy(g2h(guid), host, 16);
    u32 var = g_calloc(1, 4);
    *g2h<u32>(var) = guid;
    thunk_register_data(name, var);
    g_iids[guid] = host;
    return guid;
}

SLObjectItf obj(u32 self) {
    Wrap* w = find(self);
    return w && w->kind == kObject ? static_cast<SLObjectItf>(w->host) : nullptr;
}

// Host buffer queue callback (audio thread) -> guest callback(queue interface, context).
void queue_callback(SLAndroidSimpleBufferQueueItf, void* ctx) {
    auto* w = static_cast<Wrap*>(ctx);
    if (w->cb) Cpu::current().call(w->cb, {w->guest, w->cb_ctx});
}

// ---- SLObjectItf ----
u32 object_realize(u32 self, u32) {
    SLObjectItf o = obj(self);
    return o ? (*o)->Realize(o, SL_BOOLEAN_FALSE) : SL_RESULT_PARAMETER_INVALID;
}
u32 object_resume(u32 self, u32) {
    SLObjectItf o = obj(self);
    return o ? (*o)->Resume(o, SL_BOOLEAN_FALSE) : SL_RESULT_PARAMETER_INVALID;
}
u32 object_get_state(u32 self, SLuint32* state) {
    SLObjectItf o = obj(self);
    return o ? (*o)->GetState(o, state) : SL_RESULT_PARAMETER_INVALID;
}
u32 object_get_interface(u32 self, u32 iid, u32* out) {
    Wrap* w = find(self);
    if (!w || w->kind != kObject || !out) return SL_RESULT_PARAMETER_INVALID;
    *out = 0;
    auto known = w->itfs.find(iid);
    if (known != w->itfs.end()) {
        *out = known->second->guest;
        return SL_RESULT_SUCCESS;
    }
    Kind kind;
    if (iid == g_iid_engine) kind = kEngine;
    else if (iid == g_iid_play) kind = kPlay;
    else if (iid == g_iid_bq || iid == g_iid_asbq) kind = kQueue;
    else if (iid == g_iid_volume) kind = kVolume;
    else {
        LOGW("OpenSL ES: GetInterface for an interface that is not bridged");
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    SLObjectItf o = static_cast<SLObjectItf>(w->host);
    void* itf = nullptr;
    SLresult r = (*o)->GetInterface(o, g_iids[iid], &itf);
    if (r != SL_RESULT_SUCCESS) return r;
    Wrap* iw = make(kind, itf, w);
    w->itfs[iid] = iw;
    *out = iw->guest;
    return SL_RESULT_SUCCESS;
}
void object_destroy(u32 self) {
    Wrap* w = find(self);
    if (!w || w->kind != kObject) return;
    SLObjectItf o = static_cast<SLObjectItf>(w->host);
    (*o)->Destroy(o);  // waits for running callbacks
    for (auto& kv : w->itfs) drop(kv.second);
    drop(w);
}

// ---- SLEngineItf ----
SLEngineItf engine(u32 self) {
    Wrap* w = find(self);
    return w && w->kind == kEngine ? static_cast<SLEngineItf>(w->host) : nullptr;
}

u32 engine_create_output_mix(u32 self, u32* out, u32, u32, u32) {
    SLEngineItf e = engine(self);
    if (!e || !out) return SL_RESULT_PARAMETER_INVALID;
    SLObjectItf mix = nullptr;
    SLresult r = (*e)->CreateOutputMix(e, &mix, 0, nullptr, nullptr);
    *out = r == SL_RESULT_SUCCESS ? make(kObject, (void*)mix, nullptr)->guest : 0;
    return r;
}

u32 engine_create_audio_player(u32 self, u32* out, const u32* src, const u32* snk, u32 n, const u32* ids, const SLboolean* req) {
    SLEngineItf e = engine(self);
    if (!e || !out || !src || !snk) return SL_RESULT_PARAMETER_INVALID;
    *out = 0;
    // Source: only PCM buffer queues (what game sound engines use) are bridged.
    const u32* loc = g2h<u32>(src[0]);
    const u32* fmt = g2h<u32>(src[1]);
    if (!src[0] || !src[1] || (loc[0] != SL_DATALOCATOR_BUFFERQUEUE && loc[0] != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE) ||
        fmt[0] != SL_DATAFORMAT_PCM) {
        LOGW("OpenSL ES: audio player with locator 0x%x / format 0x%x is not bridged", src[0] ? loc[0] : 0, src[1] ? fmt[0] : 0);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    SLDataLocator_AndroidSimpleBufferQueue hloc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, loc[1]};
    SLDataFormat_PCM hfmt = {fmt[0], fmt[1], fmt[2], fmt[3], fmt[4], fmt[5], fmt[6]};
    SLDataSource hsrc = {&hloc, &hfmt};
    // Sink: { SL_DATALOCATOR_OUTPUTMIX, output mix object }
    const u32* sloc = g2h<u32>(snk[0]);
    SLObjectItf mix = snk[0] ? obj(sloc[1]) : nullptr;
    if (!mix) return SL_RESULT_PARAMETER_INVALID;
    SLDataLocator_OutputMix hsloc = {SL_DATALOCATOR_OUTPUTMIX, mix};
    SLDataSink hsnk = {&hsloc, nullptr};

    std::vector<SLInterfaceID> hids;
    std::vector<SLboolean> hreq;
    for (u32 i = 0; i < n && ids; i++) {
        u32 g = ids[i];
        if (g != g_iid_play && g != g_iid_bq && g != g_iid_asbq && g != g_iid_volume) continue;
        // A buffer queue source is driven through the Android simple buffer queue interface.
        hids.push_back(g == g_iid_bq ? SL_IID_ANDROIDSIMPLEBUFFERQUEUE : g_iids[g]);
        hreq.push_back(req ? req[i] : SL_BOOLEAN_TRUE);
    }
    SLObjectItf player = nullptr;
    SLresult r = (*e)->CreateAudioPlayer(e, &player, &hsrc, &hsnk, (SLuint32)hids.size(), hids.data(), hreq.data());
    LOGI("OpenSL ES: audio player %u ch, %u Hz, %u bit -> %s", fmt[1], fmt[2] / 1000, fmt[3], r == SL_RESULT_SUCCESS ? "ok" : "failed");
    if (r == SL_RESULT_SUCCESS) *out = make(kObject, (void*)player, nullptr)->guest;
    return r;
}

// ---- SLPlayItf ----
SLPlayItf play(u32 self) {
    Wrap* w = find(self);
    return w && w->kind == kPlay ? static_cast<SLPlayItf>(w->host) : nullptr;
}
u32 play_set_state(u32 self, u32 state) { SLPlayItf p = play(self); return p ? (*p)->SetPlayState(p, state) : SL_RESULT_PARAMETER_INVALID; }
u32 play_get_state(u32 self, SLuint32* state) { SLPlayItf p = play(self); return p ? (*p)->GetPlayState(p, state) : SL_RESULT_PARAMETER_INVALID; }
u32 play_get_duration(u32 self, SLmillisecond* ms) { SLPlayItf p = play(self); return p ? (*p)->GetDuration(p, ms) : SL_RESULT_PARAMETER_INVALID; }
u32 play_get_position(u32 self, SLmillisecond* ms) { SLPlayItf p = play(self); return p ? (*p)->GetPosition(p, ms) : SL_RESULT_PARAMETER_INVALID; }

// ---- SLBufferQueueItf / SLAndroidSimpleBufferQueueItf ----
SLAndroidSimpleBufferQueueItf queue(u32 self) {
    Wrap* w = find(self);
    return w && w->kind == kQueue ? static_cast<SLAndroidSimpleBufferQueueItf>(w->host) : nullptr;
}
u32 queue_enqueue(u32 self, const void* buf, u32 size) { auto q = queue(self); return q ? (*q)->Enqueue(q, buf, size) : SL_RESULT_PARAMETER_INVALID; }
u32 queue_clear(u32 self) { auto q = queue(self); return q ? (*q)->Clear(q) : SL_RESULT_PARAMETER_INVALID; }
u32 queue_get_state(u32 self, SLAndroidSimpleBufferQueueState* st) { auto q = queue(self); return q ? (*q)->GetState(q, st) : SL_RESULT_PARAMETER_INVALID; }
u32 queue_register_callback(u32 self, u32 cb, u32 ctx) {
    Wrap* w = find(self);
    if (!w || w->kind != kQueue) return SL_RESULT_PARAMETER_INVALID;
    auto q = static_cast<SLAndroidSimpleBufferQueueItf>(w->host);
    w->cb = cb;
    w->cb_ctx = ctx;
    return (*q)->RegisterCallback(q, cb ? queue_callback : nullptr, w);
}

// ---- SLVolumeItf ----
SLVolumeItf volume(u32 self) {
    Wrap* w = find(self);
    return w && w->kind == kVolume ? static_cast<SLVolumeItf>(w->host) : nullptr;
}
u32 volume_set(u32 self, i32 mb) { SLVolumeItf v = volume(self); return v ? (*v)->SetVolumeLevel(v, (SLmillibel)mb) : SL_RESULT_PARAMETER_INVALID; }
u32 volume_get(u32 self, SLmillibel* mb) { SLVolumeItf v = volume(self); return v ? (*v)->GetVolumeLevel(v, mb) : SL_RESULT_PARAMETER_INVALID; }
u32 volume_max(u32 self, SLmillibel* mb) { SLVolumeItf v = volume(self); return v ? (*v)->GetMaxVolumeLevel(v, mb) : SL_RESULT_PARAMETER_INVALID; }
u32 volume_set_mute(u32 self, u32 mute) { SLVolumeItf v = volume(self); return v ? (*v)->SetMute(v, mute ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE) : SL_RESULT_PARAMETER_INVALID; }
u32 volume_get_mute(u32 self, SLboolean* mute) { SLVolumeItf v = volume(self); return v ? (*v)->GetMute(v, mute) : SL_RESULT_PARAMETER_INVALID; }

u32 create_engine(u32* out, u32, u32, u32, u32, u32) {
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    SLObjectItf e = nullptr;
    SLresult r = slCreateEngine(&e, 0, nullptr, 0, nullptr, nullptr);
    LOGI("OpenSL ES: engine %s", r == SL_RESULT_SUCCESS ? "created" : "could not be created");
    *out = r == SL_RESULT_SUCCESS ? make(kObject, (void*)e, nullptr)->guest : 0;
    return r;
}

u32 table(std::initializer_list<u32> entries) {
    u32 t = g_calloc((u32)entries.size(), 4);
    u32 i = 0;
    for (u32 e : entries) g2h<u32>(t)[i++] = e;
    return t;
}

}  // namespace

void register_opensl_thunks() {
    g_iid_engine = export_iid("SL_IID_ENGINE", SL_IID_ENGINE);
    g_iid_play = export_iid("SL_IID_PLAY", SL_IID_PLAY);
    g_iid_bq = export_iid("SL_IID_BUFFERQUEUE", SL_IID_BUFFERQUEUE);
    g_iid_asbq = export_iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", SL_IID_ANDROIDSIMPLEBUFFERQUEUE);
    g_iid_volume = export_iid("SL_IID_VOLUME", SL_IID_VOLUME);
    // Known to exist but not bridged: GetInterface reports them as unsupported.
    export_iid("SL_IID_OUTPUTMIX", SL_IID_OUTPUTMIX);
    export_iid("SL_IID_SEEK", SL_IID_SEEK);
    export_iid("SL_IID_ENVIRONMENTALREVERB", SL_IID_ENVIRONMENTALREVERB);
    export_iid("SL_IID_EFFECTSEND", SL_IID_EFFECTSEND);
    export_iid("SL_IID_MUTESOLO", SL_IID_MUTESOLO);
    export_iid("SL_IID_PLAYBACKRATE", SL_IID_PLAYBACKRATE);

    g_vt[kObject] = table({stub("SLObject:Realize", object_realize), stub("SLObject:Resume", object_resume),
                           stub("SLObject:GetState", object_get_state), stub("SLObject:GetInterface", object_get_interface),
                           ok_stub("SLObject:RegisterCallback"), ok_stub("SLObject:AbortAsyncOperation"),
                           stub("SLObject:Destroy", object_destroy), ok_stub("SLObject:SetPriority"),
                           unsupported("SLObject:GetPriority"), ok_stub("SLObject:SetLossOfControlInterfaces")});
    g_vt[kEngine] = table({unsupported("SLEngine:CreateLEDDevice"), unsupported("SLEngine:CreateVibraDevice"),
                           stub("SLEngine:CreateAudioPlayer", engine_create_audio_player), unsupported("SLEngine:CreateAudioRecorder"),
                           unsupported("SLEngine:CreateMidiPlayer"), unsupported("SLEngine:CreateListener"),
                           unsupported("SLEngine:Create3DGroup"), stub("SLEngine:CreateOutputMix", engine_create_output_mix),
                           unsupported("SLEngine:CreateMetadataExtractor"), unsupported("SLEngine:CreateExtensionObject"),
                           unsupported("SLEngine:QueryNumSupportedInterfaces"), unsupported("SLEngine:QuerySupportedInterfaces"),
                           unsupported("SLEngine:QueryNumSupportedExtensions"), unsupported("SLEngine:QuerySupportedExtension"),
                           unsupported("SLEngine:IsExtensionSupported")});
    g_vt[kPlay] = table({stub("SLPlay:SetPlayState", play_set_state), stub("SLPlay:GetPlayState", play_get_state),
                         stub("SLPlay:GetDuration", play_get_duration), stub("SLPlay:GetPosition", play_get_position),
                         ok_stub("SLPlay:RegisterCallback"), ok_stub("SLPlay:SetCallbackEventsMask"),
                         unsupported("SLPlay:GetCallbackEventsMask"), ok_stub("SLPlay:SetMarkerPosition"),
                         ok_stub("SLPlay:ClearMarkerPosition"), unsupported("SLPlay:GetMarkerPosition"),
                         ok_stub("SLPlay:SetPositionUpdatePeriod"), unsupported("SLPlay:GetPositionUpdatePeriod")});
    g_vt[kQueue] = table({stub("SLBufferQueue:Enqueue", queue_enqueue), stub("SLBufferQueue:Clear", queue_clear),
                          stub("SLBufferQueue:GetState", queue_get_state), stub("SLBufferQueue:RegisterCallback", queue_register_callback)});
    g_vt[kVolume] = table({stub("SLVolume:SetVolumeLevel", volume_set), stub("SLVolume:GetVolumeLevel", volume_get),
                           stub("SLVolume:GetMaxVolumeLevel", volume_max), stub("SLVolume:SetMute", volume_set_mute),
                           stub("SLVolume:GetMute", volume_get_mute), unsupported("SLVolume:EnableStereoPosition"),
                           unsupported("SLVolume:IsEnabledStereoPosition"), unsupported("SLVolume:SetStereoPosition"),
                           unsupported("SLVolume:GetStereoPosition")});
    reg("slCreateEngine", create_engine);
}

}  // namespace a64b

#else

namespace a64b {
void register_opensl_thunks() {
    reg("slCreateEngine", +[](u32*, u32, u32, u32, u32, u32) -> u32 { return 12; /* SL_RESULT_FEATURE_UNSUPPORTED */ });
}
}  // namespace a64b

#endif
