// Euclides - euclidean MIDI sequencer for MOD Dwarf
// Parameter layout shared by the engine and the DPF wrapper.
// Sequencer only: the sound module lives in the companion BlipMachine plugin.
#pragma once
#include <cstdint>

namespace euclides {

enum Voice : uint32_t { kKick = 0, kSnare, kClosedHat, kOpenHat, kZap, kNumVoices };

enum GlobalParam : uint32_t {
    kRun = 0,
    kClockSource,   // 0 = host transport, 1 = internal
    kBpm,           // internal clock only
    kResolution,    // 0 = 1/8, 1 = 1/16, 2 = 1/32
    kSwing,         // 50..75 % (MPC style)
    kMidiOut,       // on/off
    kMidiChannel,   // 1..16
    kMidiGate,      // ms
    kNumGlobals
};

// Per-voice parameters: pattern (Euclidean steps/pulses/rotation) plus Mute/Note/Level,
// the last two needed to pick the outgoing MIDI note and velocity.
enum VoiceParam : uint32_t {
    kVMute = 0,
    kVSteps,
    kVPulses,
    kVRotate,
    kVNote,
    kVLevel,        // drives MIDI velocity
    kVoiceStride
};

static constexpr uint32_t kSeqBase    = kNumGlobals;
static constexpr uint32_t kParamCount = kNumGlobals + kNumVoices * kVoiceStride;

// logical (voice, param) -> parameter index
static inline constexpr uint32_t vp(uint32_t voice, uint32_t p)
{
    return kSeqBase + voice * kVoiceStride + p;
}

// parameter index -> logical (voice, param); returns false for globals
static inline bool splitIndex(uint32_t idx, uint32_t& voice, uint32_t& p)
{
    if (idx < kSeqBase || idx >= kParamCount) return false;
    voice = (idx - kSeqBase) / kVoiceStride;
    p     = (idx - kSeqBase) % kVoiceStride;
    return true;
}

enum ParamGroup : uint32_t { kGroupGlobal = 0, kGroupSequencer, kNumGroups };

enum ParamFlags : uint32_t {
    kPFInteger = 1 << 0,
    kPFBoolean = 1 << 1,
    kPFLog     = 1 << 2,
};

struct ParamDef {
    const char* symbol;
    const char* name;
    const char* unit;
    float min, max, def;
    uint32_t flags;
    const char* const* enumLabels; // nullptr if not an enumeration
    uint32_t enumCount;
    uint32_t group;
};

static const char* const kClockLabels[] = { "Host", "Internal" };
static const char* const kResLabels[]   = { "1/8", "1/16", "1/32" };

struct VoiceDefaults {
    const char* sym;
    const char* name;
    float steps, pulses, rotate, level, note;
};

static const VoiceDefaults kVoiceDefs[kNumVoices] = {
    // sym     name     st  pu  rot  lvl    note
    { "kick",  "Kick",  16, 4,  0, 0.90f, 36 },
    { "snare", "Snare", 16, 2,  4, 0.70f, 38 },
    { "ch",    "CH",    16, 8,  0, 0.50f, 42 },
    { "oh",    "OH",    16, 2,  1, 0.45f, 46 },
    { "zap",   "Zap",   13, 3,  2, 0.45f, 37 },
};

// Fills def for parameter index. Strings for per-voice params are written into the
// caller-provided buffers (symBuf/nameBuf must be >= 32 chars).
static inline void getParamDef(uint32_t idx, ParamDef& d, char* symBuf, char* nameBuf)
{
    d = ParamDef{ "", "", "", 0.f, 1.f, 0.f, 0, nullptr, 0, kGroupGlobal };

    switch (idx)
    {
    case kRun:         d = { "run", "Run", "", 0, 1, 1, kPFBoolean | kPFInteger, nullptr, 0, kGroupGlobal }; return;
    case kClockSource: d = { "clock", "Clock", "", 0, 1, 0, kPFInteger, kClockLabels, 2, kGroupGlobal }; return;
    case kBpm:         d = { "bpm", "BPM", "bpm", 40, 300, 120, 0, nullptr, 0, kGroupGlobal }; return;
    case kResolution:  d = { "resolution", "Resolution", "", 0, 2, 1, kPFInteger, kResLabels, 3, kGroupGlobal }; return;
    case kSwing:       d = { "swing", "Swing", "%", 50, 75, 50, 0, nullptr, 0, kGroupGlobal }; return;
    case kMidiOut:     d = { "midi_out", "MIDI Out", "", 0, 1, 1, kPFBoolean | kPFInteger, nullptr, 0, kGroupGlobal }; return;
    case kMidiChannel: d = { "midi_channel", "MIDI Channel", "", 1, 16, 10, kPFInteger, nullptr, 0, kGroupGlobal }; return;
    case kMidiGate:    d = { "midi_gate", "MIDI Gate", "ms", 5, 500, 50, 0, nullptr, 0, kGroupGlobal }; return;
    default: break;
    }

    uint32_t v, p;
    if (!splitIndex(idx, v, p))
        return;
    d.group = kGroupSequencer;
    const VoiceDefaults& vd = kVoiceDefs[v];

    auto set = [&](const char* s, const char* n) {
        int i = 0;
        for (const char* c = vd.sym; *c && i < 15; ++c) symBuf[i++] = *c;
        symBuf[i++] = '_';
        for (const char* c = s; *c && i < 31; ++c) symBuf[i++] = *c;
        symBuf[i] = 0;
        i = 0;
        for (const char* c = vd.name; *c && i < 15; ++c) nameBuf[i++] = *c;
        nameBuf[i++] = ' ';
        for (const char* c = n; *c && i < 31; ++c) nameBuf[i++] = *c;
        nameBuf[i] = 0;
        d.symbol = symBuf;
        d.name = nameBuf;
    };

    switch (p)
    {
    case kVMute:   set("mute", "Mute");     d.min = 0; d.max = 1;  d.def = 0; d.flags = kPFBoolean | kPFInteger; break;
    case kVSteps:  set("steps", "Steps");   d.min = 1; d.max = 32; d.def = vd.steps;  d.flags = kPFInteger; break;
    case kVPulses: set("pulses", "Pulses"); d.min = 0; d.max = 32; d.def = vd.pulses; d.flags = kPFInteger; break;
    case kVRotate: set("rotate", "Rotate"); d.min = 0; d.max = 31; d.def = vd.rotate; d.flags = kPFInteger; break;
    case kVNote:   set("note", "Note");     d.min = 0; d.max = 127; d.def = vd.note; d.flags = kPFInteger; break;
    case kVLevel:  set("level", "Level");   d.min = 0; d.max = 1;  d.def = vd.level; break;
    default: break;
    }
}

} // namespace euclides
