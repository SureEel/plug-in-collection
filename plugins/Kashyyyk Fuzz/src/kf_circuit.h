// Kashyyyk Fuzz - the analog component models. Everything here is circuit maths with component values;
// nothing is a hand-drawn curve. Runs at table-build time (worker thread) or at control rate, never per sample.
//
//   * Parts: published-style SPICE parameters (saturation current Is, emission coefficient n, series resistance,
//     current gain, germanium leakage). Values are typical for the part type, not one measured specimen.
//   * Diode clipper: series resistor into a diode arrangement to ground. Solved exactly (Newton to machine
//     precision) at every table point from the Shockley equation, both diodes of a pair included.
//   * Fuzz Face stage: the published two-transistor circuit (Q1 collector 33k, Q2 collector 8.2k-class + 470, 100k
//     feedback, 1k Fuzz pot bypassed from its wiper) with Ebers-Moll transistors (transport model, reverse gain,
//     leakage). DC operating point by Newton; the bias trim sets Q2's collector to 4.5 V by trimming the Q2 collector
//     resistor, as builders do (Q2's emitter sits near Q1's Vbe through the 100k feedback, so Q2's current is set by
//     the circuit and only the collector resistor moves the bias).
//     The large-signal transfer is solved QUASI-STATICALLY: coupling / bypass capacitors hold their DC charge while
//     the input sweeps (mid-band behaviour); their frequency effects are separate linear filters.
//   * Guitar + volume pot + cable into the fuzz input: the exact linear network (pickup R + L, 500k audio-taper
//     pot, cable capacitance, input capacitor, the stage's input resistance), divided by the same guitar into a
//     1 Mohm interface at full volume (that is what the recording already contains).
//   * Big Muff tone stack: the passive LP / HP network with the 100k blend pot, solved exactly.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace kf {

constexpr double kVt = 0.025865;           // thermal voltage at 27 C (300.15 K), as SPICE uses
constexpr double kVcc = 9.0;               // a fresh battery (Sag lowers it: see the vcc members)

// ---------------------------------------------------------------------------------------------------------------
// Parts
// ---------------------------------------------------------------------------------------------------------------
struct TransistorPart
{
    const char* name;
    bool germanium;        // germanium parts are PNP in the classic circuits (mirrored here)
    double beta;           // forward current gain (hFE)
    double betaR;          // reverse current gain
    double Is;             // saturation current, A
    double n;              // emission coefficient
    double leak;           // collector-emitter leakage (Iceo), A - large on germanium, negligible on silicon
    double topHz;          // where the stage's response rolls off (junction / Miller capacitance, slow germanium)
};
// OC44, 2N2613, AC107: germanium, low gain, leaky, soft knee, early top-end roll-off.
// 2N3904, 2N5088: silicon, higher gain, sharp knee, extended top end.
inline const TransistorPart kTransistors[] = {
    {"OC44",   true,   75.0, 5.0, 9.0e-7,  1.05, 180e-6,  7000.0},
    {"2N2613", true,   95.0, 5.0, 7.0e-7,  1.05, 90e-6,   7500.0},
    {"AC107",  true,   60.0, 4.0, 5.0e-7,  1.05, 35e-6,   6000.0},
    {"2N3904", false, 180.0, 2.0, 6.7e-15, 1.0,  1e-9,   18000.0},
    {"2N5088", false, 450.0, 2.0, 5.9e-15, 1.0,  1e-9,   14000.0},
};
constexpr int kNumTransistors = 5;

struct DiodePart
{
    const char* name;
    double Is, n, Rs;      // saturation current (A), emission coefficient, series resistance (ohm)
    double IsB;            // the second diode of a pair (equal to Is unless the part is known to mismatch)
};
// Forward drop at 1 mA from these values: 1N34A 0.29 V, 1N914 0.59 V, 1N6263 0.38 V, D9B 0.28 / 0.25 V, LED 1.67 V.
inline const DiodePart kDiodes[] = {
    {"1N34A",  2.0e-7,  1.30, 30.0, 2.0e-7},     // germanium point contact: low drop, soft knee
    {"1N914",  2.52e-9, 1.752, 0.568, 2.52e-9},  // silicon switching diode: sharper
    {"1N6263", 1.0e-9,  1.05, 25.0, 1.0e-9},     // Schottky: between germanium and silicon, very sharp knee
    {"D9B",    5.0e-7,  1.40, 40.0, 1.3e-6},     // Russian germanium: the pair is mismatched -> asymmetric
    {"LED",    1.0e-17, 2.0,  10.0, 1.0e-17},    // red LED: high drop, little compression
};
constexpr int kNumDiodes = 5;
enum DiodeArrangement { kPair = 0, kSingle = 1, kTwoOne = 2, kNone = 3 };   // kNone: the transistor stage alone clips

inline const double kInputCaps[] = {1.5e-9, 10e-9, 47e-9, 100e-6};
inline const char* const kInputCapNames[] = {"1n5", "10n", "47n", "100uF"};
constexpr int kNumInputCaps = 4;

// ---------------------------------------------------------------------------------------------------------------
// Small linear-algebra helpers
// ---------------------------------------------------------------------------------------------------------------
inline double safeExp(double x) { return x < 60.0 ? std::exp(x) : std::exp(60.0) * (1.0 + (x - 60.0)); }

// Solve A x = b (n <= 4) by Gaussian elimination with partial pivoting. Returns false if singular.
inline bool solve4(double A[4][4], double b[4], int n)
{
    for (int c = 0; c < n; ++c)
    {
        int p = c;
        for (int r = c + 1; r < n; ++r) if (std::fabs(A[r][c]) > std::fabs(A[p][c])) p = r;
        if (std::fabs(A[p][c]) < 1e-300) return false;
        if (p != c) { for (int k = 0; k < n; ++k) std::swap(A[c][k], A[p][k]); std::swap(b[c], b[p]); }
        for (int r = c + 1; r < n; ++r)
        {
            const double f = A[r][c] / A[c][c];
            for (int k = c; k < n; ++k) A[r][k] -= f * A[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int r = n - 1; r >= 0; --r)
    {
        double s = b[r];
        for (int k = r + 1; k < n; ++k) s -= A[r][k] * b[k];
        b[r] = s / A[r][r];
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Diode clipper: Vin -> R -> node -> diodes to ground. Exact solve of (Vin - v)/R = i_diodes(v).
// ---------------------------------------------------------------------------------------------------------------
struct DiodeClipper
{
    DiodePart d; int arr = kPair; double R = 13400.0;     // series: the stage's output resistance + 4.7k
    // diode current at junction voltage v (positive = into the forward diode)
    double cur(double v, double& g) const
    {
        double i = 0.0; g = 1e-12;
        const double vtA = d.n * kVt, vtB = d.n * kVt;
        switch (arr)
        {
            case kPair:   // antiparallel pair
            {
                const double ea = safeExp(v / vtA), eb = safeExp(-v / vtB);
                i = d.Is * (ea - 1.0) - d.IsB * (eb - 1.0);
                g += d.Is * ea / vtA + d.IsB * eb / vtB;
                break;
            }
            case kSingle: // one diode: only the positive side clips
            {
                const double ea = safeExp(v / vtA);
                i = d.Is * (ea - 1.0); g += d.Is * ea / vtA;
                break;
            }
            case kTwoOne: // two in series one way (twice the drop), one the other way
            {
                const double ea = safeExp(v / (2.0 * vtA)), eb = safeExp(-v / vtB);
                i = d.Is * (ea - 1.0) - d.IsB * (eb - 1.0);
                g += d.Is * ea / (2.0 * vtA) + d.IsB * eb / vtB;
                break;
            }
            default: break;
        }
        return i;
    }
    // output voltage for an input voltage (exact, includes the diode's own series resistance)
    double solve(double vin, double guess) const
    {
        if (arr == kNone) return vin;
        const double Rt = R + d.Rs;
        double v = guess;
        for (int it = 0; it < 200; ++it)
        {
            double g; const double i = cur(v, g);
            const double f = (vin - v) / Rt - i, df = -1.0 / Rt - g;
            double dv = -f / df;
            dv = std::max(-0.1, std::min(0.1, dv));
            v += dv;
            if (std::fabs(dv) < 1e-12) break;
        }
        double g; const double i = cur(v, g);
        return v + i * d.Rs;              // the node includes the diode's series resistance
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Fuzz Face stage with Ebers-Moll transistors.
// Nodes: v[0] Q1 base, v[1] Q1 collector = Q2 base, v[2] Q2 emitter, v[3] Q2 collector. NPN convention.
// ---------------------------------------------------------------------------------------------------------------
struct FuzzFace
{
    TransistorPart q;
    double R1 = 33000.0;       // Q1 collector (trimmed for bias)
    double Rc2 = 8200.0 + 470.0, Rf = 100000.0, Rpot = 1000.0;
    double vcc = kVcc;          // the supply (trimmed at 9 V; Sag starves it)

    struct Bjt { double ic, ib, dIc_be, dIc_bc, dIb_be, dIb_bc; };
    Bjt bjt(double vbe, double vbc) const
    {
        const double nvt = q.n * kVt;
        const double eF = safeExp(vbe / nvt), eR = safeExp(vbc / nvt);
        const double iF = q.Is * (eF - 1.0), iR = q.Is * (eR - 1.0);
        const double gF = q.Is * eF / nvt, gR = q.Is * eR / nvt;
        Bjt b;
        b.ic = iF - iR * (1.0 + 1.0 / q.betaR) + q.leak;
        b.ib = iF / q.beta + iR / q.betaR;
        b.dIc_be = gF; b.dIc_bc = -gR * (1.0 + 1.0 / q.betaR);
        b.dIb_be = gF / q.beta; b.dIb_bc = gR / q.betaR;
        return b;
    }

    // ac = false: DC operating point (input capacitor open, emitter through the whole pot).
    // ac = true: quasi-static: source vs through Rs; Cin holds vcin; the pot wiper is held at vw by its capacitor.
    struct Drive { bool ac = false; double vs = 0.0, Rs = 1e4, vcin = 0.0, vw = 0.0, Ru = 1.0; };

    void residual(const double* v, const Drive& d, double* F, double J[4][4]) const
    {
        const Bjt a = bjt(v[0] - 0.0, v[0] - v[1]);          // Q1: b=v0, c=v1, e=0
        const Bjt b = bjt(v[1] - v[2], v[1] - v[3]);         // Q2: b=v1, c=v3, e=v2
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) J[r][c] = 0.0;
        // F0: node Q1 base: i_in + (v2 - v0)/Rf - Ib1 = 0
        double iin = 0.0, diin = 0.0;
        if (d.ac) { iin = (d.vs + d.vcin - v[0]) / d.Rs; diin = -1.0 / d.Rs; }
        F[0] = iin + (v[2] - v[0]) / d_rf() - a.ib;
        J[0][0] = diin - 1.0 / d_rf() - (a.dIb_be + a.dIb_bc);
        J[0][1] = a.dIb_bc;
        J[0][2] = 1.0 / d_rf();
        // F1: node Q1 collector / Q2 base: (Vcc - v1)/R1 - Ic1 - Ib2 = 0
        F[1] = (vcc - v[1]) / R1 - a.ic - b.ib;
        J[1][0] = -(a.dIc_be + a.dIc_bc);
        J[1][1] = -1.0 / R1 + a.dIc_bc - (b.dIb_be + b.dIb_bc);
        J[1][2] = b.dIb_be;
        J[1][3] = b.dIb_bc;
        // F2: node Q2 emitter: Ie2 - (v2 - v0)/Rf - i_emitter = 0, Ie2 = Ic2 + Ib2
        double iem, diem;
        if (d.ac) { iem = (v[2] - d.vw) / d.Ru; diem = 1.0 / d.Ru; }
        else      { iem = v[2] / Rpot;          diem = 1.0 / Rpot; }
        F[2] = (b.ic + b.ib) - (v[2] - v[0]) / d_rf() - iem;
        const double dIe_be = b.dIc_be + b.dIb_be, dIe_bc = b.dIc_bc + b.dIb_bc;
        J[2][0] = 1.0 / d_rf();
        J[2][1] = dIe_be + dIe_bc;
        J[2][2] = -dIe_be - 1.0 / d_rf() - diem;
        J[2][3] = -dIe_bc;
        // F3: node Q2 collector: (Vcc - v3)/Rc2 - Ic2 = 0
        F[3] = (vcc - v[3]) / Rc2 - b.ic;
        J[3][1] = -(b.dIc_be + b.dIc_bc);
        J[3][2] = b.dIc_be;
        J[3][3] = -1.0 / Rc2 + b.dIc_bc;
    }
    double d_rf() const { return Rf; }

    bool newton(double* v, const Drive& d, int maxIt = 200) const
    {
        for (int it = 0; it < maxIt; ++it)
        {
            double F[4], J[4][4];
            residual(v, d, F, J);
            double b[4] = {-F[0], -F[1], -F[2], -F[3]};
            if (!solve4(J, b, 4)) return false;
            double m = 0.0;
            for (int k = 0; k < 4; ++k) { const double s = std::max(-0.2, std::min(0.2, b[k])); v[k] += s; m = std::max(m, std::fabs(s)); }
            // the guard band covers the input swing: a reverse-biased base follows the source below ground
            const double lo = -1.0 - (d.ac ? std::fabs(d.vs) + std::fabs(d.vcin) : 0.0), hi = vcc + 1.0 + (d.ac ? std::fabs(d.vs) + std::fabs(d.vcin) : 0.0);
            for (int k = 0; k < 4; ++k) v[k] = std::max(lo, std::min(hi, v[k]));
            if (m < 1e-10) return true;
        }
        return false;
    }

    double dc[4] = {0, 0, 0, 0};
    bool dcPoint()
    {
        const double vbe = q.germanium ? 0.18 : 0.62;
        double v[4] = {vbe, 2.0 * vbe + 0.5, vbe + 0.5, 0.5 * vcc};
        Drive d;
        if (!newton(v, d, 400)) return false;
        std::copy(v, v + 4, dc);
        return true;
    }
    // the bias trim: Q2's collector resistor so Q2's collector sits at `target` volts (the usual Fuzz Face trimmer)
    bool trimBias(double target = 4.5)
    {
        double lo = std::log(1000.0), hi = std::log(40000.0);
        for (int k = 0; k < 50; ++k)
        {
            const double mid = 0.5 * (lo + hi);
            Rc2 = std::exp(mid) + 470.0;
            if (!dcPoint()) return false;
            // larger collector resistor -> larger drop -> collector lower
            if (dc[3] > target) lo = mid; else hi = mid;
        }
        Rc2 = std::exp(0.5 * (lo + hi)) + 470.0;
        return dcPoint() && std::fabs(dc[3] - target) < 0.05;
    }

    // Large-signal transfer at mid-band: Q2 collector (AC part) for source voltages vs in [-vmax, vmax].
    // fuzz01: the Fuzz pot wiper (1 = emitter fully bypassed = most gain). Rs: the source resistance.
    void transfer(double fuzz01, double Rs, double vmax, std::vector<float>& out, double& rin) const
    {
        const int N = (int)out.size();
        Drive d; d.ac = true; d.Rs = Rs; d.vcin = dc[0]; d.vw = dc[2] * fuzz01;
        d.Ru = std::max(1.0, (1.0 - fuzz01) * Rpot);
        const int mid = (N - 1) / 2;
        double v0[4]; std::copy(dc, dc + 4, v0);
        newton(v0, d);                               // the quiescent AC solution (= DC point)
        out[(size_t)mid] = 0.0f;
        const double base = v0[3];
        // small-signal input resistance: dv(base) / di(in) at the quiescent point
        {
            double vp[4]; std::copy(v0, v0 + 4, vp);
            Drive dp = d; dp.vs = 1e-4; newton(vp, dp);
            const double iin = (dp.vs + d.vcin - vp[0]) / Rs, iin0 = (d.vcin - v0[0]) / Rs;
            rin = std::fabs((vp[0] - v0[0]) / std::max(1e-15, iin - iin0));
        }
        for (int dir = -1; dir <= 1; dir += 2)
        {
            double v[4]; std::copy(v0, v0 + 4, v);
            for (int k = 1; k <= mid; ++k)
            {
                d.vs = dir * vmax * (double)k / mid;
                newton(v, d);
                out[(size_t)(mid + dir * k)] = (float)(v[3] - base);
            }
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Single-transistor common-emitter stage with collector-to-base feedback (self-biased), optionally with a diode
// arrangement in the feedback (the Big Muff clipping stage: the diodes sit in series with a 1 uF capacitor from collector
// to base, so they see only the signal swing, never the DC bias) and an emitter resistor.
//   Tone Bender / booster stage: Rc 10k, emitter grounded, no diodes.     Big Muff stage: Rc 15k, Re 100, diodes.
// Driven through Rs from the previous stage (coupling cap holding its DC charge, quasi-static like the Fuzz Face).
// Bias trim: the feedback resistor so the collector sits at half the 9 V supply.
// Nodes: v[0] base, v[1] collector, v[2] emitter.
// ---------------------------------------------------------------------------------------------------------------
struct CEStage
{
    TransistorPart q;
    DiodePart d = kDiodes[1]; int arr = kNone;
    double Rc = 10000.0, Re = 0.0, Rf = 470000.0, Rs = 10000.0, vcc = kVcc;
    double vcap = 0.0;          // the diode capacitor's DC charge (collector - base at the operating point)

    FuzzFace::Bjt bjt(double vbe, double vbc) const { FuzzFace f; f.q = q; return f.bjt(vbe, vbc); }
    // diode current from collector to base for a voltage v = vc - vb
    double diode(double v) const
    {
        if (arr == kNone) return 0.0;
        DiodeClipper c; c.d = d; c.arr = arr; double g; return c.cur(v, g);
    }
    void residual(const double* v, bool ac, double vs, double vcin, double* F) const
    {
        const double ve = Re > 0.0 ? v[2] : 0.0;
        const FuzzFace::Bjt b = bjt(v[0] - ve, v[0] - v[1]);
        const double id = ac ? diode(v[1] - v[0] - vcap) : 0.0;     // the capacitor blocks DC through the diodes
        const double iin = ac ? (vs + vcin - v[0]) / Rs : 0.0;
        F[0] = iin + (v[1] - v[0]) / Rf + id - b.ib;                 // base
        F[1] = (vcc - v[1]) / Rc - (v[1] - v[0]) / Rf - id - b.ic;   // collector
        F[2] = Re > 0.0 ? (b.ic + b.ib) - v[2] / Re : v[2];         // emitter (or pinned to 0)
    }
    bool newton(double* v, bool ac, double vs, double vcin, int maxIt = 300) const
    {
        for (int it = 0; it < maxIt; ++it)
        {
            double F[3], J[4][4] = {};
            residual(v, ac, vs, vcin, F);
            for (int c = 0; c < 3; ++c)
            {   // numerical Jacobian (offline only)
                double w[3] = {v[0], v[1], v[2]}; const double h = 1e-7;
                w[c] += h; double G[3]; residual(w, ac, vs, vcin, G);
                for (int r = 0; r < 3; ++r) J[r][c] = (G[r] - F[r]) / h;
            }
            double b[4] = {-F[0], -F[1], -F[2], 0};
            if (!solve4(J, b, 3)) return false;
            double m = 0.0;
            for (int k = 0; k < 3; ++k) { const double s = std::max(-0.2, std::min(0.2, b[k])); v[k] += s; m = std::max(m, std::fabs(s)); }
            const double sw = ac ? std::fabs(vs) + std::fabs(vcin) : 0.0;     // as in the Fuzz Face: room for the input swing
            for (int k = 0; k < 3; ++k) v[k] = std::max(-1.0 - sw, std::min(vcc + 1.0 + sw, v[k]));
            if (m < 1e-10) return true;
        }
        return false;
    }
    double dc[3] = {0, 0, 0};
    bool dcPoint()
    {
        const double vbe = q.germanium ? 0.18 : 0.62;
        double v[3] = {vbe + 0.1, 0.5 * vcc, 0.05};
        if (!newton(v, false, 0, 0, 600)) return false;
        std::copy(v, v + 3, dc);
        vcap = dc[1] - dc[0];
        return true;
    }
    bool trimBias(double target = 4.5)
    {
        double lo = std::log(5000.0), hi = std::log(2e7);
        for (int k = 0; k < 60; ++k)
        {
            const double mid = 0.5 * (lo + hi);
            Rf = std::exp(mid);
            if (!dcPoint()) { hi = mid; continue; }
            if (dc[1] < target) lo = mid; else hi = mid;              // more feedback resistance -> less base current -> collector higher
        }
        Rf = std::exp(0.5 * (lo + hi));
        return dcPoint() && std::fabs(dc[1] - target) < 0.05;
    }
};

// Full-wave rectifier (the octave): a centre-tapped transformer and two diodes into a 10k load; the output is
// the load voltage for |input|. Exact solve of x = Vd(i) + i R.
inline double rectify(const DiodePart& d, double x)
{
    const double ax = std::fabs(x), R = 10000.0;
    if (ax < 1e-12) return 0.0;
    double i = 0.0;
    for (int it = 0; it < 200; ++it)
    {   // f(i) = n Vt ln(i/Is + 1) + i (R + Rs) - ax
        const double f = d.n * kVt * std::log(i / d.Is + 1.0) + i * (R + d.Rs) - ax;
        const double df = d.n * kVt / (i + d.Is) + (R + d.Rs);
        double di = -f / df;
        if (i + di < 0.0) di = -0.5 * i;
        i += di;
        if (std::fabs(di) < 1e-15) break;
    }
    return i * R;
}

// ---------------------------------------------------------------------------------------------------------------
// Polynomials in s and the bilinear transform (for the exact linear networks)
// ---------------------------------------------------------------------------------------------------------------
struct Poly
{
    double c[6] = {0, 0, 0, 0, 0, 0};       // c[k] * s^k
    int deg = 0;
    static Poly k(double a) { Poly p; p.c[0] = a; return p; }
    static Poly lin(double a, double b) { Poly p; p.c[0] = a; p.c[1] = b; p.deg = 1; return p; }   // a + b s
};
inline Poly operator+(const Poly& a, const Poly& b) { Poly r; r.deg = std::max(a.deg, b.deg); for (int i = 0; i <= r.deg; ++i) r.c[i] = a.c[i] + b.c[i]; return r; }
inline Poly operator*(const Poly& a, const Poly& b) { Poly r; r.deg = std::min(5, a.deg + b.deg); for (int i = 0; i <= a.deg; ++i) for (int j = 0; j <= b.deg && i + j <= 5; ++j) r.c[i + j] += a.c[i] * b.c[j]; return r; }
inline Poly operator*(double s, const Poly& a) { Poly r = a; for (double& x : r.c) x *= s; return r; }

// |N(jw)/D(jw)|
inline double magAt(const Poly& N, const Poly& D, double hz)
{
    const double w = 2.0 * 3.14159265358979 * hz;
    auto ev = [w](const Poly& p, double& re, double& im) {
        re = im = 0.0; double pr = 1.0, pi = 0.0;                     // (jw)^k
        for (int k = 0; k <= p.deg; ++k) { re += p.c[k] * pr; im += p.c[k] * pi; const double nr = -pi * w, ni = pr * w; pr = nr; pi = ni; } };
    double nr, ni, dr, di; ev(N, nr, ni); ev(D, dr, di);
    return std::sqrt((nr * nr + ni * ni) / std::max(1e-300, dr * dr + di * di));
}

// Digital IIR of order <= 5 from an analog N(s)/D(s) by the bilinear transform (direct form II transposed, double).
struct Iir
{
    double b[6] = {1, 0, 0, 0, 0, 0}, a[6] = {1, 0, 0, 0, 0, 0}, z[6] = {0, 0, 0, 0, 0, 0};
    int order = 0;
    void design(Poly N, Poly D, double fs)
    {
        const int n = std::max(N.deg, D.deg);
        order = n;
        const double K = 2.0 * fs;
        // s^k -> K^k (1 - q)^k (1 + q)^(n - k), q = z^-1
        double nb[6] = {0}, na[6] = {0};
        for (int k = 0; k <= n; ++k)
        {
            double poly[6] = {1, 0, 0, 0, 0, 0}; int pd = 0;
            auto mul = [&](double s1) { double t[6] = {0}; for (int i = 0; i <= pd; ++i) { t[i] += poly[i]; t[i + 1] += s1 * poly[i]; } ++pd; std::copy(t, t + 6, poly); };
            for (int i = 0; i < k; ++i) mul(-1.0);
            for (int i = 0; i < n - k; ++i) mul(1.0);
            const double kk = std::pow(K, (double)k);
            for (int i = 0; i <= n; ++i) { nb[i] += N.c[k] * kk * poly[i]; na[i] += D.c[k] * kk * poly[i]; }
        }
        const double a0 = na[0];
        for (int i = 0; i <= n; ++i) { b[i] = nb[i] / a0; a[i] = na[i] / a0; }
        for (int i = n + 1; i < 6; ++i) { b[i] = 0.0; a[i] = 0.0; }
    }
    void reset() { for (double& v : z) v = 0.0; }
    inline double run(double x)
    {
        double y = b[0] * x + z[0];
        if (std::fabs(y) < 1e-20) y = 0.0;                 // no denormals as a signal dies away
        for (int i = 1; i <= order; ++i) { double v = b[i] * x - a[i] * y + (i < order ? z[i] : 0.0); z[i - 1] = std::fabs(v) < 1e-25 ? 0.0 : v; }
        return y;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// The guitar into the fuzz input (pickup R + L, 500k audio-taper volume, cable C, input capacitor, stage input R),
// relative to the same guitar into a 1 Mohm interface at full volume.
// ---------------------------------------------------------------------------------------------------------------
struct Guitar
{
    double Rp = 6500.0, Lp = 2.5, Cc = 470e-12, Rpot = 500000.0, Rint = 1.0e6;
    // audio (log) taper: 10 % of the resistance at half travel; v01 in 0..1
    static double taper(double v01) { const double k = 4.394449; return std::max(1e-3, (std::exp(k * v01) - 1.0) / (std::exp(k) - 1.0)); }
    // Thevenin source resistance seen by the fuzz at mid-band (real part at 1 kHz)
    double sourceR(double v01) const
    {
        const double t = taper(v01), R2 = t * Rpot, R1 = (1.0 - t) * Rpot;
        const double w = 2.0 * 3.14159265358979 * 1000.0;
        // Zth = R2 || (R1 + Rp + jwLp)
        const double ar = R1 + Rp, ai = w * Lp;
        const double nr = R2 * ar, ni = R2 * ai, dr = R2 + ar, di = ai;
        const double den = dr * dr + di * di;
        return std::max(100.0, (nr * dr + ni * di) / den);
    }
    // The input filter. rin: the stage's small-signal input resistance; Rs: the source resistance the nonlinear
    // stage already sees (its mid-band divider is divided out here). levelFromPot = false: the pot's level drop is
    // already in the signal (Vol Follow), keep only the impedance (tone) effects.
    void design(Iir& f, double v01, double cin, double rin, double Rs, bool levelFromPot, double fs) const
    {
        const double t = taper(v01), R2 = t * Rpot, R1 = (1.0 - t) * Rpot;
        // Hb(s) = s Cin Rin / (Zs Nw + Dw), Zs = Rp + R1 + s Lp, Nw = (1/R2 + s Cc)(1 + s Cin Rin) + s Cin, Dw = 1 + s Cin Rin
        const Poly Zs = Poly::lin(Rp + R1, Lp), Dw = Poly::lin(1.0, cin * rin);
        const Poly Nw = Poly::lin(1.0 / R2, Cc) * Dw + Poly::lin(0.0, cin);
        const Poly Num = Poly::lin(0.0, cin * rin), Den = Zs * Nw + Dw;
        // Href(s) = 1 / (1 + (Rp + s Lp)(Gref + s Cc)) -> multiply by its denominator
        const Poly Dref = Poly::k(1.0) + Poly::lin(Rp, Lp) * Poly::lin(1.0 / Rpot + 1.0 / Rint, Cc);
        Poly N = Num * Dref;
        // divide out the resistive divider the nonlinear stage applies itself
        const double dmid = rin / (Rs + rin);
        double scale = 1.0 / dmid;
        if (!levelFromPot)
        {   // the real pot already lowered the level: keep the tone change only
            const double full = magAt(potOnly(1.0), Poly::k(1.0), 1000.0), here = magAt(potOnly(v01), Poly::k(1.0), 1000.0);
            scale *= full / std::max(1e-9, here);
        }
        N = scale * N;
        f.design(N, Den, fs);
    }
    // the pot's open-circuit divide at mid-band (for Vol Follow's mapping and the level-free mode)
    Poly potOnly(double v01) const { const double t = taper(v01); return Poly::k(t); }
    double potDb(double v01) const { return 20.0 * std::log10(taper(v01)); }
};

// ---------------------------------------------------------------------------------------------------------------
// Big Muff tone stack: in -> 39k -> A (10n to ground: low-pass); in -> 4n -> B (22k to ground: high-pass);
// 100k pot from A to B, output at the wiper (t = 0: A, bass side; t = 1: B, treble side). Exact solve.
// ---------------------------------------------------------------------------------------------------------------
inline void muffStack(Iir& f, double t, double fs)
{
    const double R1 = 39000.0, C1 = 10e-9, C2 = 4e-9, R2 = 22000.0, Rp = 100000.0;
    t = std::max(0.001, std::min(0.999, t));
    // KCL with admittances (polynomials in s):
    // A: (Vin - A)/R1 - s C1 A - (A - B)/Rp = 0  ->  A (1/R1 + s C1 + 1/Rp) - B/Rp = Vin/R1
    // B: (Vin - B) s C2 - B/R2 + (A - B)/Rp = 0  ->  -A/Rp + B (s C2 + 1/R2 + 1/Rp) = Vin s C2
    const Poly a11 = Poly::lin(1.0 / R1 + 1.0 / Rp, C1), a12 = Poly::k(-1.0 / Rp), a21 = Poly::k(-1.0 / Rp), a22 = Poly::lin(1.0 / R2 + 1.0 / Rp, C2);
    const Poly b1 = Poly::k(1.0 / R1), b2 = Poly::lin(0.0, C2);
    const Poly det = a11 * a22 + (-1.0) * (a12 * a21);
    const Poly An = b1 * a22 + (-1.0) * (a12 * b2);        // A = An / det
    const Poly Bn = a11 * b2 + (-1.0) * (a21 * b1);        // B = Bn / det
    // wiper: W = A + (B - A) t  (unloaded)
    const Poly Wn = (1.0 - t) * An + t * Bn;
    f.design(Wn, det, fs);
}

} // namespace kf
