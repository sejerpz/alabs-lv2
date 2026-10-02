// Offline runner: raw float32 mono in -> raw float32 out (+ log: subHz, env)
// usage: run_engine in.f32 out.f32 log.f32 sr [key=value ...]
//   keys: ll synth oct cut tone inst tun rel
#include "../src/rootline_dsp.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "args\n"); return 1; }
    FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f) / 4; fseek(f, 0, SEEK_SET);
    std::vector<float> in(n), dry(n, 0.f), out(n), bout(n), lg(4 * n);
    const char* dryPath = getenv("DRY");
    if (dryPath) { FILE* fd = fopen(dryPath, "rb"); if (fread(dry.data(), 4, n, fd)) {} fclose(fd); }
    if (fread(in.data(), 4, n, f)) {} fclose(f);
    rootline::Engine e; e.init(atof(argv[4]));
    rootline::Params p; p.dryDb = -60.f; p.dualInput = true;
    for (int i = 5; i < argc; ++i) {
        char* eq = strchr(argv[i], '='); if (!eq) continue; *eq = 0; float v = atof(eq + 1);
        const char* k = argv[i];
        if (!strcmp(k, "ll")) p.lowLatency = v > 0.5f;
        else if (!strcmp(k, "synth")) p.synthDb = v;
        else if (!strcmp(k, "oct")) p.octDb = v;
        else if (!strcmp(k, "cut")) p.cutoffNote = v;
        else if (!strcmp(k, "tone")) p.synthTone = v;
        else if (!strcmp(k, "inst")) p.instrument = (int)v;
        else if (!strcmp(k, "tun")) p.tuning = (int)v;
        else if (!strcmp(k, "rel")) p.releaseMs = v;
        else if (!strcmp(k, "mode")) p.mode = (int)v;
        else if (!strcmp(k, "sens")) p.melodySens = v;
        else if (!strcmp(k, "uni")) p.unison = v > 0.5f;
        else if (!strcmp(k, "unioct")) p.unisonOct = (int)v;
        else if (!strcmp(k, "gonly")) p.mainGuitarOnly = v > 0.5f;
        else if (!strcmp(k, "dry")) p.dryDb = v;
    }
    e.setParams(p);
    const int B = 8;
    for (long i = 0; i < n; i += B) {
        int m = (int)std::min<long>(B, n - i);
        e.process(in.data() + i, (dryPath ? dry.data() : in.data()) + i, out.data() + i, bout.data() + i, m);
        for (int j = 0; j < m; ++j) { lg[4*(i+j)] = e.subFreq(); lg[4*(i+j)+1] = e.synthEnv(); lg[4*(i+j)+2] = (float)e.context(); lg[4*(i+j)+3] = e.muteGain(); }
    }
    f = fopen(argv[2], "wb"); fwrite(out.data(), 4, n, f); fclose(f);
    f = fopen(argv[3], "wb"); fwrite(lg.data(), 4, 4 * n, f); fclose(f);
    f = fopen("/tmp/bass.f32", "wb"); fwrite(bout.data(), 4, n, f); fclose(f);
    return 0;
}
