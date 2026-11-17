// lufs_test - verification of LUFS Master 1.1 (desktop only, not shipped).
// usage: lufs_test <lufs-master.dll> [wav dir]
#include "mpc_vst2_abi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <windows.h>

namespace {
const int SR = 48000;
VstTimeInfo g_time;
VstIntPtr cb(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float) { if (op == audioMasterVersion) return 2400; if (op == audioMasterGetTime) return (VstIntPtr)&g_time; return 0; }
typedef AEffect* (*MainProc)(audioMasterCallback);
MainProc g_main;
int g_pass = 0, g_fail = 0;
std::string g_wavDir;
void check(bool ok, const std::string& what, const std::string& d = "")
{
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), d.empty() ? "" : "\n        ", d.c_str());
    ok ? ++g_pass : ++g_fail;
}
void transport(bool on) { g_time.flags = kVstTempoValid | kVstTransportChanged | (on ? kVstTransportPlaying : 0); }
double dB(double x) { return 20.0 * std::log10(std::fmax(x, 1e-12)); }

struct Stereo { std::vector<float> l, r; size_t size() const { return l.size(); } };

struct Plug
{
    AEffect* e = nullptr;
    std::map<std::string, int> idx;
    VstIntPtr call(VstInt32 op, VstInt32 i = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0) { return e->dispatcher(e, op, i, v, p, o); }
    void open() { e = g_main(cb); call(effSetSampleRate, 0, 0, nullptr, (float)SR); call(effSetBlockSize, 0, 256); call(effOpen); call(effSetProgram, 0, 0); call(effMainsChanged, 0, 1); call(effStartProcess); }
    void close() { call(effStopProcess); call(effMainsChanged, 0, 0); call(effClose); }
    int find(const char* n)
    {
        if (idx.empty()) { char b[256]; for (int i = 0; i < e->numParams; ++i) { b[0] = 0; call(effGetParamName, i, 0, b); idx[b] = i; } }
        auto it = idx.find(n); if (it == idx.end()) { std::printf("no param %s\n", n); std::exit(99); } return it->second;
    }
    void set(const char* n, float norm) { e->setParameter(e, find(n), norm); }
    float get(const char* n) { return e->getParameter(e, find(n)); }
    std::string text(const char* n) { char b[256] = {0}; call(effGetParamDisplay, find(n), 0, b); return b; }
    double num(const char* n) { return std::atof(text(n).c_str()); }
    void tap(const char* n) { set(n, get(n) > 0.5f ? 0.0f : 1.0f); }
    // plays `in` (or silence of `silence` seconds), appends output; calls tick every 100 ms
    template <class F> void play(const Stereo& in, Stereo* out, F tick)
    {
        float l[256], r[256];
        for (size_t p = 0; p < in.size(); p += 256)
        {
            const int n = (int)std::min<size_t>(256, in.size() - p);
            for (int i = 0; i < n; ++i) { l[i] = in.l[p + i]; r[i] = in.r[p + i]; }
            float* io[2] = {l, r};
            e->processReplacing(e, io, io, n);
            if (out) for (int i = 0; i < n; ++i) { out->l.push_back(l[i]); out->r.push_back(r[i]); }
            if ((p / 256) % 19 == 0) tick(p);
        }
    }
    void play(const Stereo& in, Stereo* out = nullptr) { play(in, out, [](size_t) {}); }
    void silence(double sec) { Stereo s; s.l.assign((size_t)(sec * SR), 0.0f); s.r = s.l; play(s); }
};

// ---- independent BS.1770 loudness (integrated, gated) ----------------------------------------------
struct BQ { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0; double run(double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
double integratedLufs(const Stereo& s, size_t from = 0)
{
    const double pi = 3.14159265358979323846;
    auto shelf = [&]() { const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196; const double K = std::tan(pi * f0 / SR), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416), a0 = 1.0 + K / Q + K * K;
        return BQ{(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0}; };
    auto hp = [&]() { const double f0 = 38.13547087602444, Q = 0.5003270373238773, K = std::tan(pi * f0 / SR), a0 = 1.0 + K / Q + K * K;
        return BQ{1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0}; };
    BQ s1[2] = {shelf(), shelf()}, h1[2] = {hp(), hp()};
    std::vector<double> e(s.size() - from);
    for (size_t i = from; i < s.size(); ++i) { const double a = h1[0].run(s1[0].run(s.l[i])), b = h1[1].run(s1[1].run(s.r[i])); e[i - from] = a * a + b * b; }
    const size_t step = SR / 10, win = 4 * step;
    std::vector<double> blocks;
    for (size_t p = 0; p + win <= e.size(); p += step) { double t = 0; for (size_t k = 0; k < win; ++k) t += e[p + k]; blocks.push_back(t / win); }
    auto L = [](double x) { return -0.691 + 10 * std::log10(std::fmax(x, 1e-30)); };
    double sum = 0; int n = 0; for (double b : blocks) if (L(b) > -70) { sum += b; ++n; }
    if (!n) return -200;
    const double rel = L(sum / n) - 10; sum = 0; n = 0;
    for (double b : blocks) if (L(b) > -70 && L(b) > rel) { sum += b; ++n; }
    return n ? L(sum / n) : -200;
}
// ---- independent true peak: 32x windowed-sinc interpolation, 64 taps --------------------------------
double truePeakDb(const Stereo& s, size_t from = 0)
{
    const int P = 32, T = 64; const double pi = 3.14159265358979323846;
    static std::vector<double> h;
    if (h.empty())
    {
        h.resize((size_t)P * T);
        for (int p = 0; p < P; ++p)
            for (int k = 0; k < T; ++k)
            {
                const double t = (double)k - (T / 2 - 1) - (double)p / P;
                const double sinc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(pi * t) / (pi * t);
                const double u = t / (T / 2.0), w = std::fabs(u) >= 1 ? 0 : 0.42 + 0.5 * std::cos(pi * u) + 0.08 * std::cos(2 * pi * u);
                h[(size_t)p * T + k] = sinc * w;
            }
    }
    double m = 0;
    for (const std::vector<float>* ch : {&s.l, &s.r})
        for (size_t i = from + T; i < ch->size(); ++i)
            for (int p = 0; p < P; ++p)
            {
                double acc = 0; const double* hp = &h[(size_t)p * T];
                for (int k = 0; k < T; ++k) acc += hp[k] * (*ch)[i - T + 1 + k];
                m = std::fmax(m, std::fabs(acc));
            }
    return dB(m);
}

// ---- test signals ---------------------------------------------------------------------------------
uint32_t g_rng = 12345;
float rnd() { g_rng = g_rng * 1664525u + 1013904223u; return (float)((g_rng >> 8) & 0xFFFF) / 32768.0f - 1.0f; }
// music-like: a bass line, chords with harmonics, hats; scaled to `lufs`
Stereo music(double sec, double lufs, uint32_t seed = 1)
{
    g_rng = seed;
    Stereo s; const size_t n = (size_t)(sec * SR); s.l.resize(n); s.r.resize(n);
    const double notes[] = {55, 65.4, 73.4, 49};
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double)i / SR, beat = std::fmod(t, 0.5), bar = std::floor(t / 2.0);
        const double f = notes[(int)bar % 4];
        double x = 0.5 * std::exp(-beat * 6) * std::sin(2 * 3.14159265 * f * t);
        for (int h = 1; h <= 6; ++h) x += 0.12 / h * std::sin(2 * 3.14159265 * f * 4 * h * t + h);
        x += 0.2 * std::exp(-std::fmod(t, 0.25) * 40) * rnd();
        s.l[i] = (float)x; s.r[i] = (float)(x * 0.9 + 0.05 * rnd());
    }
    const double now = integratedLufs(s), g = std::pow(10.0, (lufs - now) / 20.0);
    for (size_t i = 0; i < n; ++i) { s.l[i] = (float)(s.l[i] * g); s.r[i] = (float)(s.r[i] * g); }
    return s;
}
Stereo concat(std::initializer_list<Stereo> parts) { Stereo o; for (const Stereo& p : parts) { o.l.insert(o.l.end(), p.l.begin(), p.l.end()); o.r.insert(o.r.end(), p.r.begin(), p.r.end()); } return o; }
void wav(const std::string& name, const Stereo& s)
{
    if (g_wavDir.empty()) return;
    FILE* f = std::fopen((g_wavDir + "/" + name + ".wav").c_str(), "wb"); if (!f) return;
    const uint32_t n = (uint32_t)s.size(), bytes = n * 8;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(3); u16(2); u32(SR); u32(SR * 8); u16(8); u16(32);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (uint32_t i = 0; i < n; ++i) { std::fwrite(&s.l[i], 4, 1, f); std::fwrite(&s.r[i], 4, 1, f); }
    std::fclose(f);
}
float norm(double v, double lo, double hi) { return (float)((v - lo) / (hi - lo)); }
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return 1;
    if (argc > 2) g_wavDir = argv[2];
    g_main = (MainProc)GetProcAddress(LoadLibraryA(argv[1]), "VSTPluginMain");
    if (!g_main) { std::printf("cannot load\n"); return 1; }
    std::memset(&g_time, 0, sizeof g_time); g_time.tempo = 120; g_time.sampleRate = SR; transport(false);
    char d[700];
    const float kManual = 0.0f, kMeasure = 1.0f, kFast = 0.0f, kOff = 0.5f, kSlow = 1.0f;

    const Stereo quiet = music(40, -32, 1), loud = music(40, -6, 2);
    const Stereo varying = concat({music(8, -30, 3), music(8, -12, 4), music(8, -26, 5), music(8, -9, 6), music(8, -20, 7)});
    std::printf("signals (independent BS.1770): quiet %.1f LUFS, loud %.1f LUFS (true peak %.1f dBTP), varying %.1f LUFS\n",
                integratedLufs(quiet), integratedLufs(loud), truePeakDb(loud), integratedLufs(varying));

    // ---- 1. ceiling: output true peak never above it -----------------------------------------------
    {
        // inter-sample torture: fs/4 sine at 45 degrees (samples +-0.707 of a 1.0 peak), a square, full-scale noise
        Stereo torture; const size_t n = 3 * SR; torture.l.resize(n); torture.r.resize(n); g_rng = 9;
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double)i / SR; float x;
            if (i < (size_t)SR) x = (float)std::sin(2 * 3.14159265358979 * (SR / 4.0) * t + 0.7853981634);
            else if (i < (size_t)(2 * SR)) x = std::fmod(t * 997.0, 1.0) < 0.5 ? 0.99f : -0.99f;
            else x = rnd();
            torture.l[i] = x; torture.r[i] = -x;
        }
        std::string worst; double maxOver = -99; bool ok = true;
        for (double ceil : {0.0, -1.0, -3.0, -6.0, -12.0})
            for (int sig = 0; sig < 3; ++sig)
            {
                Plug p; p.open();
                p.set("Ceiling", norm(ceil, -12, 0)); p.set("Mode", kManual); p.set("Input Gain", norm(sig == 1 ? 12.0 : 6.0, -24, 24));
                const Stereo& in = sig == 0 ? torture : (sig == 1 ? loud : varying);
                Stereo out; p.play(in, &out);
                const double tp = truePeakDb(out, SR / 2), over = tp - ceil;
                std::printf("      ceiling %5.1f  %-14s output %+.2f dBTP (%+.2f)  limiter max %s\n", ceil, sig == 0 ? "torture" : (sig == 1 ? "loud +12" : "varying +6"), tp, over, p.text("GR Max").c_str());
                if (over > maxOver) { maxOver = over; std::snprintf(d, sizeof d, "ceiling %.0f dBTP, %s: output %.2f dBTP", ceil, sig == 0 ? "torture" : (sig == 1 ? "loud +12 dB" : "varying +6 dB"), tp); worst = d; }
                if (over > 0.0) ok = false;
                if (sig == 1 && ceil == -1.0) wav("ceiling_minus1_loud_plus12", out);
                p.close();
            }
        check(ok, "ceiling: output true peak (independent 32x meter) never above the ceiling: 0, -1, -3, -6, -12 dBTP x 3 signals",
              "closest: " + worst + std::string("  (") + std::to_string(maxOver).substr(0, 5) + " dB vs ceiling)");
        Plug p; p.open();
        const VstInt32 lat = p.e->initialDelay; p.close();
        std::snprintf(d, sizeof d, "%d samples = %.2f ms at 48 kHz, %.2f ms at 44.1 kHz", lat, lat * 1000.0 / 48000, lat * 1000.0 / 44100);
        check(lat == 160, "latency reported to the host", d);
    }

    // ---- 2. input level meter and peak hold --------------------------------------------------------
    {
        Plug p; p.open(); p.set("Mode", kManual); p.set("Input Gain", norm(-12, -24, 24));
        Stereo s; s.l.assign(SR, 0.0f); s.r = s.l;
        for (int i = 0; i < SR / 2; ++i) s.l[i] = (float)(0.5 * std::sin(2 * 3.14159265 * 1000 * i / SR));   // -6 dBFS, then quieter
        for (int i = SR / 2; i < SR; ++i) s.l[i] = (float)(0.1 * std::sin(2 * 3.14159265 * 1000 * i / SR));   // -20 dBFS
        p.play(s);
        const std::string lvl = p.text("In Level"), hold = p.text("In Peak Hold");
        p.close();
        std::snprintf(d, sizeof d, "0.5 s at -6.0 dBFS then -20 dBFS (gain -12 dB, not counted): level %s, hold %s", lvl.c_str(), hold.c_str());
        check(std::fabs(std::atof(lvl.c_str()) + 20.0) < 0.2 && std::fabs(std::atof(hold.c_str()) + 6.0) < 0.2, "input level meter: raw level before any gain, with peak hold", d);
    }

    // ---- 3. Fast reacts quickly, Slow gently; ramps are smooth -------------------------------------------
    {
        struct R { double down90, up90, maxRate, maxStep, maxCurve; std::string grMax; };
        auto run = [&](float speed, const char* name) {
            Plug p; p.open(); p.set("Mode", kMeasure); p.set("Live Speed", speed); p.set("Max Change", norm(12, 1, 24));
            p.set("Target", 1.0f); p.set("Custom LUFS", norm(-26, -30, -6)); p.set("Ceiling", 1.0f);   // target -26, ceiling 0: the step stays under it
            // 20 s on target (-14), +10 dB step for 30 s, back down for 30 s. Right channel: tiny DC = the gain probe.
            Stereo a = music(20, -26, 11), b = music(30, -18, 12), c = music(30, -26, 13), in = concat({a, b, c});   // an 8 dB step
            for (float& x : in.r) x = 0.001f;
            std::vector<double> gainCurve; std::vector<std::pair<double, double>> readout;
            Stereo out;
            transport(true);
            p.play(in, &out, [&](size_t pos) { readout.push_back({(double)pos / SR, p.num("Gain Change")}); });
            transport(false); const std::string p2grMax = p.text("GR Max"); p.close();
            for (size_t i = 0; i < out.size(); ++i) gainCurve.push_back(out.r[i] / 0.001);
            R r{};
            auto tReach = [&](double from, double lvl, bool down) { for (auto& x : readout) if (x.first > from && (down ? x.second <= lvl : x.second >= lvl)) return x.first - from; return 99.0; };
            const double g0 = readout[(size_t)(19.5 * 10)].second;          // before the step (about 0 dB)
            r.down90 = tReach(20.0, g0 - 7.2, true);
            r.up90 = tReach(50.0, g0 - 1.0, false);
            r.maxRate = 0;
            for (size_t k = 5; k < readout.size(); ++k) r.maxRate = std::fmax(r.maxRate, std::fabs(readout[k].second - readout[k - 5].second) / (readout[k].first - readout[k - 5].first));
            // gain probe: per-sample step and curvature in dB (limiter included)
            r.maxStep = 0; r.maxCurve = 0;
            for (size_t i = SR; i + 1 < gainCurve.size(); ++i)
            {
                const double a0 = dB(gainCurve[i - 1]), a1 = dB(gainCurve[i]), a2 = dB(gainCurve[i + 1]);
                r.maxStep = std::fmax(r.maxStep, std::fabs(a1 - a0));
                r.maxCurve = std::fmax(r.maxCurve, std::fabs(a2 - 2 * a1 + a0));
            }
            r.grMax = p2grMax;
            wav(std::string("speed_") + name, out);
            return r;
        };
        const R f = run(kFast, "fast"), s = run(kSlow, "slow");
        std::snprintf(d, sizeof d, "Fast: 90%% of an 8 dB cut in %.1f s, back up in %.1f s, fastest %.1f dB/s | Slow: cut in %.1f s, back up in %.1f s, fastest %.2f dB/s",
                      f.down90, f.up90, f.maxRate, s.down90, s.up90, s.maxRate);
        check(f.down90 <= 2.0 && f.up90 <= 8.0, "Live Speed Fast reacts quickly (90 % of an 8 dB correction within 2 s down, back within 1 dB in 8 s)", d);
        check(s.down90 >= 8.0 && s.maxRate <= 1.05, "Live Speed Slow reacts gently (no faster than 1 dB per second)", d);
        std::snprintf(d, sizeof d, "limiter idle (GR max %s / %s); largest gain change between two samples: Fast %.5f dB, Slow %.5f dB; largest curvature %.6f / %.6f dB",
                      f.grMax.c_str(), s.grMax.c_str(), f.maxStep, s.maxStep, f.maxCurve, s.maxCurve);
        check(f.grMax == "0.0 dB" && s.grMax == "0.0 dB" && f.maxStep < 0.005 && s.maxStep < 0.005, "gain ramps smoothly, sample by sample: no jumps, no zipper", d);

        // Off: measures only, never adjusts; Max Change limits the ride
        Plug p; p.open(); p.set("Mode", kMeasure); p.set("Live Speed", kOff);
        transport(true); p.play(music(10, -30, 14)); const double off = p.num("Gain Change"); const std::string sug = p.text("Suggested"); transport(false); p.close();
        Plug q; q.open(); q.set("Mode", kMeasure); q.set("Live Speed", kFast); q.set("Max Change", norm(4, 1, 24));
        transport(true); q.play(music(15, -30, 14)); const double capped = q.num("Gain Change"); transport(false); q.close();
        std::snprintf(d, sizeof d, "quiet input, Off: gain %.1f dB, suggested %s; Fast with Max Change 4 dB: gain %.1f dB", off, sug.c_str(), capped);
        check(std::fabs(off) < 0.05 && std::fabs(std::atof(sug.c_str()) - 16.0) < 0.6 && std::fabs(capped - 4.0) < 0.15, "Live Speed Off only measures (suggests gain); Max Change caps Fast / Slow", d);
    }

    // ---- 4. auto-set on stop, then a second pass lands on target -----------------------------------------
    {
        std::string all; bool ok = true, okSet = true;
        for (int sig = 0; sig < 3; ++sig)
        {
            const Stereo& in = sig == 0 ? quiet : (sig == 1 ? loud : varying);
            const char* name = sig == 0 ? "quiet" : (sig == 1 ? "loud" : "varying");
            for (int tgt : {0, 2})                 // -14 and -23 LUFS
            {
                const double target = tgt == 0 ? -14.0 : -23.0;
                Plug p; p.open(); p.set("Target", tgt / 4.0f); p.set("Mode", kMeasure); p.set("Live Speed", kOff); p.set("Ceiling", norm(-1, -12, 0));
                transport(true); p.play(in); transport(false);
                p.silence(1.0);                                  // the suggestion is refined after the stop
                const double gainSet = p.num("Input Gain"); const std::string status = p.text("Auto-Set"), mode = p.text("Mode"), sug = p.text("Suggested");
                const double expect = target - integratedLufs(in);
                Stereo out;
                transport(true); p.play(in, &out); transport(false);
                const double outInt = integratedLufs(out, 160), shown = p.num("Out Int LUFS");
                p.close();
                std::snprintf(d, sizeof d, "%s -> %.0f: input gain set to %+.2f dB (target - input = %+.2f, suggested %s, '%s', mode %s); 2nd pass %.2f LUFS (meter shows %.1f), off by %+.2f LU\n        ",
                              name, target, gainSet, expect, sug.c_str(), status.c_str(), mode.c_str(), outInt, shown, outInt - target);
                all += d;
                if (std::fabs(outInt - target) > 0.5 || std::fabs(shown - outInt) > 0.15) ok = false;
                if (status.rfind("APPLIED", 0) != 0 || mode != "Manual" || std::fabs(gainSet - std::atof(sug.c_str())) > 0.06) okSet = false;
                if (sig == 1 && tgt == 0) wav("second_pass_loud_minus14", out);
            }
        }
        check(okSet, "auto-set: stopping a Measure pass writes the suggested gain into Input Gain, switches to Manual, shows APPLIED", all);
        check(ok, "second pass lands on target (within 0.5 LU), and the output integrated meter matches the output", "");
    }

    // ---- 5. Clear and Undo; auto-set with Fast keeps Measure and starts the next ride corrected -----------
    {
        Plug p; p.open(); p.set("Mode", kMeasure); p.set("Live Speed", kOff); p.set("Input Gain", norm(2, -24, 24));
        transport(true); p.play(quiet); transport(false); p.silence(1.0);
        const double set1 = p.num("Input Gain");
        p.tap("Undo"); p.silence(0.2);
        const double undone = p.num("Input Gain"); const std::string modeU = p.text("Mode"), stU = p.text("Auto-Set");
        transport(true); p.play(quiet); transport(false); p.silence(1.0);
        p.tap("Clear"); p.silence(0.2);
        const double cleared = p.num("Input Gain"); const std::string stC = p.text("Auto-Set");
        p.close();
        std::snprintf(d, sizeof d, "auto-set %+.1f dB; Undo -> %+.1f dB, mode %s, '%s'; Clear -> %+.1f dB, '%s'", set1, undone, modeU.c_str(), stU.c_str(), cleared, stC.c_str());
        check(std::fabs(undone - 2.0) < 0.05 && modeU == "Measure" && std::fabs(cleared) < 0.05 && stC.find("cleared") != std::string::npos, "Clear and Undo of the auto-set", d);

        Plug q; q.open(); q.set("Mode", kMeasure); q.set("Live Speed", kSlow);
        transport(true); q.play(quiet); transport(false); q.silence(1.0);
        const std::string m = q.text("Mode"); const double g = q.num("Input Gain");
        transport(true); q.play(music(1, -32, 1)); const double start = q.num("Gain Change"); transport(false); q.close();
        std::snprintf(d, sizeof d, "Slow pass on quiet input: Input Gain %+.1f dB, mode stays %s, next pass starts its ride at %+.1f dB", g, m.c_str(), start);
        check(m == "Measure" && std::fabs(start - std::fmin(g, 12.0)) < 1.0, "auto-set with Fast / Slow keeps Measure and starts the next ride corrected", d);
    }

    // ---- 6. saved with the project: input gain, auto-set, mode; an old 1.0 Locked project migrates --------
    {
        Plug p; p.open(); p.set("Mode", kMeasure); p.set("Live Speed", kOff);
        transport(true); p.play(quiet); transport(false); p.silence(1.0);
        const std::string g1 = p.text("Input Gain"), s1 = p.text("Auto-Set"), m1 = p.text("Mode");
        void* ptr = nullptr; const VstIntPtr len = p.call(effGetChunk, 0, 0, &ptr);
        std::vector<char> chunk((char*)ptr, (char*)ptr + len);
        p.close();
        Plug q; q.open(); q.call(effSetChunk, 0, (VstIntPtr)chunk.size(), chunk.data()); q.call(effSetProgram, 0, 0); q.silence(0.3);
        const std::string g2 = q.text("Input Gain"), s2 = q.text("Auto-Set"), m2 = q.text("Mode");
        q.tap("Undo"); q.silence(0.2); const std::string g3 = q.text("Input Gain");
        q.close();
        std::snprintf(d, sizeof d, "saved: %s, '%s', %s -> reloaded: %s, '%s', %s; Undo after reload -> %s", g1.c_str(), s1.c_str(), m1.c_str(), g2.c_str(), s2.c_str(), m2.c_str(), g3.c_str());
        check(g1 == g2 && s1 == s2 && m1 == m2 && g3.rfind("0.0 dB", 0) == 0, "project save / reload keeps Input Gain, the auto-set indicator, mode and the undo", d);

        // a 1.0 chunk (Mode = Locked, locked gain -4.5 dB)
        std::vector<uint8_t> c = {'M', 'P', 'C', 'P'};
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) c.push_back((uint8_t)(v >> (8 * i))); };
        auto f32 = [&](float f) { uint32_t b; std::memcpy(&b, &f, 4); u32(b); };
        u32(1); u32(2);
        c.push_back(4); for (char ch : std::string("mode")) c.push_back((uint8_t)ch); f32(2.0f);
        c.push_back(7); for (char ch : std::string("ceiling")) c.push_back((uint8_t)ch); f32(-2.0f);
        std::vector<uint8_t> ex = {1, 1}; for (float f : {-4.5f, -14.0f, -1.0f, -14.0f, 0.0f}) { uint8_t b[4]; std::memcpy(b, &f, 4); ex.insert(ex.end(), b, b + 4); }
        for (int i = 0; i < 4; ++i) ex.push_back(0);
        u32((uint32_t)ex.size()); c.insert(c.end(), ex.begin(), ex.end());
        Plug m; m.open(); m.call(effSetChunk, 0, (VstIntPtr)c.size(), c.data()); m.silence(0.2);
        const std::string mg = m.text("Input Gain"), mm = m.text("Mode"), mc = m.text("Ceiling");
        m.close();
        std::snprintf(d, sizeof d, "1.0 project (Locked at -4.5 dB, ceiling -2): now %s, Input Gain %s, Ceiling %s", mm.c_str(), mg.c_str(), mc.c_str());
        check(mm == "Manual" && mg == "-4.5 dB" && mc == "-2.0 dBTP", "a 1.0 project with a Locked gain opens in Manual with that gain", d);
    }

    // ---- 7. LUFS over time updates live in every mode, against any target; output integrated is the output -
    {
        std::string res; bool ok = true;
        const char* names[4] = {"Manual", "Measure Off", "Measure Fast", "Measure Slow"};
        for (int m = 0; m < 4; ++m)
        {
            Plug p; p.open(); p.set("Mode", m == 0 ? kManual : kMeasure); p.set("Live Speed", m == 1 ? kOff : (m == 2 ? kFast : kSlow));
            p.set("Target", 1.0f); p.set("Custom LUFS", norm(-18, -30, -6));          // custom target -18
            std::vector<std::string> st; std::vector<float> hist;
            transport(m % 2 == 0);                                                      // with and without the transport
            p.play(varying, nullptr, [&](size_t) { st.push_back(p.text("Out ST LUFS")); hist.push_back(p.get("Hist 48")); });
            transport(false);
            int changes = 0; for (size_t k = 1; k < st.size(); ++k) changes += st[k] != st[k - 1];
            int hchanges = 0; for (size_t k = 1; k < hist.size(); ++k) hchanges += hist[k] != hist[k - 1];
            const std::string vs = p.text("Vs Target"), ost = p.text("Out ST LUFS");
            p.close();
            const double expectVs = std::atof(ost.c_str()) + 18.0;
            std::snprintf(d, sizeof d, "%s: short-term changed %d times, graph column %d times, now %s = %s vs -18\n        ", names[m], changes, hchanges, ost.c_str(), vs.c_str());
            res += d;
            if (changes < 100 || hchanges < 20 || std::fabs(std::atof(vs.c_str()) - expectVs) > 0.15) ok = false;
        }
        check(ok, "LUFS over time keeps updating in every mode, with or without the transport, tracking a custom target", res);
        Plug p; p.open(); p.set("Mode", kManual); p.set("Input Gain", norm(-6, -24, 24));
        Stereo out; transport(true); p.play(quiet, &out); transport(false);
        const double shown = p.num("Out Int LUFS"), inShown = p.num("In Int LUFS"); p.close();
        std::snprintf(d, sizeof d, "quiet input %.1f LUFS with -6 dB: output meter %.1f, independent %.2f; input meter %.1f", integratedLufs(quiet), shown, integratedLufs(out, 160), inShown);
        check(std::fabs(shown - integratedLufs(out, 160)) < 0.15 && std::fabs(shown - inShown + 6.0) < 0.2, "output integrated meter shows what is sent out, after the gain", d);
    }

    // ---- 8. labels: every display shows a unit -------------------------------------------------------------
    {
        Plug p; p.open(); p.set("Mode", kMeasure); transport(true); p.play(music(5, -14, 1)); transport(false);
        std::string missing;
        for (const char* n : {"Ceiling", "Custom LUFS", "Input Gain", "Max Change", "In Level", "In Peak Hold", "Vs Target", "Out Int LUFS", "Out ST LUFS",
                              "Gain Change", "Limiter GR", "Suggested", "In TP Max", "Out TP Max", "In Int LUFS"})
        {
            const std::string t = p.text(n);
            if (t.find("dB") == std::string::npos && t.find("LU") == std::string::npos && t != "--") missing += std::string(n) + " '" + t + "', ";
        }
        p.close();
        check(missing.empty(), "every control and meter value carries its unit (dB, dBTP, dBFS, LU, LUFS)", missing);
    }
    std::printf("\n%d passed, %d failure(s)\n", g_pass, g_fail);
    return g_fail;
}
