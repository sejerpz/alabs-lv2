// Offline renderer: triggers one MIDI note and writes the result to a WAV.
// Usage: render out.wav [dur_ms] [note] [vel] [key=value ...]
//   key = parameter symbol (e.g. kick_tune=50 kick_decay=180)
#include "Engine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

using namespace blipmachine;

static void writeWav(const char* path, const std::vector<float>& inter, int sr)
{
    FILE* f = std::fopen(path, "wb");
    const uint32_t n = (uint32_t)inter.size(), bytes = n * 2;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(sr); u32(sr * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (float x : inter) { int v = (int)(x * 32767.f); v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v); u16((uint16_t)(int16_t)v); }
    std::fclose(f);
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: render out.wav [dur_ms] [note] [vel] [sym=val...]\n"); return 1; }
    const int dur_ms = argc > 2 ? std::atoi(argv[2]) : 500;
    const int note    = argc > 3 ? std::atoi(argv[3]) : 36;
    const int vel      = argc > 4 ? std::atoi(argv[4]) : 114;
    const int sr = 48000, block = 128;

    Engine e;
    e.init(sr);
    for (int a = 5; a < argc; ++a) {
        const char* eq = std::strchr(argv[a], '=');
        if (!eq) continue;
        std::string key(argv[a], eq - argv[a]);
        char s[32], n[32]; ParamDef d;
        for (uint32_t i = 0; i < kParamCount; ++i) { getParamDef(i, d, s, n); if (key == d.symbol) e.setParam(i, (float)std::atof(eq + 1)); }
    }

    const uint64_t total = (uint64_t)dur_ms * sr / 1000;
    std::vector<float> L(block), R(block), out;
    out.reserve(total * 2);
    bool triggered = false;
    for (uint64_t f = 0; f < total; f += block) {
        const uint32_t n = (uint32_t)std::min<uint64_t>(block, total - f);
        if (!triggered) { e.noteOn((uint8_t)note, (uint8_t)vel); triggered = true; }
        e.process(L.data(), R.data(), n);
        for (uint32_t i = 0; i < n; ++i) { out.push_back(L[i]); out.push_back(R[i]); }
    }
    writeWav(argv[1], out, sr);
    float peak = 0.f; for (float x : out) peak = std::max(peak, std::fabs(x));
    std::printf("wrote %s  %.2fs  note %d vel %d  peak %.3f\n", argv[1], (double)total / sr, note, vel, peak);
    return 0;
}
