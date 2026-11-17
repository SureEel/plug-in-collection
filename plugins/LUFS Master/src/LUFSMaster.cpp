// LUFS Master - loudness meter, leveler and true-peak limiter for the MPC / Force. By Sure Eel.
//
// SIGNAL FLOW
//
//   in --+--> input level meter (raw sample peak, dBFS, with 2 s peak hold)
//        +--> K-weighting --> input loudness (momentary / short-term / integrated of this pass)
//        |                    + one stored record per 100 ms (energy + true peak) for the suggestion
//        |
//        +--> x gain --> 8x true-peak detector --> look-ahead limiter gain
//             (Manual: Input Gain;                              |
//              Measure + Live Speed Off: 0 dB;                  v
//              Measure + Fast / Slow: automatic) --> 140-sample delay --> x limiter gain --> out
//                                                                                          |
//                                  output meters (loudness, integrated, true peak) <-------+
//
// MODES
//   Manual   The Input Gain knob sets the gain (ramped, never stepped).
//   Measure  Listens: the input's integrated loudness of this pass gives the Suggested Gain
//            (target - input). Live Speed decides whether it also adjusts:
//              Fast (live broadcast)  follows the short-term loudness, pulls loud bursts down in
//                                     about half a second, comes back up in a few seconds;
//              Off                    measures only, 0 dB;
//              Slow (mastering)       follows a 15 s average, moves at most 1 dB per second.
//            Both are limited to +-Max Change dB and ramp smoothly sample by sample.
//
// PASSES AND AUTO-SET
//   A pass is one run of the MPC transport (play or record). Each pass starts a new measurement:
//   input / output integrated, the stored records and the suggestion start again. When the
//   transport stops after at least 5 s of measured audio in Measure mode, the Suggested Gain is
//   refined with a model of the limiter (so that the output, after limiting, lands on target)
//   and written into Input Gain ("auto-set"). With Live Speed Off the mode also switches to
//   Manual, so the next pass plays at that fixed gain and lands on target. With Fast / Slow the
//   mode stays Measure and the next pass starts its ride from that gain. Clear sets Input Gain
//   to 0 dB; Undo puts back the gain and mode from before the auto-set.
//
// LIMITER (no coloration)
//   One smooth gain shared by both channels multiplies the delayed signal: no clipping, no
//   saturation. Peaks are found on an 8x oversampled copy (64-tap polyphase windowed sinc), so
//   inter-sample peaks are caught; 2.9 ms look-ahead with an S-shaped gain curve into each peak,
//   150 ms release, aiming 0.12 dB under the Ceiling. The detector's interpolator is 64 taps long:
//   shorter ones under-read treble-heavy peaks. Latency: 160 samples (3.6 ms at 44.1 kHz, 3.3 ms
//   at 48 kHz), reported to the host.
//
// METER  ITU-R BS.1770-4 / EBU R128: K-weighting, 400 ms momentary, 3 s short-term, integrated
//        with -70 LUFS absolute and -10 LU relative gates.

#include "mpc_plugin.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #include <arm_neon.h>
#endif

namespace {

// Parameter order is permanent (saved projects). New parameters are appended at the end.
enum ParamIndex
{
    kCeiling = 0,   // dBTP. Index 0 is nudged by the host on every prepare: harmless here.
    kTarget, kCustom,
    kMode,          // Manual / Measure
    kReset,         // acts on every change: start a new pass (clears the measurement)
    kRefresh,       // acts on every change: clears the max readouts
    // displays
    kInMom, kInShort, kInInteg, kInAvg,
    kOutMom, kOutShort, kOutInteg, kOutAvg,
    kInTpMax, kOutTpMax,
    kGain,          // gain change being applied (manual or automatic), dB
    kGr,            // limiter gain reduction now
    kGrMax,
    kPredicted,     // Suggested Gain (target - input integrated, refined at the end of a pass)
    kHistFirst,     // output loudness over time, 5 s columns, oldest first
    kHelp = kHistFirst + 48,
    kHelp2,
    // ---- appended in version 1.1 ----
    kInputGain,     // dB, used in Manual mode
    kLiveSpeed,     // Fast / Off / Slow
    kMaxChange,     // largest automatic gain, +-dB
    kAutoClear,     // acts on every change
    kAutoUndo,      // acts on every change
    kInLevel,       // display: raw input sample peak, dBFS
    kInHold,        // display: peak hold
    kVsTarget,      // display: output short-term - target, LU
    kAutoStatus,    // display: auto-set indicator
    kPanel,         // display: never changes; static text labels on the screen are bound to it
    kNumParams
};

constexpr int kFirstReadout = kInMom;
constexpr int kNumReadouts  = kPredicted + 1 - kFirstReadout;
constexpr int kHistCols     = kHelp - kHistFirst;

// Help text (first / second line), one entry per help item. Max 31 characters per line.
const char* const kHelpText[][2] = {
    {"Touch a meter or a label",       "to see what it does."},            // 0
    {"Manual: your Input Gain.",       "Measure: listen, suggest gain."},  // 1
    {"Fast: rides quickly (live).",    "Off: measure. Slow: gentle."},     // 2
    {"Gain into the limiter, in dB.",  "Used in Manual mode only."},       // 3
    {"Largest gain Fast / Slow may",   "apply, up or down, in dB."},       // 4
    {"Raw input peak level, dBFS,",    "before any gain. Hold: 2 s."},     // 5
    {"Loudness of this pass at the",   "output, after gain, in LUFS."},    // 6
    {"Output loudness of the last 3",  "s in LUFS, and LU vs target."},    // 7
    {"Loudness to aim for. Double-",   "tap to pick a preset."},           // 8
    {"Gain being applied now,",        "manual or automatic, in dB."},     // 9
    {"Limiter gain reduction now,",    "in dB (0 = not limiting)."},       // 10
    {"Gain that would put this pass",  "on target, in dB."},               // 11
    {"Highest true peak allowed at",   "the output, in dBTP."},            // 12
    {"Your own target in LUFS,",       "used when Target is Custom."},     // 13
    {"On stop (Measure): sets Input",  "Gain to the suggested gain."},     // 14
    {"Clear: Input Gain to 0 dB.",     "Undo: back to before auto-set."},  // 15
    {"Reset: start a new pass.",       "Refresh: clear max readouts."},    // 16
    {"Highest true peak at the",       "output since Refresh, dBTP."},     // 17
    {"Output loudness every 5 s,",     "line = target. Scale in LU."},     // 18
};
constexpr int kNumHelp = (int)(sizeof(kHelpText) / sizeof(kHelpText[0]));
static_assert(kNumHelp == 19, "the skin's help buttons expect 19 items");
constexpr int kHistColBlocks = 50;          // 50 x 100 ms = 5 s per column -> 4 minutes on screen

enum Mode { kModeManual = 0, kModeMeasure = 1 };
enum Speed { kFast = 0, kSpeedOff = 1, kSlow = 2 };

constexpr float kPresetTargets[4] = { -14.0f, -16.0f, -23.0f, -24.0f };   // index 4 = Custom

constexpr int    kBoxLen      = 65;
constexpr int    kLookahead   = 2 * (kBoxLen - 1);         // 128 samples
constexpr int    kTaps        = 24;                        // meters (BS.1770 4x true peak)
#ifndef DET_TAPS
#define DET_TAPS 64          // 64-tap, beta 8: holds the ceiling on HF-heavy material (24 taps under-read by up to 0.6 dB)
#endif
#ifndef DET_BETA
#define DET_BETA 8.0
#endif
constexpr int    kDetTaps     = DET_TAPS;                  // the limiter's 8x detector
constexpr int    kLatency     = kLookahead + kDetTaps / 2;  // 160 samples
constexpr int    kDelaySize   = 256;                       // power of two > kLatency
static_assert(kLatency < kDelaySize, "delay line");
constexpr int    kMaxBlocks   = 72000;                     // 2 hours of 100 ms records
constexpr float  kMarginDb    = 0.12f;                     // limiter aims this far below the ceiling
constexpr float  kInputGainMax = 24.0f;
constexpr int    kMinPassBlocks = 50;                      // 5 s of measured audio before auto-set
constexpr int    kLockIterations = 10;
constexpr int    kLockBlocksPerFrame = 4;
constexpr float  kNone = -1000.0f;
constexpr float  kBusy = -2000.0f;
constexpr double kAbsGateEnergy = 1.1724653045822964e-7;  // 10^((-70 + 0.691) / 10)

inline double energyToLufs(double e) { return e > 1e-20 ? -0.691 + 10.0 * std::log10(e) : (double)kNone; }
inline float  dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }
inline float  linToDb(float g)  { return g > 1e-9f ? 20.0f * std::log10(g) : -180.0f; }

// ---- K-weighting (BS.1770-4), double precision ---------------------------------------------------
struct Biquad64
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double process(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void   reset() { z1 = z2 = 0; }
};
struct KWeighting
{
    Biquad64 shelf[2], hp[2];
    void prepare(double sr)
    {
        const double pi = 3.14159265358979323846;
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan(pi * f0 / sr), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            Biquad64 b;
            b.b0 = (Vh + Vb * K / Q + K * K) / a0; b.b1 = 2.0 * (K * K - Vh) / a0; b.b2 = (Vh - Vb * K / Q + K * K) / a0;
            b.a1 = 2.0 * (K * K - 1.0) / a0; b.a2 = (1.0 - K / Q + K * K) / a0;
            shelf[0] = shelf[1] = b;
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan(pi * f0 / sr);
            const double a0 = 1.0 + K / Q + K * K;
            Biquad64 b;
            b.b0 = 1.0; b.b1 = -2.0; b.b2 = 1.0; b.a1 = 2.0 * (K * K - 1.0) / a0; b.a2 = (1.0 - K / Q + K * K) / a0;
            hp[0] = hp[1] = b;
        }
        reset();
    }
    void reset() { for (int c = 0; c < 2; ++c) { shelf[c].reset(); hp[c].reset(); } }
    double energy(float l, float r)
    {
        const double yl = hp[0].process(shelf[0].process((double)l));
        const double yr = hp[1].process(shelf[1].process((double)r));
        return yl * yl + yr * yr;
    }
};

// ---- P-times oversampled true-peak estimate (P = 4 for the meters, 8 for the limiter) ----------
// Polyphase windowed sinc (Kaiser, 24 taps per phase). Returns the largest |value| over both
// channels for the sample kInterpDelay samples ago and the P-1 points between it and the next.
template <int P, int kTaps = ::kTaps, int kInterpDelay = kTaps / 2>
struct TruePeak
{
    static constexpr double kBeta = P == 8 ? DET_BETA : 5.0;
    alignas(16) float h[kTaps][P];
    float hist[2][2 * kTaps];
    int   pos = 0;

    static double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 30; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
        return sum;
    }
    void design()
    {
        const double pi = 3.14159265358979323846, beta = kBeta, half = kTaps / 2.0;
        for (int p = 1; p < P; ++p)
        {
            const double frac = (double)p / P;
            double sum = 0.0, tmp[kTaps];
            for (int k = 0; k < kTaps; ++k)
            {
                const double t = (double)k - (kInterpDelay - 1) - frac;
                const double sinc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(pi * t) / (pi * t);
                const double u = t / half;
                const double w = std::fabs(u) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - u * u)) / besselI0(beta);
                tmp[k] = sinc * w; sum += tmp[k];
            }
            for (int k = 0; k < kTaps; ++k) h[k][p] = (float)(tmp[k] / sum);
        }
        for (int k = 0; k < kTaps; ++k) h[k][0] = (k == kInterpDelay - 1) ? 1.0f : 0.0f;
        reset();
    }
    void reset() { std::memset(hist, 0, sizeof(hist)); pos = 0; }

    float push(float l, float r)
    {
        hist[0][pos] = hist[0][pos + kTaps] = l;
        hist[1][pos] = hist[1][pos + kTaps] = r;
        if (++pos == kTaps) pos = 0;
        const float* wl = &hist[0][pos];
        const float* wr = &hist[1][pos];
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        float32x4_t m4 = vdupq_n_f32(0.0f);
        for (int g = 0; g < P; g += 4)
        {
            float32x4_t vl = vdupq_n_f32(0.0f), vr = vdupq_n_f32(0.0f);
            for (int k = 0; k < kTaps; ++k)
            {
                const float32x4_t hk = vld1q_f32(&h[k][g]);
                vl = vmlaq_n_f32(vl, hk, wl[k]);
                vr = vmlaq_n_f32(vr, hk, wr[k]);
            }
            m4 = vmaxq_f32(m4, vmaxq_f32(vabsq_f32(vl), vabsq_f32(vr)));
        }
        float32x2_t m2 = vpmax_f32(vget_low_f32(m4), vget_high_f32(m4));
        m2 = vpmax_f32(m2, m2);
        return vget_lane_f32(m2, 0);
#else
        alignas(16) float al[P] = {}, ar[P] = {};
        for (int k = 0; k < kTaps; ++k)
        {
            const float xl = wl[k], xr = wr[k];
            for (int lane = 0; lane < P; ++lane) { al[lane] += h[k][lane] * xl; ar[lane] += h[k][lane] * xr; }
        }
        float peak = 0.0f;
        for (int lane = 0; lane < P; ++lane) peak = std::fmax(peak, std::fmax(std::fabs(al[lane]), std::fabs(ar[lane])));
        return peak;
#endif
    }
};

struct BoxAverage
{
    float  ring[kBoxLen];
    int    pos = 0;
    double sum = kBoxLen;
    void reset() { for (float& v : ring) v = 1.0f; pos = 0; sum = kBoxLen; }
    float process(float x)
    {
        sum += (double)x - (double)ring[pos];
        ring[pos] = x;
        if (++pos == kBoxLen) { pos = 0; double s = 0.0; for (float v : ring) s += v; sum = s; }
        return (float)(sum * (1.0 / kBoxLen));
    }
};

// Look-ahead limiter gain: minimum of the required gains over the look-ahead (monotonic queue),
// instant attack / 150 ms release, then two box averages (an S-curve into each peak). Every value
// in the averaged span covers the target sample, so the output never exceeds its required gain.
struct LimiterGain
{
    static constexpr int kQ = 256;
    float   qVal[kQ];
    int64_t qIdx[kQ];
    int64_t head = 0, tail = 0, n = 0;
    float   env = 1.0f;
    BoxAverage boxA, boxB;
    float   releaseCoef = 0.0f;
    void prepare(double sr) { releaseCoef = (float)(1.0 - std::exp(-1.0 / (0.150 * sr))); reset(); }
    void reset() { head = tail = n = 0; env = 1.0f; boxA.reset(); boxB.reset(); }
    float process(float required)
    {
        while (tail > head && qVal[(tail - 1) & (kQ - 1)] >= required) --tail;
        qVal[tail & (kQ - 1)] = required; qIdx[tail & (kQ - 1)] = n; ++tail;
        while (qIdx[head & (kQ - 1)] < n - kLookahead) ++head;
        const float held = qVal[head & (kQ - 1)];
        ++n;
        if (held < env) env = held;
        else { env += (held - env) * releaseCoef; if (held - env < 1e-6f) env = held; }
        return boxB.process(boxA.process(env));
    }
};

// Gated loudness histogram (EBU R128 integrated), constant memory.
struct LoudnessHistogram
{
    static constexpr int kBins = 800;
    uint32_t count[kBins];
    double   energy[kBins];
    uint64_t totalCount = 0;
    double   totalEnergy = 0.0;
    void clear() { std::memset(count, 0, sizeof(count)); std::memset(energy, 0, sizeof(energy)); totalCount = 0; totalEnergy = 0.0; }
    void add(double e)
    {
        if (!(e > kAbsGateEnergy)) return;
        const double l = energyToLufs(e);
        const int b = mpc::clamp((int)((l + 70.0) * 10.0), 0, kBins - 1);
        ++count[b]; energy[b] += e; ++totalCount; totalEnergy += e;
    }
    double integrated() const
    {
        if (totalCount == 0) return kNone;
        const double rel = energyToLufs(totalEnergy / (double)totalCount) - 10.0;
        const int first = mpc::clamp((int)std::floor((rel + 70.0) * 10.0), 0, kBins - 1);
        double e = 0.0; uint64_t c = 0;
        for (int b = first; b < kBins; ++b) { e += energy[b]; c += count[b]; }
        return c == 0 ? (double)kNone : energyToLufs(e / (double)c);
    }
};

struct BlockRecord { float energy; float peak; };   // one 100 ms block of the raw input

} // namespace

class LUFSMaster : public mpc::Plugin
{
public:
    LUFSMaster() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("ceiling", "Ceiling", -12.0f, 0.0f, -1.0f, "dBTP", 1));
        addParam(mpc::choiceParam("target", "Target", {"-14 LUFS Streaming", "-16 LUFS Apple/Podcast", "-23 LUFS EBU R128",
                                                       "-24 LUFS ATSC A/85", "Custom"}, 0));
        addParam(mpc::floatParam("custom", "Custom LUFS", -30.0f, -6.0f, -14.0f, "LUFS", 1));
        addParam(mpc::choiceParam("mode", "Mode", {"Manual", "Measure"}, 1));
        addParam(mpc::boolParam("reset", "Reset", false));
        addParam(mpc::boolParam("refresh", "Refresh", false));

        static const char* const ids[kNumReadouts] = {
            "in_mom", "in_short", "in_integ", "in_avg", "out_mom", "out_short", "out_integ", "out_avg",
            "in_tp_max", "out_tp_max", "gain", "gr", "gr_max", "predicted" };
        static const char* const names[kNumReadouts] = {
            "In Mom LUFS", "In ST LUFS", "In Int LUFS", "In Avg LUFS",
            "Out Mom LUFS", "Out ST LUFS", "Out Int LUFS", "Out Avg LUFS",
            "In TP Max", "Out TP Max", "Gain Change", "Limiter GR", "GR Max", "Suggested" };
        for (int i = 0; i < kNumReadouts; ++i)
        {
            mpc::Param p = mpc::floatParam(ids[i], names[i], 0.0f, 1.0f, 0.0f, "", 2);
            p.automatable = false;
            addParam(p);
            display_[i].store(kNone);
        }
        display_[kGain - kFirstReadout].store(0.0f);
        display_[kGr - kFirstReadout].store(0.0f);
        display_[kGrMax - kFirstReadout].store(0.0f);
        for (int i = 0; i < kHistCols; ++i)
        {
            char id[16], name[16];
            std::snprintf(id, sizeof(id), "hist_%02d", i);
            std::snprintf(name, sizeof(name), "Hist %02d", i + 1);
            mpc::Param p = mpc::floatParam(id, name, 0.0f, 1.0f, 0.0f, "", 2);
            p.automatable = false;
            addParam(p);
            histVal_[i] = kNone; histDisp_[i].store(kNone); histDirty_[i] = true;
        }
        std::vector<std::string> help1, help2;
        for (const auto& h : kHelpText) { help1.push_back(h[0]); help2.push_back(h[1]); }
        mpc::Param h1 = mpc::choiceParam("help", "Help", help1, 0), h2 = mpc::choiceParam("help2", "Help 2", help2, 0);
        h1.automatable = h2.automatable = false;
        addParam(h1); addParam(h2);
        // ---- 1.1 ----
        addParam(mpc::floatParam("input_gain", "Input Gain", -kInputGainMax, kInputGainMax, 0.0f, "dB", 1));
        addParam(mpc::choiceParam("live_speed", "Live Speed", {"Fast", "Off", "Slow"}, 1));
        addParam(mpc::floatParam("max_change", "Max Change", 1.0f, 24.0f, 12.0f, "dB", 1));
        addParam(mpc::boolParam("auto_clear", "Clear", false));
        addParam(mpc::boolParam("auto_undo", "Undo", false));
        for (const char* d : {"In Level", "In Peak Hold", "Vs Target", "Auto-Set", "Panel"})
        {
            std::string id(d); for (char& c : id) c = (c == ' ' || c == '-') ? '_' : (char)std::tolower((unsigned char)c);
            mpc::Param p = mpc::floatParam(("disp_" + id).c_str(), d, 0.0f, 1.0f, 0.0f, "", 2);
            p.automatable = false;
            addParam(p);
        }
        static_assert(kPanel + 1 == kNumParams, "parameter list");

        // Programs set only Ceiling, Target and Custom (the first three parameters).
        addProgram("Streaming -14", { -1.0f, 0.0f, -14.0f });
        addProgram("Apple/Podcast -16", { -1.0f, 1.0f, -16.0f });
        addProgram("EBU R128 -23", { -1.0f, 2.0f, -23.0f });
        addProgram("ATSC A/85 -24", { -2.0f, 3.0f, -24.0f });

        blocks_.assign(kMaxBlocks, BlockRecord{0.0f, 0.0f});
        inHist_.clear(); outHist_.clear();
        tp_.design(); inTp_.design(); outTp_.design();
    }

    bool programSetsParam(int index) const override { return index == kCeiling || index == kTarget || index == kCustom; }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sampleRate_ = sampleRate;
        blockLen_ = std::max(1, (int)std::lround(sampleRate * 0.1));
        inK_.prepare(sampleRate); outK_.prepare(sampleRate);
        limiter_.prepare(sampleRate);
        smoothCoef_ = (float)(1.0 - std::exp(-1.0 / (0.010 * sampleRate)));    // 10 ms corner rounding
        reset();
    }

    // Runs on resume and after a state / program load. Clears DSP memory, keeps the measurement.
    void reset() override
    {
        inK_.reset(); outK_.reset(); tp_.reset(); inTp_.reset(); outTp_.reset(); limiter_.reset();
        std::memset(delay_, 0, sizeof(delay_));
        delayPos_ = 0;
        blockPos_ = 0; inAcc_ = outAcc_ = 0.0; inPeakAcc_ = outPeakAcc_ = rawPeakAcc_ = 0.0f; grAcc_ = 1.0f;
        if (stagePending_.exchange(false, std::memory_order_acq_rel))
        {
            autoApplied_ = stagedAutoApplied_; autoValue_ = stagedAutoValue_;
            undoValid_ = stagedUndoValid_; undoGain_ = stagedUndoGain_; undoMode_ = stagedUndoMode_;
            rideDb_ = stagedRideDb_;
            autoStatus_ = autoApplied_ ? kAutoApplied : kAutoNone;
        }
        // the current switch positions are the baseline: a load or resume never "presses" them
        for (int b : {kReset, kRefresh, kAutoClear, kAutoUndo}) btnLast_[b] = getBool(b);
        const float g = gainTargetLin();
        applied_ = smoothed_ = rampTarget_ = g; rampLeft_ = 0; rampInc_ = 0.0f;
        publishCounter_ = 0;
        for (int& s : lastShown_) s = INT32_MIN;
        lastExtra_[0] = lastExtra_[1] = lastExtra_[2] = INT32_MIN; lastAutoShown_ = -1; lastTargetForBars_ = 1e9f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int numFrames = ctx.numFrames;
        const int mode = getInt(kMode);
        const float target = effectiveTarget();
        const float ceiling = get(kCeiling);

        // ----- buttons (act on every change of their switch) ---------------------------------
        if (acted(kReset))   { startPass(); clearReadouts(); }
        if (acted(kRefresh)) clearReadouts();
        if (acted(kAutoClear)) clearAutoSet();
        if (acted(kAutoUndo))  undoAutoSet();
        if (getInt(kHelp2) != getInt(kHelp)) { set(kHelp2, (float)getInt(kHelp)); notifyHostParameterChanged(kHelp2); }

        // ----- passes follow the transport ---------------------------------------------------
        const bool rolling = ctx.transport.valid && (ctx.transport.playing || ctx.transport.recording);
        if (rolling && !wasRolling_) startPass();
        if (!rolling && wasRolling_) endPass(mode);
        wasRolling_ = rolling;
        if (job_.active) runLockJob(numFrames * kLockBlocksPerFrame);

        // ----- gain: a linear ramp toward the target, rounded by a 10 ms smoother -------------
        const float gTarget = gainTargetLin();
        if (gTarget != rampTarget_)
        {
            rampTarget_ = gTarget;
            rampLeft_ = (mode == kModeMeasure && getInt(kLiveSpeed) != kSpeedOff) ? blockLen_ : std::max(1, blockLen_ / 2);
            rampInc_ = (gTarget - applied_) / (float)rampLeft_;
        }
        const float ceilLin = dbToLin(ceiling - kMarginDb);

        const bool haveIn = ctx.inputs != nullptr && ctx.numInputs > 0 && ctx.inputs[0] != nullptr;
        const float* inL = haveIn ? ctx.inputs[0] : nullptr;
        const float* inR = haveIn ? ((ctx.numInputs > 1 && ctx.inputs[1] != nullptr) ? ctx.inputs[1] : ctx.inputs[0]) : nullptr;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        for (int i = 0; i < numFrames; ++i)
        {
            const float l = haveIn ? inL[i] : 0.0f;
            const float r = haveIn ? inR[i] : 0.0f;
            rawPeakAcc_ = std::fmax(rawPeakAcc_, std::fmax(std::fabs(l), std::fabs(r)));
            inAcc_ += inK_.energy(l, r);
            inPeakAcc_ = std::fmax(inPeakAcc_, inTp_.push(l, r));        // raw input true peak (records)

            if (rampLeft_ > 0) { applied_ += rampInc_; if (--rampLeft_ == 0) applied_ = rampTarget_; }
            smoothed_ += (applied_ - smoothed_) * smoothCoef_;
            const float g = smoothed_;
            const float xl = l * g, xr = r * g;

            const float peak = tp_.push(xl, xr);                          // 8x true peak, kInterpDelay ago
            const float required = peak > ceilLin ? ceilLin / peak : 1.0f;
            const float lg = limiter_.process(required);
            grAcc_ = std::fmin(grAcc_, lg);

            delay_[0][delayPos_] = xl; delay_[1][delayPos_] = xr;
            const int rd = (delayPos_ - kLatency) & (kDelaySize - 1);
            const float yl = delay_[0][rd] * lg, yr = delay_[1][rd] * lg;
            delayPos_ = (delayPos_ + 1) & (kDelaySize - 1);
            outL[i] = yl;
            if (outR != nullptr) outR[i] = yr;

            outAcc_ += outK_.energy(yl, yr);
            outPeakAcc_ = std::fmax(outPeakAcc_, outTp_.push(yl, yr));
            if (++blockPos_ >= blockLen_) finishBlock(mode, target, rolling);
        }
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        char buf[48];
        switch (index)
        {
            case kCustom: std::snprintf(buf, sizeof(buf), "%.1f LUFS", (double)roundHalf(plainValue)); out = buf; return true;
            case kCeiling: std::snprintf(buf, sizeof(buf), "%.1f dBTP", (double)plainValue); out = buf; return true;
            case kInputGain:
                std::snprintf(buf, sizeof(buf), "%+.1f dB%s", (double)plainValue, getInt(kMode) == kModeManual ? "" : " (manual)");
                out = std::strncmp(buf, "+0.0 dB", 7) == 0 || std::strncmp(buf, "-0.0 dB", 7) == 0 ? std::string("0.0 dB") + (buf + 7) : buf;
                return true;
            case kMaxChange: std::snprintf(buf, sizeof(buf), "+/- %.1f dB", (double)plainValue); out = buf; return true;
            case kInLevel: case kInHold:
            {
                const float v = (index == kInLevel ? inLevelDisp_ : inHoldDisp_).load(std::memory_order_relaxed);
                if (v <= -59.95f) out = "-inf dBFS";
                else { std::snprintf(buf, sizeof(buf), "%.1f dBFS", (double)v); out = buf; }
                return true;
            }
            case kVsTarget:
            {
                const float v = vsTargetDisp_.load(std::memory_order_relaxed);
                if (v <= kNone + 1.0f) { out = "--"; return true; }
                std::snprintf(buf, sizeof(buf), "%+.1f LU", (double)v);
                out = (std::strcmp(buf, "+0.0 LU") == 0 || std::strcmp(buf, "-0.0 LU") == 0) ? "0.0 LU" : buf;
                return true;
            }
            case kAutoStatus:
            {
                const int s = autoStatusDisp_.load(std::memory_order_relaxed);
                const float v = autoValueDisp_.load(std::memory_order_relaxed);
                if (s == kAutoApplied) std::snprintf(buf, sizeof(buf), "APPLIED %+.1f dB", (double)v);
                else if (s == kAutoCalc) std::snprintf(buf, sizeof(buf), "calculating...");
                else if (s == kAutoCleared) std::snprintf(buf, sizeof(buf), "cleared (0 dB)");
                else if (s == kAutoUndone) std::snprintf(buf, sizeof(buf), "undone");
                else if (s == kAutoShort) std::snprintf(buf, sizeof(buf), "pass too short");
                else std::snprintf(buf, sizeof(buf), "waiting for stop");
                out = buf; return true;
            }
            case kPanel: out = ""; return true;
            default: break;
        }
        if (index >= kHistFirst && index < kHistFirst + kHistCols)
        {
            const float v = histDisp_[index - kHistFirst].load(std::memory_order_relaxed);
            if (v <= kNone + 1.0f) { out = "--"; return true; }
            std::snprintf(buf, sizeof(buf), "%.1f", (double)v);
            out = buf; return true;
        }
        if (index < kFirstReadout || index >= kFirstReadout + kNumReadouts) return false;
        const float v = display_[index - kFirstReadout].load(std::memory_order_relaxed);
        if (v <= kBusy + 1.0f) { out = "calc"; return true; }
        if (v <= kNone + 1.0f) { out = "--"; return true; }
        if (index == kGain || index == kPredicted) std::snprintf(buf, sizeof(buf), "%+.1f dB", (double)v);
        else if (index == kGr || index == kGrMax) std::snprintf(buf, sizeof(buf), "%.1f dB", (double)v);
        else if (index == kInTpMax || index == kOutTpMax) std::snprintf(buf, sizeof(buf), "%.1f dBTP", (double)v);
        else std::snprintf(buf, sizeof(buf), "%.1f LUFS", (double)v);
        if (std::strncmp(buf, "-0.0 ", 5) == 0 || std::strncmp(buf, "+0.0 ", 5) == 0) { std::string s = std::string("0.0 ") + (buf + 5); out = s; return true; }
        out = buf;
        return true;
    }

    // ----- state, version 2 ------------------------------------------------------------------
    //   u8 2 | u8 autoApplied | f32 autoValue | u8 undoValid | f32 undoGain | u8 undoMode | f32 rideDb
    // Version 1 (1.0: Bypass / Measure / Locked) is read for migration: a valid lock becomes the
    // Input Gain in Manual mode.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        out.push_back(2);
        out.push_back(autoApplied_ ? 1 : 0); putF32(out, autoValue_);
        out.push_back(undoValid_ ? 1 : 0); putF32(out, undoGain_); out.push_back((uint8_t)undoMode_);
        putF32(out, rideDb_);
    }
    void loadExtraState(const uint8_t* d, size_t size) override
    {
        if (d == nullptr || size < 1) return;
        if (d[0] == 2 && size >= 16)
        {
            size_t pos = 1;
            const bool applied = d[pos++] != 0; const float value = getF32(d, pos);
            const bool undoValid = d[pos++] != 0; const float undoGain = getF32(d, pos); const int undoMode = d[pos++];
            const float ride = getF32(d, pos);
            stagedAutoApplied_ = applied && std::isfinite(value);
            stagedAutoValue_ = std::isfinite(value) ? mpc::clamp(value, -kInputGainMax, kInputGainMax) : 0.0f;
            stagedUndoValid_ = undoValid && std::isfinite(undoGain);
            stagedUndoGain_ = std::isfinite(undoGain) ? mpc::clamp(undoGain, -kInputGainMax, kInputGainMax) : 0.0f;
            stagedUndoMode_ = undoMode == kModeManual ? kModeManual : kModeMeasure;
            stagedRideDb_ = std::isfinite(ride) ? mpc::clamp(ride, -24.0f, 24.0f) : 0.0f;
            stagePending_.store(true, std::memory_order_release);
        }
        else if (d[0] == 1 && size >= 26)
        {   // 1.0 project: a valid lock becomes Input Gain in Manual; otherwise Measure, speed Off
            size_t pos = 2;
            const bool lockValid = d[1] != 0;
            const float lockedDb = getF32(d, pos);
            if (lockValid && std::isfinite(lockedDb))
            {
                set(kInputGain, mpc::clamp(lockedDb, -kInputGainMax, kInputGainMax));
                set(kMode, (float)kModeManual);
            }
            else set(kMode, (float)getInt(kMode));
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "LUFS Master"; info.vendor = "sureeel";
        info.uniqueId = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);
        info.version = 1100;
        info.isInstrument = false; info.numInputs = 2; info.numOutputs = 2;
        info.latencySamples = kLatency; info.tailSamples = kLatency;
        return info;
    }
    static constexpr char kUid[5] = "S6yP";

    static float roundHalf(float v) { return std::round(v * 2.0f) * 0.5f; }
    static void  putF32(std::vector<uint8_t>& o, float f) { uint8_t b[4]; std::memcpy(b, &f, 4); o.insert(o.end(), b, b + 4); }
    static float getF32(const uint8_t* d, size_t& pos) { float f; std::memcpy(&f, d + pos, 4); pos += 4; return f; }

    float effectiveTarget() const
    {
        const int t = getInt(kTarget);
        return (t >= 0 && t < 4) ? kPresetTargets[t] : roundHalf(get(kCustom));
    }
    float gainTargetDb() const
    {
        if (getInt(kMode) == kModeManual) return get(kInputGain);
        return getInt(kLiveSpeed) == kSpeedOff ? 0.0f : rideDb_;
    }
    float gainTargetLin() const { const float db = gainTargetDb(); return db == 0.0f ? 1.0f : dbToLin(db); }

    bool acted(int idx)
    {
        const bool now = getBool(idx);
        if (now == btnLast_[idx]) return false;
        btnLast_[idx] = now;
        return true;
    }

    // ----- passes -----------------------------------------------------------------------------
    void startPass()
    {
        if (job_.active) { job_.active = false; autoStatus_ = kAutoShort; }   // restarted before the suggestion finished
        count_ = 0; passGated_ = 0;
        inHist_.clear(); outHist_.clear();
        inInteg_ = outInteg_ = kNone;
        slowE_ = -1.0;
        suggested_ = kNone; suggestionFinal_ = false;
        // the ride starts from the Input Gain (an auto-set from the last pass starts it corrected)
        rideDb_ = mpc::clamp(get(kInputGain), -get(kMaxChange), get(kMaxChange));
        rideDesired_ = rideDb_;
    }
    void endPass(int mode)
    {
        if (mode != kModeMeasure) return;
        if (passGated_ < kMinPassBlocks) { autoStatus_ = kAutoShort; return; }
        startLock(effectiveTarget(), get(kCeiling));
        autoStatus_ = job_.active ? kAutoCalc : kAutoShort;
        autoMode_ = getInt(kLiveSpeed) == kSpeedOff ? kModeManual : kModeMeasure;
    }
    void applyAutoSet(float gainDb)
    {
        undoValid_ = true; undoGain_ = get(kInputGain); undoMode_ = getInt(kMode);
        autoValue_ = mpc::clamp(gainDb, -kInputGainMax, kInputGainMax);
        set(kInputGain, autoValue_); notifyHostParameterChanged(kInputGain);
        if (autoMode_ != getInt(kMode)) { set(kMode, (float)autoMode_); notifyHostParameterChanged(kMode); }
        autoApplied_ = true; autoStatus_ = kAutoApplied;
    }
    void clearAutoSet()
    {
        if (autoApplied_) { undoValid_ = true; undoGain_ = get(kInputGain); undoMode_ = getInt(kMode); }
        set(kInputGain, 0.0f); notifyHostParameterChanged(kInputGain);
        autoApplied_ = false; autoStatus_ = kAutoCleared;
    }
    void undoAutoSet()
    {
        if (!undoValid_) return;
        set(kInputGain, undoGain_); notifyHostParameterChanged(kInputGain);
        if (undoMode_ != getInt(kMode)) { set(kMode, (float)undoMode_); notifyHostParameterChanged(kMode); }
        undoValid_ = false; autoApplied_ = false; autoStatus_ = kAutoUndone;
    }

    // ----- every 100 ms -------------------------------------------------------------------------
    void finishBlock(int mode, float target, bool rolling)
    {
        const double inE = inAcc_ / blockLen_, outE = outAcc_ / blockLen_;
        inAcc_ = outAcc_ = 0.0; blockPos_ = 0;
        const bool audio = rawPeakAcc_ > 3.2e-5f;              // above -90 dBFS
        const bool live = rolling || audio;                    // something is playing

        inRing_[ringPos_] = inE; outRing_[ringPos_] = outE;
        ringPos_ = (ringPos_ + 1) % 30;
        if (ringN_ < 30) ++ringN_;
        auto ringMean = [this](const double* ring, int len) {
            double s = 0.0; const int n = len < ringN_ ? len : ringN_;
            for (int k = 1; k <= n; ++k) s += ring[(ringPos_ - k + 30) % 30];
            return n > 0 ? s / n : 0.0;
        };
        const double inMomE = ringMean(inRing_, 4), outMomE = ringMean(outRing_, 4);
        inMom_ = (float)energyToLufs(inMomE); inShort_ = (float)energyToLufs(ringMean(inRing_, 30));
        outMom_ = (float)energyToLufs(outMomE); outShort_ = (float)energyToLufs(ringMean(outRing_, 30));

        // the measurement of this pass (also when the transport is stopped and audio is playing)
        if (live && ringN_ >= 4)
        {
            if (count_ < (uint32_t)kMaxBlocks) { blocks_[count_].energy = (float)inE; blocks_[count_].peak = inPeakAcc_; ++count_; }
            inHist_.add(inMomE); outHist_.add(outMomE);
            if (inMomE > kAbsGateEnergy) ++passGated_;
        }
        if ((publishCounter_ & 1) == 0) inInteg_ = (float)inHist_.integrated();
        else                            outInteg_ = (float)outHist_.integrated();
        // live suggestion while the pass runs; the refined one (limiter model) stays until the next pass
        if (!job_.active && !suggestionFinal_ && inInteg_ > kNone + 1.0f) suggested_ = mpc::clamp(target - inInteg_, -kInputGainMax, kInputGainMax);

        // input level meter: sample peak with a 2 s hold
        const float lvl = rawPeakAcc_ > 1e-6f ? std::fmax(-60.0f, linToDb(rawPeakAcc_)) : -60.0f;
        inLevel_ = lvl;
        if (lvl >= inHold_ || ++holdAge_ > 20) { inHold_ = lvl; holdAge_ = 0; }

        if (live) historyAddBlock(outE);
        inAvgSum_ += inE; outAvgSum_ += outE; ++avgN_;
        inTpMax_ = std::fmax(inTpMax_, inPeakAcc_);
        outTpMax_ = std::fmax(outTpMax_, outPeakAcc_);
        grNow_ = linToDb(grAcc_);
        grMax_ = std::fmin(grMax_, grNow_);
        inPeakAcc_ = outPeakAcc_ = rawPeakAcc_ = 0.0f; grAcc_ = 1.0f;

        // automatic gain (Measure, Fast / Slow)
        if (mode == kModeMeasure && getInt(kLiveSpeed) != kSpeedOff) ride(target, inE, getInt(kLiveSpeed) == kFast);

        publishReadouts(mode, target, publishCounter_ & 1);
        publishHistory(target);
        ++publishCounter_;
    }

    // Fast: short-term loudness (momentary when a burst is 3 LU louder), down ~0.4 s, up ~2.5 s.
    // Slow: a 15 s gated average, down ~6 s, up ~10 s, never faster than 1 dB per second.
    // Gate: below the pass's relative gate (integrated - 10 LU) or -60 LUFS the gain holds, so
    // pauses and fades are not pulled up. Range: +-Max Change.
    void ride(float target, double inE, bool fast)
    {
        const float range = get(kMaxChange);
        const float gate = inInteg_ > kNone + 1.0f ? std::fmax(-60.0f, inInteg_ - 10.0f) : std::fmax(-60.0f, target - 20.0f);
        float ref;
        if (fast) ref = (inMom_ > inShort_ + 3.0f) ? inMom_ : inShort_;
        else
        {
            if (inShort_ > gate && inE > 0.0) slowE_ = slowE_ < 0.0 ? inE : slowE_ + (inE - slowE_) * (0.1 / 15.0);
            ref = slowE_ > 0.0 ? (float)energyToLufs(slowE_) : kNone;
        }
        if (ringN_ >= 4 && ref > kNone + 1.0f && (fast ? ref : inShort_) > gate)
            rideDesired_ = mpc::clamp(target - ref, -range, range);
        rideDesired_ = mpc::clamp(rideDesired_, -range, range);
        const float d = rideDesired_ - rideDb_;
        const float tau = fast ? (d < 0.0f ? 0.4f : 1.5f) : (d < 0.0f ? 6.0f : 10.0f);
        float step = d * std::fmin(1.0f, 0.1f / tau);
        const float maxStep = fast ? (d < 0.0f ? 3.0f : 1.0f) : 0.1f;      // per 100 ms
        step = mpc::clamp(step, -maxStep, maxStep);
        rideDb_ = mpc::clamp(rideDb_ + step, -range, range);
    }

    // ----- loudness over time (display): output loudness, 5 s columns ----------------------------
    void historyAddBlock(double energy)
    {
        if (histBlocks_ % kHistColBlocks == 0)
        {
            for (int i = 0; i + 1 < kHistCols; ++i) histVal_[i] = histVal_[i + 1];
            for (bool& d : histDirty_) d = true;
            histAccE_ = 0.0; histAccN_ = 0;
        }
        ++histBlocks_;
        histAccE_ += energy; ++histAccN_;
        const float v = (float)energyToLufs(histAccE_ / histAccN_);
        if (v != histVal_[kHistCols - 1]) { histVal_[kHistCols - 1] = v; histDirty_[kHistCols - 1] = true; }
    }
    void publishHistory(float target)
    {
        if (target != histTarget_) { histTarget_ = target; for (bool& d : histDirty_) d = true; }
        int sent = 0;
        for (int k = 0; k < kHistCols && sent < 8; ++k)
        {
            const int i = (histPublishPos_ + k) % kHistCols;
            if (!histDirty_[i]) continue;
            histDirty_[i] = false;
            float x = histVal_[i];
            if (x > kNone + 1.0f && x < -99.0f) x = kNone;
            histDisp_[i].store(x, std::memory_order_relaxed);
            setNormalised(kHistFirst + i, relNorm(x, target));
            notifyHostParameterChanged(kHistFirst + i);
            ++sent;
        }
        histPublishPos_ = (histPublishPos_ + 8) % kHistCols;
    }
    // bars relative to the target: target - 24 .. target + 12 LU (36 steps, target at step 24)
    static float relNorm(float v, float target)
    {
        if (v <= kNone + 1.0f) return 0.0f;
        return mpc::clamp((v - target + 24.0f) / 36.0f, 0.0f, 1.0f);
    }

    void publishReadouts(int mode, float target, int half)
    {
        float v[kNumReadouts];
        v[kInMom - kFirstReadout] = inMom_;   v[kInShort - kFirstReadout] = inShort_;
        v[kInInteg - kFirstReadout] = inInteg_;
        v[kInAvg - kFirstReadout] = avgN_ ? (float)energyToLufs(inAvgSum_ / avgN_) : kNone;
        v[kOutMom - kFirstReadout] = outMom_; v[kOutShort - kFirstReadout] = outShort_;
        v[kOutInteg - kFirstReadout] = outInteg_;
        v[kOutAvg - kFirstReadout] = avgN_ ? (float)energyToLufs(outAvgSum_ / avgN_) : kNone;
        v[kInTpMax - kFirstReadout] = inTpMax_ > 0.0f ? linToDb(inTpMax_) : kNone;
        v[kOutTpMax - kFirstReadout] = outTpMax_ > 0.0f ? linToDb(outTpMax_) : kNone;
        v[kGain - kFirstReadout] = linToDb(smoothed_);
        v[kGr - kFirstReadout] = grNow_;
        v[kGrMax - kFirstReadout] = grMax_;
        v[kPredicted - kFirstReadout] = job_.active ? kBusy : suggested_;
        (void)mode;
        const bool targetMoved = target != lastTargetForBars_;
        lastTargetForBars_ = target;
        for (int k = 0; k < kNumReadouts; ++k)
        {
            const int idx = kFirstReadout + k;
            const bool meter = idx == kInMom || idx == kOutMom || idx == kInShort || idx == kOutShort || idx == kGr || idx == kGain;
            if (!meter && (k & 1) != half && !(targetMoved && idx == kOutInteg)) continue;
            float x = v[k];
            if (x > kNone + 1.0f && x < -99.0f && idx != kGain) x = kNone;
            const int shown = (int)std::lround(x * 10.0f);
            const float norm = readoutNorm(idx, x, target);
            if (shown == lastShown_[k] && std::fabs(norm - getNormalised(idx)) < 1e-3f) continue;
            lastShown_[k] = shown;
            display_[k].store(x, std::memory_order_relaxed);
            setNormalised(idx, norm);
            notifyHostParameterChanged(idx);
        }
        // input level, peak hold, vs target, auto-set status
        publishExtra(kInLevel, inLevel_, inLevelDisp_, 0, mpc::clamp((inLevel_ + 48.0f) / 48.0f, 0.0f, 1.0f));
        publishExtra(kInHold, inHold_, inHoldDisp_, 1, mpc::clamp((inHold_ + 48.0f) / 48.0f, 0.0f, 1.0f));
        const float vs = outShort_ > kNone + 1.0f && outShort_ > -99.0f ? outShort_ - target : kNone;
        publishExtra(kVsTarget, vs, vsTargetDisp_, 2, vs <= kNone + 1.0f ? 0.0f : mpc::clamp((vs + 24.0f) / 36.0f, 0.0f, 1.0f));
        const int key = autoStatus_ * 10000 + (int)std::lround(autoValue_ * 10.0f);
        if (key != lastAutoShown_)
        {
            lastAutoShown_ = key;
            autoStatusDisp_.store(autoStatus_); autoValueDisp_.store(autoValue_);
            setNormalised(kAutoStatus, autoStatus_ == kAutoApplied ? 1.0f : (float)(autoStatus_ + 1) / 10.0f);
            notifyHostParameterChanged(kAutoStatus);
        }
        if (mode != lastModeShown_) { lastModeShown_ = mode; notifyHostParameterChanged(kInputGain); }   // "(manual)" suffix
    }
    void publishExtra(int idx, float v, std::atomic<float>& disp, int slot, float norm)
    {
        const int shown = (int)std::lround(v * 10.0f);
        if (shown == lastExtra_[slot]) return;
        lastExtra_[slot] = shown;
        disp.store(v, std::memory_order_relaxed);
        setNormalised(idx, norm);
        notifyHostParameterChanged(idx);
    }
    // Bar positions (0..1) used by the LED film strips. Display only.
    static float readoutNorm(int index, float v, float target)
    {
        if (index == kGain) return mpc::clamp((v + 24.0f) / 48.0f, 0.0f, 1.0f);      // +-24 dB, centre = 0
        if (v <= kNone + 1.0f) return 0.0f;
        switch (index)
        {
            case kInTpMax: case kOutTpMax: return mpc::clamp((v + 30.0f) / 30.0f, 0.0f, 1.0f);
            case kPredicted:               return mpc::clamp((v + 24.0f) / 48.0f, 0.0f, 1.0f);
            case kGr: case kGrMax:         return mpc::clamp(-v / 12.0f, 0.0f, 1.0f);
            case kOutInteg: case kOutShort: case kInInteg: return relNorm(v, target);
            default:                       return mpc::clamp((v + 48.0f) / 48.0f, 0.0f, 1.0f);
        }
    }

    void clearReadouts()
    {
        inAvgSum_ = outAvgSum_ = 0.0; avgN_ = 0;
        inTpMax_ = outTpMax_ = 0.0f; grMax_ = 0.0f;
        for (int& s : lastShown_) s = INT32_MIN;
    }

    // ----- suggestion at the end of a pass ----------------------------------------------------------
    // From the stored 100 ms records: G0 = target - input integrated, then the output loudness for a
    // gain G is predicted with a block-rate model of the limiter (fitted to 37 renders of this
    // limiter, RMS error 0.19 LU) and G is corrected with secant steps until the prediction is within
    // 0.02 LU of the target. Spread over audio blocks so it never causes a dropout.
    static constexpr double kModelLossK = 0.78;
    static constexpr double kModelReleaseKeep = 0.60;
    static constexpr double kModelThroughMaxDb = 1.5;
    struct LockJob
    {
        bool active = false; int eval = 0, pass = 0;
        uint32_t b = 0, n = 0;
        double gr = 0.0, e1 = 0.0, e2 = 0.0, e3 = 0.0, sum = 0.0; uint64_t cnt = 0;
        double relThreshold = 0.0, gainDb = 0.0, prevGainDb = 0.0, prevResult = 0.0;
        double target = -14.0, ceilDb = -1.1, lastResult = kNone;
    } job_;
    void startLock(float target, float ceiling)
    {
        job_ = LockJob{};
        job_.n = count_; job_.target = target; job_.ceilDb = (double)ceiling - kMarginDb;
        job_.active = job_.n >= 4;
    }
    void runLockJob(int budget)
    {
        const bool model = job_.eval > 0;
        const double G2 = model ? std::pow(10.0, job_.gainDb / 10.0) : 1.0;
        const double threshold = job_.pass == 0 ? kAbsGateEnergy : job_.relThreshold;
        uint32_t end = job_.b + (uint32_t)(budget > 16 ? budget : 16);
        if (end > job_.n) end = job_.n;
        for (uint32_t i = job_.b; i < end; ++i)
        {
            double e = (double)blocks_[i].energy * G2;
            if (model)
            {
                const double pk = blocks_[i].peak;
                const double over = pk > 1e-9 ? 20.0 * std::log10(pk) + job_.gainDb - job_.ceilDb : -200.0;
                job_.gr = std::fmax(over, job_.gr * kModelReleaseKeep);
                if (job_.gr > 1e-4)
                {
                    const double through = kModelThroughMaxDb * (1.0 - std::exp(-(1.0 - kModelLossK) * job_.gr / kModelThroughMaxDb));
                    e *= std::exp(-0.230258509299404568 * (job_.gr - through));
                }
                else job_.gr = 0.0;
            }
            if (i >= 3)
            {
                const double w = (job_.e3 + job_.e2 + job_.e1 + e) * 0.25;
                if (w > threshold) { job_.sum += w; ++job_.cnt; }
            }
            job_.e3 = job_.e2; job_.e2 = job_.e1; job_.e1 = e;
        }
        job_.b = end;
        if (job_.b < job_.n) return;
        if (job_.cnt == 0) { finishLock(false); return; }
        if (job_.pass == 0) { job_.relThreshold = std::fmax(kAbsGateEnergy, job_.sum / (double)job_.cnt * 0.1); job_.pass = 1; }
        else
        {
            const double I = energyToLufs(job_.sum / (double)job_.cnt);
            const double err = job_.target - I;
            double next;
            if (job_.eval == 0) next = err;
            else
            {
                job_.lastResult = I;
                if (std::fabs(err) < 0.02 || job_.eval > kLockIterations) { finishLock(true); return; }
                double slope = 1.0;
                if (job_.eval >= 2 && std::fabs(job_.gainDb - job_.prevGainDb) > 1e-6)
                {
                    slope = (I - job_.prevResult) / (job_.gainDb - job_.prevGainDb);
                    if (err > 0.0 && slope < 0.1)
                    {
                        if (job_.prevGainDb < job_.gainDb) { job_.gainDb = job_.prevGainDb; job_.lastResult = job_.prevResult; }
                        finishLock(true); return;
                    }
                    slope = mpc::clamp(slope, 0.1, 1.0);
                }
                next = job_.gainDb + err / slope;
            }
            job_.prevGainDb = job_.gainDb; job_.prevResult = I;
            next = mpc::clamp(next, -(double)kInputGainMax, (double)kInputGainMax);
            if (job_.eval > 0 && std::fabs(next - job_.gainDb) < 1e-4) { finishLock(true); return; }
            job_.gainDb = next; ++job_.eval; job_.pass = 0;
        }
        job_.b = 0; job_.gr = 0.0; job_.e1 = job_.e2 = job_.e3 = 0.0; job_.sum = 0.0; job_.cnt = 0;
    }
    void finishLock(bool ok)
    {
        job_.active = false;
        if (!ok) { autoStatus_ = kAutoShort; return; }
        suggested_ = (float)job_.gainDb; suggestionFinal_ = true;
        applyAutoSet(suggested_);
    }

    enum AutoStatus { kAutoNone = 0, kAutoCalc, kAutoApplied, kAutoCleared, kAutoUndone, kAutoShort };

    // ----- members -----------------------------------------------------------------------------
    double sampleRate_ = 44100.0;
    int    blockLen_ = 4410;
    float  smoothCoef_ = 0.002f;
    float  applied_ = 1.0f, smoothed_ = 1.0f, rampTarget_ = 1.0f, rampInc_ = 0.0f;
    int    rampLeft_ = 0;

    KWeighting  inK_, outK_;
    TruePeak<8, kDetTaps> tp_;      // the limiter's detector
    TruePeak<4> inTp_, outTp_;      // the meters (BS.1770 true peak)
    LimiterGain limiter_;
    float delay_[2][kDelaySize] = {};
    int   delayPos_ = 0;

    int    blockPos_ = 0;
    double inAcc_ = 0.0, outAcc_ = 0.0;
    float  inPeakAcc_ = 0.0f, outPeakAcc_ = 0.0f, rawPeakAcc_ = 0.0f, grAcc_ = 1.0f;
    double inRing_[30] = {}, outRing_[30] = {};
    int    ringPos_ = 0, ringN_ = 0;
    float  inMom_ = kNone, inShort_ = kNone, outMom_ = kNone, outShort_ = kNone;
    float  inInteg_ = kNone, outInteg_ = kNone, suggested_ = kNone;
    bool   suggestionFinal_ = false;
    float  inLevel_ = -60.0f, inHold_ = -60.0f; int holdAge_ = 0;
    double inAvgSum_ = 0.0, outAvgSum_ = 0.0; uint64_t avgN_ = 0;
    float  inTpMax_ = 0.0f, outTpMax_ = 0.0f, grNow_ = 0.0f, grMax_ = 0.0f;
    LoudnessHistogram inHist_, outHist_;
    std::vector<BlockRecord> blocks_;
    uint32_t count_ = 0;
    int    passGated_ = 0;
    bool   wasRolling_ = false;

    float  rideDb_ = 0.0f, rideDesired_ = 0.0f;
    double slowE_ = -1.0;

    bool  autoApplied_ = false, undoValid_ = false;
    float autoValue_ = 0.0f, undoGain_ = 0.0f;
    int   undoMode_ = kModeMeasure, autoMode_ = kModeManual, autoStatus_ = kAutoNone;

    std::atomic<bool> stagePending_{false};
    bool  stagedAutoApplied_ = false, stagedUndoValid_ = false;
    float stagedAutoValue_ = 0.0f, stagedUndoGain_ = 0.0f, stagedRideDb_ = 0.0f;
    int   stagedUndoMode_ = kModeMeasure;

    bool btnLast_[kNumParams] = {};

    std::atomic<float> display_[kNumReadouts];
    std::atomic<float> inLevelDisp_{-60.0f}, inHoldDisp_{-60.0f}, vsTargetDisp_{kNone}, autoValueDisp_{0.0f};
    std::atomic<int>   autoStatusDisp_{kAutoNone};
    float histVal_[kHistCols];
    std::atomic<float> histDisp_[kHistCols];
    bool  histDirty_[kHistCols];
    double histAccE_ = 0.0;
    int   histAccN_ = 0, histPublishPos_ = 0;
    uint32_t histBlocks_ = 0;
    float histTarget_ = 1e9f, lastTargetForBars_ = 1e9f;
    int lastShown_[kNumReadouts] = {};
    int lastExtra_[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
    int lastAutoShown_ = -1, lastModeShown_ = -1;
    int publishCounter_ = 0;
};

MPC_PLUGIN_ENTRY(LUFSMaster)
