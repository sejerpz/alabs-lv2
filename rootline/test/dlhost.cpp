// dlopen host (no lilv): dlhost plugin.so in.f32 out.f32 bass.f32 mode unison
#include <lv2/core/lv2.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int, char** argv) {
    void* lib = dlopen(argv[1], RTLD_NOW); if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
    auto fn = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
    const LV2_Descriptor* d = fn(0); printf("URI %s\n", d->URI);
    FILE* f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f) / 4; fseek(f, 0, SEEK_SET);
    std::vector<float> in(n), out(n), bass(n); if (fread(in.data(), 4, n, f)) {} fclose(f);
    //              0  1  2  mode ll cut  thr  syn  tone  oct  grit att rel  dry inst tun  16 mode             sens uni               uoct mainout blevel ctx
    float ctl[24] = {0, 0, 0, 0,  1, 57, -50, 0,   0.3f, -60, 0.5f, 3, 150, -60, 1, 0,  0, (float)atof(argv[5]), 7, (float)atof(argv[6]), 2, 0, 0, 0};
    LV2_Handle h = d->instantiate(d, 48000, "", nullptr);
    for (int i = 3; i < 24; ++i) if (i != 16) d->connect_port(h, i, &ctl[i]);
    float bi[128], bo[128], bb[128];
    d->connect_port(h, 0, bi); d->connect_port(h, 1, bi); d->connect_port(h, 2, bo); d->connect_port(h, 16, bb);
    d->activate(h);
    for (long i = 0; i < n; i += 128) { int m = n - i < 128 ? n - i : 128; memcpy(bi, &in[i], m * 4); d->run(h, m); memcpy(&out[i], bo, m * 4); memcpy(&bass[i], bb, m * 4); }
    d->cleanup(h);
    f = fopen(argv[3], "wb"); fwrite(out.data(), 4, n, f); fclose(f);
    f = fopen(argv[4], "wb"); fwrite(bass.data(), 4, n, f); fclose(f);
    printf("context=%g\n", ctl[23]);
    return 0;
}
