// BlipMachine - engine: 5 synthesized voices, triggered by MIDI note number.
// Framework-independent: the DPF wrapper and the offline renderer both use it.
#pragma once
#include "Params.hpp"
#include "Dsp.hpp"
#include <cmath>

namespace blipmachine {

class Engine {
public:
    float params[kParamCount];

    Engine()
    {
        char s[32], n[32];
        for (uint32_t i = 0; i < kParamCount; ++i) { ParamDef d; getParamDef(i, d, s, n); params[i] = d.def; }
    }

    void init(double sampleRate) { sr = (float)sampleRate; updateVoices(); }

    void setParam(uint32_t idx, float v) { if (idx < kParamCount) params[idx] = v; }

    // Triggers every voice whose Note param matches, scaled by velocity (1..127).
    void noteOn(uint8_t note, uint8_t vel)
    {
        const float velGain = clampf((float)vel / 127.f, 0.f, 1.f);
        for (uint32_t v = 0; v < kNumVoices; ++v) {
            if (P(v, kVMute) > 0.5f) continue;
            if ((uint8_t)clampi((int)P(v, kVNote), 0, 127) != note) continue;
            voiceVel[v] = velGain;
            switch (v) {
            case kKick:      kick.trigger(); break;
            case kSnare:     snare.trigger(); break;
            case kClosedHat: oh.choke(); ch.trigger(seed++ * 2654435761u); break;
            case kOpenHat:   oh.trigger(seed++ * 2654435761u); break;
            case kZap:       zap.trigger(); break;
            }
        }
    }

    // One-shot voices: note-off is accepted for MIDI-input symmetry but has no effect.
    void noteOff(uint8_t) {}

    void process(float* outL, float* outR, uint32_t frames)
    {
        updateVoices();
        for (uint32_t i = 0; i < frames; ++i)
            renderSample(outL[i], outR[i]);
    }

private:
    float sr = 48000.f;

    KickVoice kick; SnareVoice snare; MetalBank metal; HatVoice ch, oh; ZapVoice zap;
    float gainL[kNumVoices] = {}, gainR[kNumVoices] = {};
    float voiceVel[kNumVoices] = { 1.f, 1.f, 1.f, 1.f, 1.f };
    float master = 0.5f;
    float muteGain[kNumVoices] = { 1.f, 1.f, 1.f, 1.f, 1.f };   // smoothed 1 = open, 0 = muted
    float muteTarget[kNumVoices] = { 1.f, 1.f, 1.f, 1.f, 1.f };
    float muteStep = 1.f / 240.f;                               // ~5 ms ramp
    uint32_t seed = 1;

    static int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
    float P(uint32_t v, uint32_t p) const { return params[vp(v, p)]; }

    VoiceParams vparams(uint32_t v) const
    {
        return { P(v, kVLevel), P(v, kVTune), P(v, kVDecay), P(v, kVTone), P(v, kVMod) };
    }

    void updateVoices()
    {
        kick.update(vparams(kKick), sr);
        snare.update(vparams(kSnare), sr);
        ch.update(vparams(kClosedHat), sr);
        oh.update(vparams(kOpenHat), sr);
        zap.update(vparams(kZap), sr);
        // hats share the metal bank; CH tune drives it (as a pair, like the 808)
        metal.ratio = std::exp2(P(kClosedHat, kVTune) / 12.f);
        for (uint32_t v = 0; v < kNumVoices; ++v) {
            const float pan = clampf(P(v, kVPan), -1.f, 1.f);
            const float a = (pan + 1.f) * kPi * 0.25f;
            const float lvl = clampf(P(v, kVLevel), 0.f, 1.f);
            gainL[v] = std::cos(a) * lvl * 1.4142f;
            gainR[v] = std::sin(a) * lvl * 1.4142f;
        }
        master = dbToGain(params[kMaster]);
        muteStep = 1.f / (0.005f * sr);
        for (uint32_t v = 0; v < kNumVoices; ++v)
            muteTarget[v] = P(v, kVMute) > 0.5f ? 0.f : 1.f;
    }

    inline void renderSample(float& L, float& R)
    {
        float s[kNumVoices];
        s[kKick]  = kick.process(sr);
        s[kSnare] = snare.process(sr);
        const float m = (ch.amp.active() || oh.amp.active()) ? metal.process(sr) : 0.f;
        s[kClosedHat] = ch.process(m);
        s[kOpenHat]   = oh.process(m);
        s[kZap]   = zap.process(sr);
        float l = 0.f, r = 0.f;
        for (uint32_t v = 0; v < kNumVoices; ++v) {
            // mute also cuts the ringing tail, with a short ramp to avoid clicks
            float& g = muteGain[v];
            if (g < muteTarget[v]) { g += muteStep; if (g > muteTarget[v]) g = muteTarget[v]; }
            else if (g > muteTarget[v]) { g -= muteStep; if (g < muteTarget[v]) g = muteTarget[v]; }
            const float x = s[v] * g * voiceVel[v];
            l += x * gainL[v]; r += x * gainR[v];
        }
        L = softClip(l * master);
        R = softClip(r * master);
    }
};

} // namespace blipmachine
