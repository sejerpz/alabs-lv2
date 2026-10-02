// Offline sequencer run: simulates a host transport and prints the MIDI it would send.
// No audio: Euclides only sequences now, see the companion BlipMachine plugin for sound.
// Usage: sequence [bars] [bpm] [key=value ...]
//   key = parameter symbol (e.g. kick_pulses=5 swing=62)
#include "Engine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace euclides;

struct PrintSink : MidiSink {
    uint64_t base = 0; int count = 0; int printed = 0;
    void midi(uint32_t frame, uint8_t b0, uint8_t b1, uint8_t b2) override
    {
        ++count;
        if (printed < (getenv("ALLMIDI") ? 1000000 : 48)) { std::printf("midi @%8llu  %02X %3u %3u\n", (unsigned long long)(base + frame), b0, b1, b2); ++printed; }
    }
};

int main(int argc, char** argv)
{
    const int bars = argc > 1 ? std::atoi(argv[1]) : 4;
    const double bpm = argc > 2 ? std::atof(argv[2]) : 120.0;
    const int sr = 48000, block = 128;

    Engine e;
    e.init(sr);
    for (int a = 3; a < argc; ++a) {
        const char* eq = std::strchr(argv[a], '=');
        if (!eq) continue;
        std::string key(argv[a], eq - argv[a]);
        char s[32], n[32]; ParamDef d;
        for (uint32_t i = 0; i < kParamCount; ++i) { getParamDef(i, d, s, n); if (key == d.symbol) e.setParam(i, (float)std::atof(eq + 1)); }
    }

    const uint64_t total = (uint64_t)(bars * 4 * 60.0 / bpm * sr) + sr; // + 1 s tail
    const uint64_t playEnd = total - sr;
    PrintSink sink;
    for (uint64_t f = 0; f < total; f += block) {
        HostTransport h;
        h.valid = true; h.playing = f < playEnd; h.bbtValid = true; h.bpm = bpm; h.beatType = 4;
        h.beatPos = (double)f / sr * bpm / 60.0;
        sink.base = f;
        e.process(block, h, sink);
    }
    std::printf("%.2fs  midi events %d\n", (double)total / sr, sink.count);
    return 0;
}
