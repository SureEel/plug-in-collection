// hostile_test - throws host events at a plugin while it plays, and checks that the output never
// goes silent while the input is active (and, if it does, which action brings it back).
// Desktop only. Build:  zig c++ -std=c++17 -O2 -I framework -o hostile_test.exe tools/hostile_test.cpp
// usage: hostile_test <plugin.dll|.so> [program]
#include "mpc_vst2_abi.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>
#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

static VstTimeInfo g_time;
static VstIntPtr cb(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float)
{
    if (op == audioMasterVersion) return 2400;
    if (op == audioMasterGetTime) return (VstIntPtr)&g_time;
    return 0;
}
typedef AEffect* (*MainProc)(audioMasterCallback);
static MainProc g_main;
static int g_prog = 0;

struct H
{
    AEffect* e = nullptr;
    double sr = 44100;
    int block = 128;
    double t = 0;                 // input phase clock
    bool badNext = false; float badValue = 0;
    VstIntPtr call(VstInt32 op, VstInt32 i = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0) { return e->dispatcher(e, op, i, v, p, o); }
    void open()
    {
        e = g_main(cb);
        call(effSetSampleRate, 0, 0, nullptr, (float)sr); call(effSetBlockSize, 0, block); call(effOpen);
        call(effSetProgram, 0, g_prog); call(effMainsChanged, 0, 1); call(effStartProcess);
    }
    void close() { call(effStopProcess); call(effMainsChanged, 0, 0); call(effClose); }
    // plays plucked guitar-like notes; returns the output RMS (dBFS) over the call
    double play(double sec, int frames = -1)
    {
        const int n = frames > 0 ? frames : block;
        std::vector<float> l(n), r(n);
        double s = 0; long cnt = 0;
        for (long k = 0; k < (long)(sec * sr / n); ++k)
        {
            for (int i = 0; i < n; ++i)
            {
                const double tn = std::fmod(t, 0.5), f = ((int)(t / 0.5) % 2) ? 165.0 : 110.0;
                const double ph = std::fmod(t * f, 1.0);
                l[i] = r[i] = (float)(0.25 * std::fmin(1.0, tn / 0.003) * std::exp(-tn * 3.0) * (2 * ph - 1));
                t += 1.0 / sr;
            }
            if (badNext) { l[n / 2] = badValue; r[n / 2] = badValue; badNext = false; }
            float* io[2] = {l.data(), r.data()};
            e->processReplacing(e, io, io, n);
            for (int i = 0; i < n; ++i) { const float v = std::isfinite(l[i]) ? l[i] : 0.0f; s += (double)v * v; ++cnt; }
        }
        return 10 * std::log10(s / std::max(1L, cnt) + 1e-20);
    }
    void midi(int count)
    {
        // a burst like a MIDI guitar on connect: notes on 6 channels, CCs (incl. all-notes-off, reset
        // all controllers), program changes, pitch bend, channel pressure
        std::vector<VstMidiEvent> ev((size_t)count);
        std::vector<char> store(sizeof(VstEvents) + sizeof(VstEvent*) * (size_t)count);
        VstEvents* list = reinterpret_cast<VstEvents*>(store.data());
        list->numEvents = count; list->reserved = 0;
        for (int i = 0; i < count; ++i)
        {
            VstMidiEvent& m = ev[(size_t)i];
            std::memset(&m, 0, sizeof m);
            m.type = kVstMidiType; m.byteSize = sizeof m; m.deltaFrames = i % 200;   // some offsets past the block
            const int ch = i % 6, kind = i % 7;
            const unsigned char st[] = {0x90, 0x80, 0xB0, 0xC0, 0xE0, 0xD0, 0xB0};
            m.midiData[0] = (char)(st[kind] | ch);
            m.midiData[1] = (char)(kind == 6 ? 120 + (i % 8) : (i * 7) % 128);
            m.midiData[2] = (char)((i * 13) % 128);
            list->events[i] = reinterpret_cast<VstEvent*>(&m);
        }
        call(effProcessEvents, 0, 0, list);
    }
};

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: hostile_test <plugin> [program]\n"); return 2; }
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA(argv[1]); if (lib) g_main = (MainProc)GetProcAddress(lib, "VSTPluginMain");
#else
    void* lib = dlopen(argv[1], RTLD_NOW); if (lib) g_main = (MainProc)dlsym(lib, "VSTPluginMain");
#endif
    if (!g_main) { std::printf("cannot load %s\n", argv[1]); return 2; }
    if (argc > 2) g_prog = std::atoi(argv[2]);
    std::memset(&g_time, 0, sizeof g_time); g_time.tempo = 120; g_time.sampleRate = 44100; g_time.flags = kVstTempoValid;
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();

    if (argc > 3 && !std::strcmp(argv[3], "probe"))
    {   // after one NaN in the input: which single parameter touch brings the sound back?
        H h0; h0.open(); const int np = h0.e->numParams; h0.close();
        std::string ok;
        for (int i = 0; i < np; ++i)
        {
            H g; g.open(); const double before = g.play(1.0);
            if (g.call(effCanBeAutomated, i) == 0) { g.close(); continue; }
            g.badNext = true; g.badValue = nan; g.play(0.5);
            const float v = g.e->getParameter(g.e, i);
            g.e->setParameter(g.e, i, v > 0.5f ? v - 0.2f : v + 0.2f); g.play(0.3);
            g.e->setParameter(g.e, i, v); g.play(0.3);
            const double back = g.play(1.0);
            char nm[64] = {0}; g.call(effGetParamName, i, 0, nm);
            if (back > before - 6.0) ok += std::string(nm) + ", ";
            g.close();
        }
        std::printf("touching any one of these brings the sound back: %s\n", ok.empty() ? "(none)" : ok.c_str());
        return 0;
    }
    struct Case { const char* name; std::function<void(H&)> act; };
    const std::vector<Case> cases = {
        {"MIDI burst (3000 events / block, 1 s)", [](H& h) { for (int k = 0; k < 344; ++k) { h.midi(3000); h.play(0.0029); } }},
        {"suspend + resume", [](H& h) { h.call(effMainsChanged, 0, 0); h.call(effMainsChanged, 0, 1); }},
        {"sample rate 48 kHz while running (no suspend)", [](H& h) { h.call(effSetSampleRate, 0, 0, nullptr, 48000.0f); h.sr = 48000; }},
        {"sample rate 0 then back, no resume", [](H& h) { h.call(effSetSampleRate, 0, 0, nullptr, 0.0f); h.play(0.05); h.call(effSetSampleRate, 0, 0, nullptr, 44100.0f); }},
        {"sample rate 0, suspend + resume, back", [](H& h) { h.call(effSetSampleRate, 0, 0, nullptr, 0.0f); h.call(effMainsChanged, 0, 0); h.call(effMainsChanged, 0, 1); h.play(0.05); h.call(effSetSampleRate, 0, 0, nullptr, 44100.0f); }},
        {"block size 512 while running", [](H& h) { h.call(effSetBlockSize, 0, 512); h.block = 512; }},
        {"block size 0 + resume", [](H& h) { h.call(effSetBlockSize, 0, 0); h.call(effMainsChanged, 0, 0); h.call(effMainsChanged, 0, 1); }},
        {"re-open + program 0 (host start-up again)", [](H& h) { h.call(effOpen); h.call(effSetProgram, 0, 0); }},
        {"parameter 0 nudged to 1 and 0 (host quirk)", [](H& h) { const float v = h.e->getParameter(h.e, 0); h.e->setParameter(h.e, 0, 1.0f); h.e->setParameter(h.e, 0, 0.0f); h.e->setParameter(h.e, 0, v); }},
        {"one NaN sample in the input", [nan](H& h) { h.badNext = true; h.badValue = nan; }},
        {"one +inf sample in the input", [inf](H& h) { h.badNext = true; h.badValue = inf; }},
        {"one 1e30 sample in the input", [](H& h) { h.badNext = true; h.badValue = 1e30f; }},
        {"state chunk reloaded while running", [](H& h) { void* d = nullptr; const VstIntPtr n = h.call(effGetChunk, 0, 0, &d); std::vector<uint8_t> c((uint8_t*)d, (uint8_t*)d + n); h.call(effSetChunk, 0, (VstIntPtr)c.size(), c.data()); }},
        {"NaN sent to every parameter by the host", [nan](H& h) { for (int i = 0; i < h.e->numParams; ++i) h.e->setParameter(h.e, i, nan); }},
        {"project state with NaN values loaded", [nan](H& h) {
            void* d = nullptr; const VstIntPtr n = h.call(effGetChunk, 0, 0, &d); std::vector<uint8_t> c((uint8_t*)d, (uint8_t*)d + n);
            // the framework's chunk: "MPCP" | u32 version | u32 count | { u8 len, id, f32 plain, f32 normalised }...
            size_t pos = 12; uint32_t cnt; std::memcpy(&cnt, c.data() + 8, 4);
            for (uint32_t k = 0; k < cnt && pos < c.size(); ++k) { const size_t len = c[pos]; pos += 1 + len; std::memcpy(c.data() + pos, &nan, 4); std::memcpy(c.data() + pos + 4, &nan, 4); pos += 8; }
            h.call(effSetChunk, 0, (VstIntPtr)c.size(), c.data()); }},
        {"zero-frame and 1-frame process calls", [](H& h) { h.play(0.0, 1); float a = 0, b = 0; float* io[2] = {&a, &b}; h.e->processReplacing(h.e, io, io, 0); }},
    };
    int silent = 0;
    for (const Case& c : cases)
    {
        H h; h.open();
        const double before = h.play(1.0);
        c.act(h);
        h.play(0.3);
        const double after = h.play(1.0);
        const bool dead = after < before - 40.0;
        std::string fix;
        if (dead)
        {
            ++silent;
            // which action brings it back? (each tried on a fresh copy of the failure)
            struct Fix { const char* name; std::function<void(H&)> act; };
            const std::vector<Fix> fixes = {
                {"touch parameter 1", [](H& x) { const float v = x.e->getParameter(x.e, 1); x.e->setParameter(x.e, 1, v > 0.5f ? v - 0.01f : v + 0.01f); }},
                {"select the same program", [](H& x) { x.call(effSetProgram, 0, (VstIntPtr)x.call(effGetProgram)); }},
                {"suspend + resume", [](H& x) { x.call(effMainsChanged, 0, 0); x.call(effMainsChanged, 0, 1); }},
            };
            for (const Fix& f : fixes)
            {
                H g; g.open(); g.play(1.0); c.act(g); g.play(0.3);
                f.act(g); g.play(0.3);
                const double back = g.play(1.0);
                fix += std::string(" | ") + f.name + (back > before - 6.0 ? ": recovers" : ": still silent");
                g.close();
            }
        }
        std::printf("%s  %-48s output %6.1f -> %6.1f dBFS%s\n", dead ? "SILENT" : "ok    ", c.name, before, after, fix.c_str());
        h.close();
    }
    return silent;
}
