// Rootline — bass companion for guitar (LV2 DSP core, no LV2 dependency)
//
// Tracks the LOWEST note played on a guitar (acoustic or electric; standard,
// drop D, drop C or B tuning) and plays a bass line one octave below it,
// with two timbres: a clean synth and an OC-2 style octaver.
//
// Main tracking path (runs at fs/D ~ 6 kHz):
//   in_track -> LP 4th (anti-alias, IIR, <1 ms) -> decimate xD -> HP 40 Hz
//            -> bank of 14 Chebyshev-II lowpasses, grid C#2..D#4 every 2 semitones
//               each band: analytic signal by derivative quadrature,
//               amplitude A_k, raw phase increment, and a one-period
//               energy-weighted average frequency F_k
//            -> lowest-note selection: lowest band whose dominant frequency lies
//               BELOW its own grid point and agrees with the two bands above
//            -> half-phase tracking: the synth phase advances by dphi/2 of the
//               selected band (exactly one octave down, no added lag)
//            -> synth  : Chebyshev-harmonic oscillator (tone = harmonics)
//               octaver: band signal * soft flip-flop    (OC-2 style)
//            -> zero-stuff xD -> LP 4th -> bass signal
//
// On top of the main path:
//   ContextDetector : harmonic residual (comb on the tracked period) + onset
//                     timing -> silence / single notes / melody / chords
//   Mode Smart mono : bass muted while a fast single-note line is detected
//   Mode Dual       : adaptive comb notch removes the tracked low note (and its
//                     harmonics) from the dry guitar
//   Unison          : second tracking core at ~12 kHz, grid up to B6 (notes up
//                     to E6), no range limit, octave -1 / -2 / auto (fold)
//   Outputs         : main out (guitar + bass, or guitar only) and bass out
//
// low_latency = true : 4th-order bands, 1 ms hold      -> audible after ~6-14 ms
// low_latency = false: 6th-order bands, longer window,
//                      4 ms hold, slower phase lock    -> ~20-40 ms, cleaner
//                      onsets and fewer wrong notes on dense chords
// (measured offline on synthetic plucked strings, see test/)

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace rootline {

static constexpr float kPi  = 3.14159265358979f;
static constexpr float k2Pi = 6.28318530717959f;

inline float wrapPi(float x) {
    // wrap to [-pi, pi)
    x -= k2Pi * std::floor((x + kPi) * (1.f / k2Pi));
    return x;
}

// Flush denormals to zero while processing (decaying IIR states would
// otherwise slow the plugin down several times when the input goes silent).
struct ScopedFlushDenormals {
#if defined(__SSE__) || defined(__x86_64__)
    unsigned int old;
    ScopedFlushDenormals() : old(_mm_getcsr()) { _mm_setcsr(old | 0x8040); }
    ~ScopedFlushDenormals() { _mm_setcsr(old); }
#elif defined(__aarch64__)
    uint64_t old;
    ScopedFlushDenormals() {
        asm volatile("mrs %0, fpcr" : "=r"(old));
        const uint64_t fz = old | (1ull << 24);
        asm volatile("msr fpcr, %0" : : "r"(fz));
    }
    ~ScopedFlushDenormals() { asm volatile("msr fpcr, %0" : : "r"(old)); }
#else
    ScopedFlushDenormals() {}
#endif
};

inline float midiToHz(float m) { return 440.f * std::exp2((m - 69.f) / 12.f); }
inline float dbToGain(float db) { return db <= -59.9f ? 0.f : std::pow(10.f, db / 20.f); }
inline float onePoleCoef(float ms, float fs) {
    return ms <= 0.f ? 1.f : 1.f - std::exp(-1000.f / (ms * fs));
}

// ---------------------------------------------------------------- biquad (DF2T)
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    inline float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0.f; }

    void setLowpass(float fc, float q, float fs) {
        const float w = k2Pi * fc / fs, c = std::cos(w), al = std::sin(w) / (2.f * q);
        const float a0 = 1.f + al;
        b0 = (1.f - c) * 0.5f / a0; b1 = (1.f - c) / a0; b2 = b0;
        a1 = -2.f * c / a0;         a2 = (1.f - al) / a0;
    }
    void setNotch(float fc, float q, float fs) {
        const float w = k2Pi * fc / fs, c = std::cos(w), al = std::sin(w) / (2.f * q);
        const float a0 = 1.f + al;
        b0 = 1.f / a0; b1 = -2.f * c / a0; b2 = b0;
        a1 = -2.f * c / a0; a2 = (1.f - al) / a0;
    }
    void setBandpass(float fc, float q, float fs) {   // 0 dB peak
        const float w = k2Pi * fc / fs, c = std::cos(w), al = std::sin(w) / (2.f * q);
        const float a0 = 1.f + al;
        b0 = al / a0; b1 = 0.f; b2 = -al / a0;
        a1 = -2.f * c / a0; a2 = (1.f - al) / a0;
    }
    void setHighpass(float fc, float q, float fs) {
        const float w = k2Pi * fc / fs, c = std::cos(w), al = std::sin(w) / (2.f * q);
        const float a0 = 1.f + al;
        b0 = (1.f + c) * 0.5f / a0; b1 = -(1.f + c) / a0; b2 = b0;
        a1 = -2.f * c / a0;         a2 = (1.f - al) / a0;
    }
};

// Butterworth section Qs (anti-alias / anti-image filters)
static constexpr float kQ4[2] = {0.54119610f, 1.30656296f};

struct ButterLP4 {
    Biquad s[2];
    void design(float fc, float fs) { for (int i = 0; i < 2; ++i) s[i].setLowpass(fc, kQ4[i], fs); }
    inline float process(float x) { return s[1].process(s[0].process(x)); }
    void reset() { s[0].reset(); s[1].reset(); }
};

// Chebyshev type II analog prototypes, normalised to the band's grid
// frequency c = 1, stopband edge 1.35 c. Section: g (s^2 + wz) / (s^2 + a1 s + wp)
// Chosen over Butterworth because, for the same group delay, they reject the
// fifth (power chords) and the octave (2nd harmonic) 15-25 dB better.
struct ProtoSection { float g, wz, a1, wp; };
static constexpr ProtoSection kCheby4[2] = {   // N=4, 35 dB  (low latency)
    {0.281858977f,  2.135191565f, 0.503163214f, 0.601822911f},
    {0.063091104f, 12.444808435f, 1.584791725f, 0.785156705f},
};
static constexpr ProtoSection kCheby6[3] = {   // N=6, 50 dB  (robust)
    {0.357740487f,  1.953349613f, 0.346781193f, 0.698792242f},
    {0.229878863f,  3.645000000f, 1.136037862f, 0.837908455f},
    {0.038453234f, 27.206650387f, 1.937594756f, 1.046183681f},
};

struct BandLP {
    Biquad s[3];
    const ProtoSection* proto = kCheby4;
    int n = 2;
    float tanC = 1.f;   // tan(pi c / fs) for magnitude evaluation

    void design(float c, float fs, bool ll) {
        proto = ll ? kCheby4 : kCheby6;
        n = ll ? 2 : 3;
        tanC = std::tan(kPi * c / fs);
        const double K = 2.0 * fs, W = K * tanC, K2 = K * K, W2 = W * W;
        for (int i = 0; i < n; ++i) {
            const ProtoSection& p = proto[i];
            const double A0 = K2 + p.a1 * W * K + p.wp * W2;
            const double A1 = -2.0 * K2 + 2.0 * p.wp * W2;
            const double A2 = K2 - p.a1 * W * K + p.wp * W2;
            const double B0 = p.g * (K2 + p.wz * W2);
            const double B1 = p.g * (-2.0 * K2 + 2.0 * p.wz * W2);
            s[i].b0 = (float)(B0 / A0); s[i].b1 = (float)(B1 / A0); s[i].b2 = (float)(B0 / A0);
            s[i].a1 = (float)(A1 / A0); s[i].a2 = (float)(A2 / A0);
        }
    }
    inline float process(float x) {
        for (int i = 0; i < n; ++i) x = s[i].process(x);
        return x;
    }
    // |H| at frequency f (bilinear-warped analog evaluation)
    float magnitude(float f, float fs) const {
        const float w = std::tan(kPi * f / fs) / tanC, w2 = w * w;
        float m = 1.f;
        for (int i = 0; i < n; ++i) {
            const ProtoSection& p = proto[i];
            const float re = p.wp - w2, im = p.a1 * w;
            m *= p.g * std::fabs(p.wz - w2) / std::sqrt(re * re + im * im);
        }
        return m;
    }
    void reset() { for (auto& b : s) b.reset(); }
};

// ---------------------------------------------------------------- parameters
enum Mode { kClassic = 0, kSmartMono = 1, kDual = 2 };
enum Context { kCtxSilence = 0, kCtxSingle = 1, kCtxMelody = 2, kCtxChords = 3, kCtxUnison = 4 };
enum UnisonOct { kUniMinus1 = 0, kUniMinus2 = 1, kUniAuto = 2 };

struct Params {
    bool  dualInput   = true;   // true: dry from in2, false: dry from in1
    bool  lowLatency  = true;
    float cutoffNote  = 50.f;   // MIDI, 36 (C2) .. 57 (A3)
    int   instrument  = 0;      // 0 acoustic, 1 electric
    int   tuning      = 2;      // 0 standard E, 1 drop D, 2 drop C, 3 B (7-string/baritone)
    float thresholdDb = -50.f;
    float synthDb     = 0.f;
    float synthTone   = 0.3f;   // 0..1
    float octDb       = -60.f;
    float octGrit     = 0.5f;   // 0..1
    float attackMs    = 3.f;
    float releaseMs   = 150.f;
    float dryDb       = 0.f;
    int   mode        = kClassic;
    float melodySens  = 7.f;    // notes per second above which single notes count as melody
    bool  unison      = false;  // bass always on, no range limit (overrides mode)
    int   unisonOct   = kUniAuto;
    bool  mainGuitarOnly = false;
    float bassOutDb   = 0.f;
};

// ---------------------------------------------------------------- tracker + voice
// One tracking/synthesis core running at its own decimated rate.
//   main core   : ~6 kHz, grid C#2..D#4 (lowest note up to A3)
//   unison core : ~12 kHz, grid C#2..B6 (notes up to E6), only runs with Unison on
class Core {
public:
    static constexpr int kMaxBands = 30;
    static constexpr int kFirstMidi = 37;   // grid starts at C#2, 2 semitones apart

    struct TickIn {
        float thr, cutHz, cAtt, cRel, gritK, gritN, tone;
        int   octave;        // 1 = one octave down, 2 = two, 0 = auto (fold into E1..E3)
    };

    void init(float fsd, int nBands) {
        fsd_ = fsd;
        nBands_ = std::min(nBands, kMaxBands);
        for (int k = 0; k < nBands_; ++k) cutoff_[k] = midiToHz((float)(kFirstMidi + 2 * k));
    }

    void configure(bool ll, int instrument, int tuning) {
        static constexpr float kProf[2][3] = {
            {1.0f, 4.0f, 0.25f},   // acoustic: hold LL / robust (ms), relative floor
            {0.7f, 3.0f, 0.16f},   // electric: strong fundamental, clean attack
        };
        static constexpr float kLowestMidi[4] = {40.f, 38.f, 36.f, 35.f};  // E2 D2 C2 B1
        const int ins = std::min(std::max(instrument, 0), 1);
        const int tun = std::min(std::max(tuning, 0), 3);

        for (int k = 0; k < nBands_; ++k) {
            band_[k].lp.design(cutoff_[k], fsd_, ll);
            const int L = (int)std::lround((ll ? 1.0f : 1.2f) * fsd_ / cutoff_[k]);
            band_[k].L = std::min(std::max(L, 8), Band::kMaxL);
        }
        aF_    = onePoleCoef(ll ? 0.5f : 2.f,  fsd_);
        aA_    = onePoleCoef(ll ? 1.0f : 2.f,  fsd_);
        hold_  = (int)std::lround((ll ? kProf[ins][0] : kProf[ins][1]) * 1e-3f * fsd_);
        pllB_  = onePoleCoef(ll ? 4.f : 10.f,  fsd_);
        xfIncr_ = 1.f / std::max(1.f, (ll ? 3.f : 8.f) * 1e-3f * fsd_);
        gateAtt_ = onePoleCoef(ll ? 0.7f : 3.f, fsd_);
        relFloor_ = kProf[ins][2];
        holdSmooth_ = onePoleCoef(5.f, fsd_);
        slowEvery_ = fsd_ > 9000.f ? 15 : 7;          // ~1.3 ms control rate

        // tuning: nothing below (lowest open string - 1 semitone) can be a note
        fMin_ = midiToHz(kLowestMidi[tun] - 1.f) * 0.985f;
        minBand_ = 0;
        while (minBand_ < nBands_ - 3 && cutoff_[minBand_] <= fMin_) ++minBand_;
        hp_.setHighpass(tun == 3 ? 30.f : 40.f, 0.7071f, fsd_);
    }

    void reset() {
        hp_.reset();
        for (int k = 0; k < nBands_; ++k) {
            Band& b = band_[k];
            b.lp.reset();
            b.y1 = b.y2 = b.pi = b.pq = b.A = b.iq_i = b.dphi = 0.f;
            b.F = cutoff_[k] * 1.3f;
            b.validCount = 0;
            std::memset(b.wd, 0, sizeof(b.wd)); std::memset(b.wa, 0, sizeof(b.wa));
            b.sumWD = b.sumWA = 0; b.pos = 0;
        }
        sel_ = 0; prevSel_ = 0; selValid_ = false; xf_ = 1.f;
        cand_ = -1; candCount_ = 0;
        psi_ = psiS_ = lastDphi_ = 0.f; env_ = 0.f; octGate_ = 0.f; cutGate_ = 0.f;
        octShift_ = 1;
    }

    // state for the engine (context detection, dual comb, diagnostics)
    bool  noteValid() const { return selValid_; }
    float noteHz() const { return band_[sel_].F; }
    float noteHzFast() const { return lastDphi_ * fsd_ / k2Pi; }   // ~5 ms smoothing
    float gateOpen() const { return cutGate_; }
    float env() const { return env_; }
    float subHz() const { return subHz_; }
    float hpOut() const { return lastX_; }

    // one sample at fsd_; x = decimated (not yet high-passed) input
    void tick(float xin, const TickIn& in, float& synthOut, float& octOut) {
        const float x = hp_.process(xin);
        lastX_ = x;
        const float twoPiOverFs = k2Pi / fsd_;
        const bool slow = (tickCount_++ & slowEvery_) == 0;
        const int nb = nBands_;

        for (int k = 0; k < nb; ++k) {
            Band& b = band_[k];
            const float y = b.lp.process(x);
            if (slow) {
                const float f = std::min(std::max(b.F, 50.f), cutoff_[k] * 1.25f);
                b.invTwoSinW = 1.f / (2.f * std::sin(f * twoPiOverFs));
                b.invMag = 1.f / std::max(b.lp.magnitude(std::min(b.F, cutoff_[k]), fsd_), 0.15f);
            }
            // analytic signal centred at n-1 via central difference
            const float iv = b.y1;
            const float qv = (b.y2 - y) * b.invTwoSinW;
            b.y2 = b.y1; b.y1 = y;
            const float mag = std::sqrt(iv * iv + qv * qv);
            b.A += aA_ * (mag - b.A);
            const float re = iv * b.pi + qv * b.pq;
            const float im = qv * b.pi - iv * b.pq;
            b.pi = iv; b.pq = qv;
            b.iq_i = iv;

            const float dphiRaw = std::atan2(im, re);
            b.dphi = dphiRaw;                           // signed: integrates to the true phase
            const float dphi = std::max(dphiRaw, 0.f);
            const float wgt = mag * mag;                // energy weighting favours the settled part
            b.sumWD += (double)(wgt * dphi - b.wd[b.pos]);
            b.sumWA += (double)(wgt - b.wa[b.pos]);
            b.wd[b.pos] = wgt * dphi; b.wa[b.pos] = wgt;
            if (++b.pos >= b.L) {                       // re-sum once per window (drift)
                b.pos = 0;
                double sd = 0, sa = 0;
                for (int j = 0; j < b.L; ++j) { sd += b.wd[j]; sa += b.wa[j]; }
                b.sumWD = sd; b.sumWA = sa;
            }
            const float Fm = (b.sumWA > 1e-12)
                ? (float)(b.sumWD / b.sumWA) / twoPiOverFs : cutoff_[k] * 1.3f;
            b.F += aF_ * (Fm - b.F);
            b.Ac = b.A * b.invMag;
        }

        // A band is valid when its dominant component lies below its grid point
        // AND the next two bands up see the same component (a real note passes
        // all three; filter ringing from the pick attack does not).
        const float floorA = std::max(in.thr, relFloor_ * band_[nb - 1].A);
        int lowest = -1;
        for (int k = 0; k < nb; ++k) {
            Band& b = band_[k];
            bool valid = false;
            if (k >= minBand_ && k < nb - 2) {
                const Band& u = band_[k + 1];
                const Band& v = band_[k + 2];
                const float r1 = b.F / std::max(u.F, 1.f);
                const float r2 = b.F / std::max(v.F, 1.f);
                valid = b.Ac > floorA && b.Ac > 0.5f * u.Ac
                     && b.F < cutoff_[k] * 0.985f && b.F > fMin_
                     && r1 > 0.965f && r1 < 1.036f        // +-0.6 semitone
                     && r2 > 0.944f && r2 < 1.059f;       // +-1 semitone
            }
            b.validCount = valid ? std::min(b.validCount + 1, 1 << 20) : 0;
            if (lowest < 0 && valid) lowest = k;
        }

        // selection: enter = lowest valid band stable for hold_; stay = relaxed
        // test; move = only to a genuinely lower note (> 0.7 semitone)
        int want = -1;
        if (!selValid_) {
            want = lowest;
        } else {
            const Band& c = band_[sel_];
            const bool keep = c.Ac > 0.6f * floorA && c.F < cutoff_[sel_] * 1.03f && c.F > fMin_ * 0.97f;
            if (lowest >= 0 && lowest != sel_ && (!keep || band_[lowest].F < c.F * 0.9604f))
                want = lowest;
            else if (!keep && lowest < 0)
                selValid_ = false;
        }
        if (want >= 0) {
            if (cand_ == want) ++candCount_; else { cand_ = want; candCount_ = 1; }
            if (candCount_ >= std::max(hold_, 1)) switchTo(want, !selValid_);
        } else {
            cand_ = -1; candCount_ = 0;
        }

        const Band& s = band_[sel_];

        // octave of the bass: fixed, or folded into E1..E3 with 3-semitone hysteresis
        if (slow) {
            if (in.octave > 0) octShift_ = in.octave;
            else if (selValid_) {
                const float lo = 41.2f * 0.84f, hi = 164.8f * 1.19f;
                const float b = s.F / (float)(1 << octShift_);
                if (b > hi || b < lo) {
                    int k = 1;
                    while (k < 5 && s.F / (float)(1 << k) > 164.8f) ++k;
                    octShift_ = k;
                }
            }
        }
        const float div = 1.f / (float)(1 << octShift_);

        // octaver phase: psi locked to half the band phase (flip-flop alignment)
        float srcY = s.iq_i;
        if (xf_ < 1.f) {
            xf_ = std::min(1.f, xf_ + xfIncr_);
            srcY = xf_ * srcY + (1.f - xf_) * band_[prevSel_].iq_i;
        }
        psi_ += 0.5f * (s.dphi + pllB_ * wrapPi(std::atan2(s.pq, s.pi) - 2.f * psi_));
        if (psi_ > k2Pi) psi_ -= k2Pi; else if (psi_ < 0.f) psi_ += k2Pi;
        // synth phase: exact division of the band phase, never re-aligned
        if (selValid_) {
            lastDphi_ += holdSmooth_ * (s.dphi - lastDphi_);
            psiS_ += div * s.dphi;
        } else {
            psiS_ += div * lastDphi_;                  // releasing: hold the last pitch
        }
        if (psiS_ > k2Pi) psiS_ -= k2Pi; else if (psiS_ < 0.f) psiS_ += k2Pi;
        subHz_ += 0.02f * (div * lastDphi_ / twoPiOverFs - subHz_);

        // gates / envelope
        const bool on = selValid_;
        const float cutT = (on && s.F <= in.cutHz) ? 1.f : 0.f;
        cutGate_ += gateAtt_ * (cutT - cutGate_);
        const float comp = std::min(s.invMag, 5.f);
        const int ka = std::min(sel_ + 1, nb - 1);     // level from the band above (faster)
        const float target = on ? band_[ka].Ac * cutGate_ : 0.f;
        env_ += (target > env_ ? in.cAtt : in.cRel) * (target - env_);
        const float og = on ? cutGate_ : 0.f;
        octGate_ += (og > octGate_ ? gateAtt_ : in.cRel) * (og - octGate_);

        // synth: Chebyshev harmonics of cos(psiS), band-limited below 0.4 fsd
        const float c = std::cos(psiS_);
        const float fb = std::max(div * lastDphi_ / twoPiOverFs, 20.f);
        const int kmax = std::min(6, (int)(0.4f * fsd_ / fb));
        float tkm1 = 1.f, tk = c, sum = c, amp = 1.f;
        if (in.tone > 0.001f) {
            for (int k = 2; k <= kmax; ++k) {
                const float tn = 2.f * c * tk - tkm1;
                tkm1 = tk; tk = tn;
                amp *= in.tone;
                sum += amp * tk / (float)k;
            }
        }
        synthOut = env_ * sum;

        // octaver: OC-2 flip-flop on the band signal (one octave down); for
        // lower octaves a flip-flop has no musical meaning, so a soft square
        // oscillator on the synth phase takes its place
        if (octShift_ == 1) {
            const float sq = std::tanh(in.gritK * std::cos(psi_ + 0.25f * kPi)) * in.gritN;
            octOut = srcY * sq * comp * octGate_;
        } else {
            octOut = env_ * std::tanh(in.gritK * c) * in.gritN;
        }
    }

private:
    struct Band {
        BandLP lp;
        float y1 = 0, y2 = 0;       // history of lp output
        float pi = 0, pq = 0;       // previous analytic sample
        float A = 0;                // smoothed amplitude
        float Ac = 0;               // amplitude compensated for band response
        float F = 0;                // smoothed inst. frequency (Hz)
        float iq_i = 0;             // current in-phase sample (y[n-1])
        float dphi = 0;             // raw phase increment (rad/sample)
        float invTwoSinW = 1.f;     // 1 / (2 sin w) for the quadrature
        float invMag = 1.f;         // 1 / |H(F)|
        int   validCount = 0;
        // one-period, energy-weighted moving average of the phase increment:
        // cancels the beat between the fundamental and the leaking 2nd
        // harmonic (period 1/f) and the quadrature ellipse error (1/2f)
        static constexpr int kMaxL = 256;
        float wd[kMaxL] = {};       // A^2 * dphi
        float wa[kMaxL] = {};       // A^2
        double sumWD = 0, sumWA = 0;
        int   L = 64, pos = 0;
    };

    void switchTo(int k, bool fresh) {
        if (fresh) {
            // start the octaver phase on the band's phase immediately (fast attack)
            const Band& pb = band_[k];
            const float ph = std::atan2(pb.pq, pb.pi);   // phase only needed here
            psi_ = 0.5f * (ph < 0.f ? ph + k2Pi : ph);
            xf_ = 1.f;
        } else {
            prevSel_ = sel_;
            xf_ = 0.f;
        }
        sel_ = k; selValid_ = true;
        cand_ = -1; candCount_ = 0;
    }

    float fsd_ = 6000.f;
    int   nBands_ = 14;
    Biquad hp_;
    float cutoff_[kMaxBands] = {};
    Band  band_[kMaxBands];

    float aF_ = 0, aA_ = 0, pllB_ = 0, xfIncr_ = 0, gateAtt_ = 0, holdSmooth_ = 0.03f;
    int   hold_ = 0, minBand_ = 0;
    uint32_t slowEvery_ = 7, tickCount_ = 0;
    float relFloor_ = 0.25f, fMin_ = 45.f;
    int   sel_ = 0, prevSel_ = 0, cand_ = -1, candCount_ = 0, octShift_ = 1;
    bool  selValid_ = false;
    float subHz_ = 0.f, lastX_ = 0.f;
    float psiS_ = 0.f, lastDphi_ = 0.f;
    float xf_ = 1.f, psi_ = 0.f, env_ = 0.f, octGate_ = 0.f, cutGate_ = 0.f;
};

// ---------------------------------------------------------------- context detector
// Runs on the main core's decimated signal. Two cues:
//  - harmonic residual: x[n] - x[n-T] with T = period of the tracked note removes
//    that note and all its harmonics; what is left belongs to other notes
//    (a power chord's fifth is not periodic in T, so it counts as "other")
//  - onset timing: single-note onsets closer than 1/sensitivity -> melody
class ContextDetector {
public:
    void init(float fsd) {
        fsd_ = fsd;
        aFast_ = onePoleCoef(2.f, fsd);  aSlow_ = onePoleCoef(40.f, fsd);
        aRes_  = onePoleCoef(10.f, fsd);
        refractTicks_ = (int)(0.045f * fsd);
        evalTicks_ = (int)(0.080f * fsd);
        windowTicks_ = (int)(0.045f * fsd);
        silentTicks_ = (int)(0.15f * fsd);
        reset();
    }
    void reset() {
        std::memset(buf_, 0, sizeof(buf_)); w_ = 0;
        eF_ = eS_ = Er_ = Ex_ = 0.f; refract_ = 0; pending_ = 0; t_ = 0; pitchRefract_ = 0; lastEvT_ = 0;
        nTimes_ = 0; melody_ = false; chords_ = false; quiet_ = silentTicks_; prevHz_ = 0.f; chordStreak_ = singleStreak_ = 0;
    }
    int  context() const { return quiet_ >= silentTicks_ ? kCtxSilence : melody_ ? kCtxMelody : chords_ ? kCtxChords : kCtxSingle; }
    bool melody() const { return melody_; }
    float residual() const { return Er_ / (2.f * Ex_ + 1e-12f); }
    int  lastEvent() const { return lastEvent_; }   // tests: 0 none, 1 single, 2 chord

    // noteHz: fast estimate (comb period); noteHzStable: for pitch-change events
    void tick(float x, bool noteValid, float noteHz, float noteHzStable, float thr, float sens) {
        ++t_;
        // residual comb at the tracked note period (linear fractional delay)
        buf_[w_] = x;
        if (noteValid && noteHz > 30.f) {
            const float T = std::min(fsd_ / noteHz, (float)(kBuf - 2));
            const int   i0 = (int)T; const float fr = T - (float)i0;
            const float a = buf_[(w_ - i0) & (kBuf - 1)], b = buf_[(w_ - i0 - 1) & (kBuf - 1)];
            const float r = x - (a + fr * (b - a));
            Er_ += aRes_ * (r * r - Er_);
            Ex_ += aRes_ * (x * x - Ex_);
        }
        w_ = (w_ + 1) & (kBuf - 1);

        // onsets
        const float ax = std::fabs(x);
        eF_ += aFast_ * (ax - eF_);
        eS_ += aSlow_ * (ax - eS_);
        if (eS_ > 0.5f * thr) quiet_ = 0; else if (quiet_ < silentTicks_) ++quiet_;
        // events: amplitude onsets (strums, re-picks) OR a change of the tracked
        // pitch (legato lines, where the level hardly moves)
        bool pitchChange = false;
        if (noteValid) {
            if (prevHz_ <= 0.f || std::fabs(noteHzStable - prevHz_) > 0.04f * prevHz_) {
                pitchChange = true; prevHz_ = noteHzStable;
            }
        }
        // an event (amplitude onset, or pitch change for legato lines) is judged
        // on the MINIMUM residual between +35 and +65 ms: a single note becomes
        // clean once the tracker has settled, a chord never does
        if (refract_ > 0) --refract_;
        if (pitchRefract_ > 0) --pitchRefract_;
        const bool ampOnset = refract_ == 0 && eF_ > 2.f * eS_ && eF_ > thr;
        if (ampOnset) refract_ = refractTicks_;
        const bool pitchEv = pitchChange && pitchRefract_ == 0;
        if (pitchEv) pitchRefract_ = refractTicks_;
        lastEvent_ = 0;
        if ((ampOnset || pitchEv) && t_ - lastEvT_ > (uint32_t)refractTicks_) {
            // one note = one event (its attack and its pitch change are merged).
            // A new note while the previous one is still being judged closes that
            // judgement early (fast lines), if its window had started
            lastEvT_ = t_;
            if (pending_ > 0 && pending_ <= windowTicks_ && minRes_ < 1e8f) judge(noteValid, sens);
            pending_ = evalTicks_; onsetT_ = t_; minRes_ = 1e9f;
        }
        if (pending_ > 0 && pending_ <= windowTicks_)
            minRes_ = std::min(minRes_, noteValid ? residual() : 1e9f);
        if (pending_ > 0 && --pending_ == 0) judge(noteValid, sens);
        // melody ends after a pause of 2.5 note intervals
        if (melody_ && nTimes_ > 0 && (float)(t_ - times_[nTimes_ - 1]) > 2.5f * fsd_ / std::max(sens, 1.f)
            && pending_ == 0)
            melody_ = false, nTimes_ = 0;
    }

    static constexpr float kChordRes = 0.25f;
private:
    void judge(bool noteValid, float sens) {
        pending_ = 0;
        const bool chord = noteValid && minRes_ < 1e8f && minRes_ > kChordRes;
        lastEvent_ = chord ? 2 : 1;
        if (chord) {
            ++chordStreak_; singleStreak_ = 0;
            nTimes_ = 0;
            // one ambiguous event does not break a running melody
            if (!melody_ || chordStreak_ >= 2) { melody_ = false; chords_ = true; }
        } else {
            ++singleStreak_; chordStreak_ = 0;
            if (singleStreak_ >= 2) chords_ = false;
            if (nTimes_ < 3) times_[nTimes_++] = onsetT_;
            else { times_[0] = times_[1]; times_[1] = times_[2]; times_[2] = onsetT_; }
            const float maxGap = fsd_ / std::max(sens, 1.f);
            if (nTimes_ == 3 && (float)(times_[1] - times_[0]) <= maxGap
                             && (float)(times_[2] - times_[1]) <= maxGap)
                melody_ = true;
        }
    }

    static constexpr int kBuf = 512;
    float buf_[kBuf];
    int   w_ = 0;
    float fsd_ = 6000.f, aFast_ = 0, aSlow_ = 0, aRes_ = 0;
    float eF_ = 0, eS_ = 0, Er_ = 0, Ex_ = 0;
    int   pitchRefract_ = 0, windowTicks_ = 0;
    float minRes_ = 1e9f;
    int   refract_ = 0, refractTicks_ = 0, pending_ = 0, evalTicks_ = 0, quiet_ = 0, silentTicks_ = 0;
    uint32_t t_ = 0, onsetT_ = 0, times_[3] = {}, lastEvT_ = 0;
    int   nTimes_ = 0, lastEvent_ = 0, chordStreak_ = 0, singleStreak_ = 0;
    float prevHz_ = 0.f;
    bool  melody_ = false, chords_ = false;
};

// ---------------------------------------------------------------- dual: comb notch
// y = (1+rho)/2 * (x[n] - x[n-T]) / (1 - rho z^-T): notches on the tracked note
// and all its harmonics, ~flat (+-1 dB) elsewhere. Cubic fractional delay.
class CombNotch {
public:
    void reset() { std::memset(xb_, 0, sizeof(xb_)); std::memset(yb_, 0, sizeof(yb_)); w_ = 0; P_ = 1e-6f; }
    // T is refined in place (normalised gradient descent on the output power):
    // the tracker's pitch is only accurate to a few tenths of a percent, the
    // notches need better than that on the upper harmonics
    inline float process(float x, float& T, bool adapt, float Tref) {
        const float xd = read(xb_, T), yd = read(yb_, T);
        const float y = x - xd + kRho * yd;
        if (adapt) {
            const float g = (read(xb_, T - 0.5f) - read(xb_, T + 0.5f))
                          - kRho * (read(yb_, T - 0.5f) - read(yb_, T + 0.5f));
            P_ += 0.001f * (g * g - P_);
            T -= kMu * y * g / (P_ + 1e-9f);
            T = std::min(std::max(T, 0.97f * Tref), 1.03f * Tref);
        }
        xb_[w_] = x; yb_[w_] = y;
        w_ = (w_ + 1) & (kBuf - 1);
        return y * (0.5f * (1.f + kRho));
    }
    static constexpr float kMu = 0.0005f;
    static constexpr int kBuf = 4096;           // T up to ~4000 samples (60 Hz @ 192 kHz)
    static constexpr float kRho = 0.85f;
private:
    inline float read(const float* b, float T) const {
        const int i = (int)T; const float f = T - (float)i;
        const int p = w_ - i;                       // b[p] = x[n - i]
        const float y0 = b[(p + 1) & (kBuf - 1)], y1 = b[p & (kBuf - 1)];
        const float y2 = b[(p - 1) & (kBuf - 1)], y3 = b[(p - 2) & (kBuf - 1)];
        // Catmull-Rom between y1 (delay i) and y2 (delay i+1)
        const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }
    float xb_[kBuf], yb_[kBuf];
    int   w_ = 0;
    float P_ = 1e-6f;
};

// ---------------------------------------------------------------- engine
class Engine {
public:
    void init(double sampleRate) {
        fs_ = (float)sampleRate;
        D_ = 1;                                     // main core: fs/D in [4, 8) kHz
        while (fs_ / (float)(D_ * 2) >= 4000.f) D_ *= 2;
        Du_ = std::max(1, D_ / 2);                  // unison core: twice the rate
        main_.init(fs_ / (float)D_, 14);            // grid C#2..D#4 -> notes up to A3
        uni_.init(fs_ / (float)Du_, 30);            // grid C#2..B6  -> notes up to E6
        ctx_.init(fs_ / (float)D_);

        gainSmooth_ = onePoleCoef(20.f, fs_);
        aMute_ = onePoleCoef(20.f, fs_);
        aDual_ = onePoleCoef(8.f, fs_);
        aT_ = onePoleCoef(20.f, fs_);
        decim_.design(900.f, fs_);  interp_.design(900.f, fs_);
        decimU_.design(2500.f, fs_); interpU_.design(3000.f, fs_);
        configure();
        reset();
    }

    void setParams(const Params& p) {
        const bool ll = p.lowLatency != params_.lowLatency;
        const bool cfg = ll || p.instrument != params_.instrument || p.tuning != params_.tuning;
        const bool uniOn = p.unison && !params_.unison;
        const bool uniOff = !p.unison && params_.unison;
        params_ = p;
        if (cfg) configure();
        if (ll) { main_.reset(); uni_.reset(); }
        if (uniOn) { uni_.reset(); decimU_.reset(); interpU_.reset(); phaseU_ = 0; }
        if (uniOff) { main_.reset(); ctx_.reset(); }
    }

    void reset() {
        decim_.reset(); interp_.reset(); decimU_.reset(); interpU_.reset();
        main_.reset(); uni_.reset(); ctx_.reset(); comb_.reset();
        phase_ = phaseU_ = 0; upHoldM_ = 0.f;
        mute_ = 1.f; dualG_ = 0.f; uniMix_ = params_.unison ? 1.f : 0.f; T_ = 500.f;
    }

    int   context() const { return params_.unison ? kCtxUnison : ctx_.context(); }

    // diagnostics for tests
    float subFreq() const { return params_.unison ? uni_.subHz() : main_.subHz(); }
    float synthEnv() const { return params_.unison ? uni_.env() : main_.env(); }
    float residual() const { return ctx_.residual(); }
    int   lastEvent() const { return ctx_.lastEvent(); }
    float muteGain() const { return mute_; }

    // inTrack/inDry may alias; outputs may alias inputs (processed per sample)
    void process(const float* inTrack, const float* inDry, float* out, float* bassOut, uint32_t n) {
        ScopedFlushDenormals ftz;
        const Params& p = params_;
        const float gSynth = dbToGain(p.synthDb), gOct = dbToGain(p.octDb);
        const float gDry = dbToGain(p.dryDb), gBass = dbToGain(p.bassOutDb);
        const float tone = std::min(std::max(p.synthTone, 0.f), 1.f);
        const float toneNorm = 1.f / (1.f + 0.9f * tone);
        const float gritK = 1.f + 11.f * p.octGrit * p.octGrit;
        Core::TickIn in;
        in.thr   = dbToGain(p.thresholdDb);
        in.cutHz = midiToHz(p.cutoffNote + 0.5f);
        in.gritK = gritK; in.gritN = 1.f / std::tanh(gritK);
        in.tone  = tone;  in.octave = 1;
        const float fsM = fs_ / (float)D_, fsU = fs_ / (float)Du_;
        in.cAtt = onePoleCoef(std::max(p.attackMs, 0.3f), fsM);
        in.cRel = onePoleCoef(std::max(p.releaseMs, 5.f), fsM);
        Core::TickIn inU = in;
        inU.cutHz = 1e9f;                            // unison: no range limit
        inU.octave = p.unisonOct == kUniMinus1 ? 1 : p.unisonOct == kUniMinus2 ? 2 : 0;
        inU.cAtt = onePoleCoef(std::max(p.attackMs, 0.3f), fsU);
        inU.cRel = onePoleCoef(std::max(p.releaseMs, 5.f), fsU);

        if (!gainsInit_) { sSynth_ = gSynth; sOct_ = gOct; sDry_ = gDry; sBass_ = gBass; gainsInit_ = true; }
        const float gs = gainSmooth_;
        const float uniT = p.unison ? 1.f : 0.f;
        const bool smart = p.mode == kSmartMono && !p.unison;
        const bool dual = p.mode == kDual && !p.unison;
        const float Df = (float)D_, Duf = (float)Du_;

        for (uint32_t i = 0; i < n; ++i) {
            const float xt = inTrack[i];
            const float xd = p.dualInput ? inDry[i] : xt;
            sSynth_ += gs * (gSynth - sSynth_); sOct_ += gs * (gOct - sOct_);
            sDry_ += gs * (gDry - sDry_);       sBass_ += gs * (gBass - sBass_);
            uniMix_ += aMute_ * (uniT - uniMix_);

            // main core (context detection and dual need it; idle in full unison)
            const bool mainRun = !p.unison || uniMix_ < 0.999f;
            const float xl = decim_.process(xt);
            float upM = 0.f;
            if (phase_ == 0 && mainRun) {
                float sy, oc;
                main_.tick(xl, in, sy, oc);
                ctx_.tick(main_.hpOut(), main_.noteValid(), main_.noteHzFast(), main_.noteHz(), in.thr, p.melodySens);
                upM = (sSynth_ * sy * toneNorm + sOct_ * oc) * Df;
            }
            if (++phase_ >= D_) phase_ = 0;
            float bass = interp_.process(upM);

            // smart mono: fast single-note line -> bass off
            const float muteT = (smart && ctx_.melody()) ? 0.f : 1.f;
            mute_ += aMute_ * (muteT - mute_);
            bass *= mute_;

            // unison core (only while enabled or fading out)
            if (p.unison || uniMix_ > 1e-4f) {
                const float xu = decimU_.process(xt);
                float upU = 0.f;
                if (phaseU_ == 0) {
                    float sy, oc;
                    uni_.tick(xu, inU, sy, oc);
                    upU = (sSynth_ * sy * toneNorm + sOct_ * oc) * Duf;
                }
                if (++phaseU_ >= Du_) phaseU_ = 0;
                bass += uniMix_ * (interpU_.process(upU) - bass);
            }

            // dual: remove the tracked low note (and its harmonics) from the guitar
            float dry = xd;
            if (p.mode == kDual) {
                const bool active = dual && main_.noteValid() && main_.gateOpen() > 0.5f;
                if (main_.noteValid()) {
                    Tref_ = std::min(fs_ / std::max(main_.noteHz(), 30.f), (float)(CombNotch::kBuf - 4));
                    if (std::fabs(Tref_ - T_) > 0.03f * T_) T_ = Tref_;    // new note: jump
                }
                const float notched = comb_.process(xd, T_, main_.noteValid(), Tref_);
                dualG_ += aDual_ * ((active ? 1.f : 0.f) - dualG_);
                dry = xd + dualG_ * (notched - xd);
            }

            const float o = sDry_ * dry + (p.mainGuitarOnly ? 0.f : bass);
            if (bassOut) bassOut[i] = sBass_ * bass;
            out[i] = o;
        }
    }

private:
    void configure() {
        main_.configure(params_.lowLatency, params_.instrument, params_.tuning);
        uni_.configure(params_.lowLatency, params_.instrument, params_.tuning);
    }

    Params params_;
    float fs_ = 48000.f;
    int   D_ = 8, Du_ = 4, phase_ = 0, phaseU_ = 0;
    Core  main_, uni_;
    ContextDetector ctx_;
    CombNotch comb_;
    ButterLP4 decim_, interp_, decimU_, interpU_;
    float sSynth_ = 0, sOct_ = 0, sDry_ = 0, sBass_ = 0, gainSmooth_ = 0.01f;
    bool  gainsInit_ = false;
    float mute_ = 1.f, aMute_ = 0.f, dualG_ = 0.f, aDual_ = 0.f, uniMix_ = 0.f, T_ = 500.f, Tref_ = 500.f, aT_ = 0.f;
    float upHoldM_ = 0.f;
};

}  // namespace rootline
