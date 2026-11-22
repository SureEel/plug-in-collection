// Kashyyyk Fuzz - verification (0.3). The plugin class is compiled in directly (no host).
// Build: zig c++ -std=c++17 -O2 -I ../../framework -I src -o build/kf_test.exe test/kf_test.cpp
// Run:   build/kf_test.exe [render folder]
#include "../src/KashyyykFuzz.cpp"
#include <algorithm>
#include <complex>
#include <cstring>
#include <thread>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static std::vector<std::string> lines;
static void check(bool ok, const std::string& what, const std::string& detail = "")
{
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++fails;
}
static const double SR = 44100.0;

// ---- signals ---------------------------------------------------------------------------------------------------
// a guitar-like riff: plucked notes with harmonics and decays, about -12 dBFS peaks
static float riff(long t)
{
    static const double notes[8] = {82.41, 110.0, 146.83, 110.0, 196.0, 146.83, 246.94, 164.81};
    const double tt = t / SR, len = 0.375;
    const int k = (int)(tt / len); const double age = tt - k * len, f = notes[k % 8];
    double s = 0.0;
    for (int h = 1; h <= 8; ++h) s += std::sin(2 * 3.14159265358979 * f * h * age) * (0.5 / (h * h)) * std::exp(-age * (2.0 + h * 0.6));
    return (float)(0.17 * s * std::min(1.0, age * 400.0));
}
static float sine(long t, double hz, double amp) { return (float)(amp * std::sin(2 * 3.14159265358979 * hz * t / SR)); }

struct Rig
{
    KashyyykFuzz p;
    long t = 0;
    Rig() { p.prepare(SR, 512); }
    // wait for the level-match worker to measure the current combination (it runs on its own thread)
    void settle() { run(0.02, [](long) { return 0.0f; }); for (int k = 0; k < 200 && !p.leveledForTest(); ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); run(0.01, [](long) { return 0.0f; }); } }
    std::string text(int i) { std::string s; p.formatValue(i, p.get(i), s); return s; }
    template <class Fn> void run(double sec, Fn sig, std::vector<float>* L = nullptr, std::vector<float>* R = nullptr, int block = 128, bool stereoDiff = false)
    {
        const long total = (long)(sec * SR);
        for (long done = 0; done < total; done += block)
        {
            const int n = (int)std::min<long>(block, total - done);
            std::vector<float> l((size_t)n), r((size_t)n);
            for (int i = 0; i < n; ++i) { const float v = sig(t + i); l[(size_t)i] = v; r[(size_t)i] = stereoDiff ? -0.5f * v : v; }
            float* outs[2] = {l.data(), r.data()}; const float* ins[2] = {l.data(), r.data()};
            mpc::ProcessContext ctx; ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = n; ctx.sampleRate = SR;
            p.process(ctx);
            if (L) L->insert(L->end(), l.begin(), l.end());
            if (R) R->insert(R->end(), r.begin(), r.end());
            t += n;
        }
    }
    void setChoice(int idx, int v) { p.set(idx, (float)v); }
};

static double rms(const std::vector<float>& v, size_t from = 0)
{
    double s = 0.0; size_t n = 0;
    for (size_t i = from; i < v.size(); ++i) { s += (double)v[i] * v[i]; ++n; }
    return n ? std::sqrt(s / n) : 0.0;
}
static double db(double x) { return 20.0 * std::log10(std::max(1e-12, x)); }
static double peak(const std::vector<float>& v) { double m = 0.0; for (float x : v) m = std::max(m, (double)std::fabs(x)); return m; }

// clicks: a single-sample step far larger than the steps around it (isolated, not part of the waveform's own edges)
static double clickRatio(const std::vector<float>& v, size_t from)
{
    double worst = 0.0;
    std::vector<double> d(v.size(), 0.0);
    for (size_t i = from + 1; i < v.size(); ++i) d[i] = std::fabs((double)v[i] - v[i - 1]);
    for (size_t i = from + 2048; i + 2048 < v.size(); i += 1)
    {
        if (d[i] < 0.02) continue;
        double around = 0.0;
        for (size_t k = i - 1024; k <= i + 1024; ++k) if (k + 2 < i || k > i + 2) around = std::max(around, d[k]);
        worst = std::max(worst, d[i] / std::max(1e-9, around));
    }
    return worst;
}

// spectrum: alias energy relative to the harmonic energy for a pure input tone
static void fft(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2 * 3.14159265358979 / len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) { std::complex<double> w(1); for (size_t k = 0; k < len / 2; ++k) { auto u = a[i + k], x = a[i + k + len / 2] * w; a[i + k] = u + x; a[i + k + len / 2] = u - x; w *= wl; } }
    }
}
static double aliasDb(const std::vector<float>& v, double f0)
{
    const size_t N = 16384, start = v.size() - N;
    std::vector<std::complex<double>> a(N);
    for (size_t i = 0; i < N; ++i) { const double w = 2 * 3.14159265358979 * i / (N - 1); a[i] = v[start + i] * (0.35875 - 0.48829 * std::cos(w) + 0.14128 * std::cos(2 * w) - 0.01168 * std::cos(3 * w)); }   // Blackman-Harris: leakage below -90 dB
    fft(a);
    double harm = 0.0, other = 0.0;
    const double binHz = SR / N;
    for (size_t k = 2; k < N / 2; ++k)
    {
        const double f = k * binHz, e = std::norm(a[k]);
        const double nearest = std::round(f / f0) * f0;
        if (std::fabs(f - nearest) <= 6 * binHz && nearest > 0) harm += e; else if (f > 40.0) other += e;
    }
    return 10.0 * std::log10(std::max(1e-30, other) / std::max(1e-30, harm));
}

static void writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R)
{
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    const uint32_t n = (uint32_t)L.size(), bytes = n * 4;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32((uint32_t)SR); w32((uint32_t)SR * 4); w16(4); w16(16);
    std::fwrite("data", 1, 4, f); w32(bytes);
    for (uint32_t i = 0; i < n; ++i) { int16_t a = (int16_t)std::lround(std::max(-1.0f, std::min(1.0f, L[i])) * 32767.0f), b = (int16_t)std::lround(std::max(-1.0f, std::min(1.0f, R[i])) * 32767.0f); std::fwrite(&a, 2, 1, f); std::fwrite(&b, 2, 1, f); }
    std::fclose(f);
}

int main(int argc, char** argv)
{
    const std::string out = argc > 1 ? argv[1] : "build/renders";

    // 1. every control changes its parameter and the sound; every button acts
    {
        struct C { int idx; float a, b; const char* name; int topo, stages; };
        const C ctl[] = {{kFuzz, 20, 100, "Fuzz", 0, 0}, {kTone, 0, 100, "Tone", 0, 0}, {kLevel, -12, 6, "Level", 0, 0}, {kBlend, 0, 100, "Blend", 0, 0},
                         {kGuitarVol, 3, 10, "Guitar Vol", 0, 0}, {kInput, -12, 12, "Input", 0, 0}, {kOutput, -12, 6, "Output", 0, 0},
                         {kTransistors, 0, 4, "Transistors", 0, 0}, {kDiodes, 1, 4, "Diodes", 0, 0}, {kDiodeArr, 0, 3, "Diode Arr", 0, 0},
                         {kInputCap, 0, 3, "Input Cap", 0, 0}, {kToneStack, 0, 3, "Tone Stack", 0, 0},
                         {kSag, 0, 90, "Sag", 0, 0}, {kStages, 0, 2, "Stages", 0, 0}, {kTopology, 0, 2, "Topology", 0, 0},
                         {kFeedback, 0, 80, "Feedback", 0, 0}, {kDualMix, 10, 90, "Dual Mix", 5, 1}, {kCoupling, 0, 5, "Coupling", 2, 1},
                         {kResistors, 0, 1, "Resistors", 0, 0}, {kQ2, 0, 5, "Q2", 0, 1}, {kBuffer, 0, 1, "Buffer", 0, 0}};
        std::string bad, detail;
        for (const C& c : ctl)
        {
            std::vector<float> A, B;
            auto setup = [&](Rig& r, float v) {
                r.p.set(kDiodeArr, 0.0f); r.p.set(kTopology, (float)c.topo); r.p.set(kStages, (float)c.stages);
                if (c.idx == kToneStack) r.p.set(kTone, 20.0f);
                if (c.idx == kBuffer) r.p.set(kGuitarVol, 5.0f);
                r.p.set(c.idx, v); };
            { Rig r; setup(r, c.a); r.run(1.0, riff, &A); }
            { Rig r; setup(r, c.b); r.run(1.0, riff, &B); }
            double diff = 0.0; for (size_t i = 0; i < A.size(); ++i) diff += (A[i] - B[i]) * (A[i] - B[i]);
            const double rel = db(std::sqrt(diff / A.size())) - db(rms(A));
            char d[64]; std::snprintf(d, sizeof d, "%s %.0f dB; ", c.name, rel); detail += d;
            if (rel < -40.0 && c.idx != kResistors) bad += std::string(c.name) + " ";
        }
        {   // Resistors: a clipped fuzz masks resistor noise on the notes (as in hardware); the flavour is the hiss floor
            auto hiss = [&](float res) { Rig r; r.p.set(kResistors, res); r.p.set(kFuzz, 90.0f); std::vector<float> L; r.run(1.5, [](long) { return 0.0f; }, &L); return db(rms(L, (size_t)(0.5 * SR))); };
            const double hm = hiss(0.0f), hc = hiss(1.0f);
            char hd[96]; std::snprintf(hd, sizeof hd, "Resistors hiss in silence: metal film %.0f, carbon comp %.0f dB; ", hm, hc); detail += hd;
            if (!(hc > hm + 6.0 && hc < -40.0)) bad += "Resistors ";
        }
        check(bad.empty(), "every control changes the sound (difference between two settings, relative to the signal)", detail + (bad.empty() ? "" : " NO CHANGE: " + bad));
        // buttons
        Rig r; const int q0 = r.p.getInt(kTransistors), d0 = r.p.getInt(kDiodes), c0 = r.p.getInt(kInputCap);
        r.p.set(kNextQ, 1.0f); r.p.set(kNextD, 1.0f); r.p.set(kNextC, 1.0f); r.run(0.01, riff);
        const int q1 = r.p.getInt(kTransistors), d1 = r.p.getInt(kDiodes), c1 = r.p.getInt(kInputCap);
        r.p.set(kNextQ, 0.0f); r.run(0.01, riff); const int q2 = r.p.getInt(kTransistors);
        for (int k = 0; k < 4; ++k) { r.p.set(kNextQ, (float)(k & 1)); r.run(0.01, riff); }
        const int q3 = r.p.getInt(kTransistors);
        char d[200]; std::snprintf(d, sizeof d, "Next Q %d->%d->%d (wraps to %d), Next D %d->%d, Next C %d->%d", q0, q1, q2, q3, d0, d1, c0, c1);
        check(q1 == (q0 + 1) % 5 && q2 == (q0 + 2) % 5 && q3 == (q0 + 5) % 5 && d1 == (d0 + 1) % 5 && c1 == (c0 + 1) % 4,
              "the tap-to-step buttons step their slot on every tap and wrap around", d);
    }
    // 2. renders per part: no silence, no clicks, no runaway; level matched across swaps
    {
        std::string detail; bool ok = true; double lo = 1e9, hi = -1e9;
        struct S { int idx; int n; const char* slot; };
        const S slots[] = {{kTransistors, 5, "q"}, {kDiodes, 5, "d"}, {kDiodeArr, 4, "arr"}, {kInputCap, 4, "cap"}, {kToneStack, 5, "tone"}};
        for (const S& s : slots)
            for (int v = 0; v < s.n; ++v)
            {
                Rig r; r.p.set(kDiodeArr, 0.0f); r.p.set(s.idx, (float)v);
                std::vector<float> L, R; r.run(3.0, riff, &L, &R);
                const double lv = db(rms(L, (size_t)SR)), pk = db(peak(L)), cr = clickRatio(L, (size_t)SR);
                lo = std::min(lo, lv); hi = std::max(hi, lv);
                const bool good = lv > -45.0 && pk < -0.5 && cr < 12.0;
                if (!good) { ok = false; char d[120]; std::snprintf(d, sizeof d, "[%s=%d: %.1f dB rms, peak %.1f, click %.1f] ", s.slot, v, lv, pk, cr); detail += d; }
                writeWav(out + "/part_" + s.slot + "_" + std::to_string(v) + ".wav", L, R);
            }
        char d[160]; std::snprintf(d, sizeof d, "every part of every slot: level %.1f .. %.1f dB rms (spread %.1f dB); %s", lo, hi, hi - lo, detail.c_str());
        check(ok, "renders per part: sound, no clicks, no runaway (riff, 3 s each, WAVs written)", d);
        check(hi - lo <= 3.0, "level match: swapping any one part moves the level by 3 dB or less (riff at the calibration settings)", d);
    }
    // 3. level match over 400 random combinations of every slot (measured at run time, as the plugin does), on a riff
    //    the calibration never heard
    {
        std::vector<double> lv; std::string outliers; int nOut = 0; uint32_t rs = 12345u;
        auto rn = [&](int n) { rs = rs * 1664525u + 1013904223u; return (int)((rs >> 8) % (uint32_t)n); };
        for (int i = 0; i < 400; ++i)
        {
            kf::Settings s; s.topo = rn(6); s.stages = 1 + rn(3); s.tr = rn(5); s.tr2 = rn(6) - 1; s.di = rn(5); s.arr = rn(4); s.cap = rn(4);
            s.tone = rn(5); s.coupling = rn(6); s.resistors = rn(2); s.buffer = rn(4) == 0;
            s.fuzz = 0.7f; s.toneKnob = 0.5f; s.guitarVol = 1.0f;
            const float trim = kf::calibrate(&tables(), SR, s);
            std::unique_ptr<kf::Core> cp(new kf::Core()); kf::Core& c = *cp;
            c.prepare(SR, &tables()); c.update(s); c.trim = c.trimTarget = trim;
            std::vector<float> y; for (long t = 0; t < (long)(1.5 * SR); ++t) y.push_back(c.process(riff(t)));
            const double l = db(rms(y, (size_t)(0.3 * SR)));
            lv.push_back(l);
            if (l < -23.0 || l > -17.0)
            {
                if (++nOut <= 30) { char d[120]; std::snprintf(d, sizeof d, "%s x%d %s/%s arr%d cap%d tone%d cpl%d %.1f dB; ", kTopologyNames[s.topo], s.stages, kf::kTransistors[s.tr].name, kf::kDiodes[s.di].name, s.arr, s.cap, s.tone, s.coupling, l); outliers += d; }
            }
        }
        std::sort(lv.begin(), lv.end());
        char d[4000]; std::snprintf(d, sizeof d, "riff level: median %.1f dB, %d of %zu combinations outside -23 .. -17 dB (a 6 dB window); full range %.1f .. %.1f dB. Outside: %s",
                                   lv[lv.size() / 2], nOut, lv.size(), lv.front(), lv.back(), nOut ? outliers.c_str() : "none");
        check(nOut * 100 <= 3 * (int)lv.size(), "level match over random combinations of every slot: at least 97 % within a 6 dB window", d);
    }    // 4. aliasing at 4x: a 3.1 kHz tone at full fuzz, silicon (brightest) and germanium
    {
        std::string detail; bool ok = true;
        for (int tr : {0, 4})
            for (int ar : {3, 0})
            {
                Rig r; r.p.set(kTransistors, (float)tr); r.p.set(kDiodeArr, (float)ar); r.p.set(kDiodes, 1.0f); r.p.set(kFuzz, 100.0f); r.p.set(kToneStack, 4.0f);
                std::vector<float> L; r.run(1.0, [](long t) { return sine(t, 3100.0, 0.25); }, &L);
                const double a = aliasDb(L, 3100.0);
                char d[80]; std::snprintf(d, sizeof d, "%s %s: %.0f dB; ", kf::kTransistors[tr].name, ar == 3 ? "no diodes" : "1N914 pair", a); detail += d;
                if (a > -45.0) ok = false;
            }
        for (int topo = 1; topo < 6; ++topo)
        {   // the other families at 2 stages, full fuzz (the octave doubles 3.1 kHz to 6.2 kHz: still harmonics of 3.1 kHz)
            Rig r; r.p.set(kTopology, (float)topo); r.p.set(kStages, 1.0f); r.p.set(kTransistors, 4.0f); r.p.set(kDiodes, 1.0f); r.p.set(kFuzz, 100.0f); r.p.set(kToneStack, 4.0f);
            std::vector<float> L; r.run(1.0, [](long t) { return sine(t, 3100.0, 0.25); }, &L);
            const double a = aliasDb(L, 3100.0);
            char d[80]; std::snprintf(d, sizeof d, "%s x2: %.0f dB; ", kTopologyNames[topo], a); detail += d;
            if (a > -45.0) ok = false;
        }
        check(ok, "aliasing: folded energy at least 45 dB below the harmonics (3.1 kHz, full fuzz)", detail);
    }
    // 5. warble: a steady tone keeps a steady level
    {
        Rig r; std::vector<float> L; r.run(2.0, [](long t) { return sine(t, 220.0, 0.2); }, &L);
        double lo = 1e9, hi = -1e9;
        for (size_t s = (size_t)(0.5 * SR); s + 2205 < L.size(); s += 2205)
        {
            double a = 0.0; for (size_t i = s; i < s + 2205; ++i) a += (double)L[i] * L[i];
            const double l = db(std::sqrt(a / 2205)); lo = std::min(lo, l); hi = std::max(hi, l);
        }
        char d[80]; std::snprintf(d, sizeof d, "level in 50 ms windows: %.2f dB range", hi - lo);
        check(hi - lo < 0.3, "no warble: a steady tone stays within 0.3 dB", d);
    }
    // 6. Guitar Vol: rolling back cleans up (fewer harmonics), more with germanium than silicon
    {
        auto thd = [&](int tr, float vol) {
            Rig r; r.p.set(kTransistors, (float)tr); r.p.set(kGuitarVol, vol); r.p.set(kToneStack, 4.0f); r.p.set(kDiodeArr, 3.0f);
            std::vector<float> L; r.run(1.0, [](long t) { return sine(t, 196.0, 0.25); }, &L);
            return aliasDb(L, 196.0) * 0.0 + [&] {      // harmonic content: energy above the fundamental vs the fundamental
                const size_t N = 16384, st = L.size() - N; std::vector<std::complex<double>> a(N);
                for (size_t i = 0; i < N; ++i) a[i] = L[st + i] * (0.5 - 0.5 * std::cos(2 * 3.14159265358979 * i / (N - 1)));
                fft(a); double f1 = 0, hs = 0; const double bin = SR / N;
                for (size_t k = 2; k < N / 2; ++k) { const double f = k * bin, e = std::norm(a[k]); if (std::fabs(f - 196.0) < 4 * bin) f1 += e; else if (f > 300.0) hs += e; }
                return 10.0 * std::log10(hs / f1); }(); };
        const double ge10 = thd(0, 10.0f), ge5 = thd(0, 5.0f), si10 = thd(4, 10.0f), si5 = thd(4, 5.0f);
        char d[200]; std::snprintf(d, sizeof d, "harmonics vs fundamental: OC44 %.1f -> %.1f dB (vol 10 -> 5), 2N5088 %.1f -> %.1f dB", ge10, ge5, si10, si5);
        check(ge5 < ge10 - 3.0 && si5 < si10, "Guitar Vol cleans up when rolled back", d);
    }
    // 7. Vol Follow: LEARN sets full volume; a lasting drop moves it, a decaying note does not
    {
        Rig r; r.p.set(kVolFollow, 1.0f); r.p.set(kGuitarVol, 0.0f);          // in Auto the knob is the floor: 0 lets it follow all the way
        auto plucks = [](double ampDb) { return [ampDb](long t) { const double a = std::pow(10.0, ampDb / 20.0); const long per = (long)(0.5 * SR); const double age = (t % per) / SR;
                                                                  return (float)(a * std::sin(2 * 3.14159265358979 * 196.0 * age) * std::exp(-age * 3.0) * std::min(1.0, age * 300.0)); }; };
        r.p.set(kLearn, 1.0f); r.run(4.5, plucks(-10.0));
        const std::string learned = r.text(kStatus);
        r.run(2.0, plucks(-10.0)); const std::string full = r.text(kVolNow);
        r.run(3.0, plucks(-22.0)); const std::string low = r.text(kVolNow);
        // one long decaying note at full level: the estimate must not slide down as it fades
        r.run(2.0, plucks(-10.0)); const std::string back = r.text(kVolNow);
        Rig d; d.p.set(kVolFollow, 1.0f); d.p.set(kGuitarVol, 0.0f); d.p.set(kLearn, 1.0f); d.run(4.5, plucks(-10.0));
        d.run(0.6, plucks(-10.0)); const std::string before = d.text(kVolNow);
        d.run(3.0, [](long t) { const double age = t / SR; return (float)(0.316 * std::sin(2 * 3.14159265358979 * 196.0 * age) * std::exp(-(age - 0.0) * 1.0)); });
        const std::string decayed = d.text(kVolNow);
        auto num = [](const std::string& s) { return std::atof(s.c_str() + 4); };
        char msg[300]; std::snprintf(msg, sizeof msg, "%s; full %s, 12 dB softer %s, full again %s; a 3 s decaying note: %s -> %s",
                                     learned.c_str(), full.c_str(), low.c_str(), back.c_str(), before.c_str(), decayed.c_str());
        check(learned.find("-1") != std::string::npos && num(full) > 9.0 && num(low) < 8.0 && num(back) > 9.0 && std::fabs(num(decayed) - num(before)) < 0.5,
              "Vol Follow: LEARN sets full volume; softer attacks lower it, louder bring it back; a decaying note does not move it", msg);
    }
    // 8. stereo: Blend 0 passes the stereo input through (delayed by the latency), Blend 100 is the mono fuzz in both
    {
        Rig a; a.p.set(kBlend, 0.0f); std::vector<float> L, R; a.run(0.5, riff, &L, &R, 128, true);
        bool exact = true;
        for (long i = 1024; i < (long)L.size(); ++i) if (L[(size_t)i] != riff(i - kDelay) || R[(size_t)i] != -0.5f * riff(i - kDelay)) exact = false;
        Rig b; std::vector<float> L2, R2; b.run(0.5, riff, &L2, &R2, 128, true);
        bool mono = true; for (size_t i = 0; i < L2.size(); ++i) if (L2[i] != R2[i]) mono = false;
        check(exact && mono, "stereo: Blend 0 = the stereo input exactly (35-sample latency); Blend 100 = the mono fuzz in both channels",
              std::string("dry exact ") + (exact ? "yes" : "NO") + ", wet mono " + (mono ? "yes" : "NO"));
    }
    // 9. globals survive a preset load; the project restores exactly, including the learned reference
    {
        Rig r; r.p.set(kInput, 6.0f); r.p.set(kOutput, -3.0f); r.p.set(kGuitarVol, 6.5f); r.p.set(kVolFollow, 1.0f);
        r.p.set(kFuzz, 33.0f); r.p.set(kTransistors, 4.0f); r.p.set(kToneStack, 2.0f);
        r.p.set(kLearn, 1.0f); r.run(4.5, [](long t) { const long per = (long)(0.4 * SR); const double age = (t % per) / SR; return (float)(0.5 * std::sin(2 * 3.14159265358979 * 220.0 * age) * std::exp(-age * 4.0)); });
        const std::string ref = r.text(kStatus);
        const std::vector<uint8_t> chunk = r.p.internalSaveState();
        Rig q; const bool loaded = q.p.internalLoadState(chunk.data(), chunk.size());
        int wrong = 0; for (int i = 0; i < kNumParams; ++i) if (i < kCpu && q.p.getNormalised(i) != r.p.getNormalised(i)) ++wrong;
        const std::string ref2 = q.text(kStatus);
        // a preset load on a session that was not just restored (the framework keeps a restored project's own state)
        r.p.setProgram(0); r.run(0.05, riff);
        const bool kept = std::fabs(r.p.get(kInput) - 6.0f) < 1e-3f && std::fabs(r.p.get(kOutput) + 3.0f) < 1e-3f && std::fabs(r.p.get(kGuitarVol) - 6.5f) < 1e-3f && r.p.getInt(kVolFollow) == 1;
        const bool reset = std::fabs(r.p.get(kFuzz) - 70.0f) < 1e-3f && r.p.getInt(kTransistors) == 0;
        check(loaded && wrong == 0 && ref == ref2 && kept && reset, "project restore is exact (all parameters and the learned reference); a preset load keeps Input, Output, Guitar Vol, Vol Follow",
              "restored wrong: " + std::to_string(wrong) + "; reference " + ref + " / " + ref2 + "; globals kept " + (kept ? "yes" : "NO") + ", sound reset to the preset " + (reset ? "yes" : "NO"));
    }
    // 10. robustness: NaN / inf input, suspend / resume - the next note always plays
    {
        Rig r; r.run(0.2, riff);
        r.run(0.01, [](long t) { return (t % 3) ? NAN : INFINITY; });
        std::vector<float> L; r.run(0.5, riff, &L);
        bool finite = true; for (float v : L) finite = finite && std::isfinite(v);
        r.p.reset(); std::vector<float> L2; r.run(0.5, riff, &L2);
        check(finite && rms(L, 4410) > 0.01 && rms(L2, 4410) > 0.01, "NaN / inf input and suspend / resume: output stays finite and the fuzz keeps playing");
    }
    // 12. 0.2: diodes are in the circuit by default; the board shows empty sockets for None
    {
        Rig a; std::vector<float> A, B; a.run(1.0, riff, &A);
        Rig b; b.p.set(kDiodes, 4.0f); b.run(1.0, riff, &B);                     // 1N34A -> LED at the default settings
        double diff = 0.0; for (size_t i = 0; i < A.size(); ++i) diff += (A[i] - B[i]) * (A[i] - B[i]);
        const double rel = db(std::sqrt(diff / A.size())) - db(rms(A));
        Rig c; c.p.set(kDiodeArr, 3.0f); c.run(0.2, riff); const float none = c.p.getNormalised(kBoardD);
        c.p.set(kDiodeArr, 0.0f); c.p.set(kDiodes, 2.0f); c.run(0.2, riff); const float d2 = c.p.getNormalised(kBoardD);
        char d[160]; std::snprintf(d, sizeof d, "default arrangement %d (0 = Pair); 1N34A -> LED changes the sound by %.0f dB; board picture: None %.2f (empty), 1N6263 %.2f",
                                   Rig().p.getInt(kDiodeArr), rel, none, d2);
        check(Rig().p.getInt(kDiodeArr) == 0 && rel > -30.0 && std::fabs(none - 1.0f) < 1e-4f && std::fabs(d2 - 0.4f) < 1e-4f,
              "diodes default to Pair (they change the sound); the board shows empty sockets for None", d);
    }
    // 13. Random makes a new sound (never the globals); Undo restores it exactly
    {
        Rig r; r.p.set(kInput, 5.0f); r.p.set(kOutput, -2.0f); r.p.set(kGuitarVol, 7.0f); r.p.set(kVolFollow, 1.0f); r.run(0.05, riff);
        float before[kNumParams]; for (int i = 0; i < kNumParams; ++i) before[i] = r.p.getNormalised(i);
        int changedSound = 0;
        for (int k = 0; k < 6; ++k) { r.p.set(kRandom, (float)((k + 1) & 1)); r.run(0.05, riff); }
        for (int i : kSoundParams) if (r.p.getNormalised(i) != before[i]) ++changedSound;
        const bool globals = r.p.getNormalised(kInput) == before[kInput] && r.p.getNormalised(kOutput) == before[kOutput] &&
                             r.p.getNormalised(kGuitarVol) == before[kGuitarVol] && r.p.getNormalised(kVolFollow) == before[kVolFollow];
        float afterRandom[kNumParams]; for (int i = 0; i < kNumParams; ++i) afterRandom[i] = r.p.getNormalised(i);
        r.p.set(kRandom, 1.0f); r.run(0.05, riff);                       // one more Random, then Undo: back to afterRandom
        const std::string pt = r.text(kPatch);
        r.p.set(kUndo, 1.0f); r.run(0.05, riff);
        int wrong = 0; for (int i : kSoundParams) if (r.p.getNormalised(i) != afterRandom[i]) ++wrong;
        bool sane = true; std::vector<float> L; r.run(1.0, riff, &L); for (float v : L) sane = sane && std::isfinite(v);
        check(changedSound >= 4 && globals && wrong == 0 && sane && rms(L, 4410) > 0.01,
              "Random changes the sound (never Input, Output, Guitar Vol, Vol Follow); Undo restores the sound exactly",
              "sound settings changed " + std::to_string(changedSound) + "; globals kept " + (globals ? "yes" : "NO") + "; '" + pt + "'; after Undo " + std::to_string(wrong) + " differ");
    }
    // 14. Save: a named preset joins the preset list and comes back after a restart
    {
        std::remove("./user-presets.bin");
        Rig r; r.p.set(kRandom, 1.0f); r.run(0.05, riff);
        const float fz = r.p.get(kFuzz); const int tr = r.p.getInt(kTransistors);
        r.p.set(kSave, 1.0f); r.run(0.15, riff);                          // start naming (readouts refresh at 10 Hz)
        const std::string draft = r.text(kName);
        r.p.set(kNamePos, 0.0f); r.run(0.02, riff); r.p.set(kLetter, (float)(std::strchr(kCharset, 'W') - kCharset)); r.run(0.02, riff);
        r.p.set(kNamePos, 1.0f); r.run(0.02, riff); r.p.set(kLetter, (float)(std::strchr(kCharset, 'K') - kCharset)); r.run(0.15, riff);
        const std::string typed = r.text(kName);
        r.p.set(kSave, 0.0f); r.run(0.05, riff);                          // confirm
        for (int k = 0; k < 20 && r.text(kName).rfind("Saved", 0) != 0; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); r.run(0.01, riff); }
        const int slot1 = r.p.numPrograms() - kNumUser;                    // the first user slot, after the factory presets
        const std::string saved = r.text(kName), listed = r.p.programName(slot1);
        Rig q;                                                             // "restart"
        const std::string again = q.p.programName(slot1);
        q.p.setProgram(slot1); q.run(0.05, riff);
        const bool same = std::fabs(q.p.get(kFuzz) - fz) < 1e-3f && q.p.getInt(kTransistors) == tr;
        std::remove("./user-presets.bin");
        char d[300]; std::snprintf(d, sizeof d, "draft '%s', typed '%s', '%s'; listed as the first user preset '%s'; after restart '%s', loads the same sound %s",
                                   draft.c_str(), typed.c_str(), saved.c_str(), listed.c_str(), again.c_str(), same ? "yes" : "NO");
        check(draft.rfind("Random 01", 0) == 0 && typed.rfind("W[K]", 0) == 0 && saved.rfind("Saved: WKndom 01", 0) == 0 && listed == "WKndom 01" && again == listed && same,
              "Save: name it, store it; it is listed in the MPC's preset list and loads after a restart", d);
    }
    // 15. every factory preset: sound, no clicks, no runaway, level in a sensible window (WAVs written)
    {
        std::string detail; bool ok = true; double lo = 1e9, hi = -1e9; int n = 0;
        Rig probe; const int nf = probe.p.numPrograms() - kNumUser;
        for (int pr = 0; pr < nf; ++pr)
        {
            Rig r; r.p.setProgram(pr); r.settle();
            std::vector<float> L, R; r.run(3.0, riff, &L, &R);
            const double lv = db(rms(L, (size_t)SR)), pk = db(peak(L)), cr = clickRatio(L, (size_t)SR);
            bool fin = true; for (float v : L) fin = fin && std::isfinite(v);
            lo = std::min(lo, lv); hi = std::max(hi, lv); ++n;
            const bool good = fin && lv > -40.0 && pk < 0.0 && cr < 12.0;
            if (!good) { ok = false; char d[160]; std::snprintf(d, sizeof d, "[%s: %.1f dB rms, peak %.1f, click %.1f] ", r.p.programName(pr), lv, pk, cr); detail += d; }
            std::string nm(r.p.programName(pr)); for (char& ch : nm) if (!std::isalnum((unsigned char)ch)) ch = '_';
            writeWav(out + "/preset_" + std::to_string(pr) + "_" + nm + ".wav", L, R);
        }
        char d[200]; std::snprintf(d, sizeof d, "%d presets: level %.1f .. %.1f dB rms (spread %.1f dB); %s", n, lo, hi, hi - lo, detail.c_str());
        check(ok, "renders per factory preset: sound, no clicks, no runaway (riff, 3 s each)", d);
        check(hi - lo <= 12.0, "presets sit within a 12 dB window (Sag, Feedback and Blend are part of the sound)", d);
    }
    // 16. per circuit family and stage count: sound, no clicks, no runaway, level matched
    {
        std::string detail; bool ok = true; double lo = 1e9, hi = -1e9;
        for (int topo = 0; topo < 6; ++topo)
            for (int st = 0; st < 3; ++st)
            {
                Rig r; r.p.set(kTopology, (float)topo); r.p.set(kStages, (float)st); r.settle();
                std::vector<float> L, R; r.run(2.0, riff, &L, &R);
                const double lv = db(rms(L, (size_t)SR)), pk = db(peak(L)), cr = clickRatio(L, (size_t)SR);
                lo = std::min(lo, lv); hi = std::max(hi, lv);
                if (!(lv > -40.0 && pk < 0.0 && cr < 12.0)) { ok = false; char d[120]; std::snprintf(d, sizeof d, "[%s x%d: %.1f dB, peak %.1f, click %.1f] ", kTopologyNames[topo], st + 1, lv, pk, cr); detail += d; }
                writeWav(out + "/circuit_" + kTopologyNames[topo] + "_x" + std::to_string(st + 1) + ".wav", L, R);
            }
        char d[200]; std::snprintf(d, sizeof d, "18 circuits: level %.1f .. %.1f dB rms (spread %.1f dB); %s", lo, hi, hi - lo, detail.c_str());
        check(ok && hi - lo <= 6.0, "every family at 1, 2, 3 stages: sound, no clicks, no runaway, within 6 dB of each other", d);
    }
    // 17. Sag ramps in gradually: 1 % is nearly the fresh battery, the change grows with the knob, never a hard gate
    //     at low settings; near the top quiet notes sputter (much quieter) while loud ones still play
    {
        auto render = [&](float sag, double amp) { Rig r; r.p.set(kSag, sag); r.p.set(kToneStack, 4.0f); std::vector<float> L;
            r.run(1.5, [amp](long t) { return (float)(amp / 0.17 * riff(t)); }, &L); return L; };
        const std::vector<float> s0 = render(0, 0.17);
        std::string d; double prev = -200.0; bool mono = true; double at1 = 0.0;
        for (float sg : {1.0f, 10.0f, 25.0f, 50.0f, 75.0f, 100.0f})
        {
            const std::vector<float> s = render(sg, 0.17);
            double diff = 0.0; for (size_t i = (size_t)(0.5 * SR); i < s.size(); ++i) diff += (s[i] - s0[i]) * (s[i] - s0[i]);
            const double rel = db(std::sqrt(diff / (s.size() - 0.5 * SR))) - db(rms(s0, (size_t)(0.5 * SR)));
            if (sg == 1.0f) at1 = rel;
            if (rel < prev - 1.0) mono = false;
            prev = rel;
            char b[48]; std::snprintf(b, sizeof b, "%.0f%%: %.0f dB; ", sg, rel); d += b;
        }
        const double q0 = db(rms(render(0, 0.02), (size_t)(0.5 * SR))), q10 = db(rms(render(10, 0.02), (size_t)(0.5 * SR))), q95 = db(rms(render(95, 0.02), (size_t)(0.5 * SR)));
        char b[200]; std::snprintf(b, sizeof b, "| a quiet riff (-18 dB): Sag 0 %.1f, 10 %.1f, 95 %.1f dB rms", q0, q10, q95); d += b;
        check(at1 < -35.0 && mono && std::fabs(q10 - q0) < 1.5 && q95 < q0 - 4.0 && q95 > -90.0,
              "Sag: 1 % is near-identical, the change grows with the knob; a quiet note is not gated at 10 % but sputters near the top", d);
    }
    // 18. Feedback: self-oscillates at the top yet stays under the ceiling, with and without input
    {
        Rig r; r.p.set(kFeedback, 100.0f); r.p.set(kFuzz, 100.0f); std::vector<float> L;
        r.run(2.0, riff, &L); r.run(1.0, [](long) { return 0.0f; }, &L);
        bool fin = true; for (float v : L) fin = fin && std::isfinite(v);
        const double pk = db(peak(L)), tail = db(rms(L, L.size() - (size_t)(0.5 * SR)));
        char d[160]; std::snprintf(d, sizeof d, "peak %.1f dBFS, %.1f dB rms in silence after playing (self-oscillation)", pk, tail);
        check(fin && pk <= -0.5 && tail > -50.0, "Feedback at 100 %: it squeals on its own and the limiter keeps it under the ceiling", d);
    }
    // 19. level matching follows a change of circuit within half a second (the worker thread)
    {
        Rig r; r.settle(); r.p.set(kTopology, 2.0f); r.p.set(kStages, 2.0f);
        int ms = 0; r.run(0.02, riff);
        while (!r.p.leveledForTest() && ms < 2000) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); r.run(0.01, riff); ms += 20; }
        const std::string c = r.text(kCircuit);
        check(r.p.leveledForTest() && ms <= 500, "level matching: a new circuit is measured within half a second", "'" + c + "' after about " + std::to_string(ms) + " ms");
    }
    // 20. switching while playing (the 0.2 report: the diode arrangement hissed for a few milliseconds when changed): every
    //     slot and circuit switched mid-riff; the 50 ms after a switch may not be louder than the playing before it
    //     (no burst), measured in 2 ms windows against the loudest 2 ms window of the half-second before
    {
        struct Sw { int idx, from, to; const char* name; };
        const Sw sw[] = {{kDiodeArr, 0, 3, "Diode Arr Pair->None"}, {kDiodeArr, 3, 1, "Diode Arr None->Single"}, {kDiodeArr, 1, 2, "Diode Arr Single->2+1"},
                         {kDiodes, 0, 4, "Diodes 1N34A->LED"}, {kTransistors, 0, 4, "Q OC44->2N5088"}, {kInputCap, 3, 0, "Cap 100uF->1n5"},
                         {kToneStack, 3, 0, "Tone Tilt->Muff"}, {kTopology, 0, 2, "Face->Muff"}, {kTopology, 2, 4, "Muff->Octave"},
                         {kTopology, 0, 5, "Face->Dual"}, {kStages, 0, 2, "Stages 1->3"}, {kCoupling, 4, 0, "Coupling 10u->10n"}, {kQ2, 0, 3, "Q2 Same->2N3904"}};
        std::string detail; bool ok = true; double worstJump = -99.0;
        for (const Sw& s : sw)
        {
            Rig r; r.p.set(s.idx, (float)s.from); if (s.idx == kQ2) r.p.set(kStages, 1.0f); r.settle();
            std::vector<float> A, B; r.run(1.0, riff, &A);
            r.p.set(s.idx, (float)s.to);
            r.run(0.05, [&](long t) { return riff(t); }, &B);
            const size_t w = (size_t)(0.002 * SR);
            auto loudest = [&](const std::vector<float>& v, size_t from, size_t to) { double m = 0.0; for (size_t i = from; i + w <= to; i += w / 2) { double a = 0.0; for (size_t k = i; k < i + w; ++k) a += (double)v[k] * v[k]; m = std::max(m, a / w); } return 10.0 * std::log10(m + 1e-30); };
            const double before = loudest(A, A.size() - (size_t)(0.5 * SR), A.size()), after = loudest(B, 0, B.size());
            bool fin = true; for (float v : B) fin = fin && std::isfinite(v);
            worstJump = std::max(worstJump, after - before);
            if (!fin || after > before + 3.0) { ok = false; char d[120]; std::snprintf(d, sizeof d, "[%s: +%.1f dB] ", s.name, after - before); detail += d; }
        }
        char d[300]; std::snprintf(d, sizeof d, "13 switches mid-riff: the loudest 2 ms after a switch is at most %+.1f dB against the playing before (limit +3 dB) %s", worstJump, detail.c_str());
        check(ok, "switching parts and circuits while playing: no burst or hiss", d);
    }
    // 11. CPU (desktop; the Force is roughly 15-25x slower)
    {
        // only process() is timed (the test harness's own buffers are prepared outside the clock)
        // the default sound is checked against the budget; the heaviest circuit (Dual: two paths of 3 stages at 8x,
        // Sag moving the battery, Feedback, carbon comp noise) is reported
        auto measure = [&](bool heavy) {
        KashyyykFuzz p; p.prepare(SR, 128); p.set(kDiodeArr, 0.0f);
        if (heavy) { p.set(kTopology, 5.0f); p.set(kStages, 2.0f); p.set(kSag, 60.0f); p.set(kFeedback, 20.0f); p.set(kResistors, 1.0f); }
        std::vector<float> l(128), r(128); double us = 0.0; long t = 0;
        for (int b = 0; b < (int)(10.0 * SR / 128); ++b)
        {
            for (int i = 0; i < 128; ++i) { l[(size_t)i] = r[(size_t)i] = riff(t + i); } t += 128;
            float* outs[2] = {l.data(), r.data()}; const float* ins[2] = {l.data(), r.data()};
            mpc::ProcessContext ctx; ctx.inputs = ins; ctx.outputs = outs; ctx.numInputs = 2; ctx.numOutputs = 2; ctx.numFrames = 128; ctx.sampleRate = SR;
            const auto t0 = std::chrono::steady_clock::now(); p.process(ctx);
            us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        }
        return us / 10e6 * 100.0; };
        const double pct = measure(false), worst = measure(true);
        char d[300]; std::snprintf(d, sizeof d, "default sound %.2f %% of real time on this desktop -> Force estimate %.0f .. %.0f %% (budget 15 %%); heaviest (Dual x3 8x + Sag + Feedback + carbon) %.2f %% -> MPC about %.0f %% (0.2 measured 1.9x desktop)", pct, pct * 15, pct * 25, worst, worst * 1.9);
        check(pct * 15 < 15.0, "CPU: the default sound's Force estimate (low end) is within the SDK budget", d);
    }
    std::printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
