// Build: g++ -Idpf/distrho/src/lv2 -Iplugins/Euclides test/lv2host_test.cpp -ldl
// Minimal LV2 host for testing euclides_dsp.so on aarch64 (under qemu).
// Sends one time:Position (playing, 120 bpm, bar 0 beat 0) then runs N blocks,
// counts MIDI events on the output atom port.
// Port order (DPF, no audio ports, WANT_TIMEPOS=1, WANT_MIDI_OUTPUT=1):
//   0 = atom in (time pos), 1 = atom out (MIDI), 2.. = control params.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lv2.h"
#include "urid.h"
#include "options.h"
#include "buf-size.h"
#include "atom.h"
#include "atom-forge.h"
#include "time.h"
#include "midi.h"
#include "parameters.h"
#include "Params.hpp"

static char* uris[256]; static int nuris = 0;
static LV2_URID map(LV2_URID_Map_Handle h, const char* uri)
{
    (void)h;
    for (int i = 0; i < nuris; ++i) if (!strcmp(uris[i], uri)) return i + 1;
    uris[nuris] = strdup(uri); return ++nuris;
}

int main(int argc, char** argv)
{
    const double sr = 48000; const uint32_t block = 128;
    const int blocks = argc > 2 ? atoi(argv[2]) : 750; // ~2 s
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    const LV2_Descriptor* (*desc)(uint32_t) = (const LV2_Descriptor* (*)(uint32_t))dlsym(h, "lv2_descriptor");
    const LV2_Descriptor* d = desc(0);
    printf("URI %s\n", d->URI);

    LV2_URID_Map um = { NULL, map };
    int32_t bl = block; float srf = sr;
    LV2_Options_Option opts[] = {
        { LV2_OPTIONS_INSTANCE, 0, map(0, LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t), map(0, LV2_ATOM__Int), &bl },
        { LV2_OPTIONS_INSTANCE, 0, map(0, LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), map(0, LV2_ATOM__Int), &bl },
        { LV2_OPTIONS_INSTANCE, 0, map(0, LV2_PARAMETERS__sampleRate), sizeof(float), map(0, LV2_ATOM__Float), &srf },
        { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL }
    };
    LV2_Feature fmap = { LV2_URID__map, &um }, fopt = { LV2_OPTIONS__options, opts };
    const LV2_Feature* feats[] = { &fmap, &fopt, NULL };
    LV2_Handle inst = d->instantiate(d, sr, argv[1], feats);
    if (!inst) { printf("instantiate failed\n"); return 1; }

    static uint8_t inbuf[4096] __attribute__((aligned(8))), outbuf[8192] __attribute__((aligned(8)));
    // every control port gets its default from the shared parameter table
    static float controls[euclides::kParamCount];
    for (uint32_t i = 0; i < euclides::kParamCount; ++i) {
        char sy[32], nm[32]; euclides::ParamDef pd; euclides::getParamDef(i, pd, sy, nm); controls[i] = pd.def;
    }
    for (int a = 4; a < argc; ++a) { // sym=value overrides
        const char* eq = strchr(argv[a], '='); if (!eq) continue;
        for (uint32_t i = 0; i < euclides::kParamCount; ++i) {
            char sy[32], nm[32]; euclides::ParamDef pd; euclides::getParamDef(i, pd, sy, nm);
            if (strlen(pd.symbol) == (size_t)(eq - argv[a]) && !strncmp(pd.symbol, argv[a], eq - argv[a])) controls[i] = atof(eq + 1);
        }
    }
    if (argc > 3 && !strcmp(argv[3], "x")) controls[euclides::kClockSource] = 1; // internal clock
    d->connect_port(inst, 0, inbuf); d->connect_port(inst, 1, outbuf);
    for (uint32_t i = 0; i < euclides::kParamCount; ++i) d->connect_port(inst, 2 + i, &controls[i]);
    if (d->activate) d->activate(inst);

    LV2_URID midiEv = map(0, LV2_MIDI__MidiEvent);
    LV2_Atom_Forge forge; lv2_atom_forge_init(&forge, &um);
    int nOn = 0, nOff = 0, nCC = 0;
    for (int b = 0; b < blocks; ++b) {
        lv2_atom_forge_set_buffer(&forge, inbuf, sizeof(inbuf));
        LV2_Atom_Forge_Frame seq;
        lv2_atom_forge_sequence_head(&forge, &seq, 0);
        if (b == 0 || b == blocks - 50) { // start, then stop 50 blocks before the end
            LV2_Atom_Forge_Frame obj;
            lv2_atom_forge_frame_time(&forge, 0);
            lv2_atom_forge_object(&forge, &obj, 0, map(0, LV2_TIME__Position));
            lv2_atom_forge_key(&forge, map(0, LV2_TIME__frame));          lv2_atom_forge_long(&forge, (int64_t)b * block);
            lv2_atom_forge_key(&forge, map(0, LV2_TIME__speed));          lv2_atom_forge_float(&forge, b == 0 ? 1.f : 0.f);
            lv2_atom_forge_key(&forge, map(0, LV2_TIME__bar));            lv2_atom_forge_long(&forge, 0);
            lv2_atom_forge_key(&forge, map(0, LV2_TIME__barBeat));        lv2_atom_forge_float(&forge, 0.f);
            lv2_atom_forge_key(&forge, map(0, LV2_TIME__beatUnit));       lv2_atom_forge_int(&forge, 4);
            lv2_atom_forge_key(&forge, map(0, LV2_TIME__beatsPerBar));    lv2_atom_forge_float(&forge, 4.f);
            lv2_atom_forge_key(&forge, map(0, LV2_TIME__beatsPerMinute)); lv2_atom_forge_float(&forge, 120.f);
            lv2_atom_forge_pop(&forge, &obj);
        }
        lv2_atom_forge_pop(&forge, &seq);
        LV2_Atom_Sequence* os = (LV2_Atom_Sequence*)outbuf;
        memset(outbuf, 0, sizeof(outbuf)); os->atom.type = map(0, LV2_ATOM__Chunk); os->atom.size = sizeof(outbuf) - sizeof(LV2_Atom);

        d->run(inst, block);

        if (os->atom.type == map(0, LV2_ATOM__Sequence)) LV2_ATOM_SEQUENCE_FOREACH(os, ev) {
            if (ev->body.type != midiEv) continue;
            const uint8_t* m = (const uint8_t*)(ev + 1);
            if ((m[0] & 0xF0) == 0x90) ++nOn; else if ((m[0] & 0xF0) == 0x80) ++nOff; else if ((m[0] & 0xF0) == 0xB0) ++nCC;
            if (b < 12) printf("blk %3d frame %3lld  %02X %3u %3u\n", b, (long long)ev->time.frames, m[0], m[1], m[2]);
        }
    }
    printf("note-on %d  note-off %d  cc %d\n", nOn, nOff, nCC);
    if (d->deactivate) d->deactivate(inst);
    d->cleanup(inst);
    dlclose(h);
    printf("ok\n");
    return 0;
}
