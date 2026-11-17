// elver_dsp.h - Elver's sound engine parts (original code, MIT). Allocation-free; no sin / exp / pow /
// tan per sample (those run per block or every 16 samples).
#pragma once

#include "mpc_plugin.h"
#include "dsp/adsr.h"
#include "dsp/noise.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace elver {

using mpc::clamp;
constexpr float kPi = 3.14159265358979f;

inline float sclip(float x) { if (x >= 1.5f) return 1.0f; if (x <= -1.5f) return -1.0f; return x - 0.148148f * x * x * x; }

// sine of phase 0..1: 7th-order odd polynomial on a folded quarter wave (error < 1e-4)
inline float sin01(float p)
{
    p -= std::floor(p);
    float x = p < 0.5f ? p : p - 1.0f;            // -0.5 .. 0.5
    x = x > 0.25f ? 0.5f - x : (x < -0.25f ? -0.5f - x : x);   // fold to -0.25 .. 0.25
    const float t = x * 2.0f * kPi, t2 = t * t;
    return t * (1.0f + t2 * (-0.16666667f + t2 * (0.0083333333f + t2 * -0.00019841270f)));
}

inline float polyBlep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

enum Wave { kSine = 0, kTri, kSaw, kSquare, kNoise, kShape };   // kShape: the side's table (elver_wave.h)

// one oscillator voice copy: a phase and an increment
inline float oscSample(int wave, float& phase, float inc, float pw)
{
    const float t = phase;
    float y;
    switch (wave)
    {
        case kSine: y = sin01(t); break;
        case kTri:  y = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t; break;     // corners alias very little
        case kSaw:  y = 2.0f * t - 1.0f - polyBlep(t, inc); break;
        default:
        {   // square / pulse
            y = t < pw ? 1.0f : -1.0f;
            y += polyBlep(t, inc);
            float t2 = t - pw; if (t2 < 0.0f) t2 += 1.0f;
            y -= polyBlep(t2, inc);
            y += 2.0f * pw - 1.0f;          // remove the DC a narrow pulse carries
            break;
        }
    }
    phase += inc;
    if (phase >= 1.0f) phase -= 1.0f;
    return y;
}

// one oscillator copy over a chunk, one loop per waveform (no per-sample switch). Adds y * gl to L and,
// when R is given, y * gr to R; when tap is given it also stores the raw wave (for ring modulation).
template <int W>
inline void oscRunT(float& phase, float inc, float pw, float gl, float gr, float* L, float* R, float* tap, int n)
{
    float ph = phase;
    for (int i = 0; i < n; ++i)
    {
        float y;
        if (W == kSine) y = sin01(ph);
        else if (W == kTri) y = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
        else if (W == kSaw) y = 2.0f * ph - 1.0f - polyBlep(ph, inc);
        else
        {
            y = (ph < pw ? 1.0f : -1.0f) + polyBlep(ph, inc);
            float t2 = ph - pw; if (t2 < 0.0f) t2 += 1.0f;
            y += 2.0f * pw - 1.0f - polyBlep(t2, inc);
        }
        ph += inc; if (ph >= 1.0f) ph -= 1.0f;
        L[i] += y * gl;
        if (R) R[i] += y * gr;
        if (tap) tap[i] = y;
    }
    phase = ph;
}
inline void oscRun(int wave, float& phase, float inc, float pw, float gl, float gr, float* L, float* R, float* tap, int n)
{
    switch (wave)
    {
        case kSine: oscRunT<kSine>(phase, inc, pw, gl, gr, L, R, tap, n); break;
        case kTri:  oscRunT<kTri>(phase, inc, pw, gl, gr, L, R, tap, n); break;
        case kSaw:  oscRunT<kSaw>(phase, inc, pw, gl, gr, L, R, tap, n); break;
        default:    oscRunT<kSquare>(phase, inc, pw, gl, gr, L, R, tap, n); break;
    }
}

// Cytomic / Andy Simper state-variable filter, one channel. Coefficients set every 16 samples.
struct Svf
{
    float ic1 = 0.0f, ic2 = 0.0f;
    void reset() { ic1 = ic2 = 0.0f; }
    // returns low / band / high in one go
    inline void run(float v0, float a1, float a2, float a3, float k, float& lo, float& bp, float& hp)
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
        lo = v2; bp = v1; hp = v0 - k * v1 - v2;
    }
};
struct SvfCoef
{
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, k = 1.4f, sk = 1.18f;
    void set(float hz, float res01, double sr)
    {
        const float g = std::tan(kPi * clamp(hz, 15.0f, 0.45f * (float)sr) / (float)sr);
        k = 2.0f - 1.96f * clamp(res01, 0.0f, 1.0f);     // never fully self-oscillating
        a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
        sk = std::sqrt(k);
    }
};

// filter types
enum FType { kLP24 = 0, kLP12, kBP, kHP, kNotch };

// a stereo filter: two SVF stages per channel (the 24 dB low-pass uses both)
struct StereoFilter
{
    Svf s1[2], s2[2];
    SvfCoef c1, c2;
    void reset() { for (int c = 0; c < 2; ++c) { s1[c].reset(); s2[c].reset(); } }
    // while only the left channel is processed (a mono sound), the right one follows it, so stereo
    // processing can resume without a jump
    void syncRight() { s1[1] = s1[0]; s2[1] = s2[0]; }
    void set(int type, float hz, float res01, double sr)
    {
        c2.set(hz, res01, sr);
        if (type == kLP24) c1.set(hz, 0.15f, sr);       // first stage gentle, the second carries the resonance
    }
    inline float run(int c, int type, float x)
    {
        float lo, bp, hp;
        if (type == kLP24)
        {
            s1[c].run(x, c1.a1, c1.a2, c1.a3, c1.k, lo, bp, hp);
            x = lo;
        }
        s2[c].run(x, c2.a1, c2.a2, c2.a3, c2.k, lo, bp, hp);
        switch (type)
        {
            case kBP:    return bp * c2.sk;             // constant power for wide-band input (peak +8 dB at full resonance)
            case kHP:    return hp;
            case kNotch: return lo + hp;
            default:     return lo;
        }
    }
};

// ADSR advanced a chunk (normally 16 samples) at a time: the same analogue-style exponential curves as
// the framework's ADSR, but one update per chunk instead of one per sample. The amp envelope is
// ramped linearly across the chunk by the caller, so steps never click.
struct ChunkEnv
{
    enum Stage { kIdle = 0, kAttack, kDecay, kSustain, kRelease };
    static constexpr float kAttackRatio = 0.3f, kDecayRatio = 0.0001f, kSilence = 0.0005f;
    float lvl = 0.0f, sus = 0.8f;
    float c[4] = {0, 0, 0, 0}, c16[4] = {0, 0, 0, 0};       // per-sample and per-16 factors: attack, decay, release
    int stage = kIdle;
    static float coef(float ms, float ratio, float sr)
    {
        const float samples = ms * 0.001f * sr;
        if (samples < 1.0f) return 0.0f;
        return std::exp(-std::log((1.0f + ratio) / ratio) / samples);
    }
    void set(float aMs, float dMs, float s, float rMs, double sr)
    {
        sus = clamp(s, 0.0f, 1.0f);
        c[1] = coef(aMs, kAttackRatio, (float)sr); c[2] = coef(dMs, kDecayRatio, (float)sr); c[3] = coef(rMs, kDecayRatio, (float)sr);
        for (int k = 1; k < 4; ++k) { float p = c[k]; for (int i = 0; i < 4; ++i) p *= p; c16[k] = p; }   // c^16
    }
    void noteOn() { stage = kAttack; }
    void noteOff() { if (stage != kIdle) stage = kRelease; }
    void reset() { stage = kIdle; lvl = 0.0f; }
    bool isActive() const { return stage != kIdle; }
    float advance(int n)
    {
        auto pw = [&](int k) { return n == 16 ? c16[k] : std::pow(c[k], (float)n); };
        switch (stage)
        {
            case kAttack:
            {
                const float tgt = 1.0f + kAttackRatio;
                lvl = tgt + (lvl - tgt) * pw(1);
                if (lvl >= 1.0f) { lvl = 1.0f; stage = kDecay; }
                break;
            }
            case kDecay:
            {
                const float tgt = sus - kDecayRatio;
                lvl = tgt + (lvl - tgt) * pw(2);
                if (lvl <= sus) { lvl = sus; stage = kSustain; }
                break;
            }
            case kSustain: lvl = sus; break;
            case kRelease:
                lvl = -kDecayRatio + (lvl + kDecayRatio) * pw(3);
                if (lvl <= kSilence) { lvl = 0.0f; stage = kIdle; }
                break;
            default: break;
        }
        return lvl;
    }
};

// slow random drift: a smoothed random walk, -1..1
struct Drift
{
    float v = 0.0f, t = 0.0f; int n = 0;
    float next(mpc::dsp::Xorshift32& r, int step)
    {
        n -= step;
        if (n <= 0) { t = r.nextBipolar(); n = 4000 + (int)(r.nextFloat() * 8000); }
        v += (t - v) * 0.0008f * (float)step;
        return v;
    }
};

} // namespace elver
