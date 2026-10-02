#include "../src/rootline_dsp.hpp"
#include <chrono>
#include <cstdio>
#include <vector>
#include <random>
#include <cmath>
static rootline::Engine e;
double run(const rootline::Params& p, const std::vector<float>& in, std::vector<float>& out, std::vector<float>& bo) {
    e.init(48000); e.setParams(p);
    const int B = 128; auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i + B <= in.size(); i += B) e.process(&in[i], &in[i], &out[i], &bo[i], B);
    return 100.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / (in.size() / 48000.0);
}
int main() {
    const int sr = 48000, N = sr * 20;
    std::vector<float> in(N), out(N), bo(N);
    std::mt19937 g(1); std::normal_distribution<float> nd(0, 0.01f);
    for (int i = 0; i < N; ++i) in[i] = 0.3f * std::sin(2 * 3.14159f * 73.4f * i / sr) + 0.2f * std::sin(2 * 3.14159f * 330.f * i / sr) + nd(g);
    rootline::Params p; p.octDb = 0;
    const char* names[] = {"classic", "smart mono", "dual", "unison"};
    for (int m = 0; m < 4; ++m) {
        p.mode = m < 3 ? m : 0; p.unison = m == 3;
        printf("%-10s low_latency: %.3f%%   robust: ", names[m], (p.lowLatency = true, run(p, in, out, bo)));
        p.lowLatency = false; printf("%.3f%% of one x86 core\n", run(p, in, out, bo)); p.lowLatency = true;
    }
    // robustness
    std::vector<float> t(sr * 4);
    for (int i = sr; i < 2 * sr; ++i) t[i] = 0.9f;
    for (int i = 2 * sr; i < 3 * sr; ++i) t[i] = std::uniform_real_distribution<float>(-1, 1)(g);
    for (int i = 3 * sr; i < 4 * sr; ++i) t[i] = (i % 2 ? 1 : -1) * 1e-30f;
    bool finite = true; float mx = 0;
    for (int m = 0; m < 4; ++m) {
        p.mode = m < 3 ? m : 0; p.unison = m == 3; e.init(sr); e.setParams(p);
        std::vector<float> o(t.size()), b(t.size());
        for (size_t i = 0; i < t.size(); i += 128) e.process(&t[i], &t[i], &o[i], &b[i], 128);
        for (size_t i = 0; i < o.size(); ++i) { finite &= std::isfinite(o[i]) && std::isfinite(b[i]); mx = std::max(mx, std::fabs(o[i])); }
    }
    printf("robustness (silence, DC, full-scale noise, denormals; all modes): finite=%d max|out|=%.2f\n", finite, mx);
    printf("sizeof(Engine) = %zu bytes\n", sizeof(rootline::Engine));
}
