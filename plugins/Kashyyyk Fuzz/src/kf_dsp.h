// Kashyyyk Fuzz - the real-time engine.
//
// From Hydra Eel (plugins/Hydra Eel/src/hydra_dsp.h, same author, MIT): the polyphase half-band oversampler
// (Oversampler4x), the one-pole and DC-block filters. ADAA (first-order anti-derivative anti-aliasing) follows
// Hydra's Adaa, here on tabulated curves.
//
// Per sample (mono):  guitar / volume / input-cap network (exact IIR, base rate)
//                     -> 4x up -> 1-3 stages of the chosen circuit family (each a tabulated exact solve with ADAA, its
//                        part's top-end roll-off and the coupling cap into the next), at the battery voltage Sag leaves
//                     -> diode clipper to ground (Face / Bender / Octave) -> Feedback loop -> 4x down
//                     -> output coupling (10n into the 500k Level pot: 32 Hz) -> tone stack -> level trim
// Dual: two paths in parallel (as Face and as Op-Amp), mixed by Dual Mix.
#pragma once

#include "kf_circuit.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

namespace kf {

// ---- from Hydra Eel ------------------------------------------------------------------------------------------
template <int L>
struct HalfBand
{
    static constexpr int N = L + 1;
    static constexpr int H = N <= 8 ? 8 : (N <= 16 ? 16 : 32);
    float h[N] = {};
    float hx[2 * H] = {}, we[2 * H] = {}, wo[2 * H] = {};
    int px = 0, pw = 0;
    static double i0(double x) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
    void prepare(double beta)
    {
        for (int i = 0; i < N; ++i)
        {
            const int k = 2 * i - L;
            const double sinc = std::sin(3.141592653589793 * k / 2.0) / (3.141592653589793 * k);
            const double r = (double)k / (L + 1);
            h[i] = (float)(sinc * i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(beta));
        }
        reset();
    }
    void reset() { std::memset(hx, 0, sizeof hx); std::memset(we, 0, sizeof we); std::memset(wo, 0, sizeof wo); px = pw = 0; }
    static void put(float* buf, int& p, float x) { p = (p - 1) & (H - 1); buf[p] = x; buf[p + H] = x; }
    float fir(const float* v) const { float s = 0.0f; for (int i = 0; i < N / 2; ++i) s += h[i] * (v[i] + v[N - 1 - i]); return s; }
    void up(float x, float& a, float& b) { put(hx, px, x); const float* v = hx + px; a = 2.0f * fir(v); b = v[(L - 1) / 2]; }
    float down(float a, float b) { put(we, pw, a); wo[pw] = b; wo[pw + H] = b; return fir(we + pw) + 0.5f * wo[pw + (L + 1) / 2]; }
};
struct Oversampler4x
{
    HalfBand<31> s1;
    HalfBand<7> s2;
    HalfBand<3> s3;                        // the third stage for 8x (Hydra Eel)
    void prepare() { s1.prepare(8.5); s2.prepare(6.0); s3.prepare(5.0); }
    void reset() { s1.reset(); s2.reset(); s3.reset(); }
    void up8(float x, float* o8) { float o4[4]; up(x, o4); for (int i = 0; i < 4; ++i) s3.up(o4[i], o8[2 * i], o8[2 * i + 1]); }
    float down8(const float* o8) { float o4[4]; for (int i = 0; i < 4; ++i) o4[i] = s3.down(o8[2 * i], o8[2 * i + 1]); return down(o4); }
    void up(float x, float* o4) { float a, b; s1.up(x, a, b); s2.up(a, o4[0], o4[1]); s2.up(b, o4[2], o4[3]); }
    float down(const float* o4) { const float a = s2.down(o4[0], o4[1]), b = s2.down(o4[2], o4[3]); return s1.down(a, b); }
    static constexpr int kLatency = 35;    // base-rate samples
};
struct OnePole { float z = 0.0f, a = 0.1f; float lp(float x) { z += (x - z) * a; if (std::fabs(z) < 1e-25f) z = 0.0f; return z; } void reset() { z = 0.0f; } };
struct DcBlock { float x1 = 0.0f, y1 = 0.0f, r = 0.995f; float run(float x) { float y = x - x1 + r * y1; if (std::fabs(y) < 1e-25f) y = 0.0f; x1 = x; y1 = y; return y; } void reset() { x1 = y1 = 0.0f; } };
inline float lpCoef(double hz, double sr) { return (float)(1.0 - std::exp(-2.0 * 3.14159265358979 * std::min(hz, 0.45 * sr) / sr)); }

// ---- tabulated curves on a warped axis ---------------------------------------------------------------------------
// u = x / (|x| + x0) is uniform in the table: dense near zero (where a transistor curve bends within millivolts),
// sparse far out (where it is flat). F is the antiderivative for ADAA.
constexpr int kTab = 257;
struct Curve
{
    float y[kTab], F[kTab], xs[kTab], slope[kTab];      // slope: dy/dx inside each cell (the curve is linear in x there)
    float I[kTab];                                      // the integral over each cell (small numbers: no cancellation)
    float x0 = 0.05f, umax = 0.95f, step = 0.0f, inv = 0.0f, vmax = 2.0f;
    static float xAt(int i, float x0, float umax) { const float u = -umax + 2.0f * umax * (float)i / (kTab - 1); return x0 * u / (1.0f - std::fabs(u)); }
    void setAxis(float vmax_, float x0_)
    {
        vmax = vmax_; x0 = x0_; umax = vmax / (vmax + x0); step = 2.0f * umax / (kTab - 1); inv = 1.0f / step;
        for (int i = 0; i < kTab; ++i) xs[i] = xAt(i, x0, umax);
    }
    // the antiderivative of the piecewise-linear curve (exact: trapezoids on the true x positions; F(0) = 0)
    void integrate()
    {
        for (int i = 0; i + 1 < kTab; ++i) slope[i] = (y[i + 1] - y[i]) / std::max(1e-12f, xs[i + 1] - xs[i]);
        slope[kTab - 1] = 0.0f;
        const int m = (kTab - 1) / 2;
        F[m] = 0.0f;
        for (int i = m + 1; i < kTab; ++i) F[i] = F[i - 1] + 0.5f * (y[i] + y[i - 1]) * (xAt(i, x0, umax) - xAt(i - 1, x0, umax));
        for (int i = m - 1; i >= 0; --i) F[i] = F[i + 1] - 0.5f * (y[i] + y[i + 1]) * (xAt(i + 1, x0, umax) - xAt(i, x0, umax));
        for (int i = 0; i + 1 < kTab; ++i) I[i] = (float)(0.5 * ((double)y[i] + y[i + 1]) * ((double)xs[i + 1] - xs[i]));
        I[kTab - 1] = 0.0f;
    }
    // A position on the curve: the cell (-1 below the table, kTab - 1 above it, where the curve continues flat) and the
    // offset from the cell's start. Positions depend only on the axis, so a rebuilt curve keeps them valid.
    struct Loc { int i = (kTab - 1) / 2; float d = 0.0f; };
    inline Loc loc(float x) const
    {
        Loc l;
        if (x >= vmax) { l.i = kTab - 1; l.d = x - xs[kTab - 1]; return l; }
        if (x <= -vmax) { l.i = -1; l.d = x - xs[0]; return l; }
        pos(x, l.i); l.d = x - xs[l.i];
        return l;
    }
    inline float g(const Loc& l) const { return l.i < 0 ? y[0] * l.d : l.d * (y[l.i] + 0.5f * slope[l.i] * l.d); }   // F(x) - F(cell start)
    // ADAA from the previous input (b, x1) to this one (a, x): the mean of the curve between them, exactly.
    //   same cell: the curve is linear there, so the mean is its value at the midpoint (no division at all)
    //   neighbouring cells: the rest of the lower cell plus the start of the upper one (small numbers only)
    //   further apart: the antiderivative difference (the step is at least a cell wide, so float is ample)
    inline float adaa(const Loc& a, float x, const Loc& b, float x1) const
    {
        if (a.i == b.i) return a.i < 0 ? y[0] : y[a.i] + 0.5f * slope[a.i] * (a.d + b.d);
        const bool up = a.i > b.i;
        const Loc& hi = up ? a : b; const Loc& lo = up ? b : a;
        const float xh = up ? x : x1, xl = up ? x1 : x;
        if (hi.i == lo.i + 1)
        {
            const float rest = lo.i < 0 ? -lo.d * y[0]
                                        : (xs[lo.i + 1] - xl) * (y[lo.i] + 0.5f * slope[lo.i] * ((xs[lo.i + 1] - xs[lo.i]) + lo.d));
            return (rest + g(hi)) / (xh - xl);
        }
        const float Fh = (hi.i < 0 ? F[0] : F[hi.i]) + g(hi), Fl = (lo.i < 0 ? F[0] : F[lo.i]) + g(lo);
        return (Fh - Fl) / (xh - xl);
    }
    inline float pos(float x, int& i) const
    {
        const float u = x / (std::fabs(x) + x0);
        float p = (u + umax) * inv;
        p = p < 0.0f ? 0.0f : (p > (float)(kTab - 1) - 1e-4f ? (float)(kTab - 1) - 1e-4f : p);
        i = (int)p;
        return p - (float)i;
    }
    inline float f(float x) const
    {
        if (x >= vmax) return y[kTab - 1];
        if (x <= -vmax) return y[0];
        int i; pos(x, i);
        return y[i] + slope[i] * (x - xs[i]);
    }
    inline float Fa(float x) const     // antiderivative, continued linearly beyond the table
    {
        if (x >= vmax) return F[kTab - 1] + y[kTab - 1] * (x - vmax);
        if (x <= -vmax) return F[0] + y[0] * (x + vmax);
        int i; pos(x, i);
        const float d = x - xs[i];                 // exact inside the cell: F' = f everywhere, so ADAA sees no steps
        return F[i] + d * (y[i] + 0.5f * slope[i] * d);
    }
};
struct Adaa
{
    float x1 = 0.0f, F1 = 0.0f;
    void reset(const Curve& c) { x1 = 0.0f; F1 = c.Fa(0.0f); }
    inline float run(const Curve& c, float x)
    {
        const float Fx = c.Fa(x), dx = x - x1;
        const float y = std::fabs(dx) > 1e-5f ? (Fx - F1) / dx : c.f(0.5f * (x + x1));
        x1 = x; F1 = Fx;
        return y;
    }
};

// ---- table set (generated offline by gen/gen_tables.cpp into kf_tables.inc) ---------------------------------------
constexpr int kFz = 6, kRsN = 5, kArr = 4, kNumCaps = 4, kNumTones = 5, kVccN = 13;
inline const double kFzGrid[kFz] = {0.0, 0.2, 0.4, 0.6, 0.8, 1.0};
inline const double kRsGrid[kRsN] = {1000.0, 4000.0, 15000.0, 50000.0, 150000.0};
inline const double kVccGrid[kVccN] = {3.0, 3.5, 4.0, 4.5, 5.0, 5.5, 6.0, 6.5, 7.0, 7.5, 8.0, 8.5, 9.0};   // the battery, starved (Sag) to fresh
constexpr double kStageRs = 10000.0;                                    // what a stage sees from the stage before
constexpr float kFfVmax = 2.0f, kFfX0 = 0.02f, kDiVmax = 10.0f, kDiX0 = 0.3f;
constexpr float kCeVmax = 10.0f, kCeX0 = 0.02f, kOpVmax = 10000.0f, kOpX0 = 0.05f, kReVmax = 10.0f, kReX0 = 0.05f;
// 8x oversampling for the Octave and Dual families on 64-bit hosts (the MPC); the 32-bit Force runs everything at 4x
#if (defined(__arm__) && !defined(__aarch64__)) || defined(KF_FORCE_4X)     // (KF_FORCE_4X: test the Force's 4x on a desktop)
constexpr bool kAllow8x = false;
#else
constexpr bool kAllow8x = true;
#endif
enum ToneType { kToneMuff = 0, kToneFlat = 1, kToneMid = 2, kToneTilt = 3, kToneBypass = 4 };
enum Topology { kFace = 0, kBender = 1, kMuff = 2, kOpAmp = 3, kOctave = 4, kDual = 5 };
constexpr int kNumTopologies = 6;
enum StageKind { kSFF = 0, kSCE, kSMuff, kSOp, kSRect };
constexpr int kNumCouplings = 6;
inline const double kCouplingF[kNumCouplings] = {10e-9, 47e-9, 100e-9, 1e-6, 10e-6, 100e-6};
inline const bool kCouplingElectrolytic[kNumCouplings] = {false, false, false, true, true, true};
inline const char* const kCouplingNames[kNumCouplings] = {"10n film", "47n film", "100n film", "1u elec", "10u elec", "100u elec"};

struct Tables
{
    float ff[kNumTransistors][kFz][kRsN][kVccN][kTab];   // Fuzz Face: Q2 collector (AC volts) vs source volts
    float rinFF[kNumTransistors][kFz];                    // its small-signal input resistance
    float ce[kNumTransistors][kVccN][kTab];               // booster / Tone Bender stage (no diodes)
    float rinCE[kNumTransistors];
    float muff[kNumTransistors][kNumDiodes][kArr][kVccN][kTab];   // Big Muff stage: diodes in the feedback
    float rinMuff[kNumTransistors][kNumDiodes][kArr];
    float di[kNumDiodes][kArr][kTab];                     // the diode clipper to ground (after the stages)
    float op[kNumDiodes][kArr][kVccN][kTab];              // op-amp stage: diodes in the feedback, then the rails
    float rect[kNumDiodes][kTab];                         // the octave rectifier
    float vdieFF[kNumTransistors], vdieCE[kNumTransistors], vdieMuff[kNumTransistors];   // where each stage stops working
    // the level-match estimate, measured offline (the worker refines it at run time): circuit x stages x transistor x
    // diode x arrangement at the reference tone stack / input cap / coupling, times a factor for each of those
    float est[kNumTopologies][3][kNumTransistors][kNumDiodes][kArr];
    float estTone[kNumTones], estCap[kNumCaps], estCoupling[kNumCouplings];
};
constexpr int kEstTone = kToneTilt, kEstCap = 3, kEstCoupling = 4;      // the reference settings of the estimate


// ---- the engine ----------------------------------------------------------------------------------------------------
struct Settings
{
    int tr = 0, tr2 = -1, di = 0, arr = kPair, cap = 3, tone = kToneMuff;     // tr2: the later stages' transistor (-1 = as tr)
    int topo = kFace, stages = 1, coupling = 4, resistors = 0;           // resistors: 0 metal film, 1 carbon comp
    float fuzz = 0.7f, toneKnob = 0.5f, guitarVol = 1.0f, sag = 0.0f, feedback = 0.0f, dualMix = 0.5f;
    bool levelFromPot = true, buffer = false;                            // buffer: a buffered pedal before the fuzz
    int q2() const { return tr2 < 0 ? tr : tr2; }
};
inline float levelEstimate(const Tables* T, const Settings& s)
{
    return T->est[s.topo][s.stages - 1][s.tr][s.di][s.arr] * T->estTone[s.tone] * T->estCap[s.cap] * T->estCoupling[s.coupling];
}

// the stage sequence of each circuit family (the first `stages` of them are used; Octave adds its preamp + rectifier)
inline const int kSequence[kNumTopologies][3] = {
    {kSFF, kSCE, kSCE},       // Face: the Fuzz Face pair, then single-transistor stages
    {kSCE, kSFF, kSCE},       // Bender: a transistor stage into the Fuzz Face pair (Tone Bender Mk II style)
    {kSMuff, kSMuff, kSMuff}, // Muff: transistor stages with diodes in the feedback
    {kSOp, kSOp, kSOp},       // Op-Amp: op-amp stages with diodes in the feedback
    {kSFF, kSCE, kSCE},       // Octave: (preamp + rectifier) then as Face
    {kSFF, kSCE, kSCE},       // Dual: path A as Face, path B as Op-Amp
};

// One path of stages at the oversampled rate.
// Each stage holds the curves at the two battery voltages either side of the present one and blends them per sample
// (weight t). The ADAA antiderivative blends with the same weight at both ends of the step, so a moving battery - or a
// rebuilt curve, whose stored antiderivative is re-taken from the new curve - never makes a spike.
struct Chain
{
    const Tables* T = nullptr;
    double fs4 = 176400.0;
    int kinds[5] = {0, 0, 0, 0, 0}, n = 0;      // the stage list (Octave: preamp, rectifier, then the stages)
    bool groundClip = true, hasFF = false;
    Curve curve[5][2];                          // [stage][battery below, battery above]
    float x1[5] = {}; Curve::Loc l1[5];          // ADAA state: the last input and its position (shared by both curves)
    OnePole top[5], opLp[5];                    // the part's top-end roll-off; an op-amp's gain-bandwidth limit
    float hpA[5] = {0.999f, 0.999f, 0.999f, 0.999f, 0.999f}, hpX[5] = {}, hpY[5] = {};   // coupling caps (one-pole high-pass)
    float tol[5] = {1, 1, 1, 1, 1};              // carbon comp tolerance (fixed per stage)
    Curve gclip; float gx1 = 0.0f; Curve::Loc gl1;
    float pre = 1.0f, opGain[5] = {1, 1, 1, 1, 1};
    // feedback: a resonant band-pass from the output back to the first stage
    float fbGain = 0.0f, fbY = 0.0f, bp1 = 0.0f, bp2 = 0.0f, bpF = 0.05f, bpQ = 0.3f;
    // carbon comp noise
    bool carbon = false; uint32_t rng = 0x1234567u;
    // the starved supply: the grid cell and the weight within it
    int j0 = kVccN - 2, j0Built = -1; float tv = 1.0f;
    float fuzzBuilt = -1.0f, rsBuilt = -1.0f;
    int trBuilt = -1, tr2Built = -1, diBuilt = -1, arrBuilt = -1;
    int part[5] = {0, 0, 0, 0, 0};             // each stage's transistor: Q1 for the first transistor stage, Q2 after it

    void setup(const Tables* t, double fsOs, int topo, bool pathB, int stages)
    {
        T = t; fs4 = fsOs;                         // the oversampled rate (4x, or 8x for Octave and Dual)
        n = 0;
        if (topo == kOctave) { kinds[n++] = kSCE; kinds[n++] = kSRect; }
        for (int k = 0; k < stages; ++k) kinds[n++] = pathB ? kSOp : kSequence[topo][k];
        hasFF = false; for (int k = 0; k < n; ++k) hasFF = hasFF || kinds[k] == kSFF;
        groundClip = !(pathB || topo == kMuff || topo == kOpAmp);
        j0Built = -1;
        fuzzBuilt = rsBuilt = -1.0f; trBuilt = tr2Built = diBuilt = arrBuilt = -1;
        const double w = 2.0 * 3.14159265358979 * 1400.0 / fs4;            // feedback resonance 1.4 kHz
        bpF = (float)(2.0 * std::sin(0.5 * w)); bpQ = 1.0f / 3.0f;
        // carbon comp: +/-10 % parts (fixed per stage, as on one board); metal film is exact
        for (int k = 0; k < 5; ++k) { static const float off[5] = {1.11f, 0.89f, 1.09f, 0.92f, 1.12f}; tol[k] = off[k]; }
        reset();
    }
    void resync(int k) { l1[k] = curve[k][1].loc(x1[k]); }      // (the axis may have changed with the stage kind)
    void reset()
    {
        for (int k = 0; k < 5; ++k) { top[k].reset(); opLp[k].reset(); hpX[k] = hpY[k] = 0.0f; x1[k] = 0.0f; }
        fbY = bp1 = bp2 = 0.0f;
        for (int k = 0; k < n; ++k) resync(k);
        gx1 = 0.0f; gl1 = gclip.loc(0.0f);
    }
    static void blendV(const float* const* src, const float* w, int m, float* out) { for (int i = 0; i < kTab; ++i) { float s = 0; for (int j = 0; j < m; ++j) s += w[j] * src[j][i]; out[i] = s; } }
    // the battery: picks the grid cell (rebuilding the curves only when it changes cell) and the weight. Hysteresis:
    // the present cell is kept while the battery is within a fifth of a step outside it (a short linear extension),
    // so a battery hovering on a grid voltage does not rebuild the curves over and over.
    void setVcc(float vcc)
    {
        vcc = std::max((float)kVccGrid[0], std::min((float)kVccGrid[kVccN - 1], vcc));
        if (j0Built >= 0)
        {
            const float t = (float)((vcc - kVccGrid[j0]) / (kVccGrid[j0 + 1] - kVccGrid[j0]));
            if (t > -0.2f && t < 1.2f) { tv = t; return; }
        }
        int j = 0; while (j < kVccN - 2 && vcc > kVccGrid[j + 1]) ++j;
        j0 = j;
        tv = (float)((vcc - kVccGrid[j]) / (kVccGrid[j + 1] - kVccGrid[j]));
    }
    void buildStage(int k, const Settings& s, double Rs)
    {
        for (int side = 0; side < 2; ++side)
        {
            Curve& c = curve[k][side];
            const int jv = j0 + side;
            switch (kinds[k])
            {
                case kSFF:
                {
                    c.setAxis(kFfVmax, kFfX0);
                    float fz = std::max(0.0f, std::min(1.0f, s.fuzz)) * (kFz - 1);
                    const int i0 = std::min(kFz - 2, (int)fz); const float tf = fz - (float)i0;
                    const double rs = k == 0 ? Rs : kStageRs;
                    const double lr = std::log(std::max(kRsGrid[0], std::min(kRsGrid[kRsN - 1], rs)));
                    int r0 = 0; while (r0 < kRsN - 2 && lr > std::log(kRsGrid[r0 + 1])) ++r0;
                    const float tr = (float)((lr - std::log(kRsGrid[r0])) / (std::log(kRsGrid[r0 + 1]) - std::log(kRsGrid[r0])));
                    const float* src[4]; float w[4]; int m = 0;
                    for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b)
                    { src[m] = T->ff[part[k]][i0 + a][r0 + b][jv]; w[m++] = (a ? tf : 1 - tf) * (b ? tr : 1 - tr); }
                    blendV(src, w, m, c.y);
                    break;
                }
                case kSCE: c.setAxis(kCeVmax, kCeX0); std::memcpy(c.y, T->ce[part[k]][jv], sizeof c.y); break;
                case kSMuff: c.setAxis(kCeVmax, kCeX0); std::memcpy(c.y, T->muff[part[k]][s.di][s.arr][jv], sizeof c.y); break;
                case kSOp: c.setAxis(kOpVmax, kOpX0); std::memcpy(c.y, T->op[s.di][s.arr][jv], sizeof c.y); break;
                default: c.setAxis(kReVmax, kReX0); std::memcpy(c.y, T->rect[s.di], sizeof c.y); break;
            }
            c.integrate();
        }
        resync(k);
    }
    // rebuild the stage curves when a part, the fuzz, the source or the battery's grid cell changed
    void update(const Settings& s, double Rs, float vcc)
    {
        const bool parts = s.tr != trBuilt || s.q2() != tr2Built || s.di != diBuilt || s.arr != arrBuilt;
        if (parts)
        {
            bool first = true;
            for (int k = 0; k < n; ++k)
            {
                if (kinds[k] == kSRect || kinds[k] == kSOp) { part[k] = s.tr; continue; }
                part[k] = first ? s.tr : s.q2(); first = false;
            }
        }
        const bool knobs = std::fabs(s.fuzz - fuzzBuilt) > 1e-4f || std::fabs((float)Rs - rsBuilt) > 1.0f;
        setVcc(vcc);
        const bool cell = j0 != j0Built;
        for (int k = 0; k < n; ++k)
            if (parts || cell || (kinds[k] == kSFF && knobs)) buildStage(k, s, Rs);
        j0Built = j0;
        if (parts || gclip.vmax != kDiVmax)
        {
            gclip.setAxis(kDiVmax, kDiX0);
            std::memcpy(gclip.y, T->di[s.di][s.arr], sizeof gclip.y);
            gclip.integrate();
            gl1 = gclip.loc(gx1);
        }
        if (parts || knobs)
        {
            for (int k = 0; k < n; ++k) top[k].a = lpCoef(kinds[k] == kSOp ? 20000.0 : (kinds[k] == kSRect ? 20000.0 : kTransistors[part[k]].topHz), fs4);
            // the fuzz: the Fuzz pot where there is a Fuzz Face; otherwise the drive into the first stage
            // (an op-amp path takes the fuzz as the first op-amp's gain instead)
            // (an octave: its preamp drives the rectifier hard, as in an Octavia: a fixed +18 dB, so the rectifier
            // conducts whatever the diode - an LED needs about 1.8 V)
            const bool octave = n > 1 && kinds[1] == kSRect;
            pre = octave ? 7.94f : ((hasFF || kinds[0] == kSOp) ? 1.0f : std::pow(10.0f, (-12.0f + 36.0f * s.fuzz) / 20.0f));
            int firstOp = -1; for (int k = 0; k < n && firstOp < 0; ++k) if (kinds[k] == kSOp) firstOp = k;
            for (int k = 0; k < n; ++k)
            {
                opGain[k] = kinds[k] != kSOp ? 1.0f : (k == firstOp ? std::pow(10.0f, 0.5f + 2.5f * s.fuzz) : 20.0f);
                // a TL072-class op-amp: 3 MHz gain-bandwidth, so the closed-loop bandwidth is 3 MHz / gain
                opLp[k].a = lpCoef(std::min(0.45 * fs4, 3.0e6 / std::max(1.0f, opGain[k])), fs4);
            }
        }
        fuzzBuilt = s.fuzz; rsBuilt = (float)Rs; trBuilt = s.tr; tr2Built = s.q2(); diBuilt = s.di; arrBuilt = s.arr;
        // coupling caps between the stages: 1 / (2 pi C (Rout + Rin)), Rout + Rin about 20k; electrolytics +20 % (tolerance)
        const double C = kCouplingF[s.coupling] * (kCouplingElectrolytic[s.coupling] ? 1.2 : 1.0);
        const double fc = 1.0 / (2.0 * 3.14159265358979 * C * 20000.0);
        const float a = (float)std::exp(-2.0 * 3.14159265358979 * fc / fs4);
        for (int k = 0; k < 5; ++k) hpA[k] = a;
        carbon = s.resistors == 1;
        fbGain = 2.5f * std::pow(std::max(0.0f, s.feedback), 1.5f);
    }
    // the battery moved (control rate): a new weight; the curves are rebuilt only when it crosses into another cell
    void moveVcc(float vcc, const Settings& s, double Rs)
    {
        setVcc(vcc);
        if (j0 != j0Built) { for (int k = 0; k < n; ++k) buildStage(k, s, Rs); j0Built = j0; }
    }
    inline float noise() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (float)((int32_t)rng) * (1.0f / 2147483648.0f); }
    // one stage through ADAA on the battery-blended curve
    inline float stage(int k, float x)
    {
        const Curve& c1 = curve[k][1];
        const Curve::Loc a = c1.loc(x);
        float y = c1.adaa(a, x, l1[k], x1[k]);
        if (tv != 1.0f) y = (1.0f - tv) * curve[k][0].adaa(a, x, l1[k], x1[k]) + tv * y;   // (a fresh battery: one curve)
        x1[k] = x; l1[k] = a;
        return y;
    }
    // one oversampled sample through the path (volts in, volts out)
    inline float run(float x)
    {
        float v = x * pre + fbY;
        for (int k = 0; k < n; ++k)
        {
            if (k > 0)
            {   // the coupling capacitor into this stage
                const float y = v - hpX[k] + hpA[k] * hpY[k];
                hpX[k] = v; hpY[k] = std::fabs(y) < 1e-25f ? 0.0f : y; v = hpY[k];
            }
            // carbon comp excess noise in the bias network (at the stage input, so the stage amplifies it: hiss in
            // the pauses, grit on the notes)
            if (carbon) v += noise() * (3e-5f + 2e-3f * std::fabs(v));
            float in = v * opGain[k];
            if (kinds[k] == kSOp) in = opLp[k].lp(in);
            if (carbon) in *= tol[k];
            float y = stage(k, in);
            y = top[k].lp(y);
            if (carbon) y += noise() * (1e-4f + 1.2e-2f * std::fabs(y));    // excess noise: rises with the current (signal)
            v = y;
        }
        if (groundClip)
        {
            const Curve::Loc a = gclip.loc(v);
            const float y = gclip.adaa(a, v, gl1, gx1);
            gx1 = v; gl1 = a; v = y;
        }
        if (fbGain > 0.0f)
        {   // the feedback: a state-variable band-pass on the output, fed into the first stage next sample
            const float hp = v - bp1 * bpQ * 3.0f - bp2;
            bp1 += bpF * hp; bp2 += bpF * bp1;
            fbY = fbGain * bp1;
        }
        else fbY = 0.0f;
        return v;
    }
};

struct Core
{
    const Tables* T = nullptr;
    double fs = 44100.0;
    Guitar guitar;
    Iir gfilt, tfilt;
    Oversampler4x os;
    Chain A, B;
    DcBlock outHp;
    OnePole tiltLp;
    float trim = 1.0f, trimTarget = 1.0f;
    bool dual = false, os8 = false;
    int cTopo = -1, cStages = -1, cCap = -1, cTone = -1, cTr = -1; bool cBuf = false;
    // the safety limiter (Feedback can self-oscillate): a peak limiter with a ceiling a little under full scale
    float limEnv = 0.0f, limA = 0.5f, limR = 0.0001f;
    static constexpr float kCeiling = 0.9f;
    bool limit = true;
    float cGv = -1.0f, cTk = -1.0f, cRin = -1.0f; bool cLvl = true;
    // sag: the battery and its droop on loud notes
    float env = 0.0f, envA = 0.01f, envR = 0.0001f;
    int ctl = 0;
    float vccLast = 9.0f;          // the battery voltage now (for the read-out)
    Settings now;
    double RsNow = 6500.0;

    void prepare(double sr, const Tables* t)
    {
        fs = sr; T = t; os.prepare();
        outHp.r = (float)std::exp(-2.0 * 3.14159265358979 * 32.0 / fs);
        tiltLp.a = lpCoef(1000.0, fs);
        envA = (float)(1.0 - std::exp(-1.0 / (0.005 * 4.0 * fs))); envR = (float)(1.0 - std::exp(-1.0 / (0.15 * 4.0 * fs)));
        limA = (float)(1.0 - std::exp(-1.0 / (0.0002 * fs))); limR = (float)(1.0 - std::exp(-1.0 / (0.25 * fs)));
        cTopo = -1; reset();
    }
    void reset()
    {
        os.reset(); gfilt.reset(); tfilt.reset(); outHp.reset(); tiltLp.reset(); A.reset(); B.reset();
        env = 0.0f; ctl = 0; limEnv = 0.0f;
    }
    float inputR(const Settings& s) const
    {
        switch (s.topo == kOctave ? -1 : kSequence[s.topo][0])
        {
            case kSFF: return T->rinFF[s.tr][std::min(kFz - 1, (int)std::lround(s.fuzz * (kFz - 1)))];
            case kSMuff: return T->rinMuff[s.tr][s.di][s.arr];
            case kSOp: return 10000.0f;
            default: return T->rinCE[s.tr];
        }
    }
    // Sag: a starved battery. The knob spans 9 V down to just past the voltage where the circuit's first stage to
    // give up stops working (vdie, measured offline per part), so the whole travel is usable whatever the parts: the
    // first half is subtle (sag^1.8), the sputter - a level-dependent gate, quiet notes first - arrives near the top.
    // Loud notes also pull the battery down a little (droop).
    float vEnd = 3.0f;
    void setVEnd(const Settings& s)
    {
        float vd = 3.0f;
        auto die = [&](int kind, int part) {
            switch (kind) { case kSFF: vd = std::max(vd, T->vdieFF[part]); break; case kSCE: vd = std::max(vd, T->vdieCE[part]); break;
                            case kSMuff: vd = std::max(vd, T->vdieMuff[part]); break; default: break; } };
        for (int k = 0; k < A.n; ++k) die(A.kinds[k], A.part[k]);
        if (dual) for (int k = 0; k < B.n; ++k) die(B.kinds[k], B.part[k]);
        vEnd = std::max(3.0f, std::min(8.0f, vd + 0.15f));     // just above it: hard notes still break through
    }
    float vccFor(float sag) const
    {
        sag = std::max(0.0f, std::min(1.0f, sag));
        const float vbat = 9.0f - (9.0f - vEnd) * std::pow(sag, 1.8f);
        const float droop = 0.3f * sag * std::min(1.0f, env * 6.0f);
        return std::max(3.0f, std::min(9.0f, vbat - droop));
    }
    // once per block
    void update(const Settings& s)
    {
        const bool topoChange = s.topo != cTopo || s.stages != cStages;
        if (topoChange)
        {
            dual = s.topo == kDual;
            os8 = kAllow8x && (s.topo == kDual || s.topo == kOctave);     // the octave and the parallel paths alias most: 8x
            const double fo = fs * (os8 ? 8.0 : 4.0);
            A.setup(T, fo, s.topo, false, s.stages);
            if (dual) B.setup(T, fo, s.topo, true, s.stages);
            envA = (float)(1.0 - std::exp(-1.0 / (0.005 * fo))); envR = (float)(1.0 - std::exp(-1.0 / (0.15 * fo)));
            cTopo = s.topo; cStages = s.stages;
        }
        now = s;
        // a Fuzz Face first sees the guitar (pickup + volume pot); with a buffer in front, or any other first stage, it
        // sees a low, fixed source (the buffer's output / the stage before)
        RsNow = (s.topo == kOctave || kSequence[s.topo][0] != kSFF) ? kStageRs : (s.buffer ? kRsGrid[0] : guitar.sourceR(s.guitarVol));
        A.update(s, RsNow, vccLast); if (dual) B.update(s, RsNow, vccLast);     // (parts first: the dying voltage depends on them)
        setVEnd(s);
        const float vcc = vccFor(s.sag);
        vccLast = vcc;
        A.update(s, RsNow, vcc);
        if (dual) B.update(s, RsNow, vcc);
        const float rin = s.buffer ? 1e6f : inputR(s);            // a buffer's input loads the guitar with about 1M
        if (topoChange || s.tr != cTr || s.cap != cCap || std::fabs(s.guitarVol - cGv) > 1e-4f || s.levelFromPot != cLvl || std::fabs(rin - cRin) > 1.0f || s.buffer != cBuf)
        {
            guitar.design(gfilt, s.guitarVol, kInputCaps[s.cap], rin, RsNow, s.levelFromPot, fs);
            cTr = s.tr; cCap = s.cap; cGv = s.guitarVol; cLvl = s.levelFromPot; cRin = rin; cBuf = s.buffer;
        }
        if (s.tone != cTone || std::fabs(s.toneKnob - cTk) > 1e-4f) { designTone(s.tone, s.toneKnob); cTone = s.tone; cTk = s.toneKnob; }
    }
    void designTone(int type, float t)
    {
        switch (type)
        {
            case kToneMuff: muffStack(tfilt, t, fs); break;
            case kToneMid:
            {   // the Muff network's scoop filled by a resonant peak (the "mids" mod): +9 dB, Q 0.9, 400 Hz .. 2 kHz
                const double f0 = 400.0 * std::pow(5.0, (double)t), Ag = std::pow(10.0, 9.0 / 40.0), Q = 0.9;
                const double w = 2.0 * 3.14159265358979 * f0 / fs, al = std::sin(w) / (2.0 * Q), cw = std::cos(w);
                const double a0 = 1.0 + al / Ag;
                tfilt.order = 2;
                tfilt.b[0] = (1.0 + al * Ag) / a0; tfilt.b[1] = -2.0 * cw / a0; tfilt.b[2] = (1.0 - al * Ag) / a0;
                tfilt.a[0] = 1.0; tfilt.a[1] = -2.0 * cw / a0; tfilt.a[2] = (1.0 - al / Ag) / a0;
                break;
            }
            default: tfilt.order = 0; tfilt.b[0] = 1.0; break;
        }
        toneType = type; tiltT = t;
    }
    int toneType = kToneMuff; float tiltT = 0.5f;

    inline float process(float x)
    {
        const float g = (float)gfilt.run(x);
        float o8[8];
        const int m = os8 ? 8 : 4;
        if (os8) os.up8(g, o8); else os.up(g, o8);
        // the level match is applied at the circuit's output, before the downsampler and the tone stack, so their
        // filters only ever hold levelled signal (a switch never boosts the tail of the previous circuit)
        trim += (trimTarget - trim) * 0.0005f;          // (moves smoothly, about 50 ms; jumps on a switch)
        for (int q = 0; q < m; ++q)
        {
            float& v = o8[q];
            const float a = std::fabs(v);
            env += (a - env) * (a > env ? envA : envR);
            if ((now.sag > 0.0f || vccLast < 9.0f) && (++ctl & 15) == 0)
            {   // the supply droops with the playing level (control rate: a new blend weight, no rebuild)
                const float vcc = vccFor(now.sag);
                vccLast = vcc;
                A.moveVcc(vcc, now, RsNow);
                if (dual) B.moveVcc(vcc, now, RsNow);
            }
            float y = A.run(v);
            if (dual) y = (1.0f - now.dualMix) * y + now.dualMix * B.run(v);
            v = y * trim;
        }
        float y = os8 ? os.down8(o8) : os.down(o8);
        y = outHp.run(y);
        switch (toneType)
        {
            case kToneMuff: case kToneMid: y = (float)tfilt.run(y); break;
            case kToneFlat: y *= 0.5f; break;
            case kToneTilt:
            {
                const float lp = tiltLp.lp(y), hp = y - lp;
                y = lp * (0.2f + 1.6f * (1.0f - tiltT)) + hp * (0.2f + 1.6f * tiltT);
                break;
            }
            default: break;
        }
        if (limit)
        {   // the safety limiter: never louder than the ceiling (Feedback, Sag spits, a runaway combination)
            const float a = std::fabs(y);
            limEnv = a > limEnv ? a : limEnv + (a - limEnv) * limR;          // instant attack: nothing passes the ceiling
            if (limEnv > kCeiling) y *= kCeiling / limEnv;
            if (y > 1.0f) y = 1.0f; else if (y < -1.0f) y = -1.0f;
        }
        return std::fabs(y) < 1e-20f ? 0.0f : y;
    }
};

// Level match: the engine's level for these parts on four playing styles (a strummed chord, a single-note line, the
// chord 12 dB softer, single notes ringing out), at Fuzz 70 %, Tone 50 %, no Sag or Feedback, full guitar volume. Returns the trim to -20 dBFS.
inline float refChord(long t, double fs)
{
    static const double f[6] = {82.41, 110.0, 146.83, 196.0, 246.94, 329.63};
    const double tt = t / fs; double s = 0.0;
    for (int k = 0; k < 6; ++k)
    {
        const double age = tt - 0.02 * k; if (age < 0) continue;
        const double e = std::exp(-age * 2.5);
        for (int h = 1; h <= 6; ++h) s += e * std::sin(2.0 * 3.14159265358979 * f[k] * h * age) * (0.6 / (h * h));
    }
    return (float)(0.07 * s);
}
inline float refLine(long t, double fs)
{
    static const double notes[6] = {98.0, 130.81, 174.61, 220.0, 130.81, 293.66};
    const double tt = t / fs, len = 0.2; const int k = (int)(tt / len); const double age = tt - k * len, f = notes[k % 6];
    double s = 0.0;
    for (int h = 1; h <= 7; ++h) s += std::sin(2.0 * 3.14159265358979 * f * h * age) * (0.55 / (h * h)) * std::exp(-age * (3.0 + h * 0.8));
    return (float)(0.2 * s * std::min(1.0, age * 300.0));
}
inline float refPluck(long t, double fs)
{   // single notes left to ring out over a second (the decays are where gated and octave sounds lose level)
    static const double notes[3] = {123.47, 185.0, 98.0};
    const double tt = t / fs, len = 1.0; const int k = (int)(tt / len); const double age = tt - k * len, f = notes[k % 3];
    double s = 0.0;
    for (int h = 1; h <= 6; ++h) s += std::sin(2.0 * 3.14159265358979 * f * h * age) * (0.5 / (h * h)) * std::exp(-age * (2.5 + h * 0.7));
    return (float)(0.18 * s * std::min(1.0, age * 300.0));
}
inline float calibrate(const Tables* T, double fs, Settings s)
{
    s.fuzz = 0.7f; s.toneKnob = 0.5f; s.guitarVol = 1.0f; s.sag = 0.0f; s.feedback = 0.0f; s.levelFromPot = true;
    const int N = (int)(0.4 * fs);
    double sumDb = 0.0;
    for (int sig = 0; sig < 4; ++sig)
    {
        std::unique_ptr<Core> cp(new Core()); Core& c = *cp;
        c.prepare(fs, T); c.update(s); c.trim = c.trimTarget = 1.0f; c.limit = false;    // (limiter off: measure the raw level)
        double acc = 0.0; int n = 0;
        for (int i = 0; i < N; ++i)
        {
            const float x = sig == 0 ? refChord(i, fs) : (sig == 1 ? refLine(i, fs) : (sig == 2 ? 0.25f * refChord(i, fs) : refPluck(i, fs)));
            const float y = c.process(x);
            if (i > N / 5) { acc += (double)y * y; ++n; }
        }
        sumDb += 20.0 * std::log10(std::max(1e-9, std::sqrt(acc / std::max(1, n))));
    }
    return (float)std::pow(10.0, (-20.0 - sumDb / 4.0) / 20.0);
}

} // namespace kf
