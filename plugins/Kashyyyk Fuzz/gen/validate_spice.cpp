// Kashyyyk Fuzz - checks the circuit models (kf_circuit.h) against ngspice running the same netlists.
// Build: zig c++ -std=c++17 -O2 -I src -o build/validate_spice.exe gen/validate_spice.cpp
// Run:   build/validate_spice.exe <path to ngspice_con.exe> <work folder>
#include "kf_circuit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

using namespace kf;

static std::vector<std::pair<double, double>> runSpice(const std::string& exe, const std::string& dir, const std::string& name, const std::string& netlist)
{
    const std::string cir = dir + "/" + name + ".cir", out = dir + "/" + name + ".txt";
    FILE* f = std::fopen(cir.c_str(), "w"); std::fputs(netlist.c_str(), f); std::fclose(f);
    std::remove(out.c_str());
    const std::string cmd = "\"\"" + exe + "\" -b \"" + cir + "\" > \"" + dir + "/" + name + ".log\" 2>&1\"";
    std::system(cmd.c_str());
    std::vector<std::pair<double, double>> r;
    FILE* g = std::fopen(out.c_str(), "r");
    if (!g) return r;
    double a, b;
    while (std::fscanf(g, "%lf %lf", &a, &b) == 2) r.push_back({a, b});
    std::fclose(g);
    return r;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: validate_spice <ngspice_con.exe> <work folder>\n"); return 2; }
    const std::string exe = argv[1], dir = argv[2];
    int fails = 0;

    // ---- 1. diode clipper (antiparallel pair, 13.4k series), DC sweep -10 .. 10 V ----
    for (int p = 0; p < kNumDiodes; ++p)
    {
        const DiodePart& d = kDiodes[p];
        char net[1024];
        std::snprintf(net, sizeof net,
            "* diode clipper %s\nVin in 0 DC 0\nR1 in out 13400\nDA out 0 DA\nDB 0 out DB\n"
            ".model DA D(IS=%g N=%g RS=%g)\n.model DB D(IS=%g N=%g RS=%g)\n"
            ".control\ndc Vin -10 10 0.05\nwrdata %s/d%d.txt v(out)\n.endc\n.end\n",
            d.name, d.Is, d.n, d.Rs, d.IsB, d.n, d.Rs, dir.c_str(), p);
        const auto sp = runSpice(exe, dir, "d" + std::to_string(p), net);
        DiodeClipper c; c.d = d; c.arr = kPair;
        double worst = 0.0, at = 0.0; double guess = 0.0;
        for (const auto& pt : sp) { const double ours = c.solve(pt.first, guess); guess = ours; const double e = std::fabs(ours - pt.second); if (e > worst) { worst = e; at = pt.first; } }
        const bool ok = !sp.empty() && worst < 1e-3;
        if (!ok) ++fails;
        std::printf("%s  diode clipper %-7s %zu points, largest difference %.2e V (at %.2f V in)\n", ok ? "PASS" : "FAIL", d.name, sp.size(), worst, at);
    }

    // ---- 2. Fuzz Face: DC operating point and the quasi-static transfer ----
    for (double vcc : {9.0, 7.5, 6.0, 4.5})          // trimmed at a fresh 9 V, then starved (Sag)
    for (int p = 0; p < kNumTransistors; ++p)
    {
        FuzzFace ff; ff.q = kTransistors[p];
        ff.trimBias(4.5);
        ff.vcc = vcc; ff.dcPoint();
        char vs[16]; std::snprintf(vs, sizeof vs, "%.1f", vcc);
        const TransistorPart& q = ff.q;
        // DC: the input capacitor open, the emitter through the whole 1k pot
        char net[2048];
        std::snprintf(net, sizeof net,
            "* Fuzz Face DC %s\nVcc vcc 0 DC %s\nR1 vcc c1 %g\nRc2 vcc c2 %g\nRf e2 b1 100k\nRpot e2 0 1k\n"
            "Q1 c1 b1 0 QM\nQ2 c2 c1 e2 QM\nIl1 c1 0 DC %g\nIl2 c2 e2 DC %g\n"
            ".model QM NPN(IS=%g BF=%g BR=%g NF=%g NR=%g)\n"
            ".control\nop\nprint v(b1) v(c1) v(e2) v(c2)\n.endc\n.end\n",
            q.name, vs, ff.R1, ff.Rc2, q.leak, q.leak, q.Is, q.beta, q.betaR, q.n, q.n);
        {
            const std::string cir = dir + "/ffdc" + std::to_string(p) + ".cir", log = dir + "/ffdc" + std::to_string(p) + ".log";
            FILE* f = std::fopen(cir.c_str(), "w"); std::fputs(net, f); std::fclose(f);
            std::system(("\"\"" + exe + "\" -b \"" + cir + "\" > \"" + log + "\" 2>&1\"").c_str());
            double v[4] = {0, 0, 0, 0}; int got = 0;
            if (FILE* g = std::fopen(log.c_str(), "r"))
            {
                char line[512];
                while (std::fgets(line, sizeof line, g))
                {
                    const char* keys[4] = {"v(b1) = ", "v(c1) = ", "v(e2) = ", "v(c2) = "};
                    for (int k = 0; k < 4; ++k) if (const char* s = std::strstr(line, keys[k])) { v[k] = std::atof(s + std::strlen(keys[k])); ++got; }
                }
                std::fclose(g);
            }
            double worst = 0.0; for (int k = 0; k < 4; ++k) worst = std::max(worst, std::fabs(v[k] - ff.dc[k]));
            const bool ok = got >= 4 && worst < 2e-3;
            if (!ok) ++fails;
            std::printf("%s  %s V  Fuzz Face DC %-7s ours Vb1 %.4f Vc1 %.4f Ve2 %.4f Vc2 %.4f | ngspice %.4f %.4f %.4f %.4f (largest difference %.1e V)\n",
                        ok ? "PASS" : "FAIL", vs, q.name, ff.dc[0], ff.dc[1], ff.dc[2], ff.dc[3], v[0], v[1], v[2], v[3], worst);
        }
        // the quasi-static transfer at two fuzz settings and two source resistances
        for (double fz : {0.5, 1.0})
            for (double Rs : {4000.0, 50000.0})
            {
                const double vw = ff.dc[2] * fz, Ru = std::max(1.0, (1.0 - fz) * ff.Rpot);
                std::snprintf(net, sizeof net,
                    "* Fuzz Face transfer %s\nVcc vcc 0 DC %s\nR1 vcc c1 %g\nRc2 vcc c2 %g\nRf e2 b1 100k\nRu e2 w %g\nVw w 0 DC %.9g\n"
                    "Vs s 0 DC 0\nVcin x s DC %.9g\nRs x b1 %g\n"
                    "Q1 c1 b1 0 QM\nQ2 c2 c1 e2 QM\nIl1 c1 0 DC %g\nIl2 c2 e2 DC %g\n"
                    ".model QM NPN(IS=%g BF=%g BR=%g NF=%g NR=%g)\n"
                    ".control\ndc Vs -1 1 0.005\nwrdata %s/fft%d.txt v(c2)\n.endc\n.end\n",
                    q.name, vs, ff.R1, ff.Rc2, Ru, vw, ff.dc[0], Rs, q.leak, q.leak, q.Is, q.beta, q.betaR, q.n, q.n, dir.c_str(), p);
                const auto sp = runSpice(exe, dir, "fft" + std::to_string(p), net);
                // ours: Newton on the same equations, continuing from the quiescent point
                FuzzFace::Drive d; d.ac = true; d.Rs = Rs; d.vcin = ff.dc[0]; d.vw = vw; d.Ru = Ru;
                double v0[4]; std::copy(ff.dc, ff.dc + 4, v0); ff.newton(v0, d);
                double worst = 0.0, at = 0.0, worstV = 0.0, worstH = 0.0; int bad = 0;
                for (int dir2 = -1; dir2 <= 1; dir2 += 2)
                {
                    double v[4]; std::copy(v0, v0 + 4, v);
                    for (const auto& pt : sp)
                    {
                        if ((dir2 < 0 && pt.first > 0) || (dir2 > 0 && pt.first < 0)) continue;
                    }
                    // walk outward from 0 in the same steps as the sweep
                    std::vector<std::pair<double, double>> side;
                    for (const auto& pt : sp) if ((dir2 < 0) ? pt.first <= 1e-9 : pt.first >= -1e-9) side.push_back(pt);   // ngspice writes 0 as ~1e-16
                    if (dir2 < 0) std::reverse(side.begin(), side.end());
                    for (size_t k = 0; k < side.size(); ++k)
                    {
                        const auto& pt = side[k];
                        d.vs = pt.first; ff.newton(v, d);
                        // the error as an input-voltage shift where the curve is steep (at full fuzz the gain is in the
                        // thousands, so a microvolt horizontally is millivolts vertically)
                        // the steeper of the two neighbouring slopes (at a knee the slope changes within one step)
                        double slope = 0.0;
                        if (k + 1 < side.size()) slope = std::max(slope, std::fabs((side[k + 1].second - pt.second) / (side[k + 1].first - pt.first)));
                        if (k > 0) slope = std::max(slope, std::fabs((side[k - 1].second - pt.second) / (side[k - 1].first - pt.first)));
                        const double ev = std::fabs(v[3] - pt.second), eh = ev / std::max(1e-9, slope);
                        worstV = std::max(worstV, ev);
                        // 0.2 mV of input: below a pickup's own noise
                        if (ev > 5e-3 && eh > 2e-4) { ++bad; if (eh > worst) { worst = eh; at = pt.first; } }
                        worstH = std::max(worstH, ev > 5e-3 ? eh : 0.0);
                    }
                }
                const bool ok = !sp.empty() && bad == 0;
                if (!ok) ++fails;
                std::printf("%s  %s V  Fuzz Face transfer %-7s fuzz %.1f Rs %5.0f: %zu points; largest output difference %.1f mV; where over 5 mV (steep), the curves are %.1f uV apart in input\n",
                            ok ? "PASS" : "FAIL", vs, q.name, fz, Rs, sp.size(), worstV * 1e3, worstH * 1e6);
            }
    }
    std::printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
