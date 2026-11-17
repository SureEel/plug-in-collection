// hydra_dsp.h - the DSP building blocks of Hydra Eel (all original code; algorithms credited where
// they come from published work). Everything is allocation-free after prepare(), and nothing
// calls sin / exp / pow / log / tan per sample: those run at control rate (every 8-16 samples)
// or are replaced by cheap approximations.
#pragma once

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/noise.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace hydra {

using mpc::clamp;
using mpc::dsp::fastTanh;
using mpc::dsp::fastSin01;

constexpr float kPi = 3.14159265358979f;

// Cubic soft clipper: smooth, no division, saturates at +-1 from |x| = 1.5 (fewer high harmonics
// than tanh, so less aliasing). Used in every clipping and feedback path.
inline float sclip(float x) { if (x >= 1.5f) return 1.0f; if (x <= -1.5f) return -1.0f; return x - 0.148148f * x * x * x; }

// Antiderivative of sclip, for anti-derivative anti-aliasing (ADAA): continuous, polynomial.
inline float sclipF(float x)
{
    const float a = std::fabs(x);
    if (a >= 1.5f) return a - 0.5625f;
    const float x2 = x * x;
    return 0.5f * x2 - 0.037037f * x2 * x2;
}
// First-order ADAA: the output is the average of the clipping curve between the previous and the
// current input, (F(x) - F(x1)) / (x - x1), which suppresses the harmonics that would otherwise fold
// back from far above the (oversampled) Nyquist frequency.
struct Adaa
{
    float x1 = 0.0f, F1 = 0.0f;
    void reset() { x1 = F1 = 0.0f; }
    template <class Fn, class AntiFn>
    float run(float x, Fn f, AntiFn F)
    {
        const float Fx = F(x), dx = x - x1;
        const float y = std::fabs(dx) > 1e-4f ? (Fx - F1) / dx : f(0.5f * (x + x1));
        x1 = x; F1 = Fx;
        return y;
    }
};

inline float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }
inline float linToDb(float g) { return g > 1e-9f ? 20.0f * std::log10(g) : -180.0f; }
inline float coefMs(float ms, double sr) { return (float)(1.0 - std::exp(-1.0 / (std::max(0.01f, ms) * 0.001 * sr))); }
// one-pole low-pass coefficient for a cutoff in Hz (exact form, control rate only)
inline float lpCoef(float hz, double sr) { return (float)(1.0 - std::exp(-2.0 * 3.14159265358979 * clamp(hz, 5.0f, 0.45f * (float)sr) / sr)); }

// Smoothed value (one pole). Used for every audible setting so nothing zips or clicks.
struct Glide
{
    float v = 0.0f, coef = 0.01f;
    void setTime(float ms, double sr) { coef = coefMs(ms, sr); }
    float next(float target)
    {
        if (v == target) return v;                       // settled: nothing to do
        v += (target - v) * coef;
        if (std::fabs(target - v) < 1e-6f) v = target;
        return v;
    }
};

struct OnePole            // low-pass; hp(x) = x - lp(x)
{
    float z = 0.0f, a = 0.1f;
    float lp(float x) { z += (x - z) * a; return z; }
    void reset() { z = 0.0f; }
};

struct DcBlock
{
    float x1 = 0.0f, y1 = 0.0f, r = 0.995f;
    float run(float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
    void reset() { x1 = y1 = 0.0f; }
};

// Power-of-two ring buffer. tap(0) = the newest sample.
struct Ring
{
    std::vector<float> b;
    int mask = 0, w = 0;
    void prepare(int minLen) { int n = 16; while (n < minLen) n <<= 1; b.assign((size_t)n, 0.0f); mask = n - 1; w = 0; }
    void clear() { std::fill(b.begin(), b.end(), 0.0f); w = 0; }
    void push(float x) { b[(size_t)w] = x; w = (w + 1) & mask; }
    float tap(int d) const { return b[(size_t)((w - 1 - d) & mask)]; }
    // 4-point Hermite (Catmull-Rom) interpolation: smooth, no dulling when the delay moves
    float tapHermite(float d) const
    {
        const int i = (int)d;
        const float f = d - (float)i;
        const float xm1 = tap(i - 1), x0 = tap(i), x1 = tap(i + 1), x2 = tap(i + 2);
        const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
    float tapLin(float d) const
    {
        const int i = (int)d;
        const float f = d - (float)i;
        const float a = tap(i), c = tap(i + 1);
        return a + (c - a) * f;
    }
};

// Schroeder / lattice all-pass on a ring: w = x + g*z, y = z - g*w (z = the delayed w).
struct Allpass
{
    Ring r; int len = 1; float g = 0.5f;
    void prepare(int maxLen, int l, float gain) { r.prepare(maxLen + 4); len = l; g = gain; }
    float run(float x)
    {
        const float z = r.tap(len - 1);
        const float w = x + g * z;
        r.push(w);
        return z - g * w;
    }
    float runMod(float x, float d)   // fractional, modulated length
    {
        const float z = r.tapLin(d - 1.0f);
        const float w = x + g * z;
        r.push(w);
        return z - g * w;
    }
};

// ------------------------------------------------------------------------------------------------
// 2x half-band oversampler stage, polyphase (Kaiser-windowed sinc, taps at odd offsets -L..L).
// Half the taps are zero and one phase is a pure delay; symmetric taps halve the multiplies.
// Two stages in cascade give 4x.
// ------------------------------------------------------------------------------------------------
template <int L>
struct HalfBand
{
    static constexpr int N = L + 1;        // taps at odd offsets
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

// 4x: a 63-tap stage at the base rate (the critical one: -86 dB from 26 kHz, 0.001 dB ripple to 18 kHz)
// and a 15-tap stage at 2x (-62 dB where it matters: what would fold below 22 kHz).
// A third 7-tap stage at 4x makes 8x where it helps.
struct Oversampler4x
{
    HalfBand<31> s1;
    HalfBand<7> s2;
    HalfBand<3> s3;
    void prepare() { s1.prepare(8.5); s2.prepare(6.0); s3.prepare(5.0); }
    void reset() { s1.reset(); s2.reset(); s3.reset(); }
    void up(float x, float* o4) { float a, b; s1.up(x, a, b); s2.up(a, o4[0], o4[1]); s2.up(b, o4[2], o4[3]); }
    float down(const float* o4) { const float a = s2.down(o4[0], o4[1]), b = s2.down(o4[2], o4[3]); return s1.down(a, b); }
    void up8(float x, float* o8) { float o4[4]; up(x, o4); for (int i = 0; i < 4; ++i) s3.up(o4[i], o8[2 * i], o8[2 * i + 1]); }
    float down8(const float* o8) { float o4[4]; for (int i = 0; i < 4; ++i) o4[i] = s3.down(o8[2 * i], o8[2 * i + 1]); return down(o4); }
    static constexpr int kLatency = 35;    // base-rate samples, for the dry blend (31 + 3.5, rounded up)
};

// ------------------------------------------------------------------------------------------------
// Drive. Compressors (base rate) and fuzzes (4x oversampled). Mono.
// Types, cleanest to dirtiest: 0 opto (LA-2A style), 1 smooth (Ross style), 2 FET (1176 style),
// 3 punch (Dyna Comp style), 4 germanium (Fuzz Face style), 5 gated (Tone Bender style),
// 6 sustain (Big Muff style), 7 octave-up fuzz.
// ------------------------------------------------------------------------------------------------
struct Drive
{
    double sr = 44100.0;
    int type = 0;
    float amount = 0.5f, tone = 0.5f, bias = 0.0f;

    // ---- compressors: detector -> static curve (dB) -> attack / release in the gain-reduction
    // domain, at control rate (every 4 samples); the linear gain is interpolated per sample, so
    // nothing steps or ripples into the audio.
    float det = 0.0f, det2 = 0.0f, grDb = 0.0f, memory = 0.0f;
    float gNow = 1.0f, gStep = 0.0f, gate = 1.0f, gateT = 1.0f, grMin = 1.0f;
    float dAtt = 0.0f, dRel = 0.0f, aAtt = 0.0f, aRel = 0.0f, ratio = 3.0f, knee = 6.0f, thrDb = -20.0f, makeDb = 0.0f;
    float gAtt = 0.1f, gRel = 0.001f, memUp = 0.0f, memDn = 0.0f, relFast = 0.0f, relSlow = 0.0f;
    int ctl = 0;
    OnePole cTone, punchHp;

    // ---- fuzzes, at 4x
    Oversampler4x os;
    OnePole inHp, s1Hp, interLp, postLp, muffLp, muffHp, fzTone;
    DcBlock dc;
    float g1 = 1.0f, g2 = 1.0f, dz = 0.0f, offset = 0.0f, ceil_ = 1.0f, outTrim = 1.0f;
    // starve (Gate Bias on the fuzzes): s = bias^2.2, so the low end of the knob is subtle
    float starve = 0.0f, lvlEnv = 0.0f, envAtt = 0.1f, envRel = 0.001f, gSag = 1.0f, softDz = 0.0f, outEnv = 0.0f, outDz = 0.0f;
    OnePole octHp1, octHp2, octPre, rectHp, postLp2;

    void prepare(double s)
    {
        sr = s; os.prepare();
        gAtt = coefMs(1.0f, sr); gRel = coefMs(60.0f, sr);
        reset();
    }
    void reset()
    {
        det = det2 = grDb = memory = 0.0f; gNow = 1.0f; gStep = 0.0f; gate = gateT = 1.0f; grMin = 1.0f; ctl = 0;
        cTone.reset(); punchHp.reset(); os.reset();
        for (OnePole* f : {&inHp, &s1Hp, &interLp, &postLp, &muffLp, &muffHp, &fzTone}) f->reset();
        dc.reset(); a1.reset(); a2.reset(); a3.reset(); rectLp.reset();
        for (OnePole* f : {&octHp1, &octHp2, &octPre, &rectHp, &postLp2}) f->reset();
        lvlEnv = 0.0f; gSag = 1.0f; softDz = 0.0f; outEnv = 0.0f; outDz = 0.0f;
    }
    bool isComp() const { return type < 4; }

    void set(int t, float amt01, float tone01, float bias01)
    {
        type = t; amount = amt01; tone = tone01; bias = bias01;
        osFactor = kOs[t];
        const double sr4 = (double)osFactor * sr;              // the fuzz's oversampled rate (4x or 8x)
        const double cs = sr / 4.0;                      // control rate (every 4 samples)
        auto cc = [cs](float ms) { return coefMs(ms, cs); };
        if (isComp())
        {
            switch (t)
            {
                case 0:  // opto: power (RMS-like) detector, slow attack, two-stage program-dependent release
                    dAtt = coefMs(5.0f, sr); dRel = coefMs(40.0f, sr);
                    aAtt = cc(10.0f); relFast = cc(60.0f); relSlow = cc(1800.0f);
                    ratio = 3.0f; knee = 10.0f; memUp = cc(1500.0f); memDn = cc(5000.0f); break;
                case 1:  // smooth (Ross): smoothed level detector, gentle attack, long smooth release
                    dAtt = coefMs(8.0f, sr); dRel = coefMs(60.0f, sr);
                    aAtt = cc(12.0f); aRel = cc(350.0f); ratio = 6.0f; knee = 8.0f; break;
                case 2:  // FET (1176): peak detector, very fast attack, quick release
                    dAtt = 1.0f; dRel = coefMs(6.0f, sr);
                    aAtt = cc(0.4f); aRel = cc(110.0f); ratio = 8.0f; knee = 3.0f; break;
                default: // punch (Dyna Comp): rectifier + RC, attack slow enough that the pick click gets through
                    dAtt = coefMs(1.0f, sr); dRel = coefMs(30.0f, sr);
                    aAtt = cc(5.0f); aRel = cc(260.0f); ratio = 14.0f; knee = 5.0f; break;
            }
            thrDb = -45.0f * amount;
            makeDb = -thrDb * (1.0f - 1.0f / ratio) * 0.6f;
            cTone.a = lpCoef(t == 1 ? 900.0f : 1200.0f, sr);
            punchHp.a = lpCoef(2500.0f, sr);
            gateDb_ = bias < 0.01f ? -200.0f : -84.0f + 54.0f * bias;
            return;
        }
        // fuzz gain staging (dB) and voicing per model
        switch (t)
        {
            case 4:  // Fuzz Face: one gain stage, soft asymmetric germanium clipping, round and bassy
                inHp.a = lpCoef(70.0f, sr); postLp.a = lpCoef(6500.0f, sr4);
                g1 = dbToLin(8.0f + 38.0f * amount); outTrim = 0.42f; break;
            case 5:  // Tone Bender: two stages, the second clips hard; thinner low end, mid-forward, splatty
                inHp.a = lpCoef(150.0f, sr); s1Hp.a = lpCoef(110.0f, sr4); postLp.a = lpCoef(5500.0f, sr4);
                g1 = dbToLin(10.0f + 18.0f * amount); g2 = dbToLin(8.0f + 18.0f * amount); outTrim = 0.38f; break;
            case 6:  // Big Muff: two clipping stages with filtering between, then the mid-scoop tone stack
                inHp.a = lpCoef(60.0f, sr); interLp.a = lpCoef(1800.0f, sr4); postLp.a = lpCoef(4200.0f, sr4);
                g1 = dbToLin(18.0f + 22.0f * amount); g2 = dbToLin(20.0f); outTrim = 0.5f; break;
            default: // Octavia: low-cut (12 dB/oct at 160 Hz) so the rectifier tracks cleanly, then rectify and fuzz
                inHp.a = lpCoef(160.0f, sr); octHp2.a = lpCoef(160.0f, sr); octPre.a = lpCoef(5000.0f, sr);
                rectHp.a = lpCoef(40.0f, sr4); postLp.a = lpCoef(6000.0f, sr4); postLp2.a = lpCoef(9000.0f, sr4);
                g1 = dbToLin(6.0f + 20.0f * amount); outTrim = 0.8f; break;
        }
        // Gate Bias = the starved supply, on a smooth curve (s = bias^2.2): the low end adds a little
        // sag and asymmetry; the splatty, sputtering sound arrives near the top. The gate threshold
        // follows the playing level (so quiet notes are not chopped dead) and has a soft edge.
        starve = std::pow(bias, 2.2f);
        dz = t == 5 ? 0.015f : 0.0f;                    // the Tone Bender's own built-in gate (the model, unchanged)
        offset = 0.4f * starve;
        ceil_ = 1.0f - 0.35f * starve;
        envAtt = coefMs(5.0f, sr); envRel = coefMs(120.0f, sr);
        muffLp.a = lpCoef(450.0f, sr); muffHp.a = lpCoef(1100.0f, sr);
        fzTone.a = lpCoef(1000.0f, sr);
        gateDb_ = -200.0f;
    }

    float deadZone(float x) const
    {
        if (dz <= 0.0f) return x;
        const float m = std::fabs(x) - dz;
        return m > 0.0f ? (x > 0.0f ? m : -m) : 0.0f;
    }
    // the starve gate: x^3 / (x^2 + d^2) - unity well above the threshold, fading smoothly to nothing
    // below it (no corner, so it sputters instead of clicking); d scales with the playing level
    float softGate(float x) const
    {
        if (softDz <= 0.0f) return x;
        const float x2 = x * x;
        return x * x2 / (x2 + softDz * softDz);
    }
    // the starved transistor: on the AMPLIFIED signal, below its cutoff it passes almost nothing
    // (soft curve v^3 / (v^2 + d^2): no corner, so it sputters, it does not click). d is a fraction of
    // the amplified playing level: a loud note loses only a sliver around each zero crossing, a
    // decaying note sputters as it fades, a quiet note is never chopped dead.
    float choke(float v) const
    {
        if (outDz <= 0.0f) return v;
        const float v2 = v * v;
        return v * v2 / (v2 + outDz * outDz);
    }
    // clipping curves and their antiderivatives (for ADAA)
    static float geF(float v) { return v >= 0.0f ? sclip(v) : 0.78f * sclip(1.45f * v); }                   // germanium: earlier, softer negative clip
    static float geA(float v) { return v >= 0.0f ? sclipF(v) : 0.78f / 1.45f * sclipF(1.45f * v); }
    static float hardF(float v) { return sclip(2.2f * v); }                                                 // near-hard silicon/germanium clip
    static float hardA(float v) { return sclipF(2.2f * v) / 2.2f; }
    static float diodeF(float v) { return sclip(v); }
    static float diodeA(float v) { return sclipF(v); }
    static float rectF(float v) { return std::fabs(v); }                                                    // full-wave rectifier
    static float rectA(float v) { return 0.5f * v * std::fabs(v); }

    // one fuzz evaluation at 4x; every clipping / rectifying stage is ADAA-smoothed
    float fuzz4(float x)
    {
        switch (type)
        {
            case 4:
            {   // Fuzz Face: one hot gain stage into an asymmetric germanium clip; round, thick, bassy
                const float v = choke(x * g1 * gSag) + offset;
                const float y = a1.run(v, geF, geA);
                return postLp.lp(y * ceil_);
            }
            case 5:
            {   // Tone Bender: a soft first stage, coupling high-pass, then a near-hard second stage
                float v = a1.run(x * g1 * gSag + 0.5f * offset, diodeF, diodeA);
                v = v - s1Hp.lp(v);
                v = choke(deadZone(v) * g2);
                const float y = a2.run(v, hardF, hardA);
                return postLp.lp(y * 1.05f * ceil_);
            }
            case 6:
            {   // Big Muff: two symmetric diode stages with a low-pass between them: long, smooth sustain
                float v = a1.run(choke(x * g1 * gSag) + 0.3f * offset, diodeF, diodeA);
                v = interLp.lp(v);
                v = a2.run(v * g2, diodeF, diodeA);
                return postLp.lp(v * ceil_);
            }
            default:
            {   // Octavia: a saturating input stage, a full-wave rectifier (the octave: the wave now
                // repeats twice per cycle), DC removed, then the fuzz clip; smoothed afterwards
                const float p = a1.run(choke(x * g1 * gSag), diodeF, diodeA);
                float r = a2.run(p, rectF, rectA);
                r = r - rectHp.lp(r);
                const float y = a3.run(3.2f * r + 0.6f * offset, diodeF, diodeA);
                return postLp2.lp(postLp.lp(y * ceil_));
            }
        }
    }    Adaa a1, a2, a3;
    int osFactor = 4;
    // oversampling per model (compressors: none). Measured on a 3.1 kHz note at full gain, folded
    // harmonics 4x -> 8x: germanium -62 -> -88 dB, gated -62 -> -77, sustain -83 -> -88 (kept at 4x)
    static constexpr int kOs[8] = {1, 1, 1, 1, 8, 8, 4, 8};
    OnePole rectLp;
    float process(float x)
    {
        if (isComp())
        {
            // detector (per sample): opto = power, others = rectified level with their own RC
            const float d = type == 0 ? x * x : std::fabs(x);
            det += (d - det) * (d > det ? dAtt : dRel);
            if ((ctl++ & 3) == 0)
            {
                const float lvl = type == 0 ? 10.0f * std::log10(det + 1e-12f) + 3.0f : linToDb(det + 1e-9f);
                // static curve with a soft knee
                const float over = lvl - thrDb;
                float target = 0.0f;
                if (over > knee * 0.5f) target = over * (1.0f - 1.0f / ratio);
                else if (over > -knee * 0.5f) { const float k = over + knee * 0.5f; target = (1.0f - 1.0f / ratio) * k * k / (2.0f * knee); }
                // attack / release on the gain reduction
                if (target > grDb) grDb += (target - grDb) * aAtt;
                else
                {
                    float rel = aRel;
                    if (type == 0)
                    {   // opto: the cell "remembers" how hard it worked; the longer, the slower it lets go
                        memory += ((grDb > 1.0f ? 1.0f : 0.0f) - memory) * (grDb > 1.0f ? memUp : memDn);
                        rel = relFast + (relSlow - relFast) * memory;
                        if (grDb < 3.0f) rel = std::fmax(rel, relFast * 0.5f);
                    }
                    grDb += (target - grDb) * rel;
                }
                const float gNext = dbToLin(makeDb - grDb);
                gStep = (gNext - gNow) * 0.25f;                    // reach it over the next 4 samples
                grMin = std::fmin(grMin, dbToLin(-grDb));
                gateT = (gateDb_ > -150.0f && lvl < gateDb_) ? 0.0f : 1.0f;
            }
            gNow += gStep;
            gate += (gateT - gate) * (gateT > gate ? gAtt : gRel);
            float y = x * gNow * gate;
            if (type == 2) y = sclip(y * 1.1f + 0.06f * amount * y * y) * 0.92f;   // FET: a little asymmetric bite
            if (type == 3) y += 0.4f * (y - punchHp.lp(y));                       // Dyna: bright, clicky top
            const float lp = cTone.lp(y);
            return lp + (y - lp) * (0.35f + 1.3f * tone);                          // tone: dark .. bright
        }
        // fuzz: input coupling, the starve envelope, up-sampling, the model, down-sampling
        float v = x - inHp.lp(x);
        if (type == 7) { v = v - octHp2.lp(v); v = octPre.lp(v); }          // Octavia: second low-cut pole, tame pick noise
        const float av = std::fabs(v);
        lvlEnv += (av - lvlEnv) * (av > lvlEnv ? envAtt : envRel);
        if (starve > 0.0f)
        {   // sag: the starved supply droops on loud notes; the soft gate follows the playing level
            gSag = 1.0f / (1.0f + 3.0f * starve * std::fmin(1.0f, lvlEnv * 6.0f));
            const float amp = type == 5 ? g2 * std::fmin(1.0f, lvlEnv * g1) : lvlEnv * g1;   // amplified level at the choke
            outDz = starve * 0.75f * std::fmin(amp, 2.0f);
        }
        else { gSag = 1.0f; softDz = 0.0f; outDz = 0.0f; }
        // 4x or 8x per model (kOs; tested: 2x left germanium's folded harmonics at -48 dB)
        float y;
        if (osFactor == 8)
        {
            float o8[8];
            os.up8(v, o8);
            for (float& s : o8) s = fuzz4(s);
            y = dc.run(os.down8(o8));
        }
        else
        {
            float o4[4];
            os.up(v, o4);
            for (float& s : o4) s = fuzz4(s);
            y = dc.run(os.down(o4));
        }
        { const float ay = std::fabs(y); outEnv += (ay - outEnv) * (ay > outEnv ? envAtt : envRel); }
        if (type == 6)
        {   // Muff tone stack: low-pass and high-pass branches blended; at noon the mids are scooped
            const float lo = muffLp.lp(y), hi = y - muffHp.lp(y);
            y = (1.0f - tone) * lo * 1.6f + tone * hi * 1.3f;
        }
        else
        {   // tilt: dark .. bright around 1 kHz
            const float lp = fzTone.lp(y);
            y = lp + (y - lp) * (0.3f + 1.6f * tone);
        }
        return y * outTrim;
    }
    float takeGr() { const float g = grMin; grMin = 1.0f; return g; }
    float gateDb_ = -200.0f;
};
// ------------------------------------------------------------------------------------------------
// Modulation. Mode 0: tremolo <-> vibrato (blend). Mode 1: rotary <-> phaser (blend). Mono in,
// stereo out.
// ------------------------------------------------------------------------------------------------
struct Mod
{
    double sr = 44100.0;
    Ring vib, hornL, hornR;
    float ph = 0.0f, phR = 0.0f;
    // rotary
    float hornPh = 0.0f, drumPh = 0.0f, hornHz = 0.8f, drumHz = 0.7f;
    float xover = 0.1f, xz = 0.0f;
    // phaser: 6 first-order all-passes per side
    float apL[6] = {}, apR[6] = {}, fbL = 0.0f, fbR = 0.0f, aL_ = 0.0f, aR_ = 0.0f;
    int ctl = 0;

    void prepare(double s)
    {
        sr = s;
        vib.prepare((int)(0.02 * s) + 8);
        hornL.prepare((int)(0.01 * s) + 8);
        hornR.prepare((int)(0.01 * s) + 8);
        xover = lpCoef(800.0f, s);
        reset();
    }
    void reset()
    {
        vib.clear(); hornL.clear(); hornR.clear(); ph = phR = 0.0f; hornPh = 0.0f; drumPh = 0.3f; xz = 0.0f;
        std::memset(apL, 0, sizeof apL); std::memset(apR, 0, sizeof apR); fbL = fbR = 0.0f;
    }

    // LFO shapes, -1..1. Sine: smooth. Triangle: straight ramps. Square: a true on/off (slewed in
    // tremVib by ~2 ms so it does not click).
    static float wave(int w, float phase)
    {
        const float p = phase - std::floor(phase);
        if (w == 1) return 1.0f - 4.0f * std::fabs(p - 0.5f);
        if (w == 2) return p < 0.5f ? 1.0f : -1.0f;
        return fastSin01(p);
    }
    float sqL = 0.0f, sqR = 0.0f, slew = 0.1f;
    // Vibrato: the pitch follows how fast the delay changes, so for the PITCH to have the chosen
    // shape the delay must follow its integral: sine -> (shifted) sine, square -> triangle (two pitches,
    // a jump between them), triangle -> parabolic arcs. All scaled to -1..1.
    static float vibShape(int w, float phase)
    {
        const float p = phase - std::floor(phase);
        if (w == 2) return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
        if (w == 1) return 8.0f * (p < 0.5f ? 2.0f * p * p - p : 3.0f * p - 2.0f * p * p - 1.0f);
        return fastSin01(p - 0.25f);
    }

    // first-order all-pass coefficient for a corner frequency; tan via its Taylor series (f < 3 kHz)
    float apCoef(float hz) const
    {
        const float x = kPi * hz / (float)sr, x2 = x * x;
        const float t = x * (1.0f + x2 * (0.333333f + x2 * 0.133333f));
        return (t - 1.0f) / (t + 1.0f);
    }

    // mode 0: tremolo / vibrato. Blend: 0 = tremolo, 0.5 = both at full depth, 1 = vibrato.
    void tremVib(float x, float& l, float& r, float rate, float depth, float blend, int wave_, bool stereo)
    {
        ph += rate / (float)sr; ph -= std::floor(ph);
        float lfo = wave(wave_, ph), lfoR = stereo ? wave(wave_, ph + 0.25f) : lfo;
        if (wave_ == 2) { sqL += (lfo - sqL) * slew; sqR += (lfoR - sqR) * slew; lfo = sqL; lfoR = sqR; }
        const float trem = depth * std::fmin(1.0f, 2.0f * (1.0f - blend));
        const float vibA = depth * std::fmin(1.0f, 2.0f * blend);
        // vibrato: a swept delay with Hermite interpolation; the swing grows with depth squared,
        // from a subtle shimmer (+-0.3 ms) to a dramatic warble (8 ms swing)
        vib.push(x);
        const float swing = vibA * vibA * 0.008f * (float)sr;
        const float v = vibA > 1e-4f ? vib.tapHermite(3.0f + swing * (0.5f + 0.5f * vibShape(wave_, ph)))
                                     : vib.tap(3);                     // pure tremolo: the same 3-sample delay, no interpolation
        // tremolo: a pulse-shaped dip (it spends more time near full level, so it pulses rather than
        // fades), with make-up so the effect does not just sound quieter
        auto tremGain = [&](float s) {
            const float m = 0.5f - 0.5f * s;                 // 0 = loud .. 1 = the dip
            return (1.0f - trem * m * (0.35f + 0.65f * m)) * (1.0f + 0.45f * trem);   // ~ m^1.5, no square root
        };
        l = v * tremGain(lfo);
        r = v * tremGain(lfoR);
    }
    // mode 1: rotary <-> phaser
    void rotPhase(float x, float& l, float& r, float rate, float depth, float blend, bool fast, bool stereo)
    {
        const float rg = std::fmin(1.0f, 2.0f * (1.0f - blend)), pg = std::fmin(1.0f, 2.0f * blend);
        float rl = 0.0f, rr = 0.0f, pl = 0.0f, pr = 0.0f;
        if (rg > 0.0f)
        {
            // rotor speeds glide to their targets: the horn speeds up in ~0.7 s and slows in ~1.5 s,
            // the heavy drum takes ~4 s - the characteristic rotary ramp
            const float hornT = fast ? rate : rate * 0.125f, drumT = hornT * 0.88f;
            hornHz += (hornT - hornHz) * (hornT > hornHz ? hornUp_ : hornDn_);
            drumHz += (drumT - drumHz) * (drumT > drumHz ? drumUp_ : drumDn_);
            hornPh += hornHz / (float)sr; hornPh -= std::floor(hornPh);
            drumPh += drumHz / (float)sr; drumPh -= std::floor(drumPh);
            xz += (x - xz) * xover;
            const float drum = xz, horn = x - xz;
            const float sh = fastSin01(hornPh), ch = fastSin01(hornPh + 0.25f), cd = fastSin01(drumPh + 0.25f);
            // horn: Doppler (a moving delay) and loudness; the two mics hear opposite sides
            hornL.push(horn); hornR.push(horn);
            const float dop = 0.00085f * (float)sr * depth;
            const float hl = hornL.tapLin(2.0f + dop * (1.0f + sh)) * (1.0f - 0.45f * depth * (0.5f - 0.5f * ch));
            const float hr = hornR.tapLin(2.0f + dop * (1.0f - sh)) * (1.0f - 0.45f * depth * (0.5f + 0.5f * ch));
            const float dl = drum * (1.0f - 0.32f * depth * (0.5f - 0.5f * cd));
            const float dr = drum * (1.0f - 0.32f * depth * (0.5f + 0.5f * cd));
            rl = hl + dl; rr = stereo ? hr + dr : rl;
        }
        if (pg > 0.0f)
        {
            ph += rate * 0.25f / (float)sr; ph -= std::floor(ph);            // phaser sweeps slower than the knob's Hz
            if ((ctl++ & 7) == 0)
            {   // all-pass coefficients at control rate (every 8 samples)
                const float span = 1.0f + 3.0f * depth;                          // octaves of sweep
                aL_ = apCoef(250.0f * fastExp2(span * (0.5f + 0.5f * fastSin01(ph))));
                aR_ = apCoef(250.0f * fastExp2(span * (0.5f + 0.5f * fastSin01(ph + 0.25f))));
            }
            const float aL = aL_;
            float s = x + fbL * 0.45f * depth;
            for (float& z : apL) { const float y = aL * s + z; z = s - aL * y; s = y; }
            fbL = s; pl = 0.5f * (x + s);
            if (stereo)
            {
                const float aR = aR_;
                float t = x + fbR * 0.45f * depth;
                for (float& z : apR) { const float y = aR * t + z; z = t - aR * y; t = y; }
                fbR = t; pr = 0.5f * (x + t);
            }
            else pr = pl;
        }
        const float n = 1.0f / (rg + pg);
        l = (rl * rg + pl * pg) * n;
        r = (rr * rg + pr * pg) * n;
    }
    static float fastExp2(float v)   // 2^v for 0 <= v <= 6 (cubic on the fraction, exact octaves)
    {
        const int i = (int)v; const float f = v - (float)i;
        return (float)(1 << i) * (1.0f + f * (0.6960656f + f * (0.2244635f + f * 0.0794226f)));
    }
    void setRamps(double s) { slew = coefMs(2.0f, s); hornUp_ = coefMs(700.0f, s); hornDn_ = coefMs(1500.0f, s); drumUp_ = coefMs(4000.0f, s); drumDn_ = coefMs(5000.0f, s); }
    float hornUp_ = 0.0f, hornDn_ = 0.0f, drumUp_ = 0.0f, drumDn_ = 0.0f;
};

// ------------------------------------------------------------------------------------------------
// Delay: digital (clean) or analog (bucket-brigade flavour: darker each repeat, a little wobble,
// soft saturation in the feedback). Stereo; the right side is offset slightly for width.
// ------------------------------------------------------------------------------------------------
struct Delay
{
    double sr = 44100.0;
    Ring l, r;
    OnePole lpL, lpR, lpL2, lpR2, hpL, hpR;
    float wob = 0.0f, drift = 0.0f, driftT = 0.0f;
    int ctl = 0;
    mpc::dsp::Xorshift32 rng;
    Glide time;

    void prepare(double s)
    {
        sr = s; l.prepare((int)(4.1 * s)); r.prepare((int)(4.1 * s));
        time.setTime(90.0f, s); time.v = 0.3f * (float)s;
        hpL.a = hpR.a = lpCoef(90.0f, s);
        reset();
    }
    void reset() { l.clear(); r.clear(); lpL.reset(); lpR.reset(); lpL2.reset(); lpR2.reset(); hpL.reset(); hpR.reset(); wob = drift = driftT = 0.0f; }

    void process(float inL, float inR, float& outL, float& outR, float samples, float fb, bool analog, bool stereo)
    {
        const float d = time.next(samples);
        float dl = d, dr = stereo ? d + 0.011f * (float)sr : d;
        if (analog)
        {   // wobble: a slow sine plus a drifting random offset
            wob += 0.7f / (float)sr; wob -= std::floor(wob);
            if ((ctl++ & 255) == 0) driftT = rng.nextBipolar();
            drift += (driftT - drift) * 0.0005f;
            const float m = 1.0f + 0.0006f * fastSin01(wob) + 0.0003f * drift;      // +-0.06 % : a hint of tape, no warble
            dl *= m; dr *= m;
        }
        const float maxD = (float)(l.mask - 4);
        float wl = l.tapLin(clamp(dl, 1.0f, maxD));
        float wr = stereo ? r.tapLin(clamp(dr, 1.0f, maxD)) : wl;
        float fl = wl * fb, fr = wr * fb;
        if (analog)
        {   // each repeat darker and saturated, low end thinned like a BBD pedal
            fl = lpL2.lp(lpL.lp(fl)); fl = fl - hpL.lp(fl); fl = sclip(1.3f * fl) * 0.77f;
            if (stereo) { fr = lpR2.lp(lpR.lp(fr)); fr = fr - hpR.lp(fr); fr = sclip(1.3f * fr) * 0.77f; }
        }
        l.push(inL + fl);
        if (stereo) r.push(inR + fr);
        outL = wl; outR = wr;
    }
    void setAnalogTone(double s) { lpL.a = lpR.a = lpL2.a = lpR2.a = lpCoef(2800.0f, s); }   // two poles: each repeat clearly darker
};

// ------------------------------------------------------------------------------------------------
// Reverbs. Plate: J. Dattorro's plate (J. Audio Eng. Soc. 45(9), 1997, "Effect Design Part 1"),
// delay lengths scaled from its 29761 Hz design rate. Spring: two dispersive "springs", each a
// delay and a cascade of stretched all-pass filters in a feedback loop (the chirp / drip comes
// from the all-pass dispersion). Shimmer: the plate with an octave-up pitch shifter feeding its
// output back into its input.
// ------------------------------------------------------------------------------------------------
struct Plate
{
    double sr = 44100.0;
    Allpass in1, in2, in3, in4, mA, mB, aA, aB;
    Ring dA1, dA2, dB1, dB2;
    float bw = 0.0f, dampA = 0.0f, dampB = 0.0f, fbA = 0.0f, fbB = 0.0f, lfo = 0.0f;
    int lA1, lA2, lB1, lB2, tap[14];
    float exc = 16.0f, mA0 = 672, mB0 = 908;

    double scale = 1.0; float modDepth = 0.25f, lfoHz = 0.6f;
    int sc(double n) const { return (int)std::lround(n * scale * sr / 29761.0); }
    // scale: tank size (1 = Dattorro's plate); mod: the tank's delay sweep as a fraction of Dattorro's
    // 16 samples (kept small: a deep sweep is heard as a warble on held notes)
    void prepare(double s, double tankScale = 1.0, float mod = 0.25f)
    {
        sr = s; scale = tankScale; modDepth = mod;
        in1.prepare(sc(142), sc(142), 0.75f); in2.prepare(sc(107), sc(107), 0.75f);
        in3.prepare(sc(379), sc(379), 0.625f); in4.prepare(sc(277), sc(277), 0.625f);
        mA0 = (float)sc(672); mB0 = (float)sc(908); exc = (float)sc(16) * modDepth;
        mA.prepare(sc(672 + 40), sc(672), -0.7f); mB.prepare(sc(908 + 40), sc(908), -0.7f);
        aA.prepare(sc(1800), sc(1800), 0.5f); aB.prepare(sc(2656), sc(2656), 0.5f);
        lA1 = sc(4453); lA2 = sc(3720); lB1 = sc(4217); lB2 = sc(3163);
        dA1.prepare(lA1 + 4); dA2.prepare(lA2 + 4); dB1.prepare(lB1 + 4); dB2.prepare(lB2 + 4);
        const int t[14] = {266, 2974, 1913, 1996, 1990, 187, 1066, 353, 3627, 1228, 2673, 2111, 335, 121};
        for (int i = 0; i < 14; ++i) tap[i] = sc(t[i]);
        reset();
    }
    void reset()
    {
        for (Allpass* a : {&in1, &in2, &in3, &in4, &mA, &mB, &aA, &aB}) a->r.clear();
        for (Ring* r : {&dA1, &dA2, &dB1, &dB2}) r->clear();
        bw = dampA = dampB = fbA = fbB = 0.0f; lfo = 0.0f;
    }
    // decay 0..1 tank gain, damping 0..1 (higher = darker), bandwidth one-pole coefficient
    void process(float x, float& outL, float& outR, float decay, float damping, float bandwidth)
    {
        bw += (x - bw) * bandwidth;
        float v = in4.run(in3.run(in2.run(in1.run(bw))));
        lfo += lfoHz / (float)sr; lfo -= std::floor(lfo);
        const float m = fastSin01(lfo) * exc;
        // half A
        float a = mA.runMod(v + fbB * decay, mA0 + m);
        dA1.push(a);
        float a2 = dA1.tap(lA1 - 1);
        dampA += (a2 - dampA) * (1.0f - damping);
        a2 = aA.run(dampA * decay);
        dA2.push(a2);
        const float outA = dA2.tap(lA2 - 1);
        // half B
        float b = mB.runMod(v + fbA * decay, mB0 - m);
        dB1.push(b);
        float b2 = dB1.tap(lB1 - 1);
        dampB += (b2 - dampB) * (1.0f - damping);
        b2 = aB.run(dampB * decay);
        dB2.push(b2);
        const float outB = dB2.tap(lB2 - 1);
        fbA = outA; fbB = outB;
        // output taps (Dattorro's table)
        outL = 0.6f * (dB1.tap(tap[0]) + dB1.tap(tap[1]) - aB.r.tap(tap[2]) + dB2.tap(tap[3])
                     - dA1.tap(tap[4]) - aA.r.tap(tap[5]) - dA2.tap(tap[6]));
        outR = 0.6f * (dA1.tap(tap[7]) + dA1.tap(tap[8]) - aA.r.tap(tap[9]) + dA2.tap(tap[10])
                     - dB1.tap(tap[11]) - aB.r.tap(tap[12]) - dB2.tap(tap[13]));
    }
};

// Spring: the chirp happens once per note, not on every pass of a loop. The input is dispersed by a
// short all-pass chain (the "boing" on the attack), feeds a small, dense, dark tank (a scaled plate
// with no delay sweep, so held notes decay smoothly without pitch wobble), and two dispersed
// "drip" echoes (the spring's round trips) are added once each.
struct Spring
{
    static constexpr int kStages = 6;
    struct Chain
    {
        float a = 0.55f, x1[kStages] = {}, z1[kStages] = {};
        void reset() { std::memset(x1, 0, sizeof x1); std::memset(z1, 0, sizeof z1); }
        float run(float s)
        {
            for (int i = 0; i < kStages; ++i) { const float y = a * s + x1[i] - a * z1[i]; x1[i] = s; z1[i] = y; s = y; }
            return s;
        }
    };
    double sr = 44100.0;
    Plate tank;
    Chain in, drip1, drip2;
    Ring echo;
    int t1 = 1000, t2 = 2000;
    OnePole inHp, tone;
    void prepare(double s)
    {
        sr = s;
        tank.prepare(s, 0.9, 0.0f);                       // big enough for dense modes (a small tank beats on held notes)
        echo.prepare((int)(0.08 * s) + 8);
        t1 = (int)(0.031 * s); t2 = (int)(0.063 * s);
        inHp.a = lpCoef(220.0f, s);
        drip2.a = 0.6f;
        reset();
    }
    void reset() { tank.reset(); in.reset(); drip1.reset(); drip2.reset(); echo.clear(); inHp.reset(); tone.reset(); }
    void process(float x, float decay01, float toneA, float damping, bool stereo, float& l, float& r)
    {
        float v = x - inHp.lp(x);
        tone.a = toneA;
        v = tone.lp(v);
        const float d = in.run(v);
        float tl, tr;
        tank.process(d, tl, tr, 0.3f + 0.66f * decay01, damping, 0.9f);
        echo.push(d);
        const float e1 = drip1.run(echo.tap(t1)), e2 = drip2.run(echo.tap(t2));
        l = 1.3f * tl + 0.45f * e1 + 0.25f * e2;
        r = stereo ? 1.3f * tr + 0.25f * e1 + 0.45f * e2 : l;
    }
};
// Octave-up pitch shifter: two read heads sweeping a 46 ms window at double speed, crossfaded.
struct OctaveUp
{
    Ring buf; float ph = 0.0f; int win = 2048;
    void prepare(double s) { win = (int)(0.046 * s); buf.prepare(win * 2 + 8); ph = 0.0f; }
    void reset() { buf.clear(); ph = 0.0f; }
    float process(float x)
    {
        buf.push(x);
        ph += 1.0f / (float)win; ph -= std::floor(ph);
        const float p2 = ph + 0.5f - std::floor(ph + 0.5f);
        const float d1 = (1.0f - ph) * (float)win + 1.0f, d2 = (1.0f - p2) * (float)win + 1.0f;   // shrinking delay = pitch up x2
        const float g1 = 1.0f - std::fabs(2.0f * ph - 1.0f), g2 = 1.0f - std::fabs(2.0f * p2 - 1.0f);
        return buf.tapLin(d1) * g1 + buf.tapLin(d2) * g2;
    }
};

// Sample-peak look-ahead limiter (same construction as Sleepy Android's): min-hold over the
// look-ahead window, instant attack, 100 ms release, then a moving average so it ramps into peaks.
constexpr int kLookahead = 64;
struct PeakLimiter
{
    static constexpr int kQ = 128;
    float qVal[kQ];
    int64_t qIdx[kQ];
    int64_t head = 0, tail = 0, n = 0;
    float env = 1.0f, rel = 0.0f;
    float ring[kLookahead];
    int pos = 0;
    double sum = kLookahead;
    void prepare(double sr) { rel = (float)(1.0 - std::exp(-1.0 / (0.100 * sr))); reset(); }
    void reset() { head = tail = n = 0; env = 1.0f; for (float& v : ring) v = 1.0f; pos = 0; sum = kLookahead; }
    float process(float required)
    {
        while (tail > head && qVal[(tail - 1) & (kQ - 1)] >= required) --tail;
        qVal[tail & (kQ - 1)] = required; qIdx[tail & (kQ - 1)] = n; ++tail;
        while (qIdx[head & (kQ - 1)] < n - kLookahead) ++head;
        const float held = qVal[head & (kQ - 1)];
        ++n;
        if (held < env) env = held;
        else { env += (held - env) * rel; if (held - env < 1e-6f) env = held; }
        sum += (double)env - (double)ring[pos];
        ring[pos] = env;
        if (++pos == kLookahead) { pos = 0; double s = 0.0; for (float v : ring) s += v; sum = s; }
        return (float)(sum * (1.0 / kLookahead));
    }
};

} // namespace hydra
