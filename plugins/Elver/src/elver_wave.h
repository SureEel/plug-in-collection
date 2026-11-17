// elver_wave.h - Elver's shaped / drawn single-cycle waves (original code, MIT).
//
// A side's Shape wave is built off the audio thread:
//   library wave ("Start From", generated from a recipe)  --mix-->  drawn wave (64 points)
//   -> Bend (phase distortion) -> Sync (hard-sync ratio) -> Fold (wavefolder)
//   -> spectrum: Bright (tilt), Odd/Even, Formant (resonant bump), Mutate (seeded jitter)
//   -> 11 band-limited tables (1024 .. 1 harmonics), so high notes never alias.
// Nothing here runs per sample except WaveTable::read (a table lookup with linear interpolation).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace elver {

constexpr int kWaveN = 2048;              // samples per cycle
constexpr int kMips = 11;                 // 1024, 512, ... 1 harmonics
constexpr int kDrawPoints = 64;
constexpr int kScopePoints = 32;

struct WaveTable
{
    float mip[kMips][kWaveN + 1];          // + guard sample for interpolation
    float scope[kScopePoints];            // the cycle as the scope shows it (-1..1)
    float start[kDrawPoints];             // the start (library) wave at 64 points, for Copy Start
    // mip level for a phase increment (cycles per sample): the richest table whose top harmonic
    // stays under 0.45 x sample rate
    static int level(float inc)
    {
        const float kmax = 0.45f / (inc > 1e-9f ? inc : 1e-9f);
        int L = 0;
        while (L < kMips - 1 && (float)(1024 >> L) > kmax) ++L;
        return L;
    }
};

// ---- a small radix-2 FFT (double, in place) -------------------------------------------------------
struct Fft
{
    std::vector<double> cs, sn;
    std::vector<int> rev;
    explicit Fft(int n = kWaveN) : cs((size_t)n / 2), sn((size_t)n / 2), rev((size_t)n)
    {
        const double pi = 3.14159265358979323846;
        for (int i = 0; i < n / 2; ++i) { cs[(size_t)i] = std::cos(2 * pi * i / n); sn[(size_t)i] = std::sin(2 * pi * i / n); }
        int bits = 0; while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) { int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev[(size_t)i] = r; }
    }
    void run(double* re, double* im, bool inverse) const
    {
        const int n = (int)rev.size();
        for (int i = 0; i < n; ++i) { const int r = rev[(size_t)i]; if (r > i) { std::swap(re[i], re[r]); std::swap(im[i], im[r]); } }
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, step = n / len;
            for (int s = 0; s < n; s += len)
                for (int k = 0; k < half; ++k)
                {
                    const double wr = cs[(size_t)(k * step)], wi = inverse ? sn[(size_t)(k * step)] : -sn[(size_t)(k * step)];
                    const int a = s + k, b = a + half;
                    const double xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
                }
        }
    }
};

// ---- the library: recipes, grouped -----------------------------------------------------------------
struct LibWave { const char* name; int kind; float a, b, c; };
enum LibKind { kSpecSaw, kSpecSquare, kSpecPulse, kSpecTri, kSpecSine, kSpecSawSoft, kSpecSquareSoft, kSpecTired, kSpecSub,
               kVowel, kFm, kOrgan, kTimeFold, kTimeBits, kTimePd, kTimeSync, kHarmPick, kHarmSlope, kHarmComb, kNoiseSpec };
// vowel formants (Hz) for kVowel: a = index into kVowels
struct Vowel { float f1, f2, f3, breath; };
const Vowel kVowels[] = { {730, 1090, 2440, 0}, {270, 2290, 3010, 0}, {570, 840, 2410, 0}, {300, 870, 2240, 0},
                          {530, 1840, 2480, 0}, {730, 1090, 2440, 0.6f}, {250, 2100, 2900, 0}, {700, 1220, 2600, 1.0f} };
// organ drawbars for kOrgan: a = index into kDrawbars (16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1')
const int kDrawbars[][9] = { {8,8,8,0,0,0,0,0,0}, {8,0,8,0,0,0,0,0,0}, {8,0,0,0,0,0,8,8,8}, {8,8,8,8,8,8,8,8,8},
                             {0,0,8,0,0,0,0,0,0}, {8,8,8,8,0,0,0,0,0}, {0,0,8,4,0,3,0,0,0}, {6,8,8,6,0,0,0,0,0} };
const LibWave kLibrary[] = {
    // Analog
    {"ANA Sine", kSpecSine, 0, 0, 0}, {"ANA Triangle", kSpecTri, 0, 0, 0}, {"ANA Saw", kSpecSaw, 0, 0, 0},
    {"ANA Saw Soft", kSpecSawSoft, 0, 0, 0}, {"ANA Square", kSpecSquare, 0, 0, 0}, {"ANA Square Soft", kSpecSquareSoft, 0, 0, 0},
    {"ANA Pulse 25", kSpecPulse, 0.25f, 0, 0}, {"ANA Pulse 10", kSpecPulse, 0.10f, 0, 0}, {"ANA Tired Saw", kSpecTired, 0, 0, 0},
    {"ANA Saw + Sub", kSpecSub, 0, 0, 0},
    // Vocal
    {"VOX Ah", kVowel, 0, 0, 0}, {"VOX Ee", kVowel, 1, 0, 0}, {"VOX Oh", kVowel, 2, 0, 0}, {"VOX Oo", kVowel, 3, 0, 0},
    {"VOX Eh", kVowel, 4, 0, 0}, {"VOX Choir Ah", kVowel, 5, 0, 0}, {"VOX Nasal", kVowel, 6, 0, 0}, {"VOX Whisper", kVowel, 7, 0, 0},
    // FM / bell (integer ratios: the cycle stays periodic)
    {"FM 1:1 Soft", kFm, 1, 1.0f, 0}, {"FM 1:1 Hard", kFm, 1, 3.0f, 0}, {"FM 1:2 Reed", kFm, 2, 1.5f, 0}, {"FM 1:3 Hollow", kFm, 3, 1.2f, 0},
    {"FM 1:5 Glass", kFm, 5, 1.0f, 0}, {"FM 1:7 Bell", kFm, 7, 1.4f, 0}, {"FM 2:1 Tine", kFm, 0.5f, 1.2f, 0}, {"FM 1:4 Bright", kFm, 4, 2.5f, 0},
    // Organ
    {"ORG Jazz", kOrgan, 0, 0, 0}, {"ORG Hollow", kOrgan, 1, 0, 0}, {"ORG Perc", kOrgan, 2, 0, 0}, {"ORG Full", kOrgan, 3, 0, 0},
    {"ORG Flute", kOrgan, 4, 0, 0}, {"ORG Gospel", kOrgan, 5, 0, 0}, {"ORG Reed", kOrgan, 6, 0, 0}, {"ORG Warm", kOrgan, 7, 0, 0},
    // Digital
    {"DIG Fold 1", kTimeFold, 1.5f, 0, 0}, {"DIG Fold 2", kTimeFold, 2.5f, 0, 0}, {"DIG Fold 3", kTimeFold, 4.0f, 0, 0},
    {"DIG Bits 4", kTimeBits, 4, 0, 0}, {"DIG Bits 8", kTimeBits, 8, 0, 0}, {"DIG PD Reso 1", kTimePd, 3, 0, 0},
    {"DIG PD Reso 2", kTimePd, 6, 0, 0}, {"DIG PD Saw", kTimePd, 0, 0, 0}, {"DIG Sync 2", kTimeSync, 2.0f, 0, 0},
    {"DIG Sync 3.5", kTimeSync, 3.5f, 0, 0},
    // Harmonic
    {"HRM Odd Only", kHarmSlope, 1.0f, 1, 0}, {"HRM Even Only", kHarmSlope, 1.0f, 2, 0}, {"HRM 1 + 2", kHarmPick, 2, 0, 0},
    {"HRM 1 + 3", kHarmPick, 3, 0, 0}, {"HRM 1 + 5", kHarmPick, 5, 0, 0}, {"HRM Octaves", kHarmComb, 0, 0, 0},
    {"HRM Flat Bright", kHarmSlope, 0.3f, 0, 0}, {"HRM Dark", kHarmSlope, 2.0f, 0, 0}, {"HRM Primes", kHarmComb, 1, 0, 0},
    {"HRM Fibonacci", kHarmComb, 2, 0, 0},
    // Noise-like (seeded, so always the same)
    {"NSE Spectrum 1", kNoiseSpec, 1, 1.0f, 0}, {"NSE Spectrum 2", kNoiseSpec, 2, 0.7f, 0}, {"NSE Grit", kNoiseSpec, 3, 0.4f, 0},
    {"NSE Shards", kNoiseSpec, 4, 1.5f, 1}, {"NSE Dust", kNoiseSpec, 5, 2.0f, 1}, {"NSE Static", kNoiseSpec, 6, 0.2f, 0},
};
constexpr int kLibSize = (int)(sizeof(kLibrary) / sizeof(kLibrary[0]));

inline uint32_t hashRand(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
inline double rand01(uint32_t& s) { return (double)(hashRand(s) >> 8) / 16777216.0; }

// The library wave as one cycle (kWaveN samples, peak 1). Spectral recipes are summed directly
// (band-limited by construction, up to 1024 harmonics); time-domain ones are drawn and band-limited later.
inline void libraryCycle(int index, const Fft& fft, double* out)
{
    const double pi = 3.14159265358979323846;
    const LibWave& w = kLibrary[index < 0 ? 0 : (index >= kLibSize ? kLibSize - 1 : index)];
    std::vector<double> re(kWaveN, 0.0), im(kWaveN, 0.0);
    // amplitude / phase of harmonic k (1..1023); phase 0 = sine
    auto put = [&](int k, double amp, double ph) { if (k < 1 || k >= kWaveN / 2) return; re[(size_t)k] += amp * std::sin(ph) * 0.5; im[(size_t)k] -= amp * std::cos(ph) * 0.5; };
    bool time = false;
    switch (w.kind)
    {
        case kSpecSine: put(1, 1, 0); break;
        case kSpecSaw: for (int k = 1; k < 1024; ++k) put(k, 1.0 / k, 0); break;
        case kSpecSawSoft: for (int k = 1; k < 1024; ++k) put(k, 1.0 / k / (1.0 + (k / 12.0) * (k / 12.0)), 0); break;
        case kSpecTired: { uint32_t s = 77; for (int k = 1; k < 1024; ++k) put(k, 1.0 / k / (1.0 + std::pow(k / 18.0, 3.0)), 0.25 * (rand01(s) - 0.5)); break; }
        case kSpecSquare: for (int k = 1; k < 1024; k += 2) put(k, 1.0 / k, 0); break;
        case kSpecSquareSoft: for (int k = 1; k < 1024; k += 2) put(k, 1.0 / k / (1.0 + (k / 9.0) * (k / 9.0)), 0); break;
        case kSpecPulse: for (int k = 1; k < 1024; ++k) put(k, std::sin(pi * k * w.a) / k, pi * 0.5); break;
        case kSpecTri: for (int k = 1; k < 1024; k += 2) put(k, 1.0 / ((double)k * k) * (((k / 2) % 2) ? -1 : 1), 0); break;
        case kSpecSub: put(1, 1.0, 0); for (int k = 2; k < 1024; k += 2) put(k, 1.4 / k, 0); break;
        case kVowel:
        {
            const Vowel& v = kVowels[(int)w.a];
            uint32_t s = 1234;
            const double f0 = 130.0;
            for (int k = 1; k < 1024; ++k)
            {
                const double f = f0 * k;
                double g = std::pow((double)k, -1.1);
                const double fm[3] = {v.f1, v.f2, v.f3}, bw[3] = {90, 110, 150}, lvl[3] = {1.0, 0.6, 0.35};
                double form = 0.05;
                for (int j = 0; j < 3; ++j) { const double d = (f - fm[j]) / bw[j]; form += lvl[j] / (1.0 + d * d); }
                g *= form;
                put(k, g, v.breath > 0 ? v.breath * 2 * pi * rand01(s) : 0.0);
            }
            break;
        }
        case kFm:
        {
            time = true;
            for (int i = 0; i < kWaveN; ++i)
            {
                const double ph = 2 * pi * i / kWaveN;
                // modulator ratio a (0.5 = carrier at 2x), index b
                re[(size_t)i] = w.a < 1.0f ? std::sin(2 * ph + w.b * std::sin(ph)) : std::sin(ph + w.b * std::sin(w.a * ph));
            }
            break;
        }
        case kOrgan:
        {
            const double ratio[9] = {0.5, 1.5, 1, 2, 3, 4, 5, 6, 8};   // 16' is the sub: the cycle is 16'
            const int* bars = kDrawbars[(int)w.a];
            for (int j = 0; j < 9; ++j) if (bars[j]) put((int)std::lround(ratio[j] * 2), bars[j] / 8.0, 0);
            break;
        }
        case kTimeFold:
            time = true;
            for (int i = 0; i < kWaveN; ++i) { double x = std::sin(2 * pi * i / kWaveN) * w.a; x = 4.0 * std::fabs(x / 4.0 + 0.25 - std::floor(x / 4.0 + 0.75)) - 1.0; re[(size_t)i] = x; }
            break;
        case kTimeBits:
            time = true;
            for (int i = 0; i < kWaveN; ++i) re[(size_t)i] = std::round(std::sin(2 * pi * i / kWaveN) * w.a) / w.a;
            break;
        case kTimePd:
            time = true;
            for (int i = 0; i < kWaveN; ++i)
            {
                const double p = (double)i / kWaveN;
                if (w.a == 0) { const double d = 0.08; const double q = p < d ? 0.5 * p / d : 0.5 + 0.5 * (p - d) / (1 - d); re[(size_t)i] = std::cos(2 * pi * q); }
                else re[(size_t)i] = std::cos(2 * pi * p * w.a) * (1.0 - p);       // resonant: window x fast cosine
            }
            break;
        case kTimeSync:
            time = true;
            for (int i = 0; i < kWaveN; ++i) { const double p = (double)i / kWaveN * w.a; re[(size_t)i] = 2.0 * (p - std::floor(p)) - 1.0; }
            break;
        case kHarmPick: put(1, 1.0, 0); put((int)w.a, 0.7, 0); break;
        case kHarmSlope:
            for (int k = 1; k < 1024; ++k) { if (w.b == 1 && k % 2 == 0) continue; if (w.b == 2 && k % 2 == 1 && k > 1) continue; put(k, std::pow((double)k, -w.a), 0); }
            break;
        case kHarmComb:
            if (w.a == 0) for (int k = 1; k < 1024; k *= 2) put(k, 1.0, 0);
            else if (w.a == 1) { const int pr[] = {1, 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31}; for (int k : pr) put(k, 1.0 / std::sqrt((double)k), 0); }
            else { int a = 1, b = 2; while (a < 1024) { put(a, 1.0 / std::sqrt((double)a), 0); const int c = a + b; a = b; b = c; } }
            break;
        case kNoiseSpec:
        {
            uint32_t s = 0x9E3779B9u * (uint32_t)w.a + 1;
            for (int k = 1; k < 1024; ++k)
            {
                double amp = std::pow((double)k, -w.b) * (0.3 + rand01(s));
                if (w.c > 0 && rand01(s) > 0.15) amp = 0;                 // sparse
                put(k, amp, 2 * pi * rand01(s));
            }
            break;
        }
    }
    if (!time) { fft.run(re.data(), im.data(), true); }
    double pk = 1e-12;
    for (int i = 0; i < kWaveN; ++i) pk = std::fmax(pk, std::fabs(re[(size_t)i]));
    for (int i = 0; i < kWaveN; ++i) out[i] = re[(size_t)i] / pk;
}

// ---- the shaper's settings (one side) ----------------------------------------------------------------
struct ShapeSettings
{
    int start = 2; float bright = 0, oddEven = 0, formant = 0, fold = 0, bend = 0, sync = 1, mutate = 0; int seed = 1;
    float drawMix = 0; int smooth = 0; float points[kDrawPoints] = {};
};

// drawn points -> one cycle: 0 smooth (periodic Catmull-Rom), 1 linear, 2 steps
inline void drawnCycle(const ShapeSettings& s, double* out)
{
    for (int i = 0; i < kWaveN; ++i)
    {
        const double pos = (double)i * kDrawPoints / kWaveN;
        const int i1 = (int)pos; const double t = pos - i1;
        auto P = [&](int k) { return (double)s.points[((k % kDrawPoints) + kDrawPoints) % kDrawPoints]; };
        if (s.smooth == 2) out[i] = P(i1);
        else if (s.smooth == 1) out[i] = P(i1) + (P(i1 + 1) - P(i1)) * t;
        else
        {
            const double p0 = P(i1 - 1), p1 = P(i1), p2 = P(i1 + 1), p3 = P(i1 + 2);
            out[i] = p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
        }
    }
}

// Builds a side's tables. base = the library cycle (cached by the caller).
inline void buildShape(const ShapeSettings& s, const double* base, const Fft& fft, WaveTable& out)
{
    const double pi = 3.14159265358979323846;
    std::vector<double> x(kWaveN), drawn(kWaveN), re(kWaveN), im(kWaveN);
    // 1. mix library and drawing
    drawnCycle(s, drawn.data());
    for (int i = 0; i < kWaveN; ++i) x[(size_t)i] = base[i] * (1.0 - s.drawMix) + drawn[(size_t)i] * s.drawMix;
    // 2. time domain: Bend (phase distortion), Sync, Fold
    auto readAt = [&](const std::vector<double>& v, double p) { p -= std::floor(p); const double idx = p * kWaveN; const int a = (int)idx; const double f = idx - a; return v[(size_t)(a % kWaveN)] * (1 - f) + v[(size_t)((a + 1) % kWaveN)] * f; };
    std::vector<double> y(kWaveN);
    const double d = 0.5 - 0.46 * s.bend, ratio = s.sync < 1 ? 1 : s.sync;
    for (int i = 0; i < kWaveN; ++i)
    {
        double p = (double)i / kWaveN;
        if (s.bend > 0) p = p < d ? 0.5 * p / d : 0.5 + 0.5 * (p - d) / (1 - d);
        y[(size_t)i] = readAt(x, p * ratio);
    }
    if (s.fold > 0)
    {
        double pk = 1e-12; for (double v : y) pk = std::fmax(pk, std::fabs(v));
        const double g = 1.0 + 4.0 * s.fold;
        for (double& v : y) { const double u = v / pk * g; v = 4.0 * std::fabs(u / 4.0 + 0.25 - std::floor(u / 4.0 + 0.75)) - 1.0; }
    }
    // 3. spectrum
    for (int i = 0; i < kWaveN; ++i) { re[(size_t)i] = y[(size_t)i]; im[(size_t)i] = 0.0; }
    fft.run(re.data(), im.data(), false);
    re[0] = im[0] = 0.0;                                                   // no DC
    uint32_t rs = 0x51ED27u ^ (uint32_t)(s.seed * 2654435761u);
    const double fc = 1.0 + s.formant * 40.0;
    for (int k = 1; k < kWaveN / 2; ++k)
    {
        double g = std::pow((double)k, (double)s.bright);                   // +-1: about +-6 dB / octave
        if (k > 1) g *= (k % 2) ? (1.0 + std::fmin(0.0, (double)s.oddEven)) : (1.0 - std::fmax(0.0, (double)s.oddEven));
        if (s.formant > 0) { const double dd = std::log2(k / fc) / 0.35; g *= 1.0 + 4.0 * s.formant * std::exp(-dd * dd); }
        double ph = 0.0;
        if (s.mutate > 0) { g *= std::exp(s.mutate * 1.2 * (rand01(rs) - 0.5)); ph = s.mutate * pi * (rand01(rs) * 2 - 1); }
        if (g != 1.0 || ph != 0.0)
        {
            const double c = std::cos(ph), sn = std::sin(ph);
            const double r = re[(size_t)k] * c - im[(size_t)k] * sn, ii = re[(size_t)k] * sn + im[(size_t)k] * c;
            re[(size_t)k] = r * g; im[(size_t)k] = ii * g;
        }
        re[(size_t)(kWaveN - k)] = re[(size_t)k]; im[(size_t)(kWaveN - k)] = -im[(size_t)k];   // keep it real
    }
    re[kWaveN / 2] = im[kWaveN / 2] = 0.0;
    // 4. band-limited tables, all scaled by the peak of the richest one
    double scale = 0.0;
    std::vector<double> r2(kWaveN), i2(kWaveN);
    for (int L = 0; L < kMips; ++L)
    {
        const int kmax = 1024 >> L;
        for (int k = 0; k < kWaveN; ++k)
        {
            const int kk = k <= kWaveN / 2 ? k : kWaveN - k;
            const bool keep = kk >= 1 && kk <= kmax && kk < kWaveN / 2;
            r2[(size_t)k] = keep ? re[(size_t)k] : 0.0; i2[(size_t)k] = keep ? im[(size_t)k] : 0.0;
        }
        fft.run(r2.data(), i2.data(), true);
        if (L == 0)
        {   // as loud as a saw (RMS 0.577), but never a peak above 2 (the output limiter's comfort zone)
            double pk = 1e-12, e = 0.0;
            for (int i = 0; i < kWaveN; ++i) { pk = std::fmax(pk, std::fabs(r2[(size_t)i])); e += r2[(size_t)i] * r2[(size_t)i]; }
            const double rms = std::sqrt(e / kWaveN);
            scale = std::fmin(0.577 / std::fmax(rms, 1e-12), 2.0 / pk);
        }
        for (int i = 0; i < kWaveN; ++i) out.mip[L][i] = (float)(r2[(size_t)i] * scale);
        out.mip[L][kWaveN] = out.mip[L][0];
    }
    {   // the scope shows the shape at full height whatever its loudness scaling
        float pk = 1e-6f; for (int i = 0; i < kWaveN; ++i) pk = std::fmax(pk, std::fabs(out.mip[0][i]));
        for (int j = 0; j < kScopePoints; ++j) out.scope[j] = out.mip[0][j * kWaveN / kScopePoints] / pk;
    }
}

// one oscillator copy over a chunk reading a table (with a crossfade from the previous table)
inline void tableRun(float& phase, float inc, const WaveTable* cur, const WaveTable* prev, float fade, float fadeStep,
                     float gl, float gr, float* L, float* R, float* tap, int n)
{
    const int lv = WaveTable::level(inc);
    const float* t = cur->mip[lv];
    const float* o = prev ? prev->mip[lv] : nullptr;
    float ph = phase;
    for (int i = 0; i < n; ++i)
    {
        const float idx = ph * (float)kWaveN;
        const int a = (int)idx; const float f = idx - (float)a;
        float y = t[a] + (t[a + 1] - t[a]) * f;
        if (o) { const float yo = o[a] + (o[a + 1] - o[a]) * f; fade += fadeStep; if (fade > 1.0f) fade = 1.0f; y = yo + (y - yo) * fade; }
        ph += inc; if (ph >= 1.0f) ph -= 1.0f;
        L[i] += y * gl;
        if (R) R[i] += y * gr;
        if (tap) tap[i] = y;
    }
    phase = ph;
}

} // namespace elver
