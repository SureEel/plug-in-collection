// hydra_test - functional and CPU tests for Hydra Eel (desktop only, not shipped).
// usage: hydra_test <plugin.dll> <scratch dir for user presets> [cpu]
#include "mpc_vst2_abi.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace {
VstTimeInfo g_time;
VstIntPtr hostCb(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float)
{
    if (op == audioMasterVersion) return 2400;
    if (op == audioMasterGetTime) return (VstIntPtr)&g_time;
    return 0;
}
typedef AEffect* (*MainProc)(audioMasterCallback);
MainProc g_main = nullptr;
int g_fail = 0;
const int SR = 44100;
void check(bool ok, const std::string& what, const std::string& d = "")
{
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), d.empty() ? "" : "  -- ", d.c_str());
    if (!ok) ++g_fail;
}
std::vector<float> plucks(double sec, float amp = 0.3f, double f0 = 110.0)
{
    std::vector<float> x((size_t)(sec * SR)); double ph = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double)i / SR, tn = std::fmod(t, 0.5);
        const double f = ((int)(t / 0.5) % 2) ? f0 * 1.5 : f0;
        ph += f / SR; ph -= std::floor(ph);
        x[i] = (float)(amp * std::exp(-tn * 5.0) * (2.0 * ph - 1.0));
    }
    return x;
}
std::vector<float> sine(double sec, double hz, double amp)
{
    std::vector<float> s((size_t)(sec * SR));
    for (size_t i = 0; i < s.size(); ++i) s[i] = (float)(amp * std::sin(6.283185307179586 * hz * (double)i / SR));
    return s;
}
struct Plug
{
    AEffect* e = nullptr;
    std::vector<float> right;
    VstIntPtr call(VstInt32 op, VstInt32 i = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0) { return e->dispatcher(e, op, i, v, p, o); }
    void open(int prog = 0, int block = 128)
    {
        e = g_main(hostCb);
        call(effSetSampleRate, 0, 0, nullptr, (float)SR); call(effSetBlockSize, 0, block); call(effOpen);
        call(effSetProgram, 0, prog); call(effMainsChanged, 0, 1); call(effStartProcess);
    }
    void close() { call(effStopProcess); call(effMainsChanged, 0, 0); call(effClose); }
    int find(const char* n) { char b[256]; for (int i = 0; i < e->numParams; ++i) { b[0] = 0; call(effGetParamName, i, 0, b); if (!std::strcmp(b, n)) return i; } std::printf("no param %s\n", n); std::exit(99); }
    void set(const char* n, float v) { e->setParameter(e, find(n), v); }
    float getN(const char* n) { return e->getParameter(e, find(n)); }
    std::string text(const char* n) { char b[256] = {0}; call(effGetParamDisplay, find(n), 0, b); return b; }
    void tap(const char* n) { run(std::vector<float>((size_t)(0.4 * SR), 0.0f)); set(n, getN(n) > 0.5f ? 0.0f : 1.0f); }
    std::vector<float> run(const std::vector<float>& in, int block = 128)
    {
        std::vector<float> out(in.size()); right.assign(in.size(), 0.0f);
        std::vector<float> l(block), r(block);
        for (size_t p = 0; p < in.size(); p += block)
        {
            const int n = (int)std::min<size_t>(block, in.size() - p);
            for (int i = 0; i < n; ++i) l[i] = r[i] = in[p + i];
            float* io[2] = {l.data(), r.data()};
            e->processReplacing(e, io, io, n);
            for (int i = 0; i < n; ++i) { out[p + i] = l[i]; right[p + i] = r[i]; }
        }
        return out;
    }
    // sections: all off, then switch the wanted ones on
    void only(std::initializer_list<const char*> on)
    {
        for (const char* s : {"Drive On", "Mod On", "Delay On", "Reverb On"}) set(s, 0.0f);
        for (const char* s : on) set(s, 1.0f);
    }
};
float norm(Plug& p, const char* name, int index, int count) { (void)p; (void)name; return count > 1 ? (float)index / (float)(count - 1) : 0.0f; }
double rms(const std::vector<float>& v, size_t a, size_t b) { double s = 0; for (size_t i = a; i < b && i < v.size(); ++i) s += (double)v[i] * v[i]; return std::sqrt(s / std::max<size_t>(1, b - a)); }
float peak(const std::vector<float>& v, size_t a = 0) { float m = 0; for (size_t i = a; i < v.size(); ++i) m = std::fmax(m, std::fabs(v[i])); return m; }
// Goertzel magnitude of one frequency
double tone(const std::vector<float>& v, double hz, size_t a, size_t b)
{
    // Hann-windowed (the window keeps strong neighbouring tones from leaking into the bin)
    const double w = 6.283185307179586 * hz / SR, c = 2 * std::cos(w); double s1 = 0, s2 = 0;
    const double n = (double)(b - a);
    for (size_t i = a; i < b; ++i) { const double win = 0.5 - 0.5 * std::cos(6.283185307179586 * (double)(i - a) / n); const double s0 = v[i] * win + c * s1 - s2; s2 = s1; s1 = s0; }
    return std::sqrt(std::fmax(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / n * 4.0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) return 1;
    _putenv_s("MPC_PLUGIN_FOLDER", argv[2]);
    std::remove((std::string(argv[2]) + "/user-presets.bin").c_str());
    HMODULE lib = LoadLibraryA(argv[1]); if (lib) g_main = (MainProc)GetProcAddress(lib, "VSTPluginMain");
    if (!g_main) { std::printf("cannot load\n"); return 1; }
    std::memset(&g_time, 0, sizeof g_time); g_time.tempo = 120; g_time.sampleRate = SR; g_time.flags = kVstTempoValid;
    char d[400];
    const bool cpuOnly = argc > 3 && !std::strcmp(argv[3], "cpu");

    // ---- CPU per section: desktop microseconds per 128-frame block ------------------------------
    {
        struct Case { const char* name; std::vector<std::pair<const char*, float>> s; std::initializer_list<const char*> on; };
        auto timeIt = [&](std::initializer_list<const char*> on, const std::vector<std::pair<const char*, float>>& s) {
            Plug p; p.open(0); p.only(on);
            for (const auto& kv : s) p.set(kv.first, kv.second);
            const std::vector<float> in = plucks(4.0);
            p.run(in);                                                  // warm up
            double best = 1e9;
            for (int rep = 0; rep < 5; ++rep)
            {
                const auto t0 = std::chrono::high_resolution_clock::now();
                p.run(in);
                const double us = std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
                best = std::fmin(best, us / (in.size() / 128.0));
            }
            p.close();
            return best;   // us per 128 frames (2902 us of audio)
        };
        const double base = timeIt({}, {});
        struct Row { const char* label; double us; };
        std::vector<Row> rows = {
            {"all sections off", base},
            {"drive: compressor (opto)", timeIt({"Drive On"}, {{"Drive Type", 0.0f}})},
            {"drive: fuzz (sustain, 4x oversampled)", timeIt({"Drive On"}, {{"Drive Type", 6.0f / 7.0f}})},
            {"mod: tremolo/vibrato (mid blend)", timeIt({"Mod On"}, {{"Mod Mode", 0.0f}, {"Blend", 0.5f}})},
            {"mod: rotary + phaser (mid blend)", timeIt({"Mod On"}, {{"Mod Mode", 1.0f}, {"Blend", 0.5f}})},
            {"delay: analog stereo", timeIt({"Delay On"}, {{"Delay Type", 1.0f}})},
            {"reverb: plate", timeIt({"Reverb On"}, {{"Reverb Type", 0.0f}})},
            {"reverb: spring (stereo)", timeIt({"Reverb On"}, {{"Reverb Type", 0.5f}})},
            {"reverb: shimmer", timeIt({"Reverb On"}, {{"Reverb Type", 1.0f}})},
            {"default preset (fuzz, trem, analog, spring)", timeIt({"Drive On", "Mod On", "Delay On", "Reverb On"}, {})},
            {"worst case (fuzz, rotary+phaser, analog, shimmer)", timeIt({"Drive On", "Mod On", "Delay On", "Reverb On"},
                {{"Drive Type", 6.0f / 7.0f}, {"Mod Mode", 1.0f}, {"Blend", 0.5f}, {"Delay Type", 1.0f}, {"Reverb Type", 1.0f}})},
            {"worst case, mono", timeIt({"Drive On", "Mod On", "Delay On", "Reverb On"},
                {{"Drive Type", 6.0f / 7.0f}, {"Mod Mode", 1.0f}, {"Blend", 0.5f}, {"Delay Type", 1.0f}, {"Reverb Type", 1.0f}, {"Stereo", 0.0f}})},
        };
        std::printf("CPU, desktop, per 128-frame block (2.9 ms):\n");
        for (const Row& r : rows) std::printf("  %-52s %6.1f us  %5.2f %%  (Force est. x15..25: %4.0f..%2.0f %%)\n",
                                              r.label, r.us, 100 * r.us / 2902.0, 15 * 100 * r.us / 2902.0, 25 * 100 * r.us / 2902.0);
        if (cpuOnly) return 0;
    }
    const int LAT = 64;

    // 1. bypass is exact; every factory preset, driven hot, stays finite and under -1 dBFS
    {
        Plug p; p.open(0); p.set("Bypass", 1.0f);
        const std::vector<float> in = plucks(1.0), o = p.run(in);
        int diff = 0; for (size_t i = LAT + 2000; i < in.size(); ++i) if (o[i] != in[i - LAT]) ++diff;
        p.close();
        check(diff == 0, "bypass: output == input delayed 64 samples", std::to_string(diff) + " samples differ");
        Plug q0; q0.open(0); const int nprog = q0.e->numPrograms; q0.close();
        float worst = 0; bool finite = true; std::string wn;
        for (int i = 0; i < nprog - 16; ++i)
        {
            Plug q; q.open(i); q.set("Input", 1.0f);
            const std::vector<float> o2 = q.run(plucks(3.0, 0.9f));
            for (size_t k = 0; k < o2.size(); ++k) { finite = finite && std::isfinite(o2[k]) && std::isfinite(q.right[k]); }
            const float pk = std::fmax(peak(o2), peak(q.right));
            if (pk > worst) { worst = pk; char b[64] = {0}; q.call(effGetProgramNameIndexed, i, 0, b); wn = b; }
            q.close();
        }
        std::snprintf(d, sizeof d, "%d factory presets at +24 dB input; loudest %s at %.2f dBFS", nprog - 16, wn.c_str(), 20 * std::log10(worst));
        check(finite && worst <= 0.8913f + 1e-4f, "limiter: every factory preset, driven hot, stays under -1 dBFS", d);
    }
    // 2. every selector takes every choice
    {
        Plug p; p.open(0);
        struct Sel { const char* name; std::vector<const char*> t; };
        const Sel sels[] = {{"Stereo", {"Mono", "Stereo"}},
            {"Drive Type", {"Opto", "Smooth", "FET", "Punch", "Germanium", "Gated", "Sustain", "Octave"}},
            {"Mod Mode", {"Trem / Vib", "Rotary / Phaser"}}, {"Wave", {"Sine", "Triangle", "Square"}}, {"Rotor", {"Slow", "Fast"}},
            {"Delay Type", {"Digital", "Analog"}}, {"Sync", {"ms", "Tempo"}}, {"Reverb Type", {"Plate", "Spring", "Shimmer"}}};
        for (const Sel& s : sels)
        {
            bool ok = true; std::string seen;
            for (size_t k = 0; k < s.t.size(); ++k)
            {
                p.set(s.name, s.t.size() > 1 ? (float)k / (float)(s.t.size() - 1) : 0.0f); p.run(plucks(0.02));
                const std::string got = p.text(s.name); seen += (k ? "/" : "") + got; ok = ok && got == s.t[k];
            }
            check(ok, std::string("selector: ") + s.name + " takes every choice", seen);
        }
        p.close();
    }
    // 3. compressors even out the dynamics; the gate closes on quiet signals
    {
        // 4 s loud, 4 s soft (repeated): an opto releases slowly, so measure the last second of each
        std::vector<float> in((size_t)(16 * SR));
        for (size_t i = 0; i < in.size(); ++i) { const double t = (double)i / SR; in[i] = (float)(((int)(t / 4) % 2 ? 0.05 : 0.5) * std::sin(6.283185 * 220 * t)); }
        for (int type = 0; type < 4; ++type)
        {
            Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", type / 7.0f); p.set("Amount", 0.8f);
            const std::vector<float> o = p.run(in);
            const double loud = rms(o, 11 * SR, 12 * SR - 2000), soft = rms(o, 15 * SR, 16 * SR - 2000);
            const std::string gr = p.text("Comp GR");
            p.close();
            std::snprintf(d, sizeof d, "20 dB input step -> %.1f dB at the output; Comp GR shows %s", 20 * std::log10(loud / soft), gr.c_str());
            check(20 * std::log10(loud / soft) < 12.0, std::string("compressor ") + (const char*[]){"opto", "smooth", "FET", "punch"}[type] + ": squeezes a 20 dB jump", d);
        }
        Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", 0.0f); p.set("Amount", 0.0f); p.set("Gate Bias", 0.8f);
        const std::vector<float> q = p.run(sine(2.0, 220, 0.003));     // -50 dBFS hum
        const std::string gt = p.text("Gate Bias");
        p.close();
        std::snprintf(d, sizeof d, "-50 dBFS signal with %s -> %.0f dBFS", gt.c_str(), 20 * std::log10(rms(q, SR, 2 * SR) * 1.414 + 1e-9));
        check(rms(q, SR, 2 * SR) < 0.0003, "compressor gate: closes below its threshold", d);
    }
    // 4. fuzzes: distortion, octave-up, low aliasing, starve chokes quiet notes
    {
        auto fuzz = [&](int type, const std::vector<float>& in, float amount, float bias) {
            Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", type / 7.0f); p.set("Amount", amount); p.set("Gate Bias", bias);
            p.set("Tone", 0.7f);
            std::vector<float> o = p.run(in); p.close(); return o;
        };
        const std::vector<float> s220 = sine(2.0, 220, 0.3);
        for (int type = 4; type < 8; ++type)
        {
            const std::vector<float> o = fuzz(type, s220, 0.6f, 0.0f);
            const double f1 = tone(o, 220, SR, 2 * SR), h2 = tone(o, 440, SR, 2 * SR), h3 = tone(o, 660, SR, 2 * SR);
            std::snprintf(d, sizeof d, "220 Hz in: 2nd harmonic %.0f dB, 3rd %.0f dB re fundamental", 20 * std::log10(h2 / f1), 20 * std::log10(h3 / f1));
            const bool ok = type == 7 ? h2 > f1 : (h2 + h3) / f1 > 0.1;
            check(ok, std::string("fuzz ") + (const char*[]){"germanium", "gated", "sustain", "octave"}[type - 4] + (type == 7 ? ": the octave above dominates" : ": distorts"), d);
        }
        // aliasing: a high note through the hardest fuzz; harmonics above 22 kHz fold back to inharmonic
        // frequencies. Measure the strongest inharmonic component against the fundamental.
        const std::vector<float> s31 = sine(2.0, 3100, 0.3);
        const std::vector<float> o = fuzz(6, s31, 1.0f, 0.0f);
        const double f1 = tone(o, 3100, SR, 2 * SR);
        double worst = 0; int wf = 0;
        for (int k = 8; k <= 28; ++k) { double fa = std::fmod(k * 3100.0, 44100.0); if (fa > 22050) fa = 44100 - fa; if (std::fmod(fa, 3100.0) < 50 || 3100 - std::fmod(fa, 3100.0) < 50) continue; const double a = tone(o, fa, SR, 2 * SR); if (a > worst) { worst = a; wf = (int)fa; } }
        std::snprintf(d, sizeof d, "3.1 kHz at full gain: strongest alias (%d Hz) at %.0f dB re fundamental", wf, 20 * std::log10(worst / f1));
        check(20 * std::log10(worst / f1) < -40.0, "fuzz anti-aliasing: folded harmonics stay below -40 dB", d);
        (void)0;   // starve: see test 16
    }
    // 5. modulation
    {
        auto envSpread = [&](const std::vector<float>& o) {   // loudest / quietest 10 ms window, last 2 s
            double mx = 0, mn = 1e9; for (size_t a = 2 * SR; a + 441 < o.size(); a += 441) { const double r = rms(o, a, a + 441); mx = std::fmax(mx, r); mn = std::fmin(mn, r); }
            return 20 * std::log10(mx / (mn + 1e-12));
        };
        auto zcSpread = [&](const std::vector<float>& o) {      // spread of zero-crossing periods (pitch wobble), %
            std::vector<double> per; double last = -1;
            for (size_t i = 2 * SR; i + 1 < o.size(); ++i) if (o[i] <= 0 && o[i + 1] > 0) { const double t = i + o[i] / (o[i] - o[i + 1]); if (last > 0) per.push_back(t - last); last = t; }
            double mn = 1e9, mx = 0; for (double v : per) { mn = std::fmin(mn, v); mx = std::fmax(mx, v); }
            return 100 * (mx - mn) / mn;
        };
        Plug a; a.open(0); a.only({"Mod On"}); a.set("Depth", 0.8f); a.set("Blend", 0.0f);
        const std::vector<float> trem = a.run(sine(4.0, 330, 0.3)); a.close();
        Plug b; b.open(0); b.only({"Mod On"}); b.set("Depth", 1.0f); b.set("Blend", 1.0f);
        const std::vector<float> vib = b.run(sine(4.0, 330, 0.3)); b.close();
        std::snprintf(d, sizeof d, "tremolo: level swing %.0f dB, pitch wobble %.1f %%; vibrato: level swing %.1f dB, pitch wobble %.1f %%",
                      envSpread(trem), zcSpread(trem), envSpread(vib), zcSpread(vib));
        check(envSpread(trem) > 12 && zcSpread(trem) < 0.5 && envSpread(vib) < 1.5 && zcSpread(vib) > 1.0, "mod: blend sweeps tremolo (volume) to vibrato (pitch)", d);
        // rotary: slow -> fast ramps like a real rotor
        Plug r; r.open(0); r.only({"Mod On"}); r.set("Mod Mode", 1.0f); r.set("Blend", 0.0f); r.set("Rate", std::log(6.5f / 0.1f) / std::log(120.0f));
        r.run(plucks(1.0));
        const std::string slow = r.text("Rotor Now");
        r.set("Rotor", 1.0f); r.run(plucks(0.3)); const std::string mid = r.text("Rotor Now");
        r.run(plucks(4.0)); const std::string fastS = r.text("Rotor Now");
        r.right.clear(); const std::vector<float> ro = r.run(plucks(1.0));
        double lr = 0, ee = 0; for (size_t i = 0; i < ro.size(); ++i) { lr += std::fabs(ro[i] - r.right[i]); ee += std::fabs(ro[i]); }
        r.set("Stereo", 0.0f); r.run(plucks(0.2)); const std::vector<float> mo = r.run(plucks(1.0));
        double mlr = 0; for (size_t i = 0; i < mo.size(); ++i) mlr += std::fabs(mo[i] - r.right[i]);
        r.close();
        std::snprintf(d, sizeof d, "slow %s -> after 0.3 s %s -> after 4 s %s; stereo L/R difference %.2f, mono %.6f",
                      slow.c_str(), mid.c_str(), fastS.c_str(), lr / ee, mlr);
        const double s0 = std::atof(slow.c_str() + 6), s1 = std::atof(mid.c_str() + 6), s2 = std::atof(fastS.c_str() + 6);
        check(s0 < 1.0 && s1 > s0 && s1 < 5.0 && s2 > 6.3 && lr / ee > 0.05 && mlr == 0.0, "mod: rotary ramps slow -> fast; stereo spreads, mono sums", d);
        Plug ph; ph.open(0); ph.only({"Mod On"}); ph.set("Mod Mode", 1.0f); ph.set("Blend", 1.0f); ph.set("Depth", 1.0f);
        std::vector<float> noise((size_t)(3 * SR)); uint32_t s = 7; for (float& v : noise) { s = s * 1664525u + 1013904223u; v = ((s >> 9) / 4194304.0f - 1.0f) * 0.2f; }
        const std::vector<float> po = ph.run(noise); ph.close();
        double nc = 0, nn = 0; for (size_t i = SR; i < po.size(); ++i) { nc += po[i] * noise[i - LAT]; nn += noise[i - LAT] * noise[i - LAT]; }   // the output is 64 samples late
        std::snprintf(d, sizeof d, "noise through the phaser: correlation with the input %.2f", nc / nn);
        check(nc / nn < 0.9 && nc / nn > 0.2, "mod: phaser filters (notches) the signal", d);
    }
    // 6. delay: ms and tempo time, analog darker
    {
        auto echo = [&](float syncN, float timeN, float divN, float type, double& hfRatio) {
            Plug p; p.open(0); p.only({"Delay On"}); p.set("Sync", syncN); p.run(plucks(0.05)); p.set("Time", timeN); if (syncN > 0.5f) p.set("Division", divN);
            p.set("Delay Type", type); p.set("Feedback", 0.5f); p.set("Delay Mix", 0.5f);
            std::vector<float> imp((size_t)(3 * SR), 0.0f); for (int k = 0; k < 64; ++k) imp[1000 + k] = (k % 2 ? -0.5f : 0.5f);   // a bright click
            p.run(std::vector<float>((size_t)(0.5 * SR), 0.0f));
            const std::vector<float> o = p.run(imp); const std::string now = p.text("Delay Now"); p.close();
            size_t best = 0; float bv = 0; for (size_t i = 1000 + 2000; i < o.size() / 2; ++i) if (std::fabs(o[i]) > bv) { bv = std::fabs(o[i]); best = i; }
            // brightness of the 1st and 2nd repeats (high-frequency energy ratio)
            auto hf = [&](size_t c) { double h = 0, a = 0; for (size_t i = c - 200; i < c + 200; ++i) { const double dd = o[i] - o[i - 1]; h += dd * dd; a += (double)o[i] * o[i]; } return h / (a + 1e-12); };
            const size_t gap = best - 1000;
            hfRatio = hf(best + gap) / hf(best);
            return std::make_pair((double)(best - 1000) / SR * 1000.0 - (double)LAT / SR * 1000.0, now);
        };
        double h1, h2, h3;
        const auto ms = echo(0.0f, std::log(380.0f / 20.0f) / std::log(100.0f), 0.0f, 0.0f, h1);
        const auto sy = echo(1.0f, 9.0f / 16.0f, 9.0f / 16.0f, 0.0f, h2); // 1/8D at 120 BPM = 375 ms
        echo(0.0f, std::log(380.0f / 20.0f) / std::log(100.0f), 0.0f, 1.0f, h3);
        std::snprintf(d, sizeof d, "Time 380 ms -> echo at %.1f ms (%s); Tempo 1/8D @120 -> %.1f ms (%s); 2nd repeat brightness digital %.2f, analog %.2f",
                      ms.first, ms.second.c_str(), sy.first, sy.second.c_str(), h1, h3);
        check(std::fabs(ms.first - 380) < 3 && std::fabs(sy.first - 375) < 3 && h3 < 0.7 * h1, "delay: ms and tempo-synced times; analog repeats darken", d);
    }
    // 7. reverbs: decay lengthens the tail; shimmer adds the octave
    {
        auto tail = [&](float type, float decay, const std::vector<float>& in, std::vector<float>* outp = nullptr) {
            Plug p; p.open(0); p.only({"Reverb On"}); p.set("Reverb Type", type); p.set("Decay", decay); p.set("Reverb Mix", 1.0f);
            std::vector<float> o = p.run(in); p.close(); if (outp) *outp = o;
            size_t last = 0; for (size_t i = 0; i < o.size(); ++i) if (std::fabs(o[i]) > 0.001f) last = i;   // -60 dB of a 1.0 impulse
            return (double)last / SR;
        };
        std::vector<float> imp((size_t)(8 * SR), 0.0f); imp[100] = 1.0f;
        std::string s;
        bool ok = true;
        for (int t = 0; t < 3; ++t)
        {
            const double a = tail(t / 2.0f, 0.2f, imp), b = tail(t / 2.0f, 0.9f, imp);
            char b2[80]; std::snprintf(b2, sizeof b2, "%s%s %.1f s -> %.1f s", t ? "; " : "", (const char*[]){"plate", "spring", "shimmer"}[t], a, b); s += b2;
            ok = ok && b > a * 1.8 && b > 1.5;
        }
        check(ok, "reverb: decay lengthens the tail (decay 20 % -> 90 %)", s);
        std::vector<float> pl, sh;
        std::vector<float> note((size_t)(4 * SR), 0.0f); for (size_t i = 0; i < (size_t)SR; ++i) note[i] = (float)(0.3 * std::sin(6.283185 * 440 * i / SR));
        tail(0.0f, 0.8f, note, &pl); tail(1.0f, 0.8f, note, &sh);
        const double pOct = tone(pl, 880, (size_t)(1.5 * SR), 3 * SR) / tone(pl, 440, (size_t)(1.5 * SR), 3 * SR);
        const double sOct = tone(sh, 880, (size_t)(1.5 * SR), 3 * SR) / tone(sh, 440, (size_t)(1.5 * SR), 3 * SR);
        std::snprintf(d, sizeof d, "440 Hz note, tail 0.5-2 s after: octave (880 Hz) re 440 Hz: plate %.0f dB, shimmer %.0f dB", 20 * std::log10(pOct), 20 * std::log10(sOct));
        check(sOct > 10 * pOct, "reverb: shimmer adds the octave above", d);
    }
    // 8. randomize / undo
    {
        Plug p; p.open(0); p.run(plucks(0.1));
        auto snap = [&]() { std::vector<float> v(p.e->numParams); for (int i = 0; i < p.e->numParams; ++i) v[i] = p.e->getParameter(p.e, i); return v; };
        const std::vector<float> before = snap();
        p.tap("Randomize"); p.run(plucks(0.15));
        const std::vector<float> after = snap();
        int changed = 0; bool keep = true;
        for (const char* n : {"Drive Type", "Amount", "Tone", "Blend", "Rate", "Depth", "Time", "Feedback", "Decay", "Reverb Mix"}) changed += after[p.find(n)] != before[p.find(n)];
        for (const char* n : {"Bypass", "Input", "Output", "Stereo", "Drive On", "Mod On", "Delay On", "Reverb On", "Sync"}) keep = keep && after[p.find(n)] == before[p.find(n)];
        const std::string st = p.text("Status");
        p.tap("Undo"); p.run(plucks(0.15));
        const std::vector<float> undone = snap();
        int diffs = 0;
        for (int i = 0; i < p.e->numParams; ++i) { const int lv = p.find("In Level"); if (i >= lv) continue; if (i == p.find("Randomize") || i == p.find("Undo")) continue; diffs += undone[i] != before[i]; }
        const std::string st2 = p.text("Status");
        p.close();
        std::snprintf(d, sizeof d, "%d of 10 settings moved, status '%s'; undo: %d differ, status '%s'", changed, st.c_str(), diffs, st2.c_str());
        check(changed >= 6 && keep && diffs == 0 && st.rfind("Randomized", 0) == 0 && st2 == "Undone", "randomize: moves the sound, never bypass / levels / section switches; undo restores exactly", d);
    }
    // 9. save with a name; listed after the factory presets; kept across a restart
    {
        auto progName = [](Plug& q, int i) { char b[64] = {0}; q.call(effGetProgramNameIndexed, i, 0, b); return std::string(b); };
        auto wait = [](Plug& q) { q.run(plucks(0.05)); std::this_thread::sleep_for(std::chrono::milliseconds(300)); q.run(plucks(0.3)); };
        Plug p; p.open(3);                                 // Desert Wind
        const int total = p.e->numPrograms, factory = total - 16;
        p.tap("Save"); p.run(plucks(0.1));
        const std::string draft = p.text("Preset Name");
        const char* cs = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.#&+!";
        const char* word = "Dusk";
        for (int i = 0; i < 4; ++i)
        {
            p.set("Name Pos", i / 15.0f); p.run(plucks(0.02));
            p.set("Letter", (float)(std::strchr(cs, word[i]) - cs) / (float)(std::strlen(cs) - 1)); p.run(plucks(0.02));
        }
        p.tap("Confirm"); wait(p);
        const std::string name = progName(p, factory), status = p.text("Status");
        const float dec = p.getN("Decay");
        p.close();
        Plug q; q.open(0);
        const std::string again = progName(q, factory);
        q.call(effSetProgram, 0, factory); q.run(plucks(0.05));
        const float dec2 = q.getN("Decay");
        q.close();
        std::snprintf(d, sizeof d, "%d factory presets; draft '%s' -> saved '%s' (%s); after restart '%s', decay %.3f vs %.3f",
                      factory, draft.c_str(), name.c_str(), status.c_str(), again.c_str(), dec2, dec);
        check(draft == "User 01[_]" && name == "Dusk 01" && status == "Saved: Dusk 01" && again == "Dusk 01" && std::fabs(dec - dec2) < 1e-4f,
              "save: name entry, listed after the factory presets, kept across restarts", d);
    }
    // 11. Input and Output are global: presets, randomize, undo and saved presets never move them
    {
        Plug p; p.open(0);
        p.set("Input", 0.6f); p.set("Output", 0.3f); p.run(plucks(0.05));
        bool ok = true; std::string where;
        auto same = [&](const char* when) { const bool s = std::fabs(p.getN("Input") - 0.6f) < 1e-6f && std::fabs(p.getN("Output") - 0.3f) < 1e-6f; if (!s) where += std::string(" ") + when; ok = ok && s; };
        for (int prog : {1, 5, 9, 13}) { p.call(effSetProgram, 0, prog); p.run(plucks(0.05)); same("preset"); }
        p.tap("Randomize"); p.run(plucks(0.1)); same("randomize");
        p.tap("Undo"); p.run(plucks(0.1)); same("undo");
        p.set("Input", 0.2f); p.tap("Save"); p.tap("Save");             // save a user preset with other levels
        p.run(plucks(0.05)); std::this_thread::sleep_for(std::chrono::milliseconds(300)); p.run(plucks(0.2));
        p.set("Input", 0.6f); p.run(plucks(0.05));
        const int user = p.e->numPrograms - 16;
        p.call(effSetProgram, 0, user + 1 > p.e->numPrograms - 1 ? user : user); p.run(plucks(0.05)); same("user preset");
        p.close();
        check(ok, "levels: Input / Output never change on preset load, randomize, undo or a saved preset", where.empty() ? "unchanged everywhere" : "changed on:" + where);
    }
    // 12. every drive type alone (mod, delay, reverb off): no warble, no clicks, low aliasing
    {
        const char* names[] = {"opto", "smooth", "FET", "punch", "germanium", "gated", "sustain", "octave"};
        auto steady = [&](int type, double hz, double amp) {
            Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", type / 7.0f); p.set("Amount", 0.7f);
            std::vector<float> o = p.run(sine(4.0, hz, amp)); p.close(); return o;
        };
        auto amfm = [&](const std::vector<float>& o, double hz, double& am, double& fm) {
            // level: RMS over exactly 8 periods, stepped by one period; pitch: zero-crossing periods
            const size_t w = (size_t)std::lround(8 * SR / hz), stepS = (size_t)std::lround(SR / hz);
            double mx = 0, mn = 1e9;
            for (size_t a = (size_t)(1.5 * SR); a + w < o.size(); a += stepS) { const double r = rms(o, a, a + w); mx = std::fmax(mx, r); mn = std::fmin(mn, r); }
            am = 20 * std::log10(mx / (mn + 1e-12));
            std::vector<double> per; double last = -1;
            for (size_t i = (size_t)(1.5 * SR); i + 1 < o.size(); ++i) if (o[i] <= 0 && o[i + 1] > 0) { const double t = i + o[i] / (o[i] - o[i + 1]); if (last > 0) per.push_back(t - last); last = t; }
            // a stable waveform repeats every period: compare each cycle's length with the median
            std::vector<double> sorted = per; std::sort(sorted.begin(), sorted.end());
            const double med = sorted.empty() ? 1 : sorted[sorted.size() / 2];
            double dev = 0; for (double v : per) dev = std::fmax(dev, std::fabs(v - med) / med);
            fm = 100 * dev;
        };
        std::string warble, clicks, alias;
        bool okW = true, okC = true, okA = true;
        for (int t = 0; t < 8; ++t)
        {
            double am1, fm1, am2, fm2;
            const std::vector<float> o1 = steady(t, 110, 0.25), o2 = steady(t, 330, 0.25);
            amfm(o1, 110, am1, fm1); amfm(o2, 330, am2, fm2);
            // octave fuzz doubles the zero crossings unevenly: check its level only
            const bool w = am1 < 0.5 && am2 < 0.5 && (t == 7 || (fm1 < 0.5 && fm2 < 0.5));
            char b[120]; std::snprintf(b, sizeof b, "%s%s %.2f/%.2f dB %.2f/%.2f %%", t ? "; " : "", names[t], am1, am2, fm1, fm2); warble += b;
            okW = okW && w;
            // clicks: plucked notes; a click is a jump far above the signal's own slope
            Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", t / 7.0f); p.set("Amount", 0.7f);
            std::vector<float> sp((size_t)(3 * SR)); double ph2 = 0;
            for (size_t i = 0; i < sp.size(); ++i) { const double tt = (double)i / SR, tn = std::fmod(tt, 0.5); ph2 += (((int)(tt / 0.5) % 2) ? 165.0 : 110.0) / SR;
                sp[i] = (float)(0.3 * std::fmin(1.0, tn / 0.003) * std::exp(-tn * 5.0) * (std::sin(6.283185 * ph2) + 0.3 * std::sin(6.283185 * 2 * ph2))); }
            const std::vector<float> in2 = sp;
            const std::vector<float> o = p.run(in2); p.close();
            double maxD = 0, sumD = 0; for (size_t i = SR / 2; i < o.size(); ++i) { const double dd = std::fabs(o[i] - o[i - 1]); maxD = std::fmax(maxD, dd); sumD += dd; }
            const double ratio = maxD / (sumD / (o.size() - SR / 2));
            std::snprintf(b, sizeof b, "%s%s %.0f", t ? ", " : "", names[t], ratio); clicks += b;
            okC = okC && ratio < 60;
            if (t >= 4)
            {   // aliasing: a 3.1 kHz note at full gain; folded harmonics against the fundamental
                Plug q; q.open(0); q.only({"Drive On"}); q.set("Drive Type", t / 7.0f); q.set("Amount", 1.0f); q.set("Tone", 0.8f);
                const std::vector<float> a = q.run(sine(2.0, 3100, 0.3)); q.close();
                const double f1 = std::fmax(tone(a, 3100, SR, 2 * SR), tone(a, 6200, SR, 2 * SR));
                double worst = 0;
                for (int k = 2; k <= 60; ++k)
                {
                    double fa = std::fmod(k * 3100.0, 44100.0); if (fa > 22050) fa = 44100 - fa;
                    if (k * 3100.0 < 22050 || std::fmod(fa, 3100.0) < 60 || 3100 - std::fmod(fa, 3100.0) < 60 || fa < 40) continue;
                    worst = std::fmax(worst, tone(a, fa, SR, 2 * SR));
                }
                const double db = 20 * std::log10(worst / f1 + 1e-12);
                std::snprintf(b, sizeof b, "%s%s %.0f dB", t > 4 ? ", " : "", names[t], db); alias += b;
                okA = okA && db < -55;
            }
        }
        check(okW, "drive alone: no level or pitch warble (110 / 330 Hz steady notes)", warble);
        check(okC, "drive alone: no clicks on plucked notes (largest step / average step)", clicks);
        check(okA, "fuzz anti-aliasing (4x): folded harmonics of a 3.1 kHz note below -55 dB", alias);
    }
    // 13. compressor character
    {
        // a 20 dB level step: GR rise time (attack) and recovery time (release)
        auto timing = [&](int type, double& attMs, double& relMs, double& thd) {
            Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", type / 7.0f); p.set("Amount", 0.8f);
            std::vector<float> in((size_t)(6 * SR));
            for (size_t i = 0; i < in.size(); ++i) { const double t = (double)i / SR; in[i] = (float)((t >= 1.0 && t < 3.0 ? 0.5 : 0.05) * std::sin(6.283185 * 440 * t)); }
            const std::vector<float> o = p.run(in);
            p.close();
            // envelope in 0.5 ms steps
            auto env = [&](double t) { const size_t a = (size_t)(t * SR) + 64; return rms(o, a, a + 100) * 1.414; };   // one 440 Hz cycle
            const double peakA = [&] { double m = 0; for (double t = 1.0; t < 1.02; t += 0.0005) m = std::fmax(m, env(t)); return m; }();
            const double steadyA = env(2.9);
            double ta = 1.0; while (ta < 2.0 && env(ta) > steadyA + 0.5 * (peakA - steadyA)) ta += 0.0005;
            attMs = (ta - 1.0) * 1000;
            const double lowEnd = env(5.9), lowStart = env(3.002);
            double tr = 3.0; while (tr < 5.9 && env(tr) < lowStart + 0.63 * (lowEnd - lowStart)) tr += 0.002;
            relMs = (tr - 3.0) * 1000;
            // ripple distortion on a loud low E (82 Hz)
            Plug q; q.open(0); q.only({"Drive On"}); q.set("Drive Type", type / 7.0f); q.set("Amount", 0.9f); q.set("Tone", 0.5f);
            const std::vector<float> lo = q.run(sine(3.0, 82.4, 0.5)); q.close();
            const double f1 = tone(lo, 82.4, SR, 3 * SR);
            double hs = 0; for (int k = 2; k <= 5; ++k) { const double h = tone(lo, 82.4 * k, SR, 3 * SR); hs += h * h; }
            thd = 100 * std::sqrt(hs) / f1;
        };
        double a[4], r[4], d4[4];
        for (int t = 0; t < 4; ++t) timing(t, a[t], r[t], d4[t]);
        std::snprintf(d, sizeof d, "attack (to half way) / release (63 %%) / low-E distortion: opto %.1f ms / %.0f ms / %.1f %%, smooth %.1f / %.0f / %.1f, FET %.1f / %.0f / %.1f, punch %.1f / %.0f / %.1f",
                      a[0], r[0], d4[0], a[1], r[1], d4[1], a[2], r[2], d4[2], a[3], r[3], d4[3]);
        check(a[2] < 1.5 && a[2] < a[3] && a[3] < a[0] && r[0] > 300 && r[1] > r[2] && d4[0] < 1.0 && d4[1] < 1.0 && d4[3] < 2.0,
              "compressors: FET fastest, punch lets the click through, opto slow to let go; no ripple on low notes", d);
        // the opto's release slows down after long, heavy compression (program dependence)
        auto recov = [&](double holdSec) {
            Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", 0.0f); p.set("Amount", 0.8f);
            std::vector<float> in((size_t)((holdSec + 4) * SR));
            for (size_t i = 0; i < in.size(); ++i) { const double t = (double)i / SR; in[i] = (float)((t < holdSec ? 0.5 : 0.05) * std::sin(6.283185 * 440 * t)); }
            const std::vector<float> o = p.run(in); p.close();
            auto env = [&](double t) { const size_t a2 = (size_t)(t * SR) + 64; return rms(o, a2, a2 + 441); };
            const double e0 = env(holdSec + 0.01), e1 = env(holdSec + 3.9);
            double t = holdSec; while (t < holdSec + 3.9 && env(t) < e0 + 0.63 * (e1 - e0)) t += 0.005;
            return (t - holdSec) * 1000;
        };
        const double shortHold = recov(0.3), longHold = recov(4.0);
        std::snprintf(d, sizeof d, "opto release after 0.3 s of compression %.0f ms, after 4 s %.0f ms", shortHold, longHold);
        check(longHold > 1.5 * shortHold, "opto: program-dependent release (slower after long compression)", d);
        // dry blend: Drive Mix 0 = dry, 50 = parallel (between)
        Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", 2.0f / 7.0f); p.set("Amount", 1.0f); p.set("Drive Mix", 0.0f);
        const std::vector<float> in = plucks(2.0, 0.4f), dry = p.run(in);
        int same = 0; for (size_t i = SR; i < in.size(); ++i) same += std::fabs(dry[i] - in[i - LAT]) < 1e-6f;
        p.set("Drive Mix", 0.5f); p.run(plucks(0.2)); const std::vector<float> half = p.run(in);
        p.set("Drive Mix", 1.0f); p.run(plucks(0.2)); const std::vector<float> wet = p.run(in);
        p.close();
        const double rd = rms(dry, SR, 2 * SR), rh = rms(half, SR, 2 * SR), rw = rms(wet, SR, 2 * SR);
        std::snprintf(d, sizeof d, "Drive Mix 0 %%: %.1f %% of samples equal the input; RMS dry %.3f, 50 %% %.3f, 100 %% %.3f", 100.0 * same / (in.size() - SR), rd, rh, rw);
        check(same > (int)((in.size() - SR) * 0.999) && rh > std::fmin(rd, rw) * 0.9 && rh < std::fmax(rd, rw) * 1.1, "drive mix: dry blend for parallel compression", d);
    }
    // 14. modulation: vibrato depth range and shapes, tremolo presence
    {
        auto pitchDev = [&](float depth, float waveN, std::vector<double>* devs) {
            Plug p; p.open(0); p.only({"Mod On"}); p.set("Blend", 1.0f); p.set("Depth", depth); p.set("Wave", waveN);
            p.set("Rate", std::log(4.0f / 0.1f) / std::log(120.0f));
            const std::vector<float> o = p.run(sine(4.0, 330, 0.3)); p.close();
            std::vector<double> per; double last = -1;
            for (size_t i = 2 * SR; i + 1 < o.size(); ++i) if (o[i] <= 0 && o[i + 1] > 0) { const double t = i + o[i] / (o[i] - o[i + 1]); if (last > 0) per.push_back(t - last); last = t; }
            const double nominal = SR / 330.0; double mx = 0;
            for (double v : per) { const double dv = (nominal / v - 1.0) * 100.0; mx = std::fmax(mx, std::fabs(dv)); if (devs) devs->push_back(dv); }
            return mx;   // largest pitch deviation, %
        };
        const double subtle = pitchDev(0.15f, 0.0f, nullptr), dramatic = pitchDev(1.0f, 0.0f, nullptr);
        std::vector<double> sq, sn;
        pitchDev(0.7f, 1.0f, &sq); pitchDev(0.7f, 0.0f, &sn);
        auto nearExtremes = [](const std::vector<double>& v) { double mx = 0; for (double x : v) mx = std::fmax(mx, std::fabs(x)); int n = 0; for (double x : v) n += std::fabs(x) > 0.75 * mx; return (double)n / v.size(); };
        auto nearZero = [](const std::vector<double>& v) { double mx = 0; for (double x : v) mx = std::fmax(mx, std::fabs(x)); int n = 0; for (double x : v) n += std::fabs(x) < 0.25 * mx; return (double)n / v.size(); };
        std::snprintf(d, sizeof d, "vibrato peak pitch deviation: depth 15 %% -> %.2f %%, 100 %% -> %.1f %% (%.1f semitones); share of cycles near the extremes: square %.0f %%, sine %.0f %%; near centre: square %.0f %%, sine %.0f %%",
                      subtle, dramatic, 12 * std::log2(1 + dramatic / 100), 100 * nearExtremes(sq), 100 * nearExtremes(sn), 100 * nearZero(sq), 100 * nearZero(sn));
        check(subtle < 0.3 && dramatic > 4.0 && nearExtremes(sq) > 0.7 && nearExtremes(sq) > nearExtremes(sn) + 0.2 && nearZero(sq) < 0.1,
              "vibrato: subtle to dramatic depth; square really jumps between two pitches", d);
        Plug t0; t0.open(0); t0.only({}); const double off = rms(t0.run(sine(3.0, 330, 0.2)), SR, 3 * SR); t0.close();
        Plug t1; t1.open(0); t1.only({"Mod On"}); t1.set("Blend", 0.0f); t1.set("Depth", 1.0f);
        const std::vector<float> tr = t1.run(sine(3.0, 330, 0.2)); t1.close();
        double mx = 0, mn = 1e9; for (size_t a2 = SR; a2 + 441 < tr.size(); a2 += 441) { const double r2 = rms(tr, a2, a2 + 441); mx = std::fmax(mx, r2); mn = std::fmin(mn, r2); }
        std::snprintf(d, sizeof d, "tremolo at full depth: average level %.1f dB re off, swing %.0f dB", 20 * std::log10(rms(tr, SR, 3 * SR) / off), 20 * std::log10(mx / (mn + 1e-12)));
        check(20 * std::log10(rms(tr, SR, 3 * SR) / off) > -2.5 && 20 * std::log10(mx / (mn + 1e-12)) > 20, "tremolo: deep and present (does not just get quieter)", d);
    }
    // 15. renders for listening: every drive type, and the modulation section alone
    if (argc > 4)
    {
        auto wav = [&](const std::string& path, const std::vector<float>& l, const std::vector<float>& r) {
            FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
            const uint32_t n = (uint32_t)l.size(), bytes = n * 4;
            auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
            std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2); u32(SR); u32(SR * 4); u16(4); u16(16);
            std::fwrite("data", 1, 4, f); u32(bytes);
            for (uint32_t i = 0; i < n; ++i) for (float s : {l[i], r[i]}) { const int16_t v = (int16_t)std::lround(std::fmax(-1.0f, std::fmin(1.0f, s)) * 32767); std::fwrite(&v, 2, 1, f); }
            std::fclose(f);
        };
        // a short guitar-like riff: E2 A2 B2 E3 and a held chord tone
        std::vector<float> riff((size_t)(6 * SR)); { double ph = 0; const double notes[] = {82.4, 110, 123.5, 164.8, 82.4, 110, 123.5, 196};
            for (size_t i = 0; i < riff.size(); ++i) { const double t = (double)i / SR; const int k = std::min(7, (int)(t / 0.6)); const double tn = t - k * 0.6;
                ph += notes[k] / SR; ph -= std::floor(ph); const double s = (2 * ph - 1) * 0.6 + std::sin(6.283185 * ph * 2) * 0.25;
                riff[i] = (float)(0.35 * s * std::exp(-tn * (k == 7 ? 0.6 : 3.0))); } }
        const char* names[] = {"opto", "smooth", "fet", "punch", "germanium", "gated", "sustain", "octave"};
        for (int t = 0; t < 8; ++t)
        {
            Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", t / 7.0f); p.set("Amount", t < 4 ? 0.7f : 0.6f);
            const std::vector<float> o = p.run(riff); wav(std::string(argv[4]) + "/drive_" + std::to_string(t + 1) + "_" + names[t] + ".wav", o, p.right); p.close();
        }
        struct M { const char* n; float mode, blend, wave, rotor; };
        for (const M& m : {M{"tremolo_sine", 0, 0, 0, 0}, M{"tremolo_square", 0, 0, 1, 0}, M{"vibrato_sine", 0, 1, 0, 0}, M{"vibrato_triangle", 0, 1, 0.5f, 0},
                           M{"trem_vib_mix", 0, 0.5f, 0, 0}, M{"rotary_slow_to_fast", 1, 0, 0, 1}, M{"phaser", 1, 1, 0, 0}})
        {
            Plug p; p.open(0); p.only({"Mod On"}); p.set("Mod Mode", m.mode); p.set("Blend", m.blend); p.set("Wave", m.wave); p.set("Depth", 0.8f);
            if (m.mode > 0.5f) p.set("Rate", std::log(6.5f / 0.1f) / std::log(120.0f));
            std::vector<float> first = p.run(std::vector<float>(riff.begin(), riff.begin() + riff.size() / 2)), firstR = p.right;
            p.set("Rotor", m.rotor);
            std::vector<float> second = p.run(std::vector<float>(riff.begin() + riff.size() / 2, riff.end())), secondR = p.right;
            first.insert(first.end(), second.begin(), second.end()); firstR.insert(firstR.end(), secondR.begin(), secondR.end());
            wav(std::string(argv[4]) + "/mod_" + m.n + ".wav", first, firstR); p.close();
        }
        for (int rt = 0; rt < 3; ++rt)
        {
            Plug p; p.open(0); p.only({"Reverb On"}); p.set("Reverb Type", rt / 2.0f); p.set("Reverb Mix", 0.4f); p.set("Decay", 0.65f);
            const std::vector<float> o = p.run(riff); wav(std::string(argv[4]) + "/reverb_" + (const char*[]){"plate", "spring", "shimmer"}[rt] + ".wav", o, p.right); p.close();
        }
        std::printf("      renders written to %s\n", argv[4]);
    }
    // 16. starve (Gate Bias on the fuzzes): gradual, level-relative, smooth
    {
        auto smoothPlucks = [&](double amp) {
            std::vector<float> sp((size_t)(3 * SR)); double ph = 0;
            for (size_t i = 0; i < sp.size(); ++i) { const double tt = (double)i / SR, tn = std::fmod(tt, 0.75); ph += 110.0 / SR;
                sp[i] = (float)(amp * std::fmin(1.0, tn / 0.003) * std::exp(-tn * 3.0) * (std::sin(6.283185 * ph) + 0.3 * std::sin(6.283185 * 2 * ph))); }
            return sp;
        };
        auto run = [&](float bias, const std::vector<float>& in, std::vector<float>* out = nullptr) {
            Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", 4.0f / 7.0f); p.set("Amount", 0.6f); p.set("Gate Bias", bias);
            std::vector<float> o = p.run(in); p.close(); if (out) *out = o; return rms(o, SR / 2, o.size());
        };
        const std::vector<float> norm = smoothPlucks(0.25), quiet = smoothPlucks(0.01);
        std::string curve; double r0 = 0;
        for (float b : {0.0f, 0.01f, 0.25f, 0.5f, 0.75f, 1.0f})
        {
            const double r = run(b, norm); if (b == 0.0f) r0 = r;
            char c[40]; std::snprintf(c, sizeof c, "%s%.0f%% %.1f dB", b > 0 ? ", " : "", b * 100, 20 * std::log10(r / r0)); curve += c;
        }
        const double one = 20 * std::log10(run(0.01f, norm) / r0), half = 20 * std::log10(run(0.5f, norm) / r0);
        const double q0 = run(0.0f, quiet), q1 = run(1.0f, quiet);
        std::vector<float> o0, o1; run(0.0f, norm, &o0); run(1.0f, norm, &o1);
        auto gaps = [&](const std::vector<float>& o) { const float pk = peak(o, SR / 2); int n = 0; for (size_t i = SR / 2; i < o.size(); ++i) n += std::fabs(o[i]) < 0.03f * pk; return (double)n / (o.size() - SR / 2); };
        double maxD = 0, sumD = 0; for (size_t i = SR / 2; i < o1.size(); ++i) { const double dd = std::fabs(o1[i] - o1[i - 1]); maxD = std::fmax(maxD, dd); sumD += dd; }
        std::snprintf(d, sizeof d, "level vs starve: %s; -40 dBFS notes at 100 %%: %.1f dB re 0 %%; near-silent samples 0 %% -> 100 %%: %.0f %% -> %.0f %%; click ratio at 100 %%: %.0f",
                      curve.c_str(), 20 * std::log10(q1 / q0), 100 * gaps(o0), 100 * gaps(o1), maxD / (sumD / (o1.size() - SR / 2)));
        check(std::fabs(one) < 0.2 && std::fabs(half) < 3.0 && 20 * std::log10(q1 / q0) > -15 && gaps(o1) > gaps(o0) + 0.05 && maxD / (sumD / (o1.size() - SR / 2)) < 60,
              "starve: subtle at the bottom, sputter near the top, quiet notes not chopped dead, no clicks", d);
    }
    // 17. octave (Octavia): level matched to the other fuzzes, the octave on top
    {
        std::vector<float> riff = plucks(3.0, 0.3f);
        auto lvl = [&](int type) { Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", type / 7.0f); p.set("Amount", 0.6f); const std::vector<float> o = p.run(riff); p.close(); return 20 * std::log10(rms(o, SR / 2, o.size())); };
        const double ge = lvl(4), ga = lvl(5), su = lvl(6), oc = lvl(7);
        Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", 1.0f); p.set("Amount", 0.6f);
        const std::vector<float> o = p.run(sine(2.0, 330, 0.2)); p.close();
        const double f1 = tone(o, 330, SR, 2 * SR), f2 = tone(o, 660, SR, 2 * SR);
        std::snprintf(d, sizeof d, "riff RMS: germanium %.1f, gated %.1f, sustain %.1f, octave %.1f dBFS; 330 Hz in: octave %.0f dB above the fundamental", ge, ga, su, oc, 20 * std::log10(f2 / f1));
        const double avg = (ge + ga + su) / 3;
        check(std::fabs(oc - avg) < 3.0 && f2 > 2 * f1, "octave: level matches the other fuzzes, the octave leads", d);
    }
    // 18. reverbs and delay: smooth, no warble
    {
        auto steadyWobble = [&](std::initializer_list<const char*> on, std::vector<std::pair<const char*, float>> s) {
            Plug p; p.open(0); p.only(on); for (auto& kv : s) p.set(kv.first, kv.second);
            const std::vector<float> o = p.run(sine(8.0, 196, 0.25)); p.close();
            const size_t w = (size_t)std::lround(8 * SR / 196.0), st = (size_t)std::lround(SR / 196.0);
            double mx = 0, mn = 1e9; for (size_t a2 = 3 * SR; a2 + w < o.size(); a2 += st) { const double r2 = rms(o, a2, a2 + w); mx = std::fmax(mx, r2); mn = std::fmin(mn, r2); }
            return 20 * std::log10(mx / mn);
        };
        const double pl = steadyWobble({"Reverb On"}, {{"Reverb Type", 0.0f}, {"Reverb Mix", 0.3f}});
        const double sp = steadyWobble({"Reverb On"}, {{"Reverb Type", 0.5f}, {"Reverb Mix", 0.3f}});
        const double an = steadyWobble({"Delay On"}, {{"Delay Type", 1.0f}, {"Delay Mix", 0.3f}});
        // the spring's tail after a plucked note: its decay should be smooth (no flutter)
        Plug p; p.open(0); p.only({"Reverb On"}); p.set("Reverb Type", 0.5f); p.set("Reverb Mix", 1.0f); p.set("Decay", 0.7f);
        // a guitar-like note (harmonic-rich, like a real string), not a pure sine: a pure tone makes any
        // reverb's few nearby modes beat, which is not what a guitar feeds it
        std::vector<float> in((size_t)(3 * SR), 0.0f); { double ph = 0; for (size_t i = 0; i < (size_t)(0.4 * SR); ++i) { ph += 196.0 / SR; ph -= std::floor(ph); in[i] = (float)(0.3 * std::exp(-(double)i / SR * 4) * (2 * ph - 1)); } }
        const std::vector<float> o = p.run(in); p.close();
        std::vector<double> db; for (size_t a2 = (size_t)(0.5 * SR); a2 + 441 < (size_t)(1.5 * SR); a2 += 441) db.push_back(20 * std::log10(rms(o, a2, a2 + 441) + 1e-9));
        // straight-line fit; the largest deviation from it
        double sx = 0, sy = 0, sxx = 0, sxy = 0; const double n = (double)db.size();
        for (size_t i = 0; i < db.size(); ++i) { sx += i; sy += db[i]; sxx += (double)i * i; sxy += i * db[i]; }
        const double k = (n * sxy - sx * sy) / (n * sxx - sx * sx), c0 = (sy - k * sx) / n;
        double dev = 0; for (size_t i = 0; i < db.size(); ++i) dev = std::fmax(dev, std::fabs(db[i] - (c0 + k * i)));
        std::snprintf(d, sizeof d, "steady 196 Hz note, level wobble: plate %.2f dB, spring %.2f dB, analog delay %.2f dB (before: 9.1 / 0 / 6.9); spring tail: largest step from a smooth decay %.1f dB",
                      pl, sp, an, dev);
        check(pl < 2.0 && sp < 1.0 && an < 2.0 && dev < 4.0, "reverb and delay: no warble on held notes; the spring tail decays smoothly", d);
    }
    // 19. Hz / Sync flips at the knob ends (hold 0.7 s); quick passes and loads do not flip
    {
        Plug p; p.open(0); p.only({"Mod On"}); p.run(plucks(0.1));
        p.set("Rate", 0.0f); p.run(plucks(0.3)); const std::string hint = p.text("Rate"); const bool notYet = p.getN("Rate Mode") < 0.5f;
        p.run(plucks(0.6)); const bool synced = p.getN("Rate Mode") > 0.5f; const std::string label = p.text("Rate");
        p.set("Rate", 0.5f); p.run(plucks(0.2)); const std::string mid = p.text("Rate");
        p.set("Rate", 1.0f); p.run(plucks(0.3)); p.set("Rate", 0.6f); p.run(plucks(1.0)); const bool stillSync = p.getN("Rate Mode") > 0.5f;   // a quick pass
        p.set("Rate", 1.0f); p.run(plucks(1.0)); const bool backHz = p.getN("Rate Mode") < 0.5f; const std::string hz = p.text("Rate");
        // delay time: same rule
        p.only({"Delay On"}); p.set("Time", 0.0f); p.run(plucks(1.0)); const bool tSync = p.getN("Sync") > 0.5f; const std::string tl = p.text("Time");
        p.set("Time", 1.0f); p.run(plucks(1.0)); const bool tMs = p.getN("Sync") < 0.5f; const std::string tl2 = p.text("Time");
        // a project saved with the knob at its end does not flip on load
        p.set("Rate", 0.0f); void* data = nullptr; const VstIntPtr n = p.call(effGetChunk, 0, 0, &data);
        std::vector<uint8_t> chunk((uint8_t*)data, (uint8_t*)data + n); p.close();
        Plug q; q.open(0); q.call(effSetChunk, 0, (VstIntPtr)chunk.size(), chunk.data()); q.run(plucks(1.5));
        const bool loadKept = q.getN("Rate Mode") < 0.5f; q.close();
        // no tempo from the host: Sync falls back to Hz, and says so
        g_time.flags = 0;
        Plug r; r.open(0); r.set("Rate Mode", 1.0f); r.run(plucks(0.2)); const std::string noTempo = r.text("Rate"); r.close();
        g_time.flags = kVstTempoValid;
        std::snprintf(d, sizeof d, "while held: '%s'; flipped: '%s', mid '%s'; quick pass kept Sync: %s; back: '%s'; Time: '%s' -> '%s'; loaded at the end: %s; no tempo: '%s'",
                      hint.c_str(), label.c_str(), mid.c_str(), stillSync ? "yes" : "no", hz.c_str(), tl.c_str(), tl2.c_str(), loadKept ? "no flip" : "FLIPPED", noTempo.c_str());
        check(notYet && hint == "hold -> Sync" && synced && label.rfind("Sync", 0) == 0 && stillSync && backHz && hz.rfind("Hz", 0) == 0 &&
              tSync && tl.rfind("Sync", 0) == 0 && tMs && tl2.rfind("ms", 0) == 0 && loadKept && noTempo.rfind("no tempo", 0) == 0,
              "rate / time knobs: hold at the end to flip Hz/ms <-> Sync; clear labels; no accidental flips", d);
    }
    // 20. the silence watchdog leaves intentional silence alone: a closed noise gate stays closed
    {
        Plug p; p.open(0); p.only({"Drive On"}); p.set("Drive Type", 0.0f); p.set("Amount", 0.0f); p.set("Gate Bias", 0.8f);
        const std::vector<float> o = p.run(sine(6.0, 220, 0.0056));        // -45 dBFS: above the watchdog's -50, below the -41 dB gate
        p.close();
        float late = 0; for (size_t i = SR; i < o.size(); ++i) late = std::fmax(late, std::fabs(o[i]));
        std::snprintf(d, sizeof d, "6 s of -45 dBFS through a -41 dB gate: largest output after the first second %.1g", late);
        check(late < 1e-4f, "watchdog: a closed noise gate is not 'stuck' (no resets, no blips)", d);
    }
    // 10. help
    {
        Plug p; p.open(0); p.set("Help", 6.0f / 32.0f); p.run(plucks(0.02));
        check(p.text("Help") == "Comp: how hard it squeezes." && p.text("Help 2") == "Fuzz: how much gain.", "help: Amount explanation", p.text("Help") + " / " + p.text("Help 2"));
        p.close();
    }
    std::printf("\n%d failure(s)\n", g_fail);
    return g_fail;
}
