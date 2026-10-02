// Build: g++ -Idpf/distrho/src/lv2 -Iplugins/BlipMachine test/lv2host_test.cpp -ldl
// Minimal LV2 host for testing blipmachine_dsp.so on aarch64 (under qemu).
// Sends one MIDI note-on (default kick) then runs N blocks, writes audio to a raw float file.
// Port order (DPF, NUM_OUTPUTS=2, WANT_MIDI_INPUT=1, no time pos, no MIDI out):
//   0,1 = audio out L/R, 2 = atom in (MIDI), 3.. = control params.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lv2.h"
#include "urid.h"
#include "options.h"
#include "buf-size.h"
#include "atom.h"
#include "atom-forge.h"
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
    const int blocks = argc > 2 ? atoi(argv[2]) : 375; // ~1 s
    const int note = argc > 3 ? atoi(argv[3]) : 36;
    const int vel  = argc > 4 ? atoi(argv[4]) : 114;
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

    float L[128], R[128];
    static uint8_t inbuf[4096] __attribute__((aligned(8)));
    static float controls[blipmachine::kParamCount];
    for (uint32_t i = 0; i < blipmachine::kParamCount; ++i) {
        char sy[32], nm[32]; blipmachine::ParamDef pd; blipmachine::getParamDef(i, pd, sy, nm); controls[i] = pd.def;
    }
    for (int a = 5; a < argc; ++a) { // sym=value overrides
        const char* eq = strchr(argv[a], '='); if (!eq) continue;
        for (uint32_t i = 0; i < blipmachine::kParamCount; ++i) {
            char sy[32], nm[32]; blipmachine::ParamDef pd; blipmachine::getParamDef(i, pd, sy, nm);
            if (strlen(pd.symbol) == (size_t)(eq - argv[a]) && !strncmp(pd.symbol, argv[a], eq - argv[a])) controls[i] = atof(eq + 1);
        }
    }
    d->connect_port(inst, 0, L); d->connect_port(inst, 1, R);
    d->connect_port(inst, 2, inbuf);
    for (uint32_t i = 0; i < blipmachine::kParamCount; ++i) d->connect_port(inst, 3 + i, &controls[i]);
    if (d->activate) d->activate(inst);

    LV2_URID midiEv = map(0, LV2_MIDI__MidiEvent);
    LV2_Atom_Forge forge; lv2_atom_forge_init(&forge, &um);
    FILE* raw = fopen("out.f32", "wb");
    double sum2 = 0; float peak = 0;
    for (int b = 0; b < blocks; ++b) {
        lv2_atom_forge_set_buffer(&forge, inbuf, sizeof(inbuf));
        LV2_Atom_Forge_Frame seq;
        lv2_atom_forge_sequence_head(&forge, &seq, 0);
        if (b == 0) {
            uint8_t msg[3] = { (uint8_t)0x90, (uint8_t)note, (uint8_t)vel };
            lv2_atom_forge_frame_time(&forge, 0);
            lv2_atom_forge_atom(&forge, 3, midiEv);
            lv2_atom_forge_write(&forge, msg, 3);
        }
        lv2_atom_forge_pop(&forge, &seq);

        d->run(inst, block);

        for (uint32_t i = 0; i < block; ++i) { sum2 += L[i] * L[i]; if (L[i] > peak) peak = L[i]; if (-L[i] > peak) peak = -L[i]; }
        fwrite(L, 4, block, raw); fwrite(R, 4, block, raw);
    }
    fclose(raw);
    printf("note %d vel %d  rms %.4f  peak %.3f\n", note, vel, sqrt(sum2 / (blocks * block)), peak);
    if (d->deactivate) d->deactivate(inst);
    d->cleanup(inst);
    dlclose(h);
    printf("ok\n");
    return 0;
}
