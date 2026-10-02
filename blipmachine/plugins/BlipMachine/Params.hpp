// BlipMachine - synthesized drum/percussion sound module for MOD Dwarf
// Parameter layout shared by the DSP engine and the DPF wrapper.
// MIDI-triggered: pairs naturally with Euclides (same default note numbers)
// but responds to any MIDI source.
#pragma once
#include <cstdint>

namespace blipmachine {

enum Voice : uint32_t { kKick = 0, kSnare, kClosedHat, kOpenHat, kZap, kNumVoices };

enum GlobalParam : uint32_t {
    kMaster = 0,    // dB
    kNumGlobals
};

enum VoiceParam : uint32_t {
    kVMute = 0,
    kVNote,         // incoming MIDI note that triggers this voice
    kVLevel,        // scales with incoming velocity
    kVTune,
    kVDecay,
    kVTone,
    kVMod,
    kVPan,
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

enum ParamGroup : uint32_t { kGroupGlobal = 0, kGroupVoice, kNumGroups };

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

struct VoiceDefaults {
    const char* sym;
    const char* name;
    float note, level;
    // tune
    const char* tuneUnit; float tuneMin, tuneMax, tuneDef; bool tuneLog;
    // decay (ms)
    float decMin, decMax, decDef;
    // tone
    const char* toneName; const char* toneUnit; float toneMin, toneMax, toneDef; bool toneLog;
    // mod
    const char* modName; const char* modUnit; float modMin, modMax, modDef;
    float pan;
};

static const VoiceDefaults kVoiceDefs[kNumVoices] = {
    // sym     name     note  lvl   tune                           decay              tone                                   mod                                 pan
    { "kick",  "Kick",  36, 0.90f, "Hz", 30.f, 120.f, 45.f, true,  50.f, 2000.f, 400.f, "Cutoff", "Hz", 100.f, 10000.f, 1200.f, true,  "Sweep",  "st", 0.f, 48.f, 30.f,  0.0f },
    { "snare", "Snare", 38, 0.70f, "Hz", 80.f, 400.f, 180.f, true, 30.f, 1000.f, 180.f, "Noise",  "Hz", 500.f, 10000.f, 3000.f, true,  "Snappy", "",   0.f, 1.f,  0.6f,  0.0f },
    { "ch",    "CH",    42, 0.50f, "st", -24.f, 24.f, 0.f, false,  10.f, 300.f,  45.f,  "HPF",    "Hz", 2000.f, 14000.f, 7000.f, true, "Noise",  "",   0.f, 1.f,  0.2f,  0.2f },
    { "oh",    "OH",    46, 0.45f, "st", -24.f, 24.f, 0.f, false,  50.f, 2000.f, 350.f, "HPF",    "Hz", 2000.f, 14000.f, 6500.f, true, "Noise",  "",   0.f, 1.f,  0.2f, -0.2f },
    { "zap",   "Zap",   37, 0.45f, "Hz", 40.f, 2000.f, 120.f, true, 20.f, 1000.f, 120.f, "Width",  "",   0.05f, 0.5f, 0.5f, false, "Sweep",  "oct",0.f, 7.f,  4.f,   0.0f },
};

// Fills def for parameter index. Strings for per-voice params are written into the
// caller-provided buffers (symBuf/nameBuf must be >= 32 chars).
static inline void getParamDef(uint32_t idx, ParamDef& d, char* symBuf, char* nameBuf)
{
    d = ParamDef{ "", "", "", 0.f, 1.f, 0.f, 0, nullptr, 0, kGroupGlobal };

    switch (idx)
    {
    case kMaster: d = { "master", "Master", "dB", -40, 6, -6, 0, nullptr, 0, kGroupGlobal }; return;
    default: break;
    }

    uint32_t v, p;
    if (!splitIndex(idx, v, p))
        return;
    d.group = kGroupVoice;
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
    case kVNote:   set("note", "Note");     d.min = 0; d.max = 127; d.def = vd.note; d.flags = kPFInteger; break;
    case kVLevel:  set("level", "Level");   d.min = 0; d.max = 1;  d.def = vd.level; break;
    case kVTune:   set("tune", "Tune");     d.unit = vd.tuneUnit; d.min = vd.tuneMin; d.max = vd.tuneMax; d.def = vd.tuneDef; d.flags = vd.tuneLog ? (uint32_t)kPFLog : 0u; break;
    case kVDecay:  set("decay", "Decay");   d.unit = "ms"; d.min = vd.decMin; d.max = vd.decMax; d.def = vd.decDef; d.flags = kPFLog; break;
    case kVTone:   set("tone", vd.toneName);d.unit = vd.toneUnit; d.min = vd.toneMin; d.max = vd.toneMax; d.def = vd.toneDef; d.flags = vd.toneLog ? (uint32_t)kPFLog : 0u; break;
    case kVMod:    set("mod", vd.modName);  d.unit = vd.modUnit; d.min = vd.modMin; d.max = vd.modMax; d.def = vd.modDef; break;
    case kVPan:    set("pan", "Pan");       d.min = -1; d.max = 1; d.def = vd.pan; break;
    default: break;
    }
}

} // namespace blipmachine
