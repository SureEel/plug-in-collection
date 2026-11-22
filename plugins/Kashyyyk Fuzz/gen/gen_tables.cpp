// Kashyyyk Fuzz - offline table generator. Solves every stage with the exact models in kf_circuit.h at the five battery
// voltages Sag can leave (each part bias-trimmed at a fresh 9 V, as built, then starved), and writes src/kf_tables.inc.
// Level-match trims are measured at run time (kf::calibrate), since the part / stage combinations are too many.
//   zig c++ -std=c++17 -O2 -I src -o build/gen_tables.exe gen/gen_tables.cpp && build/gen_tables.exe src/kf_tables.inc
#include "kf_dsp.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace kf;

static int failures = 0;
static int failFF[kVccN] = {}, failCE[kVccN] = {}, failMuff[kVccN] = {};   // by battery voltage (diagnostics)

// a stage's transfer at the table's x positions, continued outward from the quiescent point in both directions
template <class Solve>
static void sweep(const Curve& axis, bool mirror, Solve solve, float* out)
{
    const int mid = (kTab - 1) / 2;
    out[mid] = 0.0f;
    for (int dir = -1; dir <= 1; dir += 2)
    {
        solve(0.0, true);                                              // back to the quiescent state
        for (int k = 1; k <= mid; ++k)
        {
            const int idx = mid + dir * k;
            const double x = Curve::xAt(idx, axis.x0, axis.umax);
            const double y = solve(mirror ? -x : x, false);           // germanium parts are PNP: mirrored, y(x) = -T(-x)
            out[idx] = (float)(mirror ? -y : y);
        }
    }
}

static void ffTables(Tables& T, std::string& rep)
{
    Curve axis; axis.setAxis(kFfVmax, kFfX0);
    for (int p = 0; p < kNumTransistors; ++p)
    {
        FuzzFace ff; ff.q = kTransistors[p]; ff.vcc = 9.0;
        const bool ok = ff.trimBias(4.5);
        char line[200];
        std::snprintf(line, sizeof line, "  Fuzz Face %-7s %s Q2 collector trim %6.0f ohm\n", kTransistors[p].name, ok ? "ok  " : "FAIL", ff.Rc2);
        rep += line; if (!ok) ++failures;
        for (int v = 0; v < kVccN; ++v)
        {
            ff.vcc = kVccGrid[v];
            if (!ff.dcPoint()) ++failures;
            for (int a = 0; a < kFz; ++a)
                for (int b = 0; b < kRsN; ++b)
                {
                    FuzzFace::Drive d; d.ac = true; d.Rs = kRsGrid[b]; d.vcin = ff.dc[0]; d.vw = ff.dc[2] * kFzGrid[a];
                    d.Ru = std::max(1.0, (1.0 - kFzGrid[a]) * ff.Rpot);
                    double q0[4]; std::copy(ff.dc, ff.dc + 4, q0); ff.newton(q0, d);
                    double st[4], xp = 0.0;
                    sweep(axis, kTransistors[p].germanium, [&](double x, bool reset) {
                        if (reset) { std::copy(q0, q0 + 4, st); xp = 0.0; return 0.0; }
                        double keep[4]; std::copy(st, st + 4, keep);
                        d.vs = x;
                        if (!ff.newton(st, d, 400))
                        {   // continuation: walk from the last converged input in small steps
                            std::copy(keep, keep + 4, st); bool ok = true;
                            for (int k = 1; k <= 64 && ok; ++k) { d.vs = xp + (x - xp) * k / 64.0; ok = ff.newton(st, d, 2000); }
                            if (!ok) { ++failures; ++failFF[v]; }
                        }
                        xp = x; return st[3] - q0[3]; }, T.ff[p][a][b][v]);
                    if (v == kVccN - 1 && b == 0)
                    {   // the input resistance at this fuzz setting (fresh battery)
                        FuzzFace::Drive dp = d; dp.Rs = 1e9; dp.vs = 0.0; double vp[4]; std::copy(q0, q0 + 4, vp); ff.newton(vp, dp);
                        FuzzFace::Drive dq = dp; dq.vs = 1e-3; double vq[4]; std::copy(vp, vp + 4, vq); ff.newton(vq, dq);
                        const double i0 = (dp.vs + dp.vcin - vp[0]) / dp.Rs, i1 = (dq.vs + dq.vcin - vq[0]) / dq.Rs;
                        T.rinFF[p][a] = (float)std::fabs((vq[0] - vp[0]) / std::max(1e-18, i1 - i0));
                    }
                }
        }
    }
}

// a single-transistor stage (booster / Tone Bender, or Big Muff with diodes in the feedback)
static void ceTable(CEStage st, bool germanium, float (*out)[kTab], float& rin, std::string& rep, const char* what)
{
    Curve axis; axis.setAxis(kCeVmax, kCeX0);
    st.vcc = 9.0;
    const bool ok = st.trimBias(4.5);
    if (!ok) ++failures;
    if (what) { char line[200]; std::snprintf(line, sizeof line, "  %-28s %s feedback trim %9.0f ohm  Vb %.3f Vc %.3f\n", what, ok ? "ok  " : "FAIL", st.Rf, st.dc[0], st.dc[1]); rep += line; }
    {   // input resistance (fresh battery): the small-signal dv(base) / di(in)
        CEStage s2 = st; s2.Rs = 1e9;
        double a[3]; std::copy(st.dc, st.dc + 3, a); s2.newton(a, true, 0.0, st.dc[0]);
        double b[3]; std::copy(a, a + 3, b); s2.newton(b, true, 1e-3, st.dc[0]);
        const double i0 = (0.0 + st.dc[0] - a[0]) / s2.Rs, i1 = (1e-3 + st.dc[0] - b[0]) / s2.Rs;
        rin = (float)std::fabs((b[0] - a[0]) / std::max(1e-18, i1 - i0));
    }
    for (int v = 0; v < kVccN; ++v)
    {
        st.vcc = kVccGrid[v];
        if (!st.dcPoint()) ++failures;
        double q0[3]; std::copy(st.dc, st.dc + 3, q0); st.newton(q0, true, 0.0, st.dc[0]);
        double s[3], xp = 0.0;
        sweep(axis, germanium, [&](double x, bool reset) {
            if (reset) { std::copy(q0, q0 + 3, s); xp = 0.0; return 0.0; }
            double keep[3]; std::copy(s, s + 3, keep);
            if (!st.newton(s, true, x, st.dc[0], 600))
            {   // continuation from the last converged input
                std::copy(keep, keep + 3, s); bool ok = true;
                for (int k = 1; k <= 64 && ok; ++k) ok = st.newton(s, true, xp + (x - xp) * k / 64.0, st.dc[0], 3000);
                if (!ok) { ++failures; (st.Re > 0.0 ? failMuff : failCE)[v]++; }
            }
            xp = x; return s[1] - q0[1]; }, out[v]);
    }
}

static void stageTables(Tables& T, std::string& rep)
{
    for (int p = 0; p < kNumTransistors; ++p)
    {
        CEStage st; st.q = kTransistors[p]; st.Rc = 10000.0; st.Re = 0.0; st.arr = kNone; st.Rs = kStageRs;
        char nm[64]; std::snprintf(nm, sizeof nm, "booster %s", kTransistors[p].name);
        ceTable(st, kTransistors[p].germanium, T.ce[p], T.rinCE[p], rep, nm);
        for (int d = 0; d < kNumDiodes; ++d)
            for (int a = 0; a < kArr; ++a)
            {
                CEStage m; m.q = kTransistors[p]; m.Rc = 15000.0; m.Re = 100.0; m.d = kDiodes[d]; m.arr = a; m.Rs = kStageRs;
                char mn[64]; std::snprintf(mn, sizeof mn, "muff stage %s %s pair", kTransistors[p].name, kDiodes[d].name);
                ceTable(m, kTransistors[p].germanium, T.muff[p][d][a], T.rinMuff[p][d][a], rep, (d == 1 && a == 0) ? mn : nullptr);
            }
    }
}

// Where each stage stops working as the battery drops: the highest supply at which its large-signal swing (a hard
// note: +/-0.3 V in) has fallen to a tenth (-20 dB) of the fresh-battery swing (scanned down from 9 V in 50 mV steps).
// Below the small-signal limit quiet notes are already gated while loud ones break through - the sputter Sag reaches
// near the top of its travel; its travel ends just past this point.
template <class Gain>
static float dyingVoltage(Gain gain)
{
    const double g9 = std::fabs(gain(9.0));
    for (double v = 9.0; v >= 3.0; v -= 0.05) if (std::fabs(gain(v)) < 0.1 * g9) return (float)v;
    return 3.0f;
}
static void vdieTables(Tables& T, std::string& rep)
{
    for (int p = 0; p < kNumTransistors; ++p)
    {
        FuzzFace ff; ff.q = kTransistors[p]; ff.vcc = 9.0; ff.trimBias(4.5);
        T.vdieFF[p] = dyingVoltage([&](double vcc) {
            ff.vcc = vcc; if (!ff.dcPoint()) return 0.0;
            FuzzFace::Drive d; d.ac = true; d.Rs = 4000.0; d.vcin = ff.dc[0]; d.vw = ff.dc[2] * 0.7; d.Ru = 0.3 * ff.Rpot;
            double a[4], b[4]; std::copy(ff.dc, ff.dc + 4, a); std::copy(ff.dc, ff.dc + 4, b);
            for (int k = 1; k <= 60; ++k) { d.vs = -0.3 * k / 60.0; ff.newton(a, d, 400); }
            for (int k = 1; k <= 60; ++k) { d.vs = 0.3 * k / 60.0; ff.newton(b, d, 400); }
            return b[3] - a[3]; });
        auto ceGain = [&](CEStage st) {
            st.vcc = 9.0; st.trimBias(4.5);
            return dyingVoltage([&](double vcc) {
                st.vcc = vcc; if (!st.dcPoint()) return 0.0;
                double a[3], b[3]; std::copy(st.dc, st.dc + 3, a); std::copy(st.dc, st.dc + 3, b);
                for (int k = 1; k <= 60; ++k) st.newton(a, true, -0.3 * k / 60.0, st.dc[0], 400);
                for (int k = 1; k <= 60; ++k) st.newton(b, true, 0.3 * k / 60.0, st.dc[0], 400);
                return b[1] - a[1]; }); };
        CEStage ce; ce.q = kTransistors[p]; ce.Rc = 10000.0; ce.arr = kNone; ce.Rs = kStageRs;
        CEStage mu; mu.q = kTransistors[p]; mu.Rc = 15000.0; mu.Re = 100.0; mu.d = kDiodes[1]; mu.arr = kPair; mu.Rs = kStageRs;
        T.vdieCE[p] = ceGain(ce); T.vdieMuff[p] = ceGain(mu);
        char line[160]; std::snprintf(line, sizeof line, "  %-7s stops working at: Fuzz Face %.2f V, booster %.2f V, muff stage %.2f V\n", kTransistors[p].name, T.vdieFF[p], T.vdieCE[p], T.vdieMuff[p]);
        rep += line;
    }
}

static void diodeTables(Tables& T)
{
    Curve axis, axisOp, axisRe;
    axis.setAxis(kDiVmax, kDiX0); axisOp.setAxis(kOpVmax, kOpX0); axisRe.setAxis(kReVmax, kReX0);
    const int mid = (kTab - 1) / 2;
    for (int p = 0; p < kNumDiodes; ++p)
    {
        for (int a = 0; a < kArr; ++a)
        {
            DiodeClipper c; c.d = kDiodes[p]; c.arr = a;              // to ground, 13.4k: the stage's output resistance + 4.7k
            DiodeClipper o = c; o.R = 100000.0;                         // op-amp: diodes across the 100k feedback resistor
            T.di[p][a][mid] = 0.0f;
            for (int v = 0; v < kVccN; ++v) T.op[p][a][v][mid] = 0.0f;
            for (int dir = -1; dir <= 1; dir += 2)
            {
                double g1 = 0.0, g2 = 0.0;
                for (int k = 1; k <= mid; ++k)
                {
                    const int idx = mid + dir * k;
                    const double y = c.solve(Curve::xAt(idx, axis.x0, axis.umax), g1); g1 = y; T.di[p][a][idx] = (float)y;
                    const double z = o.solve(Curve::xAt(idx, axisOp.x0, axisOp.umax), g2); g2 = z;
                    for (int v = 0; v < kVccN; ++v)
                    {   // then the op-amp's own output swing on a single supply (about 1 V short of each rail)
                        const double rail = std::max(0.3, 0.5 * kVccGrid[v] - 1.0);
                        T.op[p][a][v][idx] = (float)(z / std::pow(1.0 + std::pow(std::fabs(z / rail), 6.0), 1.0 / 6.0));
                    }
                }
            }
        }
        for (int i = 0; i < kTab; ++i) T.rect[p][i] = (float)rectify(kDiodes[p], Curve::xAt(i, axisRe.x0, axisRe.umax));
    }
}

static void emit(FILE* f, const char* name, const float* p, size_t n)
{
    std::fprintf(f, "static const float %s[%zu] = {\n", name, n);
    for (size_t i = 0; i < n; ++i) std::fprintf(f, "%.7g,%s", (double)p[i], (i % 10 == 9) ? "\n" : "");
    std::fprintf(f, "};\n");
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "src/kf_tables.inc";
    std::unique_ptr<Tables> T(new Tables());
    std::string report;
    ffTables(*T, report);
    stageTables(*T, report);
    diodeTables(*T);
    vdieTables(*T, report);
    {   // the level-match estimate: the real engine on the three calibration signals, for every circuit, stage count,
        // transistor, diode and arrangement; then the tone stack, input cap and coupling as factors on a Fuzz Face
        for (auto& a : T->est) for (auto& b : a) for (auto& c : b) for (auto& d : c) for (float& e : d) e = 1.0f;
        for (float& v : T->estTone) v = 1.0f; for (float& v : T->estCap) v = 1.0f; for (float& v : T->estCoupling) v = 1.0f;
        const double fs = 44100.0;
        for (int tp = 0; tp < kNumTopologies; ++tp) for (int st = 1; st <= 3; ++st) for (int q = 0; q < kNumTransistors; ++q)
            for (int d = 0; d < kNumDiodes; ++d) for (int a = 0; a < kArr; ++a)
            {
                Settings s; s.topo = tp; s.stages = st; s.tr = q; s.di = d; s.arr = a; s.tone = kEstTone; s.cap = kEstCap; s.coupling = kEstCoupling;
                T->est[tp][st - 1][q][d][a] = calibrate(T.get(), fs, s);
            }
        Settings b; b.tone = kEstTone; b.cap = kEstCap; b.coupling = kEstCoupling;
        const float base = calibrate(T.get(), fs, b);
        for (int i = 0; i < kNumTones; ++i) { Settings s = b; s.tone = i; T->estTone[i] = calibrate(T.get(), fs, s) / base; }
        for (int i = 0; i < kNumCaps; ++i) { Settings s = b; s.cap = i; T->estCap[i] = calibrate(T.get(), fs, s) / base; }
        for (int i = 0; i < kNumCouplings; ++i) { Settings s = b; s.coupling = i; T->estCoupling[i] = calibrate(T.get(), fs, s) / base; }
        float lo = 1e9f, hi = -1e9f; for (auto& a : T->est) for (auto& bb : a) for (auto& c : bb) for (auto& d : c) for (float e : d) { lo = std::min(lo, e); hi = std::max(hi, e); }
        char line[160]; std::snprintf(line, sizeof line, "  level estimate: %zu combinations, trims %.1f .. %.1f dB\n", sizeof T->est / sizeof(float), 20 * std::log10(lo), 20 * std::log10(hi));
        report += line;
        static const char* tn[kNumTopologies] = {"Face", "Bender", "Muff", "Op-Amp", "Octave", "Dual"};
        int big = 0;
        for (int tp = 0; tp < kNumTopologies; ++tp) for (int st = 0; st < 3; ++st) for (int q = 0; q < kNumTransistors; ++q)
            for (int d = 0; d < kNumDiodes; ++d) for (int a = 0; a < kArr; ++a)
            {
                const float db = 20 * std::log10(T->est[tp][st][q][d][a]);
                if (db > 15.0f) { ++big; if (big <= 25) { std::snprintf(line, sizeof line, "    over +15 dB: %s x%d %s %s arr %d: %+.1f dB\n", tn[tp], st + 1, kTransistors[q].name, kDiodes[d].name, a, db); report += line; } }
            }
        std::snprintf(line, sizeof line, "    %d combinations need more than +15 dB\n", big); report += line;
    }
    FILE* f = std::fopen(path, "w");
    if (!f) { std::printf("cannot write %s\n", path); return 1; }
    std::fprintf(f, "// Generated by gen/gen_tables.cpp - do not edit. Exact circuit solves (kf_circuit.h) at five battery voltages.\n");
    emit(f, "kFfData", &T->ff[0][0][0][0][0], sizeof T->ff / sizeof(float));
    emit(f, "kRinFFData", &T->rinFF[0][0], sizeof T->rinFF / sizeof(float));
    emit(f, "kCeData", &T->ce[0][0][0], sizeof T->ce / sizeof(float));
    emit(f, "kRinCEData", &T->rinCE[0], sizeof T->rinCE / sizeof(float));
    emit(f, "kMuffData", &T->muff[0][0][0][0][0], sizeof T->muff / sizeof(float));
    emit(f, "kRinMuffData", &T->rinMuff[0][0][0], sizeof T->rinMuff / sizeof(float));
    emit(f, "kDiData", &T->di[0][0][0], sizeof T->di / sizeof(float));
    emit(f, "kOpData", &T->op[0][0][0][0], sizeof T->op / sizeof(float));
    emit(f, "kRectData", &T->rect[0][0], sizeof T->rect / sizeof(float));
    emit(f, "kEstData", &T->est[0][0][0][0][0], sizeof T->est / sizeof(float));
    { float v[kNumTones + kNumCaps + kNumCouplings]; int k = 0; for (float x : T->estTone) v[k++] = x; for (float x : T->estCap) v[k++] = x; for (float x : T->estCoupling) v[k++] = x; emit(f, "kEstFactors", v, (size_t)k); }
    { float v[3 * kNumTransistors]; for (int p = 0; p < kNumTransistors; ++p) { v[p] = T->vdieFF[p]; v[kNumTransistors + p] = T->vdieCE[p]; v[2 * kNumTransistors + p] = T->vdieMuff[p]; } emit(f, "kVdieData", v, 3 * kNumTransistors); }
    std::fclose(f);
    std::printf("%s", report.c_str());
    { int a = 0, b = 0, c = 0; for (int v = 0; v < kVccN; ++v) { a += failFF[v]; b += failCE[v]; c += failMuff[v]; } std::printf("non-converged: Fuzz Face %d, booster %d, muff %d\n", a, b, c); }
    std::printf("non-converged solves: %d\nwrote %s\n", failures, path);
    return failures ? 2 : 0;
}
