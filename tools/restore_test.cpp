// restore_test - checks that a plugin's whole state survives a project save / reload, the way the
// MPC does it (state chunk + its own saved program number), in every order the host might use.
// Desktop only. Build:  zig c++ -std=c++17 -O2 -I framework -o restore_test.exe tools/restore_test.cpp
// usage: restore_test <plugin.dll|.so> [program]
//
// Instance A: select a program, set every automatable parameter to a random value, play audio,
// save the chunk. Then for each reload order, a fresh instance B: the host's own start-up
// (program 0), then  (1) chunk, then the saved program number;  (2) program number, then chunk;
// (3) chunk only. B plays audio, and every automatable parameter and the program number must
// equal A's exactly.
#include "mpc_vst2_abi.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

struct P
{
    AEffect* e = nullptr;
    VstIntPtr call(VstInt32 op, VstInt32 i = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0) { return e->dispatcher(e, op, i, v, p, o); }
    void open() { e = g_main(cb); call(effSetSampleRate, 0, 0, nullptr, 44100.0f); call(effSetBlockSize, 0, 128); call(effOpen); call(effSetProgram, 0, 0); }
    void start() { call(effMainsChanged, 0, 1); call(effStartProcess); }
    void play(double sec)
    {
        float l[128], r[128];
        for (int n = 0; n < (int)(sec * 44100 / 128); ++n)
        {
            for (int i = 0; i < 128; ++i) l[i] = r[i] = 0.2f * std::sin(0.0314f * (float)(n * 128 + i));
            float* io[2] = {l, r};
            e->processReplacing(e, io, io, 128);
        }
    }
    void close() { call(effStopProcess); call(effMainsChanged, 0, 0); call(effClose); }
    std::string name(int i) { char b[256] = {0}; call(effGetParamName, i, 0, b); return b; }
};

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: restore_test <plugin> [program]\n"); return 2; }
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA(argv[1]); if (lib) g_main = (MainProc)GetProcAddress(lib, "VSTPluginMain");
#else
    void* lib = dlopen(argv[1], RTLD_NOW); if (lib) g_main = (MainProc)dlsym(lib, "VSTPluginMain");
#endif
    if (!g_main) { std::printf("cannot load %s\n", argv[1]); return 2; }
    std::memset(&g_time, 0, sizeof g_time); g_time.tempo = 120; g_time.sampleRate = 44100; g_time.flags = kVstTempoValid;

    P a; a.open();
    const int nprog = a.e->numPrograms, np = a.e->numParams;
    const int prog = argc > 2 ? std::atoi(argv[2]) : (nprog > 3 ? 3 : nprog - 1);
    a.call(effSetProgram, 0, prog);
    a.start();
    a.play(0.2);
    std::vector<bool> autom(np);
    uint32_t s = 12345;
    int nAuto = 0;
    for (int i = 0; i < np; ++i)
    {
        autom[i] = a.call(effCanBeAutomated, i) != 0;
        if (!autom[i]) continue;
        ++nAuto;
        s = s * 1664525u + 1013904223u;
        a.e->setParameter(a.e, i, (float)((s >> 8) & 0xFFFF) / 65535.0f);
    }
    a.play(0.5);
    std::vector<float> want(np);
    for (int i = 0; i < np; ++i) want[i] = a.e->getParameter(a.e, i);
    void* data = nullptr;
    const VstIntPtr n = a.call(effGetChunk, 0, 0, &data);
    std::vector<uint8_t> chunk((uint8_t*)data, (uint8_t*)data + n);
    const int progA = (int)a.call(effGetProgram);
    a.close();

    int fails = 0;
    const char* orders[] = {"chunk, then program number", "program number, then chunk", "chunk only"};
    for (int o = 0; o < 3; ++o)
    {
        P b; b.open();
        if (o == 0) { b.call(effSetChunk, 0, (VstIntPtr)chunk.size(), chunk.data()); b.call(effSetProgram, 0, progA); }
        if (o == 1) { b.call(effSetProgram, 0, progA); b.call(effSetChunk, 0, (VstIntPtr)chunk.size(), chunk.data()); }
        if (o == 2) { b.call(effSetChunk, 0, (VstIntPtr)chunk.size(), chunk.data()); }
        b.start();
        b.play(0.5);
        int bad = 0; std::string first;
        for (int i = 0; i < np; ++i)
        {
            if (!autom[i]) continue;
            const float got = b.e->getParameter(b.e, i);
            if (std::fabs(got - want[i]) > 1e-6f)
            {
                if (bad++ < 3) { char t[160]; std::snprintf(t, sizeof t, "%s%s %.4f != %.4f", first.empty() ? "" : ", ", b.name(i).c_str(), got, want[i]); first += t; }
            }
        }
        const int progB = (int)b.call(effGetProgram);
        b.close();
        const bool ok = bad == 0 && progB == progA;
        if (!ok) ++fails;
        std::printf("%s  reload order: %-28s  %d/%d settings restored, preset %d -> %d%s%s\n", ok ? "PASS" : "FAIL", orders[o],
                    nAuto - bad, nAuto, progA, progB, first.empty() ? "" : "  -- ", first.c_str());
    }
    return fails;
}
