// BlipMachine - DSP building blocks (header-only, no allocations, RT-safe)
#pragma once
#include <cmath>
#include <cstdint>

namespace blipmachine {

static constexpr float kPi = 3.14159265358979f;

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float dbToGain(float db) { return std::pow(10.f, db * 0.05f); }

// Soft clipper (rational tanh approximation), unity slope at 0, saturates at +-1.
static inline float softClip(float x)
{
    x = clampf(x, -3.f, 3.f);
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

static inline float polyBlep(float t, float dt)
{
    if (t < dt)        { t /= dt;             return t + t - t * t - 1.f; }
    if (t > 1.f - dt)  { t = (t - 1.f) / dt;  return t * t + t + t + 1.f; }
    return 0.f;
}

// Band-limited pulse oscillator (PolyBLEP). width = 0.5 -> square. DC removed.
struct PulseOsc {
    float phase = 0.f;
    void reset(float p = 0.f) { phase = p; }
    inline float process(float freq, float width, float sr)
    {
        const float dt = clampf(freq / sr, 1e-6f, 0.45f);
        const float t = phase;
        float y = (t < width) ? 1.f : -1.f;
        y += polyBlep(t, dt);
        float t2 = t + 1.f - width;
        if (t2 >= 1.f) t2 -= 1.f;
        y -= polyBlep(t2, dt);
        phase += dt;
        if (phase >= 1.f) phase -= 1.f;
        return y - (2.f * width - 1.f);
    }
};

// Zavalishin TPT state variable filter.
struct Svf {
    float ic1 = 0.f, ic2 = 0.f;
    float a1 = 0.f, a2 = 0.f, a3 = 0.f, k = 1.f;
    float low = 0.f, band = 0.f, high = 0.f;

    void reset() { ic1 = ic2 = 0.f; }
    void set(float fc, float q, float sr)
    {
        fc = clampf(fc, 10.f, sr * 0.45f);
        const float g = std::tan(kPi * fc / sr);
        k = 1.f / q;
        a1 = 1.f / (1.f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    inline void process(float x)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        low = v2; band = v1; high = x - k * v1 - v2;
    }
};

// Exponential decay envelope. time = time to reach -60 dB.
struct DecayEnv {
    float value = 0.f, coef = 0.f;
    void setTime(float seconds, float sr) { coef = std::exp(-6.9078f / (clampf(seconds, 1e-4f, 30.f) * sr)); }
    void trigger(float v = 1.f) { value = v; }
    inline float process() { const float v = value; value *= coef; if (value < 1e-5f) value = 0.f; return v; }
    bool active() const { return value > 0.f; }
};

struct Noise {
    uint32_t s = 0x12345678u;
    inline float process() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (float)(int32_t)s * (1.f / 2147483648.f); }
};

// ---------------------------------------------------------------------------
// Voices. Each voice reads its params at trigger time and once per block
// (update()), never per sample.

struct VoiceParams {
    float level, tune, decayMs, tone, mod;
};

// Square-wave kick: PolyBLEP square, exponential pitch sweep, LPF, exp amp env.
struct KickVoice {
    PulseOsc osc; Svf lpf; DecayEnv amp, pitch;
    float baseHz = 45.f, sweepSt = 30.f;
    void update(const VoiceParams& p, float sr)
    {
        baseHz = p.tune; sweepSt = p.mod;
        amp.setTime(p.decayMs * 0.001f, sr);
        pitch.setTime(0.08f, sr);
        lpf.set(p.tone, 0.707f, sr);
    }
    void trigger() { osc.reset(0.f); amp.trigger(); pitch.trigger(); }
    inline float process(float sr)
    {
        if (!amp.active()) return 0.f;
        const float pe = pitch.process();
        const float f = baseHz * std::exp2(sweepSt * pe * (1.f / 12.f));
        lpf.process(osc.process(f, 0.5f, sr));
        return lpf.low * amp.process();
    }
};

// Snare: square body with short pitch drop + band-passed noise.
struct SnareVoice {
    PulseOsc osc; Svf bodyLpf, noiseBp; DecayEnv bodyEnv, noiseEnv, pitch; Noise rng;
    float bodyHz = 180.f, snappy = 0.6f;
    void update(const VoiceParams& p, float sr)
    {
        bodyHz = p.tune; snappy = p.mod;
        bodyEnv.setTime(p.decayMs * 0.0005f, sr);
        noiseEnv.setTime(p.decayMs * 0.001f, sr);
        pitch.setTime(0.03f, sr);
        bodyLpf.set(bodyHz * 6.f, 0.707f, sr);
        noiseBp.set(p.tone, 0.8f, sr);
    }
    void trigger() { osc.reset(0.f); bodyEnv.trigger(); noiseEnv.trigger(); pitch.trigger(); }
    inline float process(float sr)
    {
        if (!bodyEnv.active() && !noiseEnv.active()) return 0.f;
        const float f = bodyHz * std::exp2(pitch.process() * 0.5f);
        bodyLpf.process(osc.process(f, 0.5f, sr));
        noiseBp.process(rng.process());
        return (1.f - snappy) * bodyLpf.low * bodyEnv.process()
             + snappy * 3.f * noiseBp.band * noiseEnv.process();
    }
};

// 808-style metallic source: 6 free-running squares at inharmonic ratios.
// Shared by closed and open hat, as on the original.
struct MetalBank {
    static constexpr int kN = 6;
    PulseOsc osc[kN];
    float ratio = 1.f;
    MetalBank() { for (int i = 0; i < kN; ++i) osc[i].reset(0.13f * i); }
    inline float process(float sr)
    {
        static const float f[kN] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };
        float s = 0.f;
        for (int i = 0; i < kN; ++i) s += osc[i].process(f[i] * ratio, 0.5f, sr);
        return s * (1.f / kN);
    }
};

struct HatVoice {
    Svf bp, hp; DecayEnv amp; Noise rng;
    float noiseMix = 0.2f, decayS = 0.05f, sr_ = 48000.f;
    bool choked = false;
    void update(const VoiceParams& p, float sr)
    {
        sr_ = sr; noiseMix = p.mod; decayS = p.decayMs * 0.001f;
        if (!choked) amp.setTime(decayS, sr);
        bp.set(p.tone * 1.25f, 1.2f, sr);
        hp.set(p.tone, 0.707f, sr);
    }
    void trigger(uint32_t seed) { choked = false; amp.setTime(decayS, sr_); amp.trigger(); rng.s ^= seed; }
    void choke() { if (amp.active()) { choked = true; amp.setTime(0.006f, sr_); } }
    inline float process(float metal)
    {
        if (!amp.active()) return 0.f;
        const float x = metal * (1.f - noiseMix) + rng.process() * noiseMix;
        bp.process(x);
        hp.process(bp.band);
        return hp.high * 7.f * amp.process();
    }
};

// Zap/blip: pulse with fast exponential downward sweep. Placeholder model,
// to be refined once the reference sample is analysed.
struct ZapVoice {
    PulseOsc osc; DecayEnv amp, pitch;
    float endHz = 120.f, sweepOct = 4.f, width = 0.5f;
    void update(const VoiceParams& p, float sr)
    {
        endHz = p.tune; sweepOct = p.mod; width = p.tone;
        amp.setTime(p.decayMs * 0.001f, sr);
        pitch.setTime(p.decayMs * 0.0004f, sr);
    }
    void trigger() { osc.reset(0.f); amp.trigger(); pitch.trigger(); }
    inline float process(float sr)
    {
        if (!amp.active()) return 0.f;
        const float f = endHz * std::exp2(sweepOct * pitch.process());
        return osc.process(f, width, sr) * amp.process();
    }
};

} // namespace blipmachine
