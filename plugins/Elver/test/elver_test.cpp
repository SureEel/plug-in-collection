// elver_test - functional, per-control and CPU tests for Elver (desktop only, not shipped).
// usage: elver_test <elver.dll> <scratch dir for user presets> [renders dir]
#include "mpc_vst2_abi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace {
VstTimeInfo g_time;
VstIntPtr cb(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float) { if (op == audioMasterVersion) return 2400; if (op == audioMasterGetTime) return (VstIntPtr)&g_time; return 0; }
typedef AEffect* (*MainProc)(audioMasterCallback);
MainProc g_main;
int g_fail = 0, g_pass = 0;
const int SR = 44100;
std::vector<std::string> g_controlList;
void check(bool ok, const std::string& what, const std::string& d = "")
{
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), d.empty() ? "" : "  -- ", d.c_str());
    ok ? ++g_pass : ++g_fail;
}
struct Ev { int at; unsigned char b0, b1, b2; };
struct Plug
{
    AEffect* e = nullptr;
    VstIntPtr call(VstInt32 op, VstInt32 i = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0) { return e->dispatcher(e, op, i, v, p, o); }
    void open(int prog = 4) {   // 4 = 'AMB (empty)': the default sound
        e = g_main(cb); call(effSetSampleRate, 0, 0, nullptr, (float)SR); call(effSetBlockSize, 0, 128); call(effOpen); call(effSetProgram, 0, prog); call(effMainsChanged, 0, 1); call(effStartProcess); }
    void close() { call(effStopProcess); call(effMainsChanged, 0, 0); call(effClose); }
    std::map<std::string, int> idx;
    int find(const char* n)
    {
        if (idx.empty()) { char b[256]; for (int i = 0; i < e->numParams; ++i) { b[0] = 0; call(effGetParamName, i, 0, b); idx[b] = i; } }
        auto it = idx.find(n); if (it == idx.end()) { std::printf("no param %s\n", n); std::exit(99); } return it->second;
    }
    void setN(int i, float v) { e->setParameter(e, i, v); }
    void set(const char* n, float v) { setN(find(n), v); }
    float getN(const char* n) { return e->getParameter(e, find(n)); }
    std::string text(int i) { char b[256] = {0}; call(effGetParamDisplay, i, 0, b); return b; }
    std::string text(const char* n) { return text(find(n)); }
    std::string progName(int i) { char b[64] = {0}; call(effGetProgramNameIndexed, i, 0, b); return b; }
    void tap(const char* n) { run(0.3); set(n, getN(n) > 0.5f ? 0.0f : 1.0f); run(0.05); }
    // render with MIDI events (times in samples from now); returns L, R in out
    void render(double sec, std::vector<Ev> evs, std::vector<float>* L = nullptr, std::vector<float>* R = nullptr)
    {
        const size_t total = (size_t)(sec * SR);
        if (L) L->assign(total, 0.0f); if (R) R->assign(total, 0.0f);
        std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
        size_t ei = 0; float l[128], r[128];
        for (size_t p = 0; p < total; p += 128)
        {
            const int n = (int)std::min<size_t>(128, total - p);
            std::vector<VstMidiEvent> m; std::vector<VstEvent*> ptrs;
            while (ei < evs.size() && evs[ei].at < (int)(p + n))
            {
                VstMidiEvent me; std::memset(&me, 0, sizeof me); me.type = kVstMidiType; me.byteSize = sizeof me;
                me.deltaFrames = std::max(0, evs[ei].at - (int)p); me.midiData[0] = (char)evs[ei].b0; me.midiData[1] = (char)evs[ei].b1; me.midiData[2] = (char)evs[ei].b2;
                m.push_back(me); ++ei;
            }
            if (!m.empty())
            {
                std::vector<char> store(sizeof(VstEvents) + sizeof(VstEvent*) * m.size());
                VstEvents* list = (VstEvents*)store.data(); list->numEvents = (int)m.size(); list->reserved = 0;
                for (size_t k = 0; k < m.size(); ++k) list->events[k] = (VstEvent*)&m[k];
                call(effProcessEvents, 0, 0, list);
            }
            for (int i = 0; i < n; ++i) l[i] = r[i] = 0.0f;
            float* io[2] = {l, r}; e->processReplacing(e, io, io, n);
            for (int i = 0; i < n; ++i) { if (L) (*L)[p + i] = l[i]; if (R) (*R)[p + i] = r[i]; }
        }
    }
    void run(double sec) { render(sec, {}); }
};
Ev on(int at, int note, int vel = 100) { return {at, 0x90, (unsigned char)note, (unsigned char)vel}; }
Ev off(int at, int note) { return {at, 0x80, (unsigned char)note, 0}; }
double rms(const std::vector<float>& v, size_t a, size_t b) { double s = 0; b = std::min(b, v.size()); for (size_t i = a; i < b; ++i) s += (double)v[i] * v[i]; return std::sqrt(s / std::max<size_t>(1, b - a)); }
float peak(const std::vector<float>& v) { float m = 0; for (float x : v) m = std::fmax(m, std::isfinite(x) ? std::fabs(x) : 9.0f); return m; }
double dB(double x) { return 20 * std::log10(x + 1e-12); }
double tone(const std::vector<float>& v, double hz, size_t a, size_t b)
{
    const double w = 6.283185307179586 * hz / SR, c = 2 * std::cos(w); double s1 = 0, s2 = 0; const double n = (double)(b - a);
    for (size_t i = a; i < b; ++i) { const double win = 0.5 - 0.5 * std::cos(6.283185307179586 * (double)(i - a) / n); const double s0 = v[i] * win + c * s1 - s2; s2 = s1; s1 = s0; }
    return std::sqrt(std::fmax(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / n * 4.0;
}
double clickRatio(const std::vector<float>& v)
{   // largest sample step against the signal's typical step
    double mx = 0, s = 0; for (size_t i = 1; i < v.size(); ++i) { const double d = std::fabs(v[i] - v[i - 1]); mx = std::fmax(mx, d); s += d; }
    return mx / (s / v.size() + 1e-12);
}
// A click is a step much larger than every other step around it: a waveform's own edges repeat every
// cycle (saw resets, square edges, noise), a click does not. For each step (ignoring those under 0.01,
// -40 dBFS) the ratio to the largest other step within +-1024 samples (23 ms) is taken; the result is
// the worst ratio. Steps that are not single-sample jumps (more than 3x both neighbours) are skipped:
// band-limited edges spread over several samples. Calibrated by "clickcal".
// Grit holds each sample for 1 + int(grit x 7) samples: from 1/7 (14.3 %) on it makes stairs on purpose
constexpr float kGritHold = 1.0f / 7.0f;
constexpr double kClickLimit = 10.0;     // clean waveforms ~1, fast plucks up to ~5, real clicks 27+
int g_clickAt = -1;
double localClickRatio(const std::vector<float>& v)
{
    g_clickAt = -1;
    const int W = 1024, n = (int)v.size();
    std::vector<double> d((size_t)n, 0.0);
    for (int i = 1; i < n; ++i) d[(size_t)i] = std::fabs((double)v[(size_t)i] - v[(size_t)i - 1]);
    double worst = 0;
    for (int i = 1; i < n; ++i)
    {
        const double di = d[(size_t)i];
        if (di < 0.01) continue;
        // a click is a single-sample jump: a band-limited edge spreads over its neighbours
        const double adj = std::fmax(d[(size_t)i - 1], i + 1 < n ? d[(size_t)i + 1] : 0.0);
        if (di <= 3.0 * adj) continue;
        double other = 1e-4;
        for (int k = std::max(1, i - W); k <= std::min(n - 1, i + W); ++k) if (std::abs(k - i) > 2) other = std::fmax(other, d[(size_t)k]);
        if (di / other > worst) { worst = di / other; g_clickAt = i; }
        if (worst > 1e3) break;
    }
    return worst;
}void wav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r)
{
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    const uint32_t n = (uint32_t)l.size(), bytes = n * 4;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2); u32(SR); u32(SR * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (uint32_t i = 0; i < n; ++i) for (float s : {l[i], r[i]}) { const int16_t v = (int16_t)std::lround(std::fmax(-1.0f, std::fmin(1.0f, s)) * 32767); std::fwrite(&v, 2, 1, f); }
    std::fclose(f);
}
// a rich context: both sides, all three oscillators, unison, filters, envelopes and LFOs active
void richPatch(Plug& p)
{
    p.set("Blend", 0.5f); p.set("Drift", 0.2f); p.set("Drive", 0.1f); p.set("Ring", 0.15f);
    for (const char* s : {"A", "B"})
    {
        const char* waves[3] = {"Saw", "Square", "Triangle"};
        for (int o = 1; o <= 3; ++o)
        {
            char n[32];
            std::snprintf(n, sizeof n, "%s%d Wave", s, o); p.set(n, (o == 1 ? 2 : (o == 2 ? 3 : 1)) / 5.0f); (void)waves;   // wave index / 5 (six choices)
            std::snprintf(n, sizeof n, "%s%d Level", s, o); p.set(n, 0.6f);
            std::snprintf(n, sizeof n, "%s%d Semi", s, o); p.set(n, (12.0f + (o - 1) * 7.0f) / 24.0f);
        }
        char n[32];
        std::snprintf(n, sizeof n, "%s Unison", s); p.set(n, 1.0f / 3.0f);
        std::snprintf(n, sizeof n, "%s Cutoff", s); p.set(n, 0.62f);
        std::snprintf(n, sizeof n, "%s Reso", s); p.set(n, 0.3f);
        std::snprintf(n, sizeof n, "%s Env Amt", s); p.set(n, 0.7f);
    }
    p.set("LFO1 Depth", 0.3f); p.set("LFO2 Depth", 0.3f);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) return 1;
    _putenv_s("MPC_PLUGIN_FOLDER", argv[2]);
    std::remove((std::string(argv[2]) + "/user-presets.bin").c_str());
    if (const char* skin = std::getenv("ELVER_SKIN"))
    {   // a copy of the real skin in the test plugin folder: Elver keeps its preset list up to date
        CreateDirectoryA((std::string(argv[2]) + "/Plugin Skins").c_str(), nullptr);
        CopyFileA(skin, (std::string(argv[2]) + "/Plugin Skins/TUI.json").c_str(), FALSE);
    }
    g_main = (MainProc)GetProcAddress(LoadLibraryA(argv[1]), "VSTPluginMain");
    if (!g_main) { std::printf("cannot load\n"); return 1; }
    std::memset(&g_time, 0, sizeof g_time); g_time.tempo = 120; g_time.sampleRate = SR; g_time.flags = kVstTempoValid;
    const std::string renders = argc > 3 ? argv[3] : "";
    if (argc > 4 && std::string(argv[4]) == "clickcal")
    {
        std::vector<float> sine(SR), pluck(SR), clicked(SR);
        for (int i = 0; i < SR; ++i)
        {
            const double t = (double)i / SR;
            sine[i] = (float)(0.3 * std::sin(2 * 3.14159265 * 220 * t));
            const double ph = std::fmod(t * 130.81, 1.0);
            pluck[i] = (float)(0.5 * std::exp(-t * 12) * (2 * ph - 1) * std::fmin(1.0, t / 0.004));   // a raw saw pluck (aliasing)
            clicked[i] = sine[i] + (i >= SR / 2 ? 0.25f : 0.0f);
        }
        std::vector<float> blpluck(SR); for (int i = 0; i < SR; ++i) blpluck[i] = (pluck[i] + pluck[i > 0 ? i - 1 : 0] + pluck[i > 1 ? i - 2 : 0]) / 3.0f;   // edges spread like a band-limited saw
        std::vector<float> noise(SR), cut(SR); uint32_t g_rng = 3;
        for (int i = 0; i < SR; ++i) { noise[i] = 0.3f * ((float)((g_rng = g_rng * 1664525u + 1013904223u) >> 8) / 8388608.0f - 1.0f); cut[i] = i < SR / 2 + 50 ? sine[i] : 0.0f; }
        std::printf("click ratio: sine %.1f, band-limited saw pluck %.1f, noise %.1f, (raw aliasing saw %.1f) | sine with a 0.25 jump %.1f, note cut off %.1f (limit %.0f)\n", localClickRatio(sine), localClickRatio(blpluck), localClickRatio(noise), localClickRatio(pluck), localClickRatio(clicked), localClickRatio(cut), kClickLimit);
        return 0;
    }
    if (argc > 5 && std::string(argv[4]) == "replay")
    {   // debug: replay a saved roll; with "name=normalised" overrides, to find what causes a click
        Plug p; p.open();
        std::vector<char> c; if (FILE* fp = std::fopen(argv[5], "rb")) { char b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, fp)) > 0) c.insert(c.end(), b, b + n); std::fclose(fp); }
        p.call(effSetChunk, 0, (VstIntPtr)c.size(), c.data()); p.run(0.02); std::this_thread::sleep_for(std::chrono::milliseconds(50)); p.run(0.05);
        for (int a = 6; a < argc; ++a) { std::string kv(argv[a]); const size_t q = kv.find('='); p.set(kv.substr(0, q).c_str(), (float)std::atof(kv.c_str() + q + 1)); }
        p.run(0.02); std::this_thread::sleep_for(std::chrono::milliseconds(50)); p.render(1.0, {{0, 0xB0, 120, 0}}); p.run(0.05);
        std::vector<float> L, R;
        p.render(4.0, {on(0, 48), off((int)(0.9 * SR), 48), on((int)(1.0 * SR), 55), on((int)(1.0 * SR), 60), on((int)(1.0 * SR), 64),
                       off((int)(2.0 * SR), 55), off((int)(2.0 * SR), 60), off((int)(2.0 * SR), 64), on((int)(2.2 * SR), 72), off((int)(2.6 * SR), 72)}, &L, &R);
        const double cl = localClickRatio(L), cr = localClickRatio(R);
        const std::vector<float>& v = cl >= cr ? L : R; localClickRatio(v); const int at = g_clickAt;
        if (std::getenv("REPLAY_SHOW")) { for (const char* n2 : {"LFO1 Rate", "LFO1 Depth", "LFO1 Shape", "LFO1 To", "LFO2 Rate", "LFO2 Depth", "LFO2 Shape", "LFO2 To", "Routing", "Blend", "A Filter", "A Cutoff", "A Reso", "B Filter", "B Cutoff", "B Reso", "A Release", "B Release", "Drive", "Grit"}) std::printf("%s=%s; ", n2, p.text(n2).c_str()); std::printf("\n"); }
        std::printf("peak %.1f dBFS, rms %.1f dBFS; click ratio L %.1f R %.1f; worst at %.4f s:", dB(std::fmax(peak(L), peak(R))), dB(rms(L, 0, L.size())), cl, cr, at / (double)SR);
        for (int q = std::max(0, at - 4); q >= 0 && q < std::min((int)v.size(), at + 6); ++q) std::printf(" %+.4f", v[(size_t)q]);
        std::printf("\n");
        p.close(); return 0;
    }
    if (argc > 5 && std::string(argv[4]) == "moods")
    {   // debug: roll one mood many times; print the rolls that fail the render check, with their settings
        const int cat = std::atoi(argv[5]), tries = argc > 6 ? std::atoi(argv[6]) : 100;
        int bad = 0;
        for (int k = 0; k < tries; ++k)
        {
            static Plug p; static bool opened = false; if (!opened) { p.open(); opened = true; }
            p.set("Category", cat / 6.0f); p.tap("Randomize"); p.run(0.02); std::this_thread::sleep_for(std::chrono::milliseconds(30)); p.render(1.0, {{0, 0xB0, 120, 0}}); p.run(0.05);   // all sound off between rolls
            std::vector<float> L, R;
            p.render(4.0, {on(0, 48), off((int)(0.9 * SR), 48), on((int)(1.0 * SR), 55), on((int)(1.0 * SR), 60), on((int)(1.0 * SR), 64),
                           off((int)(2.0 * SR), 55), off((int)(2.0 * SR), 60), off((int)(2.0 * SR), 64), on((int)(2.2 * SR), 72), off((int)(2.6 * SR), 72)}, &L, &R);
            const double r = rms(L, 0, (size_t)(2.6 * SR)); const float pk = std::fmax(peak(L), peak(R)); const double cr = std::fmax(localClickRatio(L), localClickRatio(R));
            const bool crushed = p.getN("Grit") >= kGritHold;
            (void)r; if (pk > 1e-3f && pk <= 0.95f && (crushed || cr < kClickLimit)) continue;   // a bit-crusher's stairs are the effect
            ++bad;
            {   // keep the whole state of a failing roll, for replay
                void* ptr = nullptr; const VstIntPtr len = p.call(effGetChunk, 0, 0, &ptr);
                char fn[64]; std::snprintf(fn, sizeof fn, "/roll_%d_%d.chunk", cat, k);
                if (FILE* fp = std::fopen((std::string(argv[2]) + fn).c_str(), "wb")) { std::fwrite(ptr, 1, (size_t)len, fp); std::fclose(fp); }
            }
            std::printf("roll %d: %.0f dBFS rms, peak %.1f dBFS, click ratio %.1f\n  ", k, dB(r), dB(pk), cr);
            if (cr >= kClickLimit)
            {
                const double cl = localClickRatio(L) >= localClickRatio(R) ? 0 : 1; const std::vector<float>& v = cl == 0 ? L : R; localClickRatio(v);
                const int at = g_clickAt;
                std::printf("worst step at %.4f s (%s), samples:", at / (double)SR, cl == 0 ? "L" : "R");
                for (int q = std::max(0, at - 6); q < std::min((int)v.size(), at + 6); ++q) std::printf(" %+.4f", v[(size_t)q]);
                std::printf("\n  ");
            }
            for (const char* n : {"Blend", "Routing", "Drive", "Grit", "Ring", "A1 Wave", "A2 Wave", "A Filter", "A Cutoff", "A Reso", "A Env Amt", "A Attack", "A Decay", "A Sustain",
                                  "B1 Wave", "B Filter", "B Cutoff", "B Reso", "B Env Amt", "B Attack", "LFO1 To", "LFO1 Depth", "LFO2 To", "LFO2 Depth", "A Fold", "A Bend", "A Sync", "A Start From"})
                std::printf("%s=%s; ", n, p.text(n).c_str());
            std::printf("\n");
        }
        std::printf("%d of %d rolls failed\n", bad, tries);
        return 0;
    }    if (argc > 4 && std::string(argv[4]) == "levels")
    {   // debug: the phrase level of the factory presets named on the command line (program numbers)
        for (int k = 5; k < argc; ++k)
        {
            Plug p; p.open(std::atoi(argv[k])); std::vector<float> L;
            p.render(4.0, {on(0, 48), off((int)(0.9 * SR), 48), on((int)(1.0 * SR), 55), on((int)(1.0 * SR), 60), on((int)(1.0 * SR), 64), off((int)(2.0 * SR), 55), off((int)(2.0 * SR), 60), off((int)(2.0 * SR), 64), on((int)(2.2 * SR), 72), off((int)(2.6 * SR), 72)}, &L);
            std::printf("%-22s %.1f dBFS\n", p.progName(std::atoi(argv[k])).c_str(), dB(rms(L, 0, (size_t)(2.6 * SR))));
            p.close();
        }
        return 0;
    }
    if (argc > 4 && std::string(argv[4]) == "xy")
    {   // debug: the XY test's setup at X = 0
        const int pre = std::getenv("PRE") ? std::atoi(std::getenv("PRE")) : 0;
        for (int k = 0; k < pre; ++k) { Plug q; q.open(); richPatch(q); q.render(0.3, {on(0, 48), off(SR / 10, 48)}); q.close(); }
        Plug p; p.open();
        p.set("A1 Wave", 0.0f); p.set("A1 Level", 0.8f); p.set("B1 Wave", 0.0f); p.set("B1 Level", 0.8f); p.set("B1 Octave", 4.0f / 6.0f);
        p.set("A Cutoff", 1.0f); p.set("B Cutoff", 1.0f); p.set("Blend", 0.0f); p.set("Pad Y", 0.5f);
        if (argc > 5) { p.set("A Env Amt", 0.5f); p.set("B Env Amt", 0.5f); }
        std::vector<float> L; p.render(1.0, {on(0, 57)}, &L);
        std::printf("220: %.1f dB  440: %.1f dB  rms %.1f\n", dB(tone(L, 220, SR / 2, SR)), dB(tone(L, 440, SR / 2, SR)), dB(rms(L, SR / 2, SR)));
        for (const char* n : {"A1 Wave", "A1 Level", "B1 Wave", "B1 Octave", "Blend", "A Cutoff", "A2 Level", "A3 Level", "Voices Now"}) std::printf("  %s = %s (idx %d)\n", n, p.text(n).c_str(), p.find(n));
        p.close(); return 0;
    }
    if (argc > 4 && std::string(argv[4]) == "env")
    {   // debug: level every 100 ms of one note, Decay 1 ms, Sustain 0
        Plug p; p.open(); if (argc > 5) { richPatch(p); p.set("Blend", 0.0f); } p.set("A Decay", 0.0f); p.set("A Sustain", 0.0f);
        std::vector<float> L; p.render(1.0, {on(0, 60)}, &L);
        for (int k = 0; k < 10; ++k) std::printf("%.1f s: %.1f dBFS\n", k * 0.1, dB(rms(L, (size_t)(k * 0.1 * SR), (size_t)((k + 1) * 0.1 * SR))));
        std::printf("A Decay %s, A Sustain %s, A Attack %s, voices %s\n", p.text("A Decay").c_str(), p.text("A Sustain").c_str(), p.text("A Attack").c_str(), p.text("Voices Now").c_str());
        p.close(); return 0;
    }
    if (argc > 4 && std::string(argv[4]) == "dump") { for (int k = 5; k < argc; ++k) { Plug p; p.open(std::atoi(argv[k])); std::printf("== %s\n", p.progName(std::atoi(argv[k])).c_str()); for (int i = 0; i < p.find("Category"); ++i) { char nm[64] = {0}; p.call(effGetParamName, i, 0, nm); std::printf("%s=%s; ", nm, p.text(i).c_str()); } std::printf("\n"); p.close(); } return 0; }
    char d[600];

    // ---- 1. every note plays ----------------------------------------------------------------------
    {
        Plug p; p.open();
        std::string silent;
        for (int note = 0; note < 128; ++note)
        {
            std::vector<float> L; p.render(0.25, {on(0, note), off((int)(0.2 * SR), note)}, &L);
            if (rms(L, 0, L.size()) < 1e-4) silent += std::to_string(note) + " ";
            p.render(0.6, {});
        }
        p.close();
        check(silent.empty(), "notes: all 128 MIDI notes make sound (Init)", silent.empty() ? "0..127 all sound" : "silent: " + silent);
    }
    // ---- 2. no stuck notes ----------------------------------------------------------------------------
    {
        Plug p; p.open();
        p.set("A Release", std::log(300.0f) / std::log(20000.0f));
        std::vector<Ev> ev; uint32_t s = 9;
        for (int k = 0; k < 400; ++k)
        {   // overlapping random notes, repeated notes, chords, sustain pedal on / off
            s = s * 1664525u + 1013904223u; const int note = 36 + (int)((s >> 10) % 48), at = (int)((s >> 4) % (int)(6 * SR));
            const int len = 200 + (int)((s >> 16) % 20000);
            ev.push_back(on(at, note, 30 + (int)((s >> 20) % 97))); ev.push_back(off(at + len, note));
        }
        ev.push_back({(int)(2 * SR), 0xB0, 64, 127}); ev.push_back({(int)(4 * SR), 0xB0, 64, 0});
        std::vector<float> L; p.render(8.0, ev, &L);
        const std::string vn = p.text("Voices Now");
        const double tail = rms(L, (size_t)(7.5 * SR), L.size());
        // the same with very long releases: everything still ends (20 s release, then all-notes-off)
        p.set("A Release", 1.0f);
        p.render(1.0, {on(0, 60), on(0, 64), on(0, 67), off(SR / 2, 60), off(SR / 2, 64), off(SR / 2, 67)});
        std::vector<float> L2; p.render(1.0, {{0, 0xB0, 120, 0}}, &L2);
        const double afterOff = rms(L2, SR / 2, L2.size());
        p.close();
        std::snprintf(d, sizeof d, "400 random overlapping notes + sustain pedal: after the last release %s voices, tail %.0f dBFS; 20 s release chord + all-sound-off: %.0f dBFS", vn.c_str(), dB(tail), dB(afterOff));
        check(vn.rfind("0 /", 0) == 0 && tail < 1e-4 && afterOff < 1e-5, "notes: every voice stops on release, no stuck notes", d);
    }
    // ---- 3. a MIDI device connecting (burst of CCs, all-notes-off, pitch bend, program change) --------
    {
        Plug p; p.open();
        std::vector<Ev> burst;
        for (int c = 0; c < 128; ++c) burst.push_back({c, 0xB0, (unsigned char)c, (unsigned char)(c * 7 % 128)});
        burst.push_back({200, 0xE0, 0, 127}); burst.push_back({201, 0xC0, 5, 0}); burst.push_back({202, 0xD0, 90, 0}); burst.push_back({203, 0xB0, 64, 0});
        p.render(0.2, burst);
        std::vector<float> L; p.render(0.5, {on(0, 60), off((int)(0.4 * SR), 60), {0, 0xE0, 0, 64}}, &L);
        p.close();
        check(rms(L, 0, L.size()) > 1e-3, "MIDI: a device's connect burst (all CCs, bend, program change) does not silence notes", "note after the burst: " + std::to_string((int)dB(rms(L, 0, L.size()))) + " dBFS");
    }
    // ---- 4. every selector takes every value ------------------------------------------------------------
    {
        Plug p; p.open();
        std::string bad; int count = 0;
        for (int i = 0; i < p.e->numParams; ++i)
        {
            char nm[256] = {0}; p.call(effGetParamName, i, 0, nm);
            const std::string name = nm;
            if (name.rfind("Key ", 0) == 0 || name == "Help" || name == "Help 2" || name == "Preset") continue;
            // action buttons act on every change: pressing Save / Confirm here would really save a preset
            static const char* const kActions[] = {"Randomize", "Undo", "Load", "Save", "Confirm", "Cancel", "Backspace", "Left", "Right", "Clear",
                                                   "A Copy Start", "A Clear Draw", "A Undo Draw", "B Copy Start", "B Clear Draw", "B Undo Draw"};
            bool action = false; for (const char* a : kActions) action = action || name == a;
            if (action) continue;
            // choices show distinct texts at their steps
            p.setN(i, 0.0f); p.run(0.02); const std::string t0 = p.text(i); p.setN(i, 1.0f); p.run(0.02); const std::string t1 = p.text(i); p.setN(i, 0.0f); p.run(0.02);
            if (p.call(effCanBeAutomated, i) && t0 == t1 && name != "CPU" && name != "Voices Now" && name != "Status" && name != "Name") bad += name + ", ";
            ++count;
        }
        p.close();
        std::remove((std::string(argv[2]) + "/user-presets.bin").c_str());   // nothing from this test may leak into the next
        check(bad.empty(), "controls: every parameter shows a different value at its two ends (" + std::to_string(count) + " checked)", bad.empty() ? "" : "no change: " + bad);
    }
    // ---- 5. every sound control changes the sound (pass / fail per control) ---------------------------------
    {
        Plug p0; p0.open(); const int nSound = p0.find("Category"); p0.close();
        std::vector<std::string> failed; int passed = 0;
        auto renderWith = [&](int idx, float v, const std::string& name) {
            Plug p; p.open(); richPatch(p);
            // context a control needs before it can matter
            const bool isA = name[0] == 'A', isB = name[0] == 'B';
            if (name.find("PW") != std::string::npos) { char w[16]; std::snprintf(w, sizeof w, "%c%c Wave", name[0], name[1]); p.set(w, 3.0f / 5.0f); }
            if (isA || isB) p.set("Blend", isA ? 0.0f : 1.0f);
            if (name == "Pad Y To") p.set("Pad Y", 0.9f);
            if (name.find("LFO1") == 0 && name != "LFO1 Depth") p.set("LFO1 Depth", 0.8f);
            if (name.find("LFO2") == 0 && name != "LFO2 Depth") p.set("LFO2 Depth", 0.8f);
            if (name.find("Detune") != std::string::npos || name.find("Spread") != std::string::npos) { char u[16]; std::snprintf(u, sizeof u, "%c Unison", name[0]); p.set(u, 1.0f); }
            if (name.find("Key Trk") != std::string::npos) {}
            if (name.find("Filter") != std::string::npos || name.find("F.") != std::string::npos) { char r[16]; std::snprintf(r, sizeof r, "%c Env Amt", name[0]); p.set(r, 0.9f); }
            p.setN(idx, v);
            std::vector<float> L, R;
            p.render(1.4, {on(0, 48), on(0, 55), on(0, 64), on(100, 72), off((int)(0.8 * SR), 48), off((int)(0.8 * SR), 55), off((int)(0.8 * SR), 64), off((int)(0.8 * SR), 72)}, &L, &R);
            p.close();
            std::vector<float> both(L); both.insert(both.end(), R.begin(), R.end());
            return both;
        };
        Plug names; names.open();
        for (int i = 0; i < nSound; ++i)
        {
            char nm[256] = {0}; names.call(effGetParamName, i, 0, nm);
            const std::string name = nm;
            if (name == "Skin") { continue; }
            // extremes, but for the lowest of a log range use a small step in from the end
            const std::vector<float> lo = renderWith(i, 0.0f, name), hi = renderWith(i, name == "Voices" ? 0.0f + 0.0f : 1.0f, name);
            std::vector<float> lo2 = lo; const std::vector<float>* hip = &hi;
            std::vector<float> alt;
            if (name == "Voices")
            {   // 1 voice vs 8: a 4-note chord must sound different
                alt = renderWith(i, 1.0f, name); hip = &alt;
            }
            double diff = 0, ref = 0;
            for (size_t k = 0; k < lo2.size(); ++k) { diff += std::fabs(lo2[k] - (*hip)[k]); ref += std::fabs(lo2[k]) + std::fabs((*hip)[k]); }
            const double rel = diff / (ref + 1e-12);
            const bool ok = rel > 0.002;
            char line[160]; std::snprintf(line, sizeof line, "%-14s %s (change %.1f %%)", name.c_str(), ok ? "PASS" : "FAIL", 100 * rel);
            g_controlList.push_back(line);
            if (ok) ++passed; else failed.push_back(name);
        }
        names.close();
        std::string f; for (const auto& s : failed) f += s + ", ";
        check(failed.empty(), "controls: every sound parameter audibly changes the output (" + std::to_string(passed) + " of " + std::to_string(passed + (int)failed.size()) + ")", failed.empty() ? "" : "no effect: " + f);
    }
    // ---- 6. XY pad: X blends A and B, Y moves the target ------------------------------------------------------------
    {
        auto run = [&](float x, float y, std::vector<float>& L) {
            Plug p; p.open();
            p.set("A1 Wave", 0.0f); p.set("A1 Level", 0.8f);                    // A: sine at the note
            p.set("B1 Wave", 0.0f); p.set("B1 Level", 0.8f); p.set("B1 Octave", 4.0f / 6.0f);   // B: sine an octave up
            p.set("A Cutoff", 1.0f); p.set("B Cutoff", 1.0f); p.set("A Env Amt", 0.5f); p.set("B Env Amt", 0.5f);
            p.set("Blend", x); p.set("Pad Y", y);
            p.render(1.0, {on(0, 57)}, &L); p.close();
        };
        std::vector<float> a, m, b, yLo, yHi;
        run(0.0f, 0.5f, a); run(0.5f, 0.5f, m); run(1.0f, 0.5f, b);
        const size_t s0 = SR / 2, s1 = SR;
        auto ratio = [&](const std::vector<float>& v) { return dB(tone(v, 440, s0, s1) / (tone(v, 220, s0, s1) + 1e-12)); };
        // Y on the cutoff (saw through a low-pass): brighter at the top
        auto runY = [&](float y, std::vector<float>& L) { Plug p; p.open(); p.set("A Cutoff", 0.4f); p.set("Pad Y", y); p.render(1.0, {on(0, 45)}, &L); p.close(); };
        runY(0.1f, yLo); runY(0.9f, yHi);
        const double brightLo = tone(yLo, 110 * 8, s0, s1), brightHi = tone(yHi, 110 * 8, s0, s1);
        std::snprintf(d, sizeof d, "B's octave over A's note: X=0 %.0f dB, X=0.5 %.0f dB, X=1 %+.0f dB; Y=0.1 vs 0.9 (cutoff): 8th harmonic %.0f -> %.0f dBFS",
                      ratio(a), ratio(m), ratio(b), dB(brightLo), dB(brightHi));
        check(ratio(a) < -40 && std::fabs(ratio(m)) < 6 && ratio(b) > 40 && brightHi > brightLo * 4, "XY pad: X blends A and B; Y moves the cutoff", d);
    }
    // ---- 7. randomize: moods, levels kept, locks, skin, undo --------------------------------------------------------
    {
        Plug p; p.open();
        p.set("Volume", 0.3f); p.set("Output", 0.6f);
        auto snap = [&]() { std::vector<float> v(p.e->numParams); for (int i = 0; i < p.e->numParams; ++i) v[i] = p.e->getParameter(p.e, i); return v; };
        const int nSound = p.find("Category");
        bool levelsKept = true, catsDiffer = true, allChanged = true; std::string per;
        std::vector<float> prevRelease;
        for (int c = 0; c < 7; ++c)
        {
            p.set("Category", c / 6.0f);
            const std::vector<float> before = snap();
            p.tap("Randomize"); p.run(0.1);
            const std::vector<float> after = snap();
            int changed = 0; for (int i = 0; i < nSound; ++i) changed += after[i] != before[i];
            levelsKept = levelsKept && after[p.find("Volume")] == before[p.find("Volume")] && after[p.find("Output")] == before[p.find("Output")];
            allChanged = allChanged && changed > 30;
            per += p.text("Status") + "; ";
        }
        // mood character: Ambient's releases long, Keys' attacks short
        p.set("Category", 0.0f); p.tap("Randomize"); p.run(0.1);
        const std::string ambRel = p.text("A Release"), ambAtt = p.text("A Attack");
        p.set("Category", 2.0f / 6.0f); p.tap("Randomize"); p.run(0.1);
        const std::string keyAtt = p.text("A Attack");
        // locks: lock A and the skin, randomize: A's oscillators and the skin stay
        p.set("Lock A", 1.0f); p.set("Lock Skin", 1.0f);
        const std::vector<float> b2 = snap();
        p.tap("Randomize"); p.run(0.1);
        const std::vector<float> a2 = snap();
        bool lockOk = a2[p.find("A1 Wave")] == b2[p.find("A1 Wave")] && a2[p.find("A2 Level")] == b2[p.find("A2 Level")] && a2[p.find("A Unison")] == b2[p.find("A Unison")] && a2[p.find("Skin")] == b2[p.find("Skin")];
        const bool bMoved = a2[p.find("B1 Level")] != b2[p.find("B1 Level")] || a2[p.find("B Cutoff")] != b2[p.find("B Cutoff")];
        // the skin changes when not locked (over several tries)
        p.set("Lock Skin", 0.0f); p.set("Lock A", 0.0f);
        std::set<int> skins; for (int k = 0; k < 14; ++k) { p.set("Category", (k % 7) / 6.0f); p.tap("Randomize"); p.run(0.05); skins.insert((int)std::lround(p.getN("Skin") * 3)); }
        // undo restores everything exactly, including the skin
        const std::vector<float> b3 = snap(); p.tap("Randomize"); p.run(0.1); p.tap("Undo"); p.run(0.1);
        const std::vector<float> u3 = snap();
        int undoDiff = 0; for (int i = 0; i < nSound; ++i) undoDiff += u3[i] != b3[i];
        p.close();
        std::snprintf(d, sizeof d, "Ambient: attack %s, release %s; Keys: attack %s; skins seen %zu; status: %s", ambAtt.c_str(), ambRel.c_str(), keyAtt.c_str(), skins.size(), per.substr(0, 120).c_str());
        check(levelsKept && allChanged, "randomize: every mood rewrites the sound; Volume and Output never change", d);
        check(lockOk && bMoved, "randomize: Lock A and Lock Skin keep their sections, the rest moves");
        check(skins.size() >= 2, "randomize: picks skins (unless locked)", std::to_string(skins.size()) + " different skins in 14 randomizes (2 per mood)");
        check(undoDiff == 0, "undo: restores every sound setting and the skin exactly", std::to_string(undoDiff) + " differ");
        const double ambS = std::atof(ambRel.c_str()) * (ambRel.find(" s") != std::string::npos ? 1000 : 1), keyA = std::atof(keyAtt.c_str());
        check(ambS >= 5000 && keyA <= 10.0, "randomize: moods have their character (Ambient release >= 5 s, Keys attack <= 10 ms)", d);
    }
    // ---- 8. save: name with the letter keys, category, listed, reloads after a restart; browser ------------------
    {
        Plug p; p.open();
        p.set("Category", 4.0f / 6.0f); p.tap("Randomize"); p.run(0.1);              // last randomize: Lo-Fi
        const float cut = p.getN("A Cutoff");
        p.set("Skin", 2.0f / 3.0f);                                                   // saved with the preset: Monolith
        p.tap("Save"); p.run(0.1);
        const std::string draft = p.text("Name"), cat = p.text("Save In");
        p.tap("Clear");
        for (const char* k : {"Key M", "Key O", "Key T", "Key H", "Key Space", "Key 7"}) p.tap(k);
        p.tap("Backspace"); p.tap("Key 9");
        const std::string typed = p.text("Name");
        p.tap("Confirm"); p.run(0.05); std::this_thread::sleep_for(std::chrono::milliseconds(300)); p.run(0.3);
        const std::string status = p.text("Status");
        const int user0 = 4 * 12 + 4;                                                     // Lo-Fi, first user slot
        const std::string listed = p.progName(user0);
        p.close();
        Plug q; q.open();                                                               // "restart"
        const std::string again = q.progName(user0);
        q.call(effSetProgram, 0, user0); q.run(0.05);
        const float cut2 = q.getN("A Cutoff");
        // the browser: category Lo-Fi, slot 5 shows it, Load selects it
        q.set("Browse", 4.0f / 6.0f); q.set("Browse Slot", 4.0f / 11.0f); q.run(0.05);
        const std::string shown = q.text("Browse Slot");
        q.call(effSetProgram, 0, 0); q.run(0.05); q.set("Skin", 0.0f); q.tap("Load"); q.run(0.1);
        const float cut3 = q.getN("A Cutoff");
        const std::string skinBack = q.text("Skin");
        q.call(effSetProgram, 0, 8); q.run(0.05);
        const std::string skinFactory = q.text("Skin");      // a factory preset / empty slot leaves the skin alone
        q.close();
        std::snprintf(d, sizeof d, "draft '%s' in '%s'; typed '%s'; status '%s'; listed '%s'; after restart '%s' (cutoff %.3f vs %.3f); browser shows '%s', Load -> %.3f",
                      draft.c_str(), cat.c_str(), typed.c_str(), status.c_str(), listed.c_str(), again.c_str(), cut2, cut, shown.c_str(), cut3);
        check(skinBack == "Monolith" && skinFactory == "Monolith", "save: a saved preset brings its skin back; other presets leave the skin alone", "after Load: " + skinBack + ", then another preset: " + skinFactory);
        check(cat == "Lo-Fi Psych" && typed == "MOTH 9|" && listed == "LOFI MOTH 9" && again == "LOFI MOTH 9" && std::fabs(cut2 - cut) < 1e-5f && shown == "MOTH 9" && std::fabs(cut3 - cut) < 1e-5f,
              "save: letter-grid name, category from the last randomize, listed in its category, kept after a restart, browser loads it", d);
    }
    // ---- 9. levels are global: presets never change Volume / Output --------------------------------------------
    {
        Plug p; p.open(); p.set("Volume", 0.31f); p.set("Output", 0.62f);
        for (int prog : {5, 17, 40, 70}) { p.call(effSetProgram, 0, prog); p.run(0.05); }
        const bool ok = std::fabs(p.getN("Volume") - 0.31f) < 1e-6f && std::fabs(p.getN("Output") - 0.62f) < 1e-6f;
        p.close();
        check(ok, "levels: Volume and Output never change on preset load");
    }
    // ---- 10. CPU meter and voice readout -------------------------------------------------------------------------
    {
        Plug p; p.open(); richPatch(p);
        std::vector<Ev> ev; for (int k = 0; k < 8; ++k) ev.push_back(on(0, 48 + k * 3));
        const auto t0 = std::chrono::steady_clock::now();
        p.render(3.0, ev);
        const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 3.0 * 100.0;
        const std::string cpu = p.text("CPU"), vn = p.text("Voices Now");
        p.close();
        std::snprintf(d, sizeof d, "8 notes, rich patch: meter %s, measured %.2f %%; voices %s", cpu.c_str(), real, vn.c_str());
        const double m = std::atof(cpu.c_str());
        check(m > 0.05 && m < real * 1.5 + 0.2 && vn.find("/") != std::string::npos, "CPU meter reads Elver's own load; voice readout", d);
    }
    // ---- 11. CPU: per patch, 8 held notes (desktop; Force est. x15..25) --------------------------------------------
    {
        struct Case { const char* name; int uni; int oscs; float blend; };
        std::printf("CPU, desktop, 8 notes held (voice budget applies):\n");
        for (int notes : {0, 1})
        {
            Plug p; p.open(); std::vector<Ev> ev; for (int k = 0; k < notes; ++k) ev.push_back(on(0, 60));
            p.render(0.5, ev); const auto t0 = std::chrono::steady_clock::now(); p.render(3.0, {});
            std::printf("  %-50s %5.2f %%\n", notes ? "Init, 1 note" : "no notes", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 3.0 * 100.0);
            p.close();
        }
        for (const Case& c : {Case{"Init (1 saw, one side)", 1, 1, 0.0f}, Case{"2 + 2 oscillators, both sides", 1, 2, 0.5f},
                              Case{"3 + 3 oscillators, unison 2", 2, 3, 0.5f}, Case{"3 + 3 oscillators, unison 4 (budget: 2 voices)", 4, 3, 0.5f}})
        {
            Plug p; p.open(); p.set("Blend", c.blend);
            for (const char* s : {"A", "B"}) { for (int o = 1; o <= 3; ++o) { char n[16]; std::snprintf(n, sizeof n, "%s%d Level", s, o); p.set(n, o <= c.oscs ? 0.6f : 0.0f); }
                char n[16]; std::snprintf(n, sizeof n, "%s Unison", s); p.set(n, (c.uni - 1) / 3.0f); }
            std::vector<Ev> ev; for (int k = 0; k < 8; ++k) ev.push_back(on(0, 48 + k * 3));
            p.render(0.5, ev);
            const auto t0 = std::chrono::steady_clock::now(); p.render(3.0, {});
            const double pct = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 3.0 * 100.0;
            std::printf("  %-50s %5.2f %%  (Force est. %2.0f..%2.0f %%)  voices %s\n", c.name, pct, pct * 15, pct * 25, p.text("Voices Now").c_str());
            p.close();
        }
    }
    // ---- 12. renders: each waveform, each filter type, each mood; silence / clicks / runaway ----------------------
    {
        std::string bad; int n = 0;
        auto phrase = [&](Plug& p, std::vector<float>& L, std::vector<float>& R) {
            p.render(4.0, {on(0, 48), off((int)(0.9 * SR), 48), on((int)(1.0 * SR), 55), on((int)(1.0 * SR), 60), on((int)(1.0 * SR), 64),
                           off((int)(2.0 * SR), 55), off((int)(2.0 * SR), 60), off((int)(2.0 * SR), 64), on((int)(2.2 * SR), 72), off((int)(2.6 * SR), 72)}, &L, &R);
        };
        auto judge = [&](const std::string& name, const std::vector<float>& L, const std::vector<float>& R, bool crushed) {
            ++n;
            const double r = rms(L, 0, (size_t)(2.6 * SR)); const float pk = std::fmax(peak(L), peak(R));
            const double cr = std::fmax(localClickRatio(L), localClickRatio(R));
            const bool ok = pk > 1e-3f && pk <= 0.95f && (crushed || cr < kClickLimit);   // sounds (peak > -60 dBFS), no runaway, no clicks (bit-crush stairs are the effect)
            char line[160]; std::snprintf(line, sizeof line, "%s: %.0f dBFS rms, peak %.1f dBFS, click ratio %.1f%s", name.c_str(), dB(r), dB(pk), cr, ok ? (crushed ? " (bit-crushed)" : "") : "  <-- FAIL");
            std::printf("      %s\n", line);
            if (!ok) bad += name + ", ";
            if (!renders.empty()) wav(renders + "/" + name + ".wav", L, R);
        };
        const char* waves[5] = {"sine", "triangle", "saw", "square", "noise"};
        for (int w = 0; w < 5; ++w) { Plug p; p.open(); p.set("A1 Wave", w / 5.0f); p.set("A Cutoff", 1.0f); p.set("A Env Amt", 0.5f); std::vector<float> L, R; phrase(p, L, R); p.close(); judge(std::string("wave_") + waves[w], L, R, false); }
        const char* filters[5] = {"lp24", "lp12", "bp", "hp", "notch"};
        for (int f = 0; f < 5; ++f) { Plug p; p.open(); p.set("A Filter", f / 4.0f); p.set("A Reso", 0.5f); std::vector<float> L, R; phrase(p, L, R); p.close(); judge(std::string("filter_") + filters[f], L, R, false); }
        const char* moods[7] = {"ambient", "pads_winds", "keys_plucks", "leads_basses", "lofi_psych", "industrial", "noise_chaos"};
        for (int c = 0; c < 7; ++c)
            for (int k = 0; k < 3; ++k)
            {
                Plug p; p.open(); p.set("Category", c / 6.0f); for (int j = 0; j <= k; ++j) p.tap("Randomize"); p.run(0.02); std::this_thread::sleep_for(std::chrono::milliseconds(60)); p.run(0.05);   // the new shapes are built
                const bool cr = p.getN("Grit") >= kGritHold;
                std::vector<float> L, R; phrase(p, L, R); p.close();
                judge(std::string("mood_") + moods[c] + "_" + std::to_string(k + 1), L, R, cr);
            }
        for (int prog = 0; prog < 84; prog += 12)
            for (int s = 0; s < 4; ++s) { Plug p; p.open(prog + s); const bool cr = p.getN("Grit") >= kGritHold; std::vector<float> L, R; phrase(p, L, R); const std::string pname = p.progName(prog + s); judge("preset_" + pname, L, R, cr); p.close();
                const double lv = dB(rms(L, 0, (size_t)(2.6 * SR))); if (lv < -36 || lv > -15) { char lb[96]; std::snprintf(lb, sizeof lb, "level of %s (%.1f dBFS), ", pname.c_str(), lv); bad += lb; } }
        check(bad.empty(), "renders: every waveform, filter type, mood and factory preset sounds, no clicks, no runaway (" + std::to_string(n) + " files)", bad.empty() ? "" : "problems: " + bad);
    }
    // ---- 13. hostile host: after each event a fresh note must sound, every sample finite ------------------
    {
        struct Act { const char* name; int id; };
        const Act acts[] = {{"suspend / resume", 0}, {"sample rate 48 kHz", 1}, {"sample rate 0 (invalid)", 2}, {"block size 512", 3},
                            {"block size 0 (invalid)", 4}, {"NaN to every parameter", 5}, {"+inf to every parameter", 6},
                            {"state chunk save / reload", 7}, {"corrupted state chunk", 8}, {"zero-frame and 1-frame calls", 9},
                            {"3000 random MIDI events in one block", 10}, {"every program selected while playing", 11},
                            {"NaN samples in the output buffers", 12}};
        std::string bad;
        for (const Act& a : acts)
        {
            Plug p; p.open();
            p.render(0.3, {on(0, 60)});
            switch (a.id)
            {
                case 0: p.call(effStopProcess); p.call(effMainsChanged, 0, 0); p.call(effMainsChanged, 0, 1); p.call(effStartProcess); break;
                case 1: p.call(effMainsChanged, 0, 0); p.call(effSetSampleRate, 0, 0, nullptr, 48000.0f); p.call(effMainsChanged, 0, 1); break;
                case 2: p.call(effMainsChanged, 0, 0); p.call(effSetSampleRate, 0, 0, nullptr, 0.0f); p.call(effMainsChanged, 0, 1); break;
                case 3: p.call(effMainsChanged, 0, 0); p.call(effSetBlockSize, 0, 512); p.call(effMainsChanged, 0, 1); break;
                case 4: p.call(effMainsChanged, 0, 0); p.call(effSetBlockSize, 0, 0); p.call(effMainsChanged, 0, 1); break;
                case 5: case 6: for (int i = 0; i < p.e->numParams; ++i) p.setN(i, a.id == 5 ? NAN : INFINITY); break;
                case 7: case 8:
                {
                    void* ptr = nullptr; const VstIntPtr len = p.call(effGetChunk, 0, 0, &ptr);
                    std::vector<char> c((char*)ptr, (char*)ptr + len);
                    if (a.id == 8) { uint32_t s = 5; for (size_t k = 12; k < c.size(); k += 7) { s = s * 1664525u + 1013904223u; c[k] = (char)(s >> 24); } }
                    p.call(effSetChunk, 0, (VstIntPtr)c.size(), c.data());
                    break;
                }
                case 9: { float l[4] = {}, r[4] = {}; float* io[2] = {l, r}; for (int k = 0; k < 200; ++k) { p.e->processReplacing(p.e, io, io, k % 2); } break; }
                case 10:
                {
                    std::vector<Ev> ev; uint32_t s = 77;
                    for (int k = 0; k < 3000; ++k) { s = s * 1664525u + 1013904223u; ev.push_back({(int)(k % 128), (unsigned char)(0x80 | ((s >> 24) & 0x7F)), (unsigned char)((s >> 8) & 0x7F), (unsigned char)((s >> 16) & 0x7F)}); }
                    p.render(0.01, ev);
                    break;
                }
                case 11: for (int k = 0; k < 84; ++k) { p.call(effSetProgram, 0, k); p.run(0.003); } p.call(effSetProgram, 0, 4); break;
                case 12: { float l[128], r[128]; for (int i = 0; i < 128; ++i) l[i] = r[i] = NAN; float* io[2] = {l, r}; p.e->processReplacing(p.e, io, io, 128); break; }
            }
            std::vector<float> L, R;
            p.render(0.8, {{0, 0xB0, 123, 0}, on(10, 64), off((int)(0.6 * SR), 64)}, &L, &R);
            bool finite = true; for (size_t i = 0; i < L.size(); ++i) finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
            const double lv = dB(rms(L, 0, L.size()));
            std::printf("      %-40s next note %4.0f dBFS%s\n", a.name, lv, finite ? "" : ", NON-FINITE");
            if (!finite || lv < -60 || peak(L) > 0.99f) bad += std::string(a.name) + ", ";
            p.close();
        }
        check(bad.empty(), "hostile host: suspend/resume, rate/block changes, NaN/inf parameters, bad chunks, odd calls, MIDI floods - the next note always plays", bad.empty() ? "" : "problems: " + bad);
    }
    // ---- 14. Shape: library, controls, aliasing, determinism, live changes, drawing, saving -------------------
    {
        auto settle = [](Plug& p) { p.run(0.02); std::this_thread::sleep_for(std::chrono::milliseconds(60)); p.run(0.06); };
        auto shapePatch = [&](Plug& p) { p.set("A1 Wave", 1.0f); p.set("A Cutoff", 1.0f); p.set("A Env Amt", 0.5f); p.set("A Sustain", 1.0f); settle(p); };
        auto pt = [](Plug& p, char side, int k) { char n[16]; std::snprintf(n, sizeof n, "%c Pt %02d", side, k + 1); return p.getN(n) * 2.0f - 1.0f; };
        const int libN = 60;
        // a) every library wave
        std::string bad; int n = 0;
        for (int w = 0; w < libN; ++w)
        {
            Plug p; p.open(); p.set("A Start From", w / (float)(libN - 1)); shapePatch(p);
            std::vector<float> L, R;
            p.render(1.2, {on(0, 48), off((int)(0.5 * SR), 48), on((int)(0.6 * SR), 67), off((int)(1.1 * SR), 67)}, &L, &R);
            const std::string nm = p.text("A Start From");
            p.close(); ++n;
            const double r = rms(L, 0, L.size()); const float pk = std::fmax(peak(L), peak(R)); const double cr = std::fmax(localClickRatio(L), localClickRatio(R));
            if (r < 1e-3 || pk > 0.95f || cr > kClickLimit) { char b[96]; std::snprintf(b, sizeof b, "%s (%.0f dB, peak %.1f, click %.1f), ", nm.c_str(), dB(r), dB(pk), cr); bad += b; }
            if (!renders.empty() && w % 6 == 0) wav(renders + "/shape_" + std::to_string(w) + ".wav", L, R);
        }
        check(bad.empty(), "Shape: all " + std::to_string(n) + " library waves play (no silence, clipping or clicks)", bad);

        // b) aliasing: note 95 (1975.5 Hz) - probe exactly where aliases of the harmonics above Nyquist would land
        auto aliasDb = [&](float start, float fold) {
            Plug p; p.open(); p.set("A Start From", start); p.set("A Fold", fold); shapePatch(p);
            std::vector<float> L; p.render(1.0, {on(0, 95)}, &L); p.close();
            const double f0 = 440.0 * std::pow(2.0, (95 - 69) / 12.0), fund = tone(L, f0, SR / 2, SR);
            double worst = 0;
            for (int m = 2; m * f0 < 3 * SR; ++m)
            {
                if (m * f0 < 0.5 * SR) continue;
                double fa = std::fmod(m * f0, (double)SR); if (fa > 0.5 * SR) fa = SR - fa;
                bool nearHarm = false; for (int h = 1; h * f0 < 0.5 * SR; ++h) nearHarm = nearHarm || std::fabs(fa - h * f0) < 40.0;
                if (!nearHarm) worst = std::fmax(worst, tone(L, fa, SR / 2, SR));
            }
            return dB(worst / (fund + 1e-12));
        };
        const double aSaw = aliasDb(2 / 59.0f, 0.0f), aFold = aliasDb(2 / 59.0f, 1.0f);
        char d2[300]; std::snprintf(d2, sizeof d2, "note 95, strongest alias vs fundamental: library saw %.0f dB, saw folded 100 %% %.0f dB", aSaw, aFold);
        check(aSaw < -60 && aFold < -60, "Shape: band-limited, no audible aliasing on high notes", d2);

        // c) every shaper control changes the sound; d) Seed is deterministic
        auto renderShape = [&](const std::vector<std::pair<const char*, float>>& sets) {
            Plug p; p.open();
            for (auto& s : sets) p.set(s.first, s.second);
            shapePatch(p);
            std::vector<float> L; p.render(0.6, {on(0, 48)}, &L); p.close(); return L;
        };
        auto diffRel = [](const std::vector<float>& a, const std::vector<float>& b) { double d = 0, r = 0; for (size_t k = 0; k < a.size(); ++k) { d += std::fabs(a[k] - b[k]); r += std::fabs(a[k]) + std::fabs(b[k]); } return d / (r + 1e-12); };
        std::string ctl; bool ctlOk = true;
        const std::vector<std::pair<const char*, std::pair<float, float>>> controls = {
            {"A Start From", {0.0f, 1.0f}}, {"A Bright", {0.0f, 1.0f}}, {"A Odd/Even", {0.0f, 1.0f}}, {"A Formant", {0.0f, 1.0f}},
            {"A Fold", {0.0f, 1.0f}}, {"A Bend", {0.0f, 1.0f}}, {"A Sync", {0.0f, 1.0f}}, {"A Mutate", {0.0f, 1.0f}},
            {"A Seed", {0.1f, 0.9f}}, {"A Draw Mix", {0.0f, 1.0f}}, {"A Smooth", {0.0f, 1.0f}}};
        for (auto& c : controls)
        {
            std::vector<std::pair<const char*, float>> base;
            if (std::string(c.first) == "A Seed") base.push_back({"A Mutate", 0.6f});
            if (std::string(c.first) == "A Smooth") base.push_back({"A Draw Mix", 1.0f});
            auto lo = base, hi = base; lo.push_back({c.first, c.second.first}); hi.push_back({c.first, c.second.second});
            const double r = diffRel(renderShape(lo), renderShape(hi));
            char b[64]; std::snprintf(b, sizeof b, "%s %.0f %%, ", c.first, 100 * r); ctl += b;
            if (r < 0.01) ctlOk = false;
        }
        check(ctlOk, "Shape: every shaper control changes the sound", ctl);
        const double same = diffRel(renderShape({{"A Mutate", 0.6f}, {"A Seed", 0.3f}}), renderShape({{"A Mutate", 0.6f}, {"A Seed", 0.3f}}));
        const double other = diffRel(renderShape({{"A Mutate", 0.6f}, {"A Seed", 0.3f}}), renderShape({{"A Mutate", 0.6f}, {"A Seed", 0.31f}}));
        std::snprintf(d2, sizeof d2, "same seed twice: %.6f %% different; next seed: %.0f %% different", 100 * same, 100 * other);
        check(same == 0.0 && other > 0.01, "Shape: Mutate is seeded - the same Seed always gives the same wave", d2);

        // e) live changes: turning Fold and Start From while a note holds - the wave changes, no click
        {
            Plug p; p.open(); shapePatch(p);
            std::vector<float> all, part;
            p.render(0.3, {on(0, 48)}, &part); all.insert(all.end(), part.begin(), part.end());
            for (int k = 0; k < 12; ++k)
            {
                p.set("A Fold", (k % 4) / 3.0f); p.set("A Start From", (k * 7 % 60) / 59.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                p.render(0.1, {}, &part); all.insert(all.end(), part.begin(), part.end());
            }
            p.close();
            double maxStep = 0; for (size_t i = 1; i < all.size(); ++i) maxStep = std::fmax(maxStep, std::fabs(all[i] - all[i - 1]));
            const double cr = localClickRatio(all);
            std::snprintf(d2, sizeof d2, "12 changes during a held note: largest sample step %.3f, click ratio %.1f (limit %.0f)", maxStep, cr, kClickLimit);
            check(cr < kClickLimit && peak(all) < 0.95f, "Shape: changing the shape while a note plays crossfades, no clicks", d2);
            if (!renders.empty()) wav(renders + "/shape_live_changes.wav", all, all);
        }

        // f) drawing with the pad: a fast stroke is filled in; Draw Mix comes up; Undo, Clear, Copy Start
        {
            Plug p; p.open(); p.set("A1 Wave", 1.0f); p.run(0.05);
            const float before16 = pt(p, 'A', 16);
            p.set("A Draw X", 0.0f); p.set("A Draw Y", 1.0f); p.run(0.01);              // touch at point 1, top
            p.set("A Draw X", 32 / 63.0f); p.set("A Draw Y", 0.0f); p.run(0.01);        // fast move to point 33, bottom
            bool mono = true; for (int k = 1; k <= 32; ++k) mono = mono && pt(p, 'A', k) <= pt(p, 'A', k - 1) + 1e-3f;
            const float p0 = pt(p, 'A', 0), p16 = pt(p, 'A', 16), p32 = pt(p, 'A', 32);
            const std::string mix = p.text("A Draw Mix");
            p.tap("A Undo Draw"); p.run(0.05);
            const float undone16 = pt(p, 'A', 16);
            p.tap("A Clear Draw"); p.run(0.05);
            float maxAbs = 0; for (int k = 0; k < 64; ++k) maxAbs = std::fmax(maxAbs, std::fabs(pt(p, 'A', k)));
            settle(p);
            p.tap("A Copy Start"); p.run(0.05);
            const float c16 = pt(p, 'A', 16), c48 = pt(p, 'A', 48);
            p.close();
            std::snprintf(d2, sizeof d2, "stroke: point 1 = %+.2f, 17 = %+.2f, 33 = %+.2f, filled monotonic %s, Draw Mix %s; Undo -> point 17 %+.2f (was %+.2f); Clear -> max %.2f; Copy Start (saw) -> 17 = %+.2f, 49 = %+.2f",
                          p0, p16, p32, mono ? "yes" : "NO", mix.c_str(), undone16, before16, maxAbs, c16, c48);
            check(mono && std::fabs(p0 - 1) < 0.02f && std::fabs(p32 + 1) < 0.02f && std::fabs(p16) < 0.1f && mix == "100 %" &&
                  std::fabs(undone16 - before16) < 1e-3f && maxAbs < 1e-3f && std::fabs(c16 - c48) > 0.3f,
                  "drawing: a fast stroke has no gaps, Draw Mix comes up, Undo / Clear / Copy Start work", d2);
        }

        // f2) drawing on the default sound (oscillators on Saw) is heard straight away
        {
            auto renderAfter = [&](bool draw) {
                Plug p; p.open(); p.run(0.05);
                if (draw)
                {   // a square-ish stroke: top half then bottom half
                    p.set("A Draw X", 0.0f); p.set("A Draw Y", 0.95f); p.run(0.01); p.set("A Draw X", 31 / 63.0f); p.run(0.01);
                    p.set("A Draw X", 32 / 63.0f); p.set("A Draw Y", 0.05f); p.run(0.01); p.set("A Draw X", 1.0f); p.run(0.01);
                    p.run(0.02); std::this_thread::sleep_for(std::chrono::milliseconds(80)); p.run(0.05);
                }
                std::vector<float> L; p.render(0.6, {on(0, 48)}, &L);
                const std::string wave = p.text("A1 Wave"), mix = p.text("A Draw Mix");
                p.close(); return std::make_pair(L, wave + ", Draw Mix " + mix);
            };
            const auto plain = renderAfter(false), drawn = renderAfter(true);
            const double rel = [&] { double d = 0, r = 0; for (size_t k = 0; k < plain.first.size(); ++k) { d += std::fabs(plain.first[k] - drawn.first[k]); r += std::fabs(plain.first[k]) + std::fabs(drawn.first[k]); } return d / (r + 1e-12); }();
            std::snprintf(d2, sizeof d2, "default sound: A1 %s; after one stroke: A1 %s; the sound changed %.0f %%", plain.second.c_str(), drawn.second.c_str(), 100 * rel);
            check(drawn.second.rfind("Shape, Draw Mix 100", 0) == 0 && rel > 0.1, "drawing is heard at once: the first stroke sets oscillator 1 to Shape", d2);
            if (!renders.empty()) wav(renders + "/drawn_wave.wav", drawn.first, drawn.first);
        }
        // g) saved: project chunk, user preset after a restart; randomize leaves the drawing alone
        {
            Plug p; p.open(); p.set("A1 Wave", 1.0f); p.set("A Fold", 0.37f); p.set("A Start From", 11 / 59.0f);
            p.set("A Draw X", 0.2f); p.set("A Draw Y", 0.9f); p.run(0.01); p.set("A Draw X", 0.6f); p.set("A Draw Y", 0.1f); p.run(0.05);
            std::vector<float> pts(64); for (int k = 0; k < 64; ++k) pts[k] = pt(p, 'A', k);
            void* ptr = nullptr; const VstIntPtr len = p.call(effGetChunk, 0, 0, &ptr); std::vector<char> chunk((char*)ptr, (char*)ptr + len);
            // randomize: the drawing and Draw Mix stay
            const std::string mixBefore = p.text("A Draw Mix");
            p.set("Category", 5 / 6.0f); p.tap("Randomize"); p.run(0.1);
            float moved = 0; for (int k = 0; k < 64; ++k) moved = std::fmax(moved, std::fabs(pt(p, 'A', k) - pts[k]));
            const std::string mixAfter = p.text("A Draw Mix");
            p.tap("Undo"); p.run(0.1);
            // save as a user preset
            p.tap("Save"); p.tap("Confirm"); p.run(0.05); std::this_thread::sleep_for(std::chrono::milliseconds(300)); p.run(0.1);
            const std::string saved = p.text("Status");
            p.close();
            Plug q; q.open(); q.call(effSetChunk, 0, (VstIntPtr)chunk.size(), chunk.data()); q.call(effSetProgram, 0, 4); q.run(0.05);
            float chunkErr = 0; for (int k = 0; k < 64; ++k) chunkErr = std::fmax(chunkErr, std::fabs(pt(q, 'A', k) - pts[k]));
            const std::string fold = q.text("A Fold"), start = q.text("A Start From"), wave = q.text("A1 Wave");
            q.close();
            Plug r; r.open();                                                           // "restart": the user preset from the file
            int prog = -1; for (int i = 0; i < 84 && prog < 0; ++i) if (r.progName(i) == saved.substr(saved.find(':') + 2)) prog = i;
            float presetErr = 9;
            if (prog >= 0) { r.call(effSetProgram, 0, prog); r.run(0.05); presetErr = 0; for (int k = 0; k < 64; ++k) presetErr = std::fmax(presetErr, std::fabs(pt(r, 'A', k) - pts[k])); }
            r.close();
            std::snprintf(d2, sizeof d2, "project: points off by %.4f, %s, %s, %s; randomize: drawing moved %.4f, Draw Mix %s -> %s; '%s' after a restart: points off by %.4f",
                          chunkErr, fold.c_str(), start.c_str(), wave.c_str(), moved, mixBefore.c_str(), mixAfter.c_str(), saved.c_str(), presetErr);
            check(chunkErr < 1e-4f && fold == "37 %" && start == "VOX Ee" && wave == "Shape", "Shape and drawing are saved with the project", d2);
            check(moved < 1e-6f && mixBefore == mixAfter, "randomize never touches the drawing or Draw Mix", d2);
            check(prog >= 0 && presetErr < 1e-4f, "a saved user preset keeps the shape and drawing after a restart", d2);
        }
        // h) an Elver 1.0 user-preset file (version 1, 88 values per preset) still loads
        if (const char* old = std::getenv("ELVER_OLD_DLL"))
        {
            std::remove((std::string(argv[2]) + "/user-presets.bin").c_str());
            MainProc newMain = g_main;
            g_main = (MainProc)GetProcAddress(LoadLibraryA(old), "VSTPluginMain");
            std::string oldName; float oldCut = -1;
            if (g_main)
            {
                Plug o; o.open(); o.set("A Cutoff", 0.321f); o.set("Category", 2 / 6.0f); o.tap("Randomize"); o.run(0.1);
                oldCut = o.getN("A Cutoff");
                o.tap("Save"); o.tap("Confirm"); o.run(0.05); std::this_thread::sleep_for(std::chrono::milliseconds(300)); o.run(0.1);
                oldName = o.text("Status"); oldName = oldName.substr(oldName.find(':') + 2);
                o.close();
            }
            g_main = newMain;
            Plug p; p.open();
            int prog = -1; for (int i = 0; i < 84 && prog < 0; ++i) if (p.progName(i) == oldName) prog = i;
            float cut = -2; std::string fold;
            if (prog >= 0) { p.call(effSetProgram, 0, prog); p.run(0.05); cut = p.getN("A Cutoff"); fold = p.text("A Fold"); }
            p.close();
            std::snprintf(d2, sizeof d2, "1.0 saved '%s' (cutoff %.3f) -> 1.1 lists it as program %d, cutoff %.3f, shaper at default (Fold %s)", oldName.c_str(), oldCut, prog, cut, fold.c_str());
            check(prog >= 0 && std::fabs(cut - oldCut) < 1e-5f && fold == "0 %", "user presets saved by Elver 1.0 load in 1.1", d2);
        }
        // i) CPU of a Shape oscillator
        {
            for (int shape = 0; shape < 2; ++shape)
            {
                Plug p; p.open(); if (shape) p.set("A1 Wave", 1.0f);
                std::vector<Ev> ev; for (int k = 0; k < 8; ++k) ev.push_back(on(0, 48 + k * 3));
                settle(p); p.render(0.5, ev);
                const auto t0 = std::chrono::steady_clock::now(); p.render(3.0, {});
                const double pct = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 3.0 * 100.0;
                std::printf("  CPU, 8 notes, one oscillator, %-6s %5.2f %%  (Force est. %2.0f..%2.0f %%)\n", shape ? "Shape" : "Saw", pct, pct * 15, pct * 25);
                p.close();
            }
        }
    }    // ---- 15. the preset drop-down, and the list in the skin kept up to date -----------------------------------
    {
        Plug ref; ref.open(); ref.call(effSetProgram, 0, 13); ref.run(0.05); const float want = ref.getN("A Cutoff"); const std::string wantName = ref.progName(13); ref.close();
        Plug p; p.open(); p.run(0.05);
        p.set("Preset", 13 / 83.0f); p.run(0.1);                                   // PAD Warm Choir
        const float got = p.getN("A Cutoff"); const std::string st = p.text("Status"), shown = p.text("Preset");
        p.call(effSetProgram, 0, 30); p.run(0.1);
        const std::string follow = p.text("Preset"); const float followN = p.getN("Preset");
        const float before = p.getN("A Cutoff");
        p.set("Preset", 11 / 83.0f); p.run(0.1);                                   // AMB user slot 8: empty
        const std::string emptySt = p.text("Status"); const float after = p.getN("A Cutoff");
        p.close();
        char d3[400]; std::snprintf(d3, sizeof d3, "pick #14 -> cutoff %.3f (preset has %.3f), status '%s', shows '%s'; host loads #31 -> drop-down shows '%s' (%.3f); empty slot -> '%s', sound kept %s",
                                    got, want, st.c_str(), shown.c_str(), follow.c_str(), followN, emptySt.c_str(), after == before ? "yes" : "NO");
        check(std::fabs(got - want) < 1e-6f && st == "Loaded: " + wantName && shown == wantName && std::fabs(followN - 30 / 83.0f) < 1e-4f &&
              emptySt == "That slot is empty" && after == before, "preset drop-down: picking loads, it follows other loads, empty slots are refused", d3);
        // the skin's list (a copy of the real TUI.json in the plugin folder): the saved preset of test 8 is listed
        std::string savedName;
        {   // save a preset now (Lo-Fi), then read the skin's list
            Plug q; q.open(); q.set("Category", 4 / 6.0f); q.tap("Randomize"); q.run(0.1);
            q.tap("Save"); q.tap("Confirm"); q.run(0.05); std::this_thread::sleep_for(std::chrono::milliseconds(400)); q.run(0.1);
            savedName = q.text("Status"); savedName = savedName.substr(savedName.find(':') + 2);
            q.close();
        }
        const std::string skin = std::string(argv[2]) + "/Plugin Skins/TUI.json";
        std::vector<char> raw; if (FILE* fp = std::fopen(skin.c_str(), "rb")) { char b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, fp)) > 0) raw.insert(raw.end(), b, b + n); std::fclose(fp); }
        const std::string j(raw.begin(), raw.end());
        const size_t k = j.find("\"Parameter 377\""), open = j.find('[', k), close = j.find(']', open);
        const std::string list = k == std::string::npos ? "" : j.substr(open, close - open + 1);
        const size_t moth = list.find("\"" + savedName + "\""), lofi1 = list.find("\"LOFI Moth Light\"");
        std::snprintf(d3, sizeof d3, "skin file %s, list %zu chars; just saved '%s': %s (right after the Lo-Fi factory presets: %s); 'AMB empty 1' %s",
                      raw.empty() ? "MISSING" : "found", list.size(), savedName.c_str(), moth != std::string::npos ? "listed" : "NOT listed", moth > lofi1 && moth - lofi1 < 24 ? "yes" : "no",
                      list.find("\"AMB empty 1\"") != std::string::npos ? "listed" : "NOT listed");
        check(!raw.empty() && moth != std::string::npos && moth > lofi1 && list.find("\"AMB empty 1\"") != std::string::npos,
              "the drop-down's list in the skin is rewritten with saved preset names", d3);
    }
    std::printf("\nper-control list:\n"); for (const auto& s : g_controlList) std::printf("  %s\n", s.c_str());
    std::printf("\n%d passed, %d failure(s)\n", g_pass, g_fail);
    return g_fail;
}
