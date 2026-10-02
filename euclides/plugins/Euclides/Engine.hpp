// Euclides - engine: clock, euclidean sequencer, MIDI out. No audio: the sound
// module lives in the companion BlipMachine plugin.
// Framework-independent: the DPF wrapper and the offline tool both use it.
#pragma once
#include "Params.hpp"
#include <cmath>

namespace euclides {

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

struct MidiSink {
    virtual ~MidiSink() {}
    virtual void midi(uint32_t frame, uint8_t b0, uint8_t b1, uint8_t b2) = 0;
};

struct HostTransport {
    bool   valid = false;       // host provides any transport info
    bool   playing = false;
    bool   bbtValid = false;
    double beatPos = 0.0;       // absolute position in host beats (bbt)
    double framePos = 0.0;      // fallback when bbt is not valid
    double bpm = 120.0;
    float  beatType = 4.f;
};

// Euclidean hit test, equivalent to Bjorklund up to rotation.
static inline bool euclidHit(int64_t step, int steps, int pulses, int rotate)
{
    if (steps < 1 || pulses <= 0) return false;
    if (pulses >= steps) return true;
    int64_t i = (step + rotate) % steps;
    if (i < 0) i += steps;
    return (i * pulses) % steps < pulses;
}

class Engine {
public:
    float params[kParamCount];

    Engine()
    {
        char s[32], n[32];
        for (uint32_t i = 0; i < kParamCount; ++i) { ParamDef d; getParamDef(i, d, s, n); params[i] = d.def; }
    }

    void init(double sampleRate) { sr = (float)sampleRate; }

    void setParam(uint32_t idx, float v) { if (idx < kParamCount) params[idx] = v; }

    // Called from the audio thread on deactivate / transport stop.
    void allNotesOff(MidiSink& m, uint32_t frame)
    {
        for (uint32_t v = 0; v < kNumVoices; ++v)
            if (pendingOff[v] >= 0) { m.midi(frame, 0x80 | pendingCh[v], pendingNote[v], 0); pendingOff[v] = -1; }
        if (midiEverSent) {
            const uint8_t ch = (uint8_t)(clampi((int)params[kMidiChannel], 1, 16) - 1);
            m.midi(frame, 0xB0 | ch, 123, 0);
        }
    }

    void process(uint32_t frames, const HostTransport& host, MidiSink& midi)
    {
        const bool run = params[kRun] > 0.5f;
        const int  source = (int)(params[kClockSource] + 0.5f);
        const bool useHost = (source == 0);

        const int res = clampi((int)(params[kResolution] + 0.5f), 0, 2);
        const double stepsPerWhole = res == 0 ? 8.0 : (res == 1 ? 16.0 : 32.0);

        double bpm, stepsPerBeat;
        bool playing;
        if (useHost) {
            playing = run && host.valid && host.playing;
            bpm = host.bpm > 1.0 ? host.bpm : (double)params[kBpm];
            stepsPerBeat = stepsPerWhole / (host.bbtValid && host.beatType > 0.f ? host.beatType : 4.0);
        } else {
            playing = run;
            bpm = params[kBpm];
            stepsPerBeat = stepsPerWhole / 4.0;
        }
        const double inc = bpm / 60.0 * stepsPerBeat / sr;   // steps per sample
        swingOffset = clampf((params[kSwing] - 50.f) / 25.f, 0.f, 1.f) * 0.5;

        if (source != lastSource) { wasPlaying = false; lastSource = source; }

        if (!playing) {
            if (wasPlaying) allNotesOff(midi, 0);
            wasPlaying = false;
            return;
        }

        if (useHost) {
            const double hostPos = host.bbtValid ? host.beatPos * stepsPerBeat
                                                 : host.framePos * inc;
            if (!wasPlaying || hostPos < pos - 0.01 || hostPos > pos + 1.0) resync(hostPos);
            else pos = hostPos; // follow host, prevents drift
        } else if (!wasPlaying) {
            resync(0.0);
        }
        wasPlaying = true;

        const bool midiOn = params[kMidiOut] > 0.5f;
        const uint8_t ch = (uint8_t)(clampi((int)params[kMidiChannel], 1, 16) - 1);
        const int32_t gate = (int32_t)(clampf(params[kMidiGate], 1.f, 2000.f) * 0.001f * sr);

        for (uint32_t i = 0; i < frames; ++i) {
            // note-offs first, so a same-note retrigger on this frame works
            for (uint32_t v = 0; v < kNumVoices; ++v)
                if (pendingOff[v] >= 0 && --pendingOff[v] <= 0) {
                    midi.midi(i, 0x80 | pendingCh[v], pendingNote[v], 0);
                    pendingOff[v] = -1;
                }

            while (pos >= boundary(nextStep)) {
                triggerStep(nextStep, i, midiOn, ch, gate, midi);
                ++nextStep;
            }

            pos += inc;
        }
    }

    int64_t currentStep() const { return nextStep - 1; }

private:
    float sr = 48000.f;

    double pos = 0.0;          // position in steps
    int64_t nextStep = 0;
    double swingOffset = 0.0;
    bool wasPlaying = false;
    int lastSource = -1;

    int32_t pendingOff[kNumVoices] = { -1, -1, -1, -1, -1 };
    uint8_t pendingNote[kNumVoices] = {};
    uint8_t pendingCh[kNumVoices] = {};
    bool midiEverSent = false;

    static int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
    float P(uint32_t v, uint32_t p) const { return params[vp(v, p)]; }

    double boundary(int64_t n) const { return (double)n + ((n & 1) ? swingOffset : 0.0); }

    void resync(double p)
    {
        pos = p;
        nextStep = (int64_t)std::ceil(p - 1e-9);
    }

    void triggerStep(int64_t step, uint32_t frame, bool midiOn, uint8_t chan, int32_t gate, MidiSink& midi)
    {
        if (!midiOn) return;
        for (uint32_t v = 0; v < kNumVoices; ++v) {
            if (P(v, kVMute) > 0.5f) continue;
            const int steps  = clampi((int)P(v, kVSteps), 1, 32);
            const int pulses = clampi((int)P(v, kVPulses), 0, steps);
            const int rot    = clampi((int)P(v, kVRotate), 0, 31);
            if (!euclidHit(step, steps, pulses, rot)) continue;

            if (pendingOff[v] >= 0) midi.midi(frame, 0x80 | pendingCh[v], pendingNote[v], 0);
            const uint8_t note = (uint8_t)clampi((int)P(v, kVNote), 0, 127);
            const uint8_t vel  = (uint8_t)clampi(1 + (int)std::lround(P(v, kVLevel) * 126.f), 1, 127);
            midi.midi(frame, 0x90 | chan, note, vel);
            pendingNote[v] = note; pendingCh[v] = chan; pendingOff[v] = gate > 0 ? gate : 1;
            midiEverSent = true;
        }
    }
};

} // namespace euclides
