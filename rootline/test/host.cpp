// Minimal lilv host: loads rootline.lv2 from the bundle, uses TTL defaults,
// overrides some controls by symbol, runs raw f32 through it.
#include <lilv/lilv.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <map>
int main(int argc, char** argv) {
    // host bundle_dir in.f32 out.f32 [sym=val ...]
    LilvWorld* w = lilv_world_new();
    std::string b = std::string("file://") + argv[1] + "/";
    LilvNode* bn = lilv_new_uri(w, b.c_str()); lilv_world_load_bundle(w, bn);
    const LilvPlugins* ps = lilv_world_get_all_plugins(w);
    LilvNode* uri = lilv_new_uri(w, "urn:alabs:rootline");
    const LilvPlugin* p = lilv_plugins_get_by_uri(ps, uri);
    if (!p) { fprintf(stderr, "plugin not found\n"); return 1; }
    uint32_t np = lilv_plugin_get_num_ports(p);
    std::vector<float> mins(np), maxs(np), defs(np);
    lilv_plugin_get_port_ranges_float(p, mins.data(), maxs.data(), defs.data());
    std::map<std::string, float> ov;
    for (int i = 4; i < argc; ++i) { char* e = strchr(argv[i], '='); *e = 0; ov[argv[i]] = atof(e + 1); }
    FILE* f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f) / 4; fseek(f, 0, SEEK_SET);
    std::vector<float> in(n), out(n); if (fread(in.data(), 4, n, f)) {} fclose(f);
    LilvInstance* inst = lilv_plugin_instantiate(p, 48000, nullptr);
    std::vector<float> ctl(np);
    LilvNode* audio = lilv_new_uri(w, LILV_URI_AUDIO_PORT);
    const int B = 128; std::vector<float> bi(B), bo(B), bb(B); std::vector<float> bass(n);
    for (uint32_t i = 0; i < np; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(p, i);
        const char* sym = lilv_node_as_string(lilv_port_get_symbol(p, port));
        if (lilv_port_is_a(p, port, audio)) continue;
        ctl[i] = ov.count(sym) ? ov[sym] : defs[i];
        printf("  %-12s = %g  [%g..%g]\n", sym, ctl[i], mins[i], maxs[i]);
        lilv_instance_connect_port(inst, i, &ctl[i]);
    }
    lilv_instance_connect_port(inst, 0, bi.data());
    lilv_instance_connect_port(inst, 1, bi.data());
    lilv_instance_connect_port(inst, 2, bo.data());
    lilv_instance_connect_port(inst, 16, bb.data());
    lilv_instance_activate(inst);
    for (long i = 0; i < n; i += B) {
        int m = (int)std::min<long>(B, n - i);
        memcpy(bi.data(), &in[i], m * 4);
        lilv_instance_run(inst, m);
        memcpy(&out[i], bo.data(), m * 4);
        memcpy(&bass[i], bb.data(), m * 4);
    }
    lilv_instance_deactivate(inst); lilv_instance_free(inst);
    f = fopen(argv[3], "wb"); fwrite(out.data(), 4, n, f); fclose(f);
    f = fopen("/tmp/host_bass.f32", "wb"); fwrite(bass.data(), 4, n, f); fclose(f);
    printf("context port = %g\n", ctl[23]);
    lilv_world_free(w);
    return 0;
}
