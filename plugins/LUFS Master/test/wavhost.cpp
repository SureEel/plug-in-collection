// wavhost - offline test host for LUFS Master (desktop only, not shipped).
//
// Loads the native build through the same VST2 ABI the MPC uses and runs a real WAV file
// through the complete workflow:
//   1. Measure pass  (Mode = Measure)          -> <prefix>_measure.wav  (leveler output)
//   2. Lock          (Mode = Locked, then silence until the calculation finishes)
//   3. Bounce pass   (Refresh, then the file again)  -> <prefix>_locked.wav
//   4. Project reload: save the chunk, load it into a fresh instance, check the lock survived
//   5. Bypass pass: output must equal the input delayed by the reported latency, bit for bit
// Output WAVs are 32-bit float, latency-compensated. Readouts are printed as the MPC shows them.
//
// usage: wavhost <plugin.dll> <in.wav> <out-prefix> [--target 0..4] [--custom LUFS] [--ceiling dBTP] [--block N]

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

namespace {

VstIntPtr hostCallback(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void*, float)
{
    return opcode == audioMasterVersion ? 2400 : 0;
}

typedef AEffect* (*MainProc)(audioMasterCallback);
MainProc g_main = nullptr;

struct Wav { int sr = 44100; std::vector<float> l, r; };

bool readWav(const char* path, Wav& w)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    int fmt = 0, ch = 0, bits = 0; size_t pos = 12; const uint8_t* data = nullptr; size_t dataLen = 0;
    auto u16 = [&](size_t p) { return (int)(d[p] | (d[p + 1] << 8)); };
    auto u32 = [&](size_t p) { return (uint32_t)(d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)); };
    while (pos + 8 <= d.size())
    {
        const uint32_t len = u32(pos + 4);
        if (!std::memcmp(&d[pos], "fmt ", 4))
        {
            fmt = u16(pos + 8); ch = u16(pos + 10); w.sr = (int)u32(pos + 12); bits = u16(pos + 22);
            if (fmt == 0xFFFE && len >= 26) fmt = u16(pos + 32);   // WAVE_FORMAT_EXTENSIBLE sub-format
        }
        else if (!std::memcmp(&d[pos], "data", 4)) { data = &d[pos + 8]; dataLen = std::min<size_t>(len, d.size() - pos - 8); }
        pos += 8 + len + (len & 1);
    }
    if (!data || ch < 1) return false;
    const int bps = bits / 8; const size_t frames = dataLen / (size_t)(bps * ch);
    w.l.resize(frames); w.r.resize(frames);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c)
        {
            const uint8_t* p = data + (i * ch + (c < ch ? c : 0)) * bps;
            float v = 0.0f;
            if (fmt == 3 && bits == 32) std::memcpy(&v, p, 4);
            else if (bits == 16) v = (int16_t)(p[0] | (p[1] << 8)) / 32768.0f;
            else if (bits == 24) v = (int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)) / 2147483648.0f;
            else if (bits == 32) v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)) / 2147483648.0f;
            else return false;
            (c == 0 ? w.l : w.r)[i] = v;
        }
    return true;
}

void writeWav(const std::string& path, int sr, const std::vector<float>& l, const std::vector<float>& r)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("cannot write %s\n", path.c_str()); return; }
    const uint32_t frames = (uint32_t)l.size(), dataLen = frames * 8;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + dataLen); std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(3); w16(2); w32((uint32_t)sr); w32((uint32_t)sr * 8); w16(8); w16(32);
    std::fwrite("data", 1, 4, f); w32(dataLen);
    for (uint32_t i = 0; i < frames; ++i) { std::fwrite(&l[i], 4, 1, f); std::fwrite(&r[i], 4, 1, f); }
    std::fclose(f);
}

struct Plug
{
    AEffect* e = nullptr;
    int block = 128;

    VstIntPtr call(VstInt32 op, VstInt32 index = 0, VstIntPtr value = 0, void* ptr = nullptr, float opt = 0.0f)
    { return e->dispatcher(e, op, index, value, ptr, opt); }

    bool open(int sr, int blk)
    {
        e = g_main(hostCallback);
        if (!e) return false;
        block = blk;
        call(effSetSampleRate, 0, 0, nullptr, (float)sr);
        call(effSetBlockSize, 0, blk < 128 ? 128 : blk);
        call(effOpen);
        call(effSetProgram, 0, 0);
        call(effMainsChanged, 0, 1);
        call(effStartProcess);
        return true;
    }
    void close() { call(effStopProcess); call(effMainsChanged, 0, 0); call(effClose); e = nullptr; }

    int find(const char* name)
    {
        char buf[256];
        for (int i = 0; i < e->numParams; ++i) { buf[0] = 0; call(effGetParamName, i, 0, buf); if (!std::strcmp(buf, name)) return i; }
        std::printf("no parameter named %s\n", name); std::exit(1);
    }
    void set(const char* name, float norm) { e->setParameter(e, find(name), norm); }
    std::string text(const char* name) { char buf[256] = {0}; call(effGetParamDisplay, find(name), 0, buf); return buf; }

    // Process a whole signal in blocks, in place like the MPC; returns latency-compensated output.
    void run(const std::vector<float>& inL, const std::vector<float>& inR, std::vector<float>* outL, std::vector<float>* outR)
    {
        const int lat = e->initialDelay;
        const size_t total = inL.size() + (size_t)lat;
        std::vector<float> bl(block), br(block);
        if (outL) { outL->clear(); outR->clear(); outL->reserve(inL.size()); outR->reserve(inL.size()); }
        for (size_t pos = 0; pos < total; pos += (size_t)block)
        {
            const int n = (int)std::min<size_t>((size_t)block, total - pos);
            for (int i = 0; i < n; ++i) { const size_t k = pos + i; bl[i] = k < inL.size() ? inL[k] : 0.0f; br[i] = k < inR.size() ? inR[k] : 0.0f; }
            float* io[2] = { bl.data(), br.data() };
            e->processReplacing(e, io, io, n);
            if (outL)
                for (int i = 0; i < n; ++i)
                    if (pos + i >= (size_t)lat) { outL->push_back(bl[i]); outR->push_back(br[i]); }
        }
    }
    void silence(double seconds, int sr) { std::vector<float> z((size_t)(seconds * sr), 0.0f); run(z, z, nullptr, nullptr); }

    void printReadouts(const char* title)
    {
        std::printf("  %s\n", title);
        const char* rows[][4] = {
            {"In Mom LUFS", "In ST LUFS", "In Int LUFS", "In Avg LUFS"},
            {"Out Mom LUFS", "Out ST LUFS", "Out Int LUFS", "Out Avg LUFS"},
            {"In TP Max", "Out TP Max", "GR", "GR Max"},
            {"Gain", "Predicted", "Mode", "Target"} };
        for (auto& row : rows)
        {
            std::printf("   ");
            for (const char* name : row) std::printf(" %-12s %-9s", name, text(name).c_str());
            std::printf("\n");
        }
        if (e->numParams > 20)
        {   // loudness history (5 s columns, oldest first)
            std::printf("    history:");
            for (int i = 1; i <= 48; ++i) { char n[16]; std::snprintf(n, sizeof(n), "Hist %02d", i); std::printf(" %s", text(n).c_str()); }
            std::printf("\n");
        }
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) { std::printf("usage: wavhost <plugin> <in.wav> <out-prefix> [--target 0..4] [--custom LUFS] [--ceiling dBTP] [--block N]\n"); return 1; }
    int target = 0, block = 128; float custom = -14.0f, ceiling = -1.0f, pregain = 0.0f;
    for (int i = 4; i + 1 < argc; i += 2)
    {
        if (!std::strcmp(argv[i], "--target")) target = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--custom")) custom = (float)std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--ceiling")) ceiling = (float)std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--block")) block = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--pregain")) pregain = (float)std::atof(argv[i + 1]);
    }
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA(argv[1]);
    if (lib) g_main = (MainProc)GetProcAddress(lib, "VSTPluginMain");
#else
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (lib) g_main = (MainProc)dlsym(lib, "VSTPluginMain");
#endif
    if (!g_main) { std::printf("cannot load %s\n", argv[1]); return 1; }

    bool meterOnly = false, monoLeft = false;
    for (int i = 4; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--meter")) meterOnly = true;
        if (!std::strcmp(argv[i], "--mono-left")) monoLeft = true;
    }

    Wav in;
    if (!readWav(argv[2], in)) { std::printf("cannot read %s (PCM 16/24/32 or float 32 WAV)\n", argv[2]); return 1; }
    if (monoLeft) for (float& v : in.r) v = 0.0f;   // a mono file measured as one channel (BS.1770)
    const std::string prefix = argv[3];

    if (meterOnly)
    {
        // Meter test: a fresh instance (reset meter), Measure mode for loudness, exactly the file's
        // length (no padding, so an incomplete last 100 ms block is discarded as Tech 3341 says).
        // The displayed In Mom / In ST / In Int values are read after every audio block, i.e. what
        // the screen would show, and written as a time series. A second instance in Bypass (gain
        // exactly 1) reports the maximum true peak.
        Plug m; if (!m.open(in.sr, block)) return 1;
        m.set("Mode", 0.5f);
        FILE* csv = std::fopen((prefix + "_series.csv").c_str(), "w");
        std::fprintf(csv, "t,mom,short,integ\n");
        auto num = [](const std::string& s) { return (s == "--" || s == "calc" || s.empty()) ? std::string("") : s; };
        std::vector<float> bl(block), br(block);
        for (size_t pos = 0; pos < in.l.size(); pos += (size_t)block)
        {
            const int n = (int)std::min<size_t>((size_t)block, in.l.size() - pos);
            std::memcpy(bl.data(), &in.l[pos], sizeof(float) * n); std::memcpy(br.data(), &in.r[pos], sizeof(float) * n);
            float* io[2] = { bl.data(), br.data() };
            m.e->processReplacing(m.e, io, io, n);
            std::fprintf(csv, "%.5f,%s,%s,%s\n", (pos + n) / (double)in.sr, num(m.text("In Mom LUFS")).c_str(),
                         num(m.text("In ST LUFS")).c_str(), num(m.text("In Int LUFS")).c_str());
        }
        std::fclose(csv);
        const std::string integ = m.text("In Int LUFS");
        m.close();
        Plug b; b.open(in.sr, block);
        b.set("Mode", 0.0f);
        b.run(in.l, in.r, nullptr, nullptr);
        std::printf("RESULT integ=%s tpmax=%s\n", integ.c_str(), b.text("In TP Max").c_str());
        b.close();
        return 0;
    }
    if (pregain != 0.0f)
    {   // scale the input (simulates a quieter, unmastered mix); also saved as the reference input
        const float g = std::pow(10.0f, pregain / 20.0f);
        for (float& v : in.l) v *= g;
        for (float& v : in.r) v *= g;
        writeWav(prefix + "_input.wav", in.sr, in.l, in.r);
    }
    std::printf("%s: %.1f s at %d Hz, block %d\n", argv[2], in.l.size() / (double)in.sr, in.sr, block);

    auto configure = [&](Plug& p) {
        p.set("Target", target / 4.0f);
        p.set("Custom LUFS", (custom + 30.0f) / 24.0f);
        p.set("Ceiling", (ceiling + 3.0f) / 3.0f);
    };

    Plug a; if (!a.open(in.sr, block)) return 1;
    configure(a);
    std::printf("  latency %d samples\n", a.e->initialDelay);
    bool helpTest = false;
    for (int i = 4; i < argc; ++i) if (!std::strcmp(argv[i], "--help-test")) helpTest = true;
    if (helpTest && a.e->numParams >= 70)
    {   // help window: press help buttons 0, 3, 11, 15 (radio group of 16), then back to 0.
        // Opt-in: it plays a little silence first, which shifts the 100 ms measurement grid.
        for (int id : {0, 3, 11, 15, 0})
        {
            a.set("Help", id / 15.0f);
            a.silence(0.01, in.sr);
            std::printf("  help button %2d -> \"%s\" / \"%s\"\n", id, a.text("Help").c_str(), a.text("Help 2").c_str());
        }
        std::printf("  target text: \"%s\"\n", a.text("Target").c_str());
    }

    // 1. measure
    a.set("Mode", 0.5f);
    std::vector<float> ol, orr;
    a.run(in.l, in.r, &ol, &orr);
    writeWav(prefix + "_measure.wav", in.sr, ol, orr);
    a.printReadouts("after Measure pass:");

    // 2. lock
    a.set("Mode", 1.0f);
    int waited = 0;
    do { a.silence(0.1, in.sr); waited++; } while (a.text("Predicted") == "calc" && waited < 600);
    std::printf("  lock calculated within %d ms of audio\n", waited * 100);

    // 3. bounce
    a.set("Refresh", 1.0f); a.silence(0.05, in.sr);
    a.run(in.l, in.r, &ol, &orr);
    writeWav(prefix + "_locked.wav", in.sr, ol, orr);
    a.printReadouts("after Locked bounce:");
    const std::string gainA = a.text("Gain"), predA = a.text("Predicted"), inIntA = a.text("In Int LUFS");

    // 4. project reload
    void* chunk = nullptr;
    const VstIntPtr size = a.call(effGetChunk, 0, 0, &chunk);
    std::vector<uint8_t> saved((uint8_t*)chunk, (uint8_t*)chunk + size);
    a.close();
    if (FILE* sf = std::fopen((prefix + "_state.bin").c_str(), "wb")) { std::fwrite(saved.data(), 1, saved.size(), sf); std::fclose(sf); }
    Plug b; b.open(in.sr, block);
    b.call(effSetChunk, 0, (VstIntPtr)saved.size(), saved.data());
    b.silence(1.0, in.sr);
    b.printReadouts("after project reload:");
    std::printf("  project reload: chunk %lld bytes; Gain %s -> %s, Predicted %s -> %s, In Int %s -> %s, Mode %s  %s\n",
                (long long)saved.size(), gainA.c_str(), b.text("Gain").c_str(), predA.c_str(), b.text("Predicted").c_str(),
                inIntA.c_str(), b.text("In Int LUFS").c_str(), b.text("Mode").c_str(),
                (gainA == b.text("Gain") && predA == b.text("Predicted") && inIntA == b.text("In Int LUFS")) ? "OK" : "MISMATCH");
    // Reset clears the measurement and lock
    b.set("Reset", 1.0f); b.silence(0.5, in.sr);
    std::printf("  after Reset: Gain %s, Predicted %s, In Int %s, Reset switch %s\n",
                b.text("Gain").c_str(), b.text("Predicted").c_str(), b.text("In Int LUFS").c_str(), b.text("Reset").c_str());
    b.close();

    // 5. bypass transparency
    Plug c; c.open(in.sr, block); configure(c);
    c.set("Mode", 0.0f);
    c.run(in.l, in.r, &ol, &orr);
    size_t diff = 0;
    for (size_t i = 0; i < in.l.size(); ++i) if (ol[i] != in.l[i] || orr[i] != in.r[i]) ++diff;
    std::printf("  bypass: %zu of %zu samples differ from the input (want 0)\n", diff, in.l.size());
    c.close();
    return 0;
}
