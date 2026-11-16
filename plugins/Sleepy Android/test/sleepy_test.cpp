// sleepy_test - functional tests for Sleepy Android (desktop only, not shipped).
// Loads the native build through the VST2 ABI like the MPC and checks each feature with a
// synthetic guitar-like signal. Prints PASS / FAIL per check; exit code = number of failures.
//
// usage: sleepy_test <plugin.dll> [out-dir-for-wavs]

#include "mpc_vst2_abi.h"

#include <chrono>
#include <cstdint>
#include <thread>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace {

VstTimeInfo g_time;
VstIntPtr hostCallback(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void*, float)
{
    if (opcode == audioMasterVersion) return 2400;
    if (opcode == audioMasterGetTime) return (VstIntPtr)&g_time;
    return 0;
}

typedef AEffect* (*MainProc)(audioMasterCallback);
MainProc g_main = nullptr;
int g_fail = 0;
const int SR = 44100, LAT = 64;

void check(bool ok, const char* what, const std::string& detail = "")
{
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++g_fail;
}

// guitar-like plucks: decaying saw notes every 0.5 s, alternating 110 / 165 Hz
std::vector<float> plucks(double seconds, float amp = 0.35f)
{
    std::vector<float> x((size_t)(seconds * SR));
    double ph = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double)i / SR, tn = std::fmod(t, 0.5);
        const double f = ((int)(t / 0.5) % 2) ? 165.0 : 110.0;
        ph += f / SR; ph -= std::floor(ph);
        x[i] = (float)(amp * std::exp(-tn * 6.0) * (2.0 * ph - 1.0));
    }
    return x;
}

struct Plug
{
    AEffect* e = nullptr;
    std::vector<float> right;    // set its size to the input length to capture the right channel
    VstIntPtr call(VstInt32 op, VstInt32 index = 0, VstIntPtr value = 0, void* ptr = nullptr, float opt = 0.0f)
    { return e->dispatcher(e, op, index, value, ptr, opt); }
    void open(int program = 0)
    {
        e = g_main(hostCallback);
        call(effSetSampleRate, 0, 0, nullptr, (float)SR);
        call(effSetBlockSize, 0, 128);
        call(effOpen);
        call(effSetProgram, 0, program);
        call(effMainsChanged, 0, 1);
        call(effStartProcess);
    }
    void close() { call(effStopProcess); call(effMainsChanged, 0, 0); call(effClose); }
    int find(const char* name)
    {
        char buf[256];
        for (int i = 0; i < e->numParams; ++i) { buf[0] = 0; call(effGetParamName, i, 0, buf); if (!std::strcmp(buf, name)) return i; }
        std::printf("no parameter %s\n", name); std::exit(99);
    }
    void set(const char* name, float norm) { e->setParameter(e, find(name), norm); }
    // an action button as the MPC touch screen sends it: each tap flips the switch. 0.4 s of
    // silence first, past the plugin's 0.35 s repeat guard.
    void tap(const char* name)
    {
        run(std::vector<float>((size_t)(0.4 * SR), 0.0f));
        set(name, getN(name) > 0.5f ? 0.0f : 1.0f);
    }
    float getN(const char* name) { return e->getParameter(e, find(name)); }
    std::string text(const char* name) { char buf[256] = {0}; call(effGetParamDisplay, find(name), 0, buf); return buf; }
    // process mono-duplicated input in 128-frame blocks; optional per-block callback
    template <class F> std::vector<float> run(const std::vector<float>& in, F perBlock)
    {
        std::vector<float> out(in.size());
        float l[128], r[128];
        for (size_t p = 0; p < in.size(); p += 128)
        {
            const int n = (int)std::min<size_t>(128, in.size() - p);
            for (int i = 0; i < n; ++i) l[i] = r[i] = in[p + i];
            float* io[2] = { l, r };
            e->processReplacing(e, io, io, n);
            for (int i = 0; i < n; ++i) { out[p + i] = l[i]; if (right.size() == in.size()) right[p + i] = r[i]; }
            perBlock(p);
        }
        return out;
    }
    std::vector<float> run(const std::vector<float>& in) { return run(in, [](size_t) {}); }
};

float norm01(float v, float lo, float hi) { return (v - lo) / (hi - lo); }
float peakAbs(const std::vector<float>& v, size_t from = 0) { float m = 0; for (size_t i = from; i < v.size(); ++i) m = std::fmax(m, std::fabs(v[i])); return m; }

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: sleepy_test <plugin>\n"); return 1; }
    // user presets go to a scratch folder (the plugin reads MPC_PLUGIN_FOLDER), starting empty
    {
        const std::string dir = std::string(argc > 2 ? argv[2] : ".") ;
#if defined(_WIN32)
        _putenv_s("MPC_PLUGIN_FOLDER", dir.c_str());
#else
        setenv("MPC_PLUGIN_FOLDER", dir.c_str(), 1);
#endif
        std::remove((dir + "/user-presets.bin").c_str());
    }
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA(argv[1]); if (lib) g_main = (MainProc)GetProcAddress(lib, "VSTPluginMain");
#else
    void* lib = dlopen(argv[1], RTLD_NOW); if (lib) g_main = (MainProc)dlsym(lib, "VSTPluginMain");
#endif
    if (!g_main) { std::printf("cannot load\n"); return 1; }
    std::memset(&g_time, 0, sizeof(g_time));
    g_time.tempo = 120.0; g_time.sampleRate = SR; g_time.flags = kVstTempoValid;

    const std::vector<float> guitar = plucks(4.0);

    auto cutNorm = [](float hz) { return std::log(hz / 40.0f) / std::log(200.0f); };      // Cutoff 40..8000 Hz (log)
    auto octNorm = [](float oct) { return (oct + 6.0f) / 12.0f; };                         // Sweep -6..+6
    auto rateNorm = [](float hz) { return std::log(hz / 0.1f) / std::log(1000.0f); };     // LFO 0.1..100 Hz (log)
    auto relNorm = [](float ms) { return std::log(ms / 5.0f) / std::log(1000.0f); };      // Release 5..5000 ms (log)
    auto attNorm = [](float ms) { return std::log(ms) / std::log(2000.0f); };             // Attack 1..2000 ms (log)
    auto sine = [](double seconds, double hz, double amp) {
        std::vector<float> s((size_t)(seconds * SR));
        for (size_t i = 0; i < s.size(); ++i) s[i] = (float)(amp * std::sin(6.283185307179586 * hz * (double)i / SR));
        return s;
    };
    auto sweep = [&](Plug& p, const std::vector<float>& in, float& lo, float& hi) {
        lo = 1e9f; hi = 0.0f;
        p.run(in, [&](size_t pos) { if (pos > 4410) { const float hz = (float)std::atof(p.text("Filter Pos").c_str()); lo = std::fmin(lo, hz); hi = std::fmax(hi, hz); } });
    };
    char d[200];

    // 1. Env mode: cutoff follows the playing level, + and - Sweep
    {
        Plug p; p.open(0); p.set("Stutter On", 0.0f);
        p.set("Cutoff", cutNorm(120.0f)); p.set("Sweep", octNorm(4.0f)); p.set("Sensitivity", 0.10f);   // set for this loud signal (LED not pinned)
        float lo, hi; sweep(p, guitar, lo, hi);
        std::snprintf(d, sizeof d, "Sweep +4 oct from 120 Hz: %.0f .. %.0f Hz; Sensitivity shows \"%s\"", lo, hi, p.text("Sensitivity").c_str());
        check(hi / lo > 3.0f && lo >= 119.0f && hi <= 1921.0f && p.text("Sensitivity") == "+5 dB", "filter (Env mode): cutoff follows the envelope", d);
        p.set("Cutoff", cutNorm(1920.0f)); p.set("Sweep", octNorm(-4.0f));
        float lo2, hi2; sweep(p, guitar, lo2, hi2);
        std::snprintf(d, sizeof d, "Sweep -4 oct from 1920 Hz: %.0f .. %.0f Hz (mirror %.0f .. %.0f)", lo2, hi2, 120 * 1920 / hi, 120 * 1920 / lo);
        check(std::fabs(lo2 - 120 * 1920 / hi) / lo2 < 0.05f && std::fabs(hi2 - 120 * 1920 / lo) / hi2 < 0.05f,
              "filter (Env mode): negative Sweep sweeps down", d);
        p.close();
    }
    // 1a. playing dynamics: with the Init patch a soft pluck (-30 dBFS) barely moves the filter and a
    //     hard pick attack (-6 dBFS) sweeps it all the way
    {
        auto oct = [&](float amp, float& lo, float& hi) {
            Plug p; p.open(0); p.set("Stutter On", 0.0f);
            sweep(p, plucks(2.0, amp), lo, hi); p.close();
            return std::log2(hi / 180.0f);          // how far above the 180 Hz resting cutoff
        };
        float l1, h1, l2, h2, l3, h3;
        const float soft = oct(0.0316f, l1, h1), mid = oct(0.126f, l2, h2), hard = oct(0.5f, l3, h3);
        std::snprintf(d, sizeof d, "Init patch, sweep above rest: -30 dBFS %.1f oct (%.0f..%.0f Hz), -18 dBFS %.1f oct, -6 dBFS %.1f oct (%.0f..%.0f Hz)",
                      soft, l1, h1, mid, hard, l3, h3);
        check(soft < 1.0f && mid > soft + 1.0f && hard > mid + 1.0f && hard > 4.0f && h3 > 3000.0f,
              "filter: soft notes barely move it, hard pick attacks sweep it wide", d);
    }    // 1b. the filter is 24 dB/octave (four poles)
    {
        auto gainAt = [&](float hz, int type) {
            Plug p; p.open(0);
            p.set("Stutter On", 0.0f); p.set("Sweep", octNorm(0.0f)); p.set("Resonance", 0.0f);
            p.set("Cutoff", cutNorm(250.0f)); p.set("Filter Type", type / 2.0f); p.set("Input Gain", 12.0f / 36.0f);   // 0 dB
            const std::vector<float> s = sine(1.0, hz, 0.1);
            const std::vector<float> o = p.run(s);
            double ei = 0, eo = 0;
            for (size_t i = SR / 2; i < s.size(); ++i) { ei += s[i] * s[i]; eo += (double)o[i] * o[i]; }
            p.close();
            return 10.0 * std::log10(eo / ei);
        };
        const double g2k = gainAt(2000, 0), g4k = gainAt(4000, 0), h60 = gainAt(62.5f, 2), h30 = gainAt(31.25f, 2);
        std::snprintf(d, sizeof d, "LP: 2 kHz %.1f dB, 4 kHz %.1f dB (slope %.1f dB/oct); HP: 62.5 Hz %.1f dB, 31 Hz %.1f dB (slope %.1f dB/oct)",
                      g2k, g4k, g2k - g4k, h60, h30, h60 - h30);
        check(g2k - g4k > 21.0 && g2k - g4k < 27.0 && h60 - h30 > 21.0 && h60 - h30 < 27.0,
              "filter: low-pass and high-pass slopes are 24 dB/octave", d);
    }
    // 1c. Gate mode: fully on past the threshold, shaped by attack and release
    {
        Plug p; p.open(0); p.set("Stutter On", 0.0f);
        p.set("Env Mode", 1.0f); p.set("Cutoff", cutNorm(150.0f)); p.set("Sweep", octNorm(3.0f));
        p.set("Sensitivity", 0.5f); p.set("Attack", attNorm(8.0f)); p.set("Release", relNorm(100.0f));
        std::vector<float> bursts((size_t)(4.0 * SR));               // 0.2 s bursts, 0.8 s silence
        for (size_t i = 0; i < bursts.size(); ++i) bursts[i] = (std::fmod(i / (double)SR, 1.0) < 0.2) ? 0.2f * std::sin(6.2831853f * 220 * i / SR) : 0.0f;
        float lo, hi; sweep(p, bursts, lo, hi);
        std::snprintf(d, sizeof d, "gate: %.0f .. %.0f Hz (expected 150 .. 1200 Hz); Sensitivity shows \"%s\"", lo, hi, p.text("Sensitivity").c_str());
        check(lo < 165.0f && hi > 1100.0f && hi < 1210.0f && p.text("Sensitivity") == "-33 dB thr",
              "filter (Gate mode): the envelope is fully on past the threshold, then fully off", d);
        p.close();
    }
    // 1d. LFO alone moves the cutoff by +- depth, up to 100 Hz
    {
        Plug p; p.open(0); p.set("Stutter On", 0.0f);
        p.set("Sweep", octNorm(0.0f)); p.set("Cutoff", cutNorm(500.0f));
        p.set("LFO Depth", 2.0f / 4.0f); p.set("LFO Rate", rateNorm(0.25f));   // slow: the 10 Hz display catches the peaks
        float lo, hi; sweep(p, plucks(8.0), lo, hi);
        p.set("LFO Rate", 1.0f);
        const std::string fast = p.text("LFO Rate");
        std::vector<std::string> shapes;
        for (int s = 0; s < 4; ++s) { p.set("LFO Shape", s / 3.0f); shapes.push_back(p.text("LFO Shape")); }
        std::snprintf(d, sizeof d, "triangle +-2 oct around 500 Hz: %.0f .. %.0f Hz; max rate %s; shapes %s/%s/%s/%s",
                      lo, hi, fast.c_str(), shapes[0].c_str(), shapes[1].c_str(), shapes[2].c_str(), shapes[3].c_str());
        check(lo < 140.0f && lo > 115.0f && hi > 1800.0f && hi < 2050.0f && fast == "100.00 Hz", "LFO: sweeps the cutoff by its depth, 0.1-100 Hz, four shapes", d);
        p.close();
    }
    // 1e. Stereo: the right channel's LFO is inverted (mono: both channels identical)
    {
        auto lrDiff = [&](float stereo) {
            Plug p; p.open(0); p.set("Stutter On", 0.0f);
            p.set("Sweep", octNorm(0.0f)); p.set("Cutoff", cutNorm(600.0f)); p.set("LFO Depth", 0.5f); p.set("LFO Rate", rateNorm(1.0f));
            p.set("Stereo", stereo);
            p.right.assign(guitar.size(), 0.0f);
            const std::vector<float> l = p.run(guitar);
            double dd = 0, ee = 0; for (size_t i = 0; i < l.size(); ++i) { dd += std::fabs(l[i] - p.right[i]); ee += std::fabs(l[i]); }
            p.close();
            return dd / ee;
        };
        const double mono = lrDiff(0.0f), st = lrDiff(1.0f);
        std::snprintf(d, sizeof d, "L/R difference: mono %.6f, stereo %.2f of the signal", mono, st);
        check(mono == 0.0 && st > 0.2, "stereo mode: the two filters move in opposite directions", d);
    }
    // 1f. resonance: a clear, long twang at the top of the knob, but it never self-oscillates or squeals
    {
        auto ring = [&](float reso, double& late, double& peakAll) {
            Plug p; p.open(0); p.set("Stutter On", 0.0f);
            p.set("Sweep", octNorm(0.0f)); p.set("Cutoff", cutNorm(1000.0f)); p.set("Resonance", reso);
            std::vector<float> imp((size_t)(2 * SR), 0.0f); imp[100] = 0.9f;
            const std::vector<float> o = p.run(imp);
            double r = 0; for (size_t i = 100 + SR / 50; i < 100 + SR / 20; ++i) r = std::fmax(r, std::fabs(o[i]));   // 20..50 ms
            late = 0;   for (size_t i = SR; i < o.size(); ++i) late = std::fmax(late, std::fabs(o[i]));             // 1..2 s
            peakAll = peakAbs(o);
            p.close();
            return r;
        };
        double l50, l100, p50, p100;
        const double r50 = ring(0.5f, l50, p50), r100 = ring(1.0f, l100, p100);
        // and played hard: loud plucks at full resonance stay well under the limiter
        Plug p; p.open(0); p.set("Stutter On", 0.0f); p.set("Resonance", 1.0f); p.set("Sensitivity", 0.4f);
        const float loud = peakAbs(p.run(plucks(3.0, 0.9f)), SR / 10); p.close();
        std::snprintf(d, sizeof d, "ring 20-50 ms: 50 %% %.0f dBFS, 100 %% %.0f dBFS; 1-2 s later at 100 %%: %.0f dBFS; loud plucks peak %.1f dBFS",
                      20 * std::log10(r50 + 1e-12), 20 * std::log10(r100 + 1e-12), 20 * std::log10(l100 + 1e-12), 20 * std::log10(loud + 1e-12));
        check(r100 > 10.0 * r50 && l100 < 1e-5 && p100 < 0.5, "resonance: strong twang at the top, no self-oscillation", d);
        check(loud <= 0.8913f + 1e-4f, "resonance: hard playing at 100 % cannot squeal past the -1 dBFS ceiling", d);
    }    // 1g. VCA: the same envelope drives the volume; with Gate mode it is a noise gate
    {
        std::vector<float> sig((size_t)(4.0 * SR));                  // -50 dBFS hum + 0.2 s loud bursts every second
        for (size_t i = 0; i < sig.size(); ++i)
            sig[i] = (float)(0.00316 * std::sin(6.283185307 * 60 * i / SR) + ((std::fmod(i / (double)SR, 1.0) < 0.2) ? 0.2 * std::sin(6.283185307 * 220 * i / SR) : 0.0));
        auto gapRms = [&](bool vca) {
            Plug p; p.open(0); p.set("Stutter On", 0.0f); p.set("Filter Mix", 0.0f);
            p.set("Env Mode", 1.0f); p.set("Sensitivity", 0.5f); p.set("Release", relNorm(50.0f)); p.set("VCA", vca ? 1.0f : 0.0f);
            const std::vector<float> o = p.run(sig);
            double e = 0; size_t n = 0;
            for (size_t i = SR; i < o.size(); ++i) if (std::fmod(i / (double)SR, 1.0) > 0.6) { e += (double)o[i] * o[i]; ++n; }
            p.close();
            return std::sqrt(e / n);
        };
        const double open = gapRms(false), gated = gapRms(true);
        std::snprintf(d, sizeof d, "hum between notes: VCA off %.1f dBFS, VCA + Gate %.1f dBFS", 20 * std::log10(open), 20 * std::log10(gated + 1e-12));
        check(gated < open * 0.01, "VCA: Gate mode + VCA gates the signal between notes (more than 40 dB)", d);
    }
    // 1h. LFO to Volume: square tremolo
    {
        Plug p; p.open(0); p.set("Stutter On", 0.0f); p.set("Filter Mix", 0.0f);
        p.set("LFO To", 0.5f); p.set("LFO Shape", 1.0f / 3.0f); p.set("LFO Depth", 1.0f); p.set("LFO Rate", rateNorm(5.0f));
        const std::vector<float> o = p.run(sine(2.0, 440, 0.1));
        double mn = 1e9, mx = 0;
        for (size_t w = SR / 2; w + 441 < o.size(); w += 441) { double e = 0; for (size_t i = w; i < w + 441; ++i) e += (double)o[i] * o[i]; mn = std::fmin(mn, e); mx = std::fmax(mx, e); }
        std::snprintf(d, sizeof d, "10 ms windows: loudest / quietest %.0f dB", 10 * std::log10(mx / (mn + 1e-20)));
        check(mx / (mn + 1e-20) > 1000.0, "LFO to Volume: full-depth square tremolo", d);
        p.close();
    }    // 2. master bypass is bit-exact (delayed by the latency)
    {
        Plug p; p.open(4);                       // "Sleepy Glitch": stutter holding
        p.set("Bypass", 1.0f);
        p.run(plucks(0.3));                      // let the bypass crossfade settle
        const std::vector<float> out = p.run(guitar);
        size_t diff = 0;
        for (size_t i = 0; i + LAT < guitar.size(); ++i) if (out[i + LAT] != guitar[i]) ++diff;
        check(diff == 0, "bypass: output == input delayed 64 samples", std::to_string(diff) + " samples differ");
        p.close();
    }
    // 3. the limiter holds -1 dBFS with everything maxed
    {
        Plug p; p.open(0);
        p.set("Input Gain", 1.0f); p.set("Output", 1.0f); p.set("Resonance", 1.0f); p.set("Feedback", 1.0f);
        p.set("Hold", 1.0f); p.set("Filter Type", 0.0f);
        std::vector<float> loud = plucks(6.0, 0.95f);
        const std::vector<float> out = p.run(loud);
        const float pk = peakAbs(out);
        char d[64]; std::snprintf(d, sizeof d, "peak %.2f dBFS (ceiling -1.00), GR shown %s", 20 * std::log10(pk), p.text("Limiter GR").c_str());
        check(pk <= 0.8913f + 1e-4f, "limiter: full gain, resonance and feedback stay under -1 dBFS", d);
        p.close();
    }
    // 4. fixed stutter loops exactly the captured slice
    {
        Plug p; p.open(0);
        p.set("Filter On", 0.0f); p.set("Stut Mode", 0.0f); p.set("Sync", 0.0f);
        p.set("Slice", std::log(10.0f) / std::log(200.0f));     // 100 ms
        p.set("Loop", std::log(20.0f) / std::log(160.0f));      // 1000 ms
        p.set("Feedback", 1.0f); p.set("Dry Mute", 1.0f);
        p.run(std::vector<float>((size_t)(0.5 * SR), 0.0f));   // let the settings glide to their values
        p.set("Hold", 1.0f);
        const std::vector<float> out = p.run(guitar);
        // capture 0..100 ms, loop 100..1100 ms: compare the loop with itself one slice later
        const size_t s = 4410, a = LAT + 2 * s;     // one slice into the loop: the 5 ms dry fade is long done
        double num = 0, den = 0;
        for (size_t i = a; i < a + 4 * s; ++i) { if (i % s < 100 || i % s > s - 100) continue; num += std::fabs(out[i] - out[i + s]); den += std::fabs(out[i]); }
        char d[96]; std::snprintf(d, sizeof d, "difference between repeats %.6f of the signal; slice text %s, loop %s",
                                  den > 0 ? num / den : 1.0, p.text("Slice").c_str(), p.text("Loop").c_str());
        check(den > 0 && num / den < 1e-4, "stutter: 100 ms slice repeats exactly at 100 % feedback", d);
        p.close();
    }
    // 5. Trigger: loops while on, stops when off (touch: tap on / tap off; Q-Link past halfway: held),
    //    with Stutter On off; the Stutter LED is lit only while it is active
    {
        Plug p; p.open(1);                                 // Quack Funk: Stutter On is off
        p.set("Slice", 0.6f); p.set("Loop", 0.3f);         // 240 ms slice
        p.run(plucks(0.3));
        const bool ledOff0 = p.getN("Stutter LED") < 0.5f;
        std::set<std::string> held;
        p.set("Trigger", 0.7f);                            // a Q-Link turned past halfway
        p.run(plucks(1.5), [&](size_t) { held.insert(p.text("Stutter Now").substr(0, 4)); });
        const std::string whileHeld = p.text("Stutter Now");
        const bool ledOn = p.getN("Stutter LED") > 0.5f;
        p.set("Trigger", 0.0f);
        p.run(plucks(0.3));
        const std::string released = p.text("Stutter Now");
        const bool ledOff = p.getN("Stutter LED") < 0.5f;
        p.tap("Trigger");                                  // touch: on ...
        p.run(plucks(2.0));
        const std::string tappedOn = p.text("Stutter Now");
        p.tap("Trigger");                                  // ... and off
        p.run(plucks(0.2));
        const std::string tappedOff = p.text("Stutter Now");
        std::string s1; for (const auto& s : held) s1 += "[" + s + "]";
        check(held.count("Loop") && whileHeld != "Idle" && released == "Idle" && ledOff0 && ledOn && ledOff,
              "stutter: Trigger loops while on (Stutter On off), stops when off; LED lit only while active",
              "held " + s1 + " still " + whileHeld + " after 1.5 s; released -> " + released);
        check(tappedOn != "Idle" && tappedOff == "Idle", "stutter: touch Trigger is tap on / tap off",
              "after tap on (2 s): " + tappedOn + "; after tap off: " + tappedOff);
        p.close();
    }
    // 5b. Rand Width: 0 % = random mode always plays the Slice length; 100 % spreads over Min..Max
    {
        auto lengths = [&](float width) {
            Plug p; p.open(0);
            p.set("Stut Mode", 1.0f); p.set("Rand Width", width); p.set("Slice", 0.5f); p.set("Hold", 1.0f);
            std::set<int> ms;
            p.run(plucks(12.0), [&](size_t) { const std::string t = p.text("Stutter Now"); if (t.rfind("Loop", 0) == 0) ms.insert(std::atoi(t.c_str() + 5)); });
            p.close();
            return ms;
        };
        const std::set<int> w0 = lengths(0.0f), w1 = lengths(1.0f);
        std::string a, b; for (int v : w0) a += std::to_string(v) + " "; for (int v : w1) b += std::to_string(v) + " ";
        check(w0.size() == 1 && w1.size() >= 3, "stutter: Rand Width 0 % keeps the Slice length, 100 % spreads it", "0 %: " + a + "/ 100 %: " + b);
    }    // 6. tempo sync + random: lengths are note values at the MPC tempo
    {
        g_time.tempo = 90.0;
        Plug p; p.open(0);
        p.set("Stut Mode", 1.0f); p.set("Sync", 1.0f); p.set("Hold", 1.0f);
        std::set<int> slices;
        p.run(plucks(20.0), [&](size_t) { const std::string t = p.text("Stutter Now"); if (t.rfind("Loop", 0) == 0) slices.insert(std::atoi(t.c_str() + 5)); });
        bool allDivisions = !slices.empty();
        const float beatMs = 60000.0f / 90.0f;
        for (int ms : slices)
        {
            bool match = false;
            for (float b : {0.0625f, 0.08333f, 0.125f, 0.16667f, 0.25f, 0.33333f, 0.375f, 0.5f, 0.66667f, 0.75f, 1.0f, 1.33333f, 1.5f, 2.0f})
                if (std::fabs(b * beatMs - ms) < 1.0f) match = true;
            allDivisions = allDivisions && match;
        }
        std::string list; for (int ms : slices) list += std::to_string(ms) + " ";
        check(slices.size() >= 3 && allDivisions, "stutter: random + tempo picks note values at 90 BPM", "slice ms seen: " + list + "; Slice Min shows " + p.text("Slice Min"));
        p.close();
        g_time.tempo = 120.0;
    }
    // 7. pedal morph: heel/toe on Resonance blends exactly, and survives a project reload
    {
        auto render = [&](float resoNorm, bool assign, float pedal) {
            Plug p; p.open(0); p.set("Stutter On", 0.0f);
            std::vector<uint8_t> chunk;
            if (assign)
            {
                p.set("Resonance", 0.5f); p.run(plucks(0.05));
                p.set("Assign", 1.0f); p.run(plucks(0.05));
                p.set("Resonance", 0.1f); p.run(plucks(0.05));            // the knob to assign
                const std::string s1 = p.text("Pedal Status");
                p.tap("Set Heel"); p.run(plucks(0.05));
                p.set("Resonance", 0.9f); p.run(plucks(0.05));
                p.tap("Set Toe"); p.run(plucks(0.05));
                const std::string s2 = p.text("Pedal Status");
                p.set("Assign", 0.0f); p.run(plucks(0.05));
                const std::string s3 = p.text("Pedal Status");
                static bool shown = false;
                if (!shown) { shown = true; std::printf("      status: \"%s\" -> \"%s\" -> \"%s\"\n", s1.c_str(), s2.c_str(), s3.c_str());
                              check(s1 == "Resonance: Set Heel" && s2 == "Resonance kept: 10 % > 90 %" && s3 == "1 knob on pedal", "pedal: Set Heel / Set Toe keep the assignment, status shows the stored values"); }
                p.set("Resonance", 0.5f);                                   // knob position no longer matters
                void* data = nullptr; const VstIntPtr n = p.call(effGetChunk, 0, 0, &data);
                chunk.assign((uint8_t*)data, (uint8_t*)data + n);
                p.close();
                Plug q; q.open(0);                                          // project reload
                q.call(effSetChunk, 0, (VstIntPtr)chunk.size(), chunk.data());
                q.set("Pedal", pedal);
                q.run(plucks(0.01));
                std::vector<float> o = q.run(guitar);
                if (pedal == 0.0f) check(q.text("Pedal Status") == "1 knob on pedal", "pedal: assignment restored after reload", q.text("Pedal Status"));
                q.close();
                return o;
            }
            p.set("Resonance", resoNorm); p.set("Pedal", pedal);
            p.run(plucks(0.01));
            std::vector<float> o = p.run(guitar);
            p.close();
            return o;
        };
        const auto heel = render(0.1f, false, 0.0f), toe = render(0.9f, false, 0.0f);
        const auto m0 = render(0, true, 0.0f), m1 = render(0, true, 1.0f);
        double d0 = 0, d1 = 0;
        for (size_t i = 0; i < guitar.size(); ++i) { d0 = std::fmax(d0, std::fabs(m0[i] - heel[i])); d1 = std::fmax(d1, std::fabs(m1[i] - toe[i])); }
        char d[96]; std::snprintf(d, sizeof d, "max difference: pedal 0 vs heel %.2g, pedal 100 vs toe %.2g", d0, d1);
        check(d0 < 1e-6 && d1 < 1e-6, "pedal: heel at 0 %, toe at 100 %, exactly", d);
    }
    // 8. help text
    {
        Plug p; p.open(0);
        p.set("Help", 9.0f / 45.0f); p.run(plucks(0.01));
        check(p.text("Help") == "Env: drive into the follower." && p.text("Help 2") == "Gate: the on/off threshold.",
              "help: button 9 shows the Sensitivity explanation", p.text("Help") + " / " + p.text("Help 2"));
        p.close();
    }
    // 9. patch randomizer
    {
        Plug p; p.open(0);
        p.run(plucks(0.05));
        // pedal-assign Resonance (heel 0.1, toe 0.9)
        p.set("Assign", 1.0f); p.run(plucks(0.02));
        p.set("Resonance", 0.1f); p.run(plucks(0.02));
        p.tap("Set Heel"); p.run(plucks(0.02));
        p.set("Resonance", 0.9f); p.run(plucks(0.02));
        p.tap("Set Toe"); p.run(plucks(0.02));
        p.set("Assign", 0.0f); p.run(plucks(0.02));

        auto snapshot = [&]() { std::vector<float> v(p.e->numParams); for (int i = 0; i < p.e->numParams; ++i) v[i] = p.e->getParameter(p.e, i); return v; };
        auto idx = [&](const char* n) { return p.find(n); };
        const char* never[] = {"Bypass", "Input Gain", "Output", "Mix", "Routing"};
        const char* stut[] = {"Feedback", "Stut Mode", "Stutter Mix", "Slice", "Loop", "Slice Min", "Slice Max", "Loop Min", "Loop Max"};

        // 0 %: nothing moves
        p.set("Rand Amount", 0.0f);
        std::vector<float> before = snapshot();
        p.tap("Randomize"); p.run(plucks(0.15));
        std::vector<float> after = snapshot();
        int moved0 = 0;
        for (const char* n : {"Sensitivity", "Sweep", "Attack", "Release", "Filter Mix", "Feedback", "Slice"}) if (after[idx(n)] != before[idx(n)]) ++moved0;
        check(moved0 == 0, "randomize: amount 0 % changes nothing");

        // 100 % with the stutter locked
        p.set("Rand Amount", 1.0f); p.set("Lock Stutter", 1.0f);
        before = snapshot();
        p.tap("Randomize"); p.run(plucks(0.15));
        after = snapshot();
        bool neverOk = true, stutOk = true;
        for (const char* n : never) neverOk = neverOk && after[idx(n)] == before[idx(n)];
        for (const char* n : stut) stutOk = stutOk && after[idx(n)] == before[idx(n)];
        const float sens = after[idx("Sensitivity")] * 100, range = -6 + 12 * after[idx("Sweep")];
        const float att = std::pow(2000.0f, after[idx("Attack")]), rel = 5 * std::pow(1000.0f, after[idx("Release")]);
        const float fmix = after[idx("Filter Mix")] * 100;
        char d[160]; std::snprintf(d, sizeof d, "Sens %.0f %%, Env Amount %+.1f oct, Attack %.1f ms, Release %.0f ms, Filter Mix %.0f %%; status \"%s\"",
                                   sens, range, att, rel, fmix, p.text("Rand Status").c_str());
        check(sens >= 39.9f && sens <= 85.1f && range >= -4.01f && range <= 5.01f && att >= 1.99f && att <= 200.5f &&
              rel >= 49.9f && rel <= 1501.0f && fmix >= 59.9f, "randomize: 100 % lands every filter value in its musical range", d);
        check(neverOk, "randomize: bypass, input gain, output, mix and routing untouched");
        check(stutOk, "randomize: Lock Stutter keeps the stutter section unchanged");
        check(after[idx("Resonance")] == before[idx("Resonance")] && p.text("Pedal Status") == "1 knob on pedal",
              "randomize: pedal-assigned knob (Resonance) keeps its heel/toe assignment");
        check(after[idx("Sensitivity")] != before[idx("Sensitivity")] && p.getN("Randomize") < 0.5f, "randomize: the second tap (switch back off) randomizes too; the plugin never writes the switch");

        // Undo restores exactly
        p.tap("Undo"); p.run(plucks(0.15));
        const std::vector<float> undone = snapshot();
        int diffs = 0;
        for (int i = 0; i < p.e->numParams; ++i)
        {
            if (i >= idx("Input Level") && i <= idx("Pedal Status")) continue;   // displays
            if (i == idx("Help") || i == idx("Help 2") || i == idx("Rand Status") || i == idx("Env Level") || i == idx("Stutter LED") || i == idx("Touch Level") || i == idx("Sweep Now") || i == idx("Preset Name") || i == idx("Undo") || i == idx("Randomize")) continue;
            if (undone[i] != before[i]) ++diffs;
        }
        check(diffs == 0, "undo: every randomized setting restored exactly", std::to_string(diffs) + " differ; status \"" + p.text("Rand Status") + "\"");

        // 25 %: a nudge, not a reroll
        p.set("Rand Amount", 0.25f); p.set("Lock Stutter", 0.0f);
        before = snapshot();
        p.tap("Randomize"); p.run(plucks(0.15));
        after = snapshot();
        float maxStep = 0;
        for (const char* n : {"Sensitivity", "Sweep", "Filter Mix", "Feedback", "Stutter Mix", "Slice", "Loop"})
            maxStep = std::fmax(maxStep, std::fabs(after[idx(n)] - before[idx(n)]));
        std::snprintf(d, sizeof d, "largest move %.0f %% of a knob's travel", maxStep * 100);
        check(maxStep <= 0.25f + 1e-4f, "randomize: 25 % only nudges", d);

        // 50 rerolls: ranges stay ordered, audio stays finite and under the ceiling
        p.set("Rand Amount", 1.0f); p.set("Hold", 1.0f);
        bool ordered = true; float pk = 0; bool finite = true;
        for (int k = 0; k < 50; ++k)
        {
            p.tap("Randomize");
            const std::vector<float> o = p.run(plucks(0.3, 0.9f));
            for (float s : o) { finite = finite && std::isfinite(s); pk = std::fmax(pk, std::fabs(s)); }
            ordered = ordered && p.getN("Slice Min") <= p.getN("Slice Max") && p.getN("Loop Min") <= p.getN("Loop Max");
        }
        std::snprintf(d, sizeof d, "peak %.2f dBFS", 20 * std::log10(pk));
        check(ordered && finite && pk <= 0.8913f + 1e-4f, "randomize: 50 rerolls keep min <= max, finite audio, limiter holds", d);
        p.close();
    }
    // 10. every selector takes every value (what the touch page writes), shown by its text
    {
        Plug p; p.open(0);
        struct Sel { const char* name; std::vector<const char*> texts; };
        const Sel sels[] = {
            {"Env Mode", {"Env", "Gate"}}, {"LFO Shape", {"Tri", "Square", "Ramp Up", "Ramp Dn"}},
            {"LFO To", {"Filter", "Volume", "Both"}}, {"Filter Type", {"LP", "BP", "HP"}}, {"Stereo", {"Mono", "Stereo"}},
            {"Routing", {"Filt>Stut", "Stut>Filt", "Parallel"}}, {"Stut Mode", {"Fixed", "Random"}}, {"Sync", {"ms", "Tempo"}},
            {"Sweep Dir", {"Up", "Down", "Up-Down"}}};
        for (const Sel& s : sels)
        {
            std::string seen; bool ok = true;
            const int n = (int)s.texts.size();
            for (int k = 0; k < n; ++k)
            {
                p.set(s.name, n > 1 ? (float)k / (float)(n - 1) : 0.0f);   // the radio button's value
                p.run(plucks(0.02));
                const std::string got = p.text(s.name);
                seen += (k ? "/" : "") + got;
                ok = ok && got == s.texts[(size_t)k];
            }
            check(ok, (std::string("selector: ") + s.name + " takes every choice").c_str(), seen);
        }
        p.close();
    }
    // 11. Save opens naming with an auto-name; Save again keeps it, or edit with Name Pos + Letter and
    //     Confirm. Stored in the preset list after the factory presets and kept across restarts.
    {
        auto progName = [](Plug& q, int i) { char b[64] = {0}; q.call(effGetProgramNameIndexed, i, 0, b); return std::string(b); };
        auto wait = [](Plug& q) { q.run(plucks(0.05)); std::this_thread::sleep_for(std::chrono::milliseconds(300)); q.run(plucks(0.3)); };
        Plug p; p.open(0);
        const int total = p.e->numPrograms, factory = total - 16;
        const std::string before = progName(p, factory);
        p.tap("Randomize"); p.run(plucks(0.1));
        const float sens = p.getN("Sensitivity");
        p.tap("Save"); p.run(plucks(0.2));
        const std::string draft = p.text("Preset Name");
        p.tap("Save"); wait(p);                            // Save again: keep the auto-name
        const std::string status = p.text("Preset Name"), after = progName(p, factory);
        // second preset: rename the auto-name "User 02" to "Funk 02" with the letter picker
        p.tap("Save"); p.run(plucks(0.1));
        const char* word = "Funk";
        for (int i = 0; i < 4; ++i)
        {
            p.set("Name Pos", i / 15.0f); p.run(plucks(0.02));
            const char* cs = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.#&+!";
            const int li = (int)(std::strchr(cs, word[i]) - cs);
            p.set("Letter", li / (float)(std::strlen(cs) - 1)); p.run(plucks(0.02));
        }
        const std::string draft2 = p.text("Preset Name");
        p.tap("Confirm"); wait(p);
        const std::string second = progName(p, factory + 1);
        // third: open naming, Cancel -> nothing stored
        p.tap("Save"); p.run(plucks(0.1)); p.tap("Cancel"); wait(p);
        const std::string third = progName(p, factory + 2), cancelled = p.text("Preset Name");
        p.close();
        Plug q; q.open(0);                                 // "after a restart"
        const std::string reloaded = progName(q, factory), reloaded2 = progName(q, factory + 1);
        q.call(effSetProgram, 0, factory); q.run(plucks(0.05));
        const float sens2 = q.getN("Sensitivity");
        q.close();
        char d[400]; std::snprintf(d, sizeof d, "%d programs; draft '%s' -> '%s' (%s); edit '%s' -> '%s'; cancel -> '%s' (%s); restart: '%s', '%s'; Sensitivity %.3f vs %.3f",
                                   total, draft.c_str(), after.c_str(), status.c_str(), draft2.c_str(), second.c_str(), third.c_str(), cancelled.c_str(),
                                   reloaded.c_str(), reloaded2.c_str(), sens2, sens);
        check(factory == 22 && before == "Empty 01" && draft == "Random 01[_]" && after == "Random 01" && status == "Saved: Random 01" &&
              draft2 == "Fun[k] 02" && second == "Funk 02" && third == "Empty 03" && cancelled == "Not saved" &&
              reloaded == "Random 01" && reloaded2 == "Funk 02" && std::fabs(sens2 - sens) < 1e-4f,
              "save: name entry (auto-name, letter picker, confirm, cancel), listed, kept across restarts", d);
    }
    // 12. auto sweep: range, three directions, alone (envelope Sweep at 0)
    {
        auto run = [&](int dir, float& lo, float& hi, std::vector<float>* trace) {
            Plug p; p.open(0); p.set("Stutter On", 0.0f);
            p.set("Sweep", octNorm(0.0f)); p.set("Cutoff", cutNorm(200.0f));
            p.set("Auto Sweep", 1.0f); p.set("Sweep Range", 3.0f / 6.0f); p.set("Sweep Dir", dir / 2.0f); p.set("Sweep Touch", 0.0f);
            p.set("Sweep Rate", std::log(0.25f / 0.05f) / std::log(200.0f));     // 0.25 Hz: the 10 Hz display catches it
            lo = 1e9f; hi = 0;
            p.run(sine(8.0, 220, 0.1), [&](size_t pos) { if (pos % 4410 < 128) { const float hz = (float)std::atof(p.text("Filter Pos").c_str());
                lo = std::fmin(lo, hz); hi = std::fmax(hi, hz); if (trace) trace->push_back(hz); } });
            p.close();
        };
        float l0, h0, l1, h1, l2, h2; std::vector<float> up, dn;
        run(0, l0, h0, &up); run(1, l1, h1, &dn); run(2, l2, h2, nullptr);
        // direction: Up mostly rises between readings, Down mostly falls
        int rises = 0, falls = 0;
        for (size_t i = 1; i < up.size(); ++i) rises += up[i] > up[i - 1];
        for (size_t i = 1; i < dn.size(); ++i) falls += dn[i] < dn[i - 1];
        std::snprintf(d, sizeof d, "Up %.0f..%.0f Hz (rising %d/%zu), Down %.0f..%.0f (falling %d/%zu), Up-Down %.0f..%.0f",
                      l0, h0, rises, up.size() - 1, l1, h1, falls, dn.size() - 1, l2, h2);
        check(l0 < 260 && h0 > 1400 && h0 < 1700 && rises > (int)(up.size() * 3 / 4) && falls > (int)(dn.size() * 3 / 4) && h2 > 1400,
              "auto sweep: 3 octaves above the cutoff, up / down / up-down", d);
    }
    // 13. touch: soft playing = slowest rate, hard = fastest; steady playing gives a steady rate
    {
        // jitter = largest change between two 100 ms readings, relative to the average
        auto rate = [&](const std::vector<float>& in, bool touch, float amt, float& jitter) {
            Plug p; p.open(0); p.set("Stutter On", 0.0f);
            p.set("Auto Sweep", 1.0f); p.set("Sweep Touch", touch ? 1.0f : 0.0f); p.set("Sweep Touch Amt", amt);
            p.set("Sweep Rate", std::log(0.5f / 0.05f) / std::log(200.0f)); p.set("Sweep Rate Max", std::log(8.0f / 0.05f) / std::log(200.0f));
            std::vector<float> rs;
            p.run(in, [&](size_t pos) { if (pos > 2 * SR && pos % 4410 < 128) rs.push_back((float)std::atof(p.text("Sweep Now").c_str())); });
            p.close();
            float s = 0, step = 0; for (size_t i = 0; i < rs.size(); ++i) { s += rs[i]; if (i) step = std::fmax(step, std::fabs(rs[i] - rs[i - 1])); }
            const float avg = s / rs.size(); jitter = step / avg;
            return avg;
        };
        float j1, j2, j3, j4, j5;
        const float soft = rate(plucks(6.0, 0.01f), true, 1.0f, j1), hard = rate(plucks(6.0, 0.5f), true, 1.0f, j2);
        const float off = rate(plucks(6.0, 0.5f), false, 1.0f, j3), half = rate(plucks(6.0, 0.5f), true, 0.5f, j4);
        const float held = rate(sine(6.0, 220, 0.25), true, 1.0f, j5);                 // a held note
        std::snprintf(d, sizeof d, "sweep speed: soft (-40 dBFS) %.2f Hz, hard (-6 dBFS) %.2f Hz, touch off %.2f Hz, amount 50 %% %.2f Hz; "
                      "largest 100 ms step: plucks %.0f %%, held note %.1f %% (%.2f Hz)", soft, hard, off, half, 100 * j2, 100 * j5, held);
        check(soft < 1.0f && hard > 5.0f && hard <= 8.01f && std::fabs(off - 0.5f) < 0.02f && half > soft && half < hard && j2 < 0.35f && j5 < 0.02f,
              "sweep touch: soft = slow, hard = fast, within min..max; amount scales it; smooth", d);
    }    // 14. Env Touch: on (100 %) soft notes barely move the filter; off, soft and hard notes sweep alike
    {
        auto oct = [&](float amp, bool touch) {
            Plug p; p.open(0); p.set("Stutter On", 0.0f); p.set("Env Touch", touch ? 1.0f : 0.0f);
            float lo, hi; sweep(p, plucks(2.0, amp), lo, hi); p.close();
            return std::log2(hi / 180.0f);
        };
        const float sOn = oct(0.0316f, true), hOn = oct(0.5f, true), sOff = oct(0.0316f, false), hOff = oct(0.5f, false);
        std::snprintf(d, sizeof d, "Env Touch on: soft %.1f / hard %.1f oct; off: soft %.1f / hard %.1f oct", sOn, hOn, sOff, hOff);
        check(sOn < 1.0f && hOn > 4.0f && sOff > 3.0f && hOff > 4.0f, "env touch: switch decides whether dynamics shape the sweep", d);
    }
    // 15. stutter re-roll speed: slow = lengths repeat, fast = new lengths often; touch makes hard playing faster
    {
        auto distinct = [&](float rate, float rateMax, bool touch, double amp) {
            Plug p; p.open(0);
            p.set("Stut Mode", 1.0f); p.set("Rand Width", 1.0f); p.set("Hold", 1.0f); p.set("Slice", 0.35f);
            p.set("Rand Rate", std::log(rate / 0.1f) / std::log(100.0f)); p.set("Rand Rate Max", std::log(rateMax / 0.1f) / std::log(100.0f));
            p.set("Stut Touch", touch ? 1.0f : 0.0f); p.set("Stut Touch Amt", 1.0f);
            std::vector<int> seq;
            p.run(plucks(10.0, (float)amp), [&](size_t) { const std::string t = p.text("Stutter Now");
                if (t.rfind("Loop", 0) == 0) { const int ms = std::atoi(t.c_str() + 5); if (seq.empty() || seq.back() != ms) seq.push_back(ms); } });
            p.close();
            return (int)seq.size();
        };
        const int slow = distinct(0.1f, 0.1f, false, 0.3), fast = distinct(8.0f, 8.0f, false, 0.3);
        const int soft = distinct(0.1f, 8.0f, true, 0.01), hard = distinct(0.1f, 8.0f, true, 0.5);
        std::snprintf(d, sizeof d, "length changes in 10 s: Rand Rate 0.1 Hz %d, 8 Hz %d; touch soft %d, hard %d", slow, fast, soft, hard);
        check(slow <= 3 && fast >= 8 && hard >= 3 * std::max(1, soft) && soft <= 4, "stutter re-roll speed: rate and touch control how often lengths change", d);
    }
    // 16. presets carry the newer settings; the Radiohead presets move the filter clearly on a normal signal
    {
        auto progIndex = [](Plug& q, const char* name) { for (int i = 0; i < q.e->numPrograms; ++i) { char b[64] = {0}; q.call(effGetProgramNameIndexed, i, 0, b); if (!std::strcmp(b, name)) return i; } return -1; };
        Plug p; p.open(0);
        const int gts = progIndex(p, "XT Chaos Glitch");
        p.call(effSetProgram, 0, gts); p.run(plucks(0.05));
        const bool autoOn = p.getN("Auto Sweep") > 0.5f;
        p.call(effSetProgram, 0, 0); p.run(plucks(0.05));
        const bool autoOff = p.getN("Auto Sweep") < 0.5f;
        p.close();
        check(autoOn && autoOff, "presets: later settings (auto sweep) load with each preset");
        for (const char* nm : {"RH Paranoid Wah", "RH Paranoid Sleep", "RH Kill Switch"})
        {
            Plug q; q.open(0); const int idx = progIndex(q, nm); q.close();
            Plug r; r.open(idx); r.set("Stutter On", 0.0f); r.set("Auto Sweep", 0.0f);   // the envelope alone
            float lo, hi; sweep(r, plucks(3.0, 0.126f), lo, hi); r.close();          // -18 dBFS: normal DI guitar
            std::snprintf(d, sizeof d, "%s, -18 dBFS plucks, envelope alone: %.0f .. %.0f Hz (%.1f oct)", nm, lo, hi, std::log2(hi / lo));
            check(std::log2(hi / lo) > 3.5f && hi > 2500.0f, "radiohead presets: deep, clearly audible wah movement", d);
        }
    }    // 17. every factory preset (including the extreme ones), driven hard: finite, under -1 dBFS
    {
        Plug p0; p0.open(0); const int nf = p0.e->numPrograms - 16; p0.close();
        std::string worst; float wpk = 0; bool finite = true;
        for (int i = 0; i < nf; ++i)
        {
            Plug p; p.open(i);
            p.set("Trigger", 1.0f);                                // stutter on as well
            const std::vector<float> o = p.run(plucks(4.0, 0.9f));
            float pk = 0; for (float s : o) { finite = finite && std::isfinite(s); pk = std::fmax(pk, std::fabs(s)); }
            if (pk > wpk) { wpk = pk; char b[64] = {0}; p.call(effGetProgramNameIndexed, i, 0, b); worst = b; }
            p.close();
        }
        std::snprintf(d, sizeof d, "%d factory presets at -1 dBFS input; loudest: %s %.2f dBFS", nf, worst.c_str(), 20 * std::log10(wpk));
        check(finite && wpk <= 0.8913f + 1e-4f, "presets: all factory presets, including the extreme ones, stay finite and under the ceiling", d);
    }
    std::printf("\n%d failure(s)\n", g_fail);
    return g_fail;
}
