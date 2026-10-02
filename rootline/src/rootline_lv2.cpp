// Rootline — LV2 wrapper
#include "rootline_dsp.hpp"

#include <lv2/core/lv2.h>
#include <cstdlib>
#include <new>

#define ROOTLINE_URI "urn:alabs:rootline"

// Prebuilt aarch64 binaries: bind expf/exp2f to the baseline glibc version so
// the .so loads on older device root filesystems (host toolchains export
// the GLIBC_2.27 variants). Not needed when building with mod-plugin-builder.
#if defined(ROOTLINE_OLD_GLIBC) && defined(__aarch64__)
__asm__(".symver expf,expf@GLIBC_2.17");
__asm__(".symver exp2f,exp2f@GLIBC_2.17");
#endif

namespace {

enum Port : uint32_t {
    P_IN_TRACK = 0,
    P_IN_DRY,
    P_OUT,
    P_INPUT_MODE,
    P_LOW_LATENCY,
    P_CUTOFF_NOTE,
    P_THRESHOLD,
    P_SYNTH_LEVEL,
    P_SYNTH_TONE,
    P_OCT_LEVEL,
    P_OCT_GRIT,
    P_ATTACK,
    P_RELEASE,
    P_DRY_LEVEL,
    P_INSTRUMENT,
    P_TUNING,
    P_BASS_OUT,        // audio output
    P_MODE,
    P_MELODY_SENS,
    P_UNISON,
    P_UNISON_OCT,
    P_MAIN_OUT,
    P_BASS_OUT_LEVEL,
    P_CONTEXT,         // control output
    P_COUNT
};

struct Plugin {
    rootline::Engine engine;
    const float* in[2] = {nullptr, nullptr};
    float* out = nullptr;
    float* bassOut = nullptr;
    float* context = nullptr;
    const float* ctl[P_COUNT] = {};
};

inline float ctlValue(const Plugin* p, uint32_t port, float def) {
    return p->ctl[port] ? *p->ctl[port] : def;
}

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*) {
    // malloc + placement new: no dependency on the C++ runtime (libstdc++)
    void* mem = std::malloc(sizeof(Plugin));
    if (!mem) return nullptr;
    Plugin* p = new (mem) Plugin();
    p->engine.init(rate);
    return p;
}

void connect_port(LV2_Handle h, uint32_t port, void* data) {
    Plugin* p = static_cast<Plugin*>(h);
    switch (port) {
        case P_IN_TRACK: p->in[0] = static_cast<const float*>(data); break;
        case P_IN_DRY:   p->in[1] = static_cast<const float*>(data); break;
        case P_OUT:      p->out   = static_cast<float*>(data); break;
        case P_BASS_OUT: p->bassOut = static_cast<float*>(data); break;
        case P_CONTEXT:  p->context = static_cast<float*>(data); break;
        default:
            if (port < P_COUNT) p->ctl[port] = static_cast<const float*>(data);
            break;
    }
}

void activate(LV2_Handle h) { static_cast<Plugin*>(h)->engine.reset(); }

void run(LV2_Handle h, uint32_t n) {
    Plugin* p = static_cast<Plugin*>(h);
    if (!p->in[0] || !p->out) return;

    rootline::Params prm;
    prm.dualInput   = ctlValue(p, P_INPUT_MODE, 0.f) > 0.5f && p->in[1] != nullptr;
    prm.lowLatency  = ctlValue(p, P_LOW_LATENCY, 1.f) > 0.5f;
    prm.cutoffNote  = ctlValue(p, P_CUTOFF_NOTE, 50.f);
    prm.thresholdDb = ctlValue(p, P_THRESHOLD, -50.f);
    prm.synthDb     = ctlValue(p, P_SYNTH_LEVEL, 0.f);
    prm.synthTone   = ctlValue(p, P_SYNTH_TONE, 0.3f);
    prm.octDb       = ctlValue(p, P_OCT_LEVEL, -60.f);
    prm.octGrit     = ctlValue(p, P_OCT_GRIT, 0.5f);
    prm.attackMs    = ctlValue(p, P_ATTACK, 3.f);
    prm.releaseMs   = ctlValue(p, P_RELEASE, 150.f);
    prm.dryDb       = ctlValue(p, P_DRY_LEVEL, 0.f);
    prm.instrument  = (int)(ctlValue(p, P_INSTRUMENT, 0.f) + 0.5f);
    prm.tuning      = (int)(ctlValue(p, P_TUNING, 2.f) + 0.5f);
    prm.mode        = (int)(ctlValue(p, P_MODE, 0.f) + 0.5f);
    prm.melodySens  = ctlValue(p, P_MELODY_SENS, 7.f);
    prm.unison      = ctlValue(p, P_UNISON, 0.f) > 0.5f;
    prm.unisonOct   = (int)(ctlValue(p, P_UNISON_OCT, 2.f) + 0.5f);
    prm.mainGuitarOnly = ctlValue(p, P_MAIN_OUT, 0.f) > 0.5f;
    prm.bassOutDb   = ctlValue(p, P_BASS_OUT_LEVEL, 0.f);
    p->engine.setParams(prm);

    // in mono mode in[1] may be unconnected: pass in[0] for both
    const float* dry = prm.dualInput ? p->in[1] : p->in[0];
    p->engine.process(p->in[0], dry, p->out, p->bassOut, n);
    if (p->context) *p->context = (float)p->engine.context();
}

void cleanup(LV2_Handle h) {
    Plugin* p = static_cast<Plugin*>(h);
    p->~Plugin();
    std::free(p);
}

const void* extension_data(const char*) { return nullptr; }

const LV2_Descriptor descriptor = {
    ROOTLINE_URI, instantiate, connect_port, activate, run, nullptr, cleanup, extension_data
};

}  // namespace

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : nullptr;
}
