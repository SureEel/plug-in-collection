// LUFS Master - loudness leveler, meter and transparent true-peak limiter for the MPC / Force.
// By sureeel.
//
// SIGNAL FLOW
//
//   in --+--> K-weighting --> input meter (momentary / short-term / integrated / average)
//        |                    and, in Measure mode, one stored record per 100 ms block
//        |                    (K-weighted energy + true peak: 8 bytes, never audio)
//        |
//        +--> x gain ------> 4x true-peak detector --> limiter gain computer
//        |    (Bypass: 0 dB,                                  |
//        |     Measure: slow leveler,                         v
//        |     Locked: fixed offset)  --> 140-sample delay --> x limiter gain --> out
//                                                                                 |
//                                           output meter (same readouts) <--------+
//
// MODES (the Mode switch)
//   Bypass   Gain 0 dB, limiter off. Audio is passed through bit-exact (delayed by the
//            same 140 samples / 3.2 ms as the other modes, so switching never shifts timing).
//   Measure  Stores the input measurement and rides the gain slowly toward the target from
//            the input short-term loudness (+/-12 dB, ~2 s down / ~6 s up). Gate: the gain
//            holds whenever the input is more than 10 LU below its own integrated loudness so
//            far (or below -60 LUFS), so silence, breaks and fades are not pulled up.
//   Locked   Switching to Locked IS the Lock button. From the stored blocks it computes the
//            input integrated loudness, offset = target - integrated, then simulates the
//            limiter on the stored peaks and corrects the offset until the predicted result
//            lands on target (see "Lock calculation" below). That one gain is then held fixed
//            (no riding). The calculation is spread over audio blocks so it never causes a
//            dropout. Changing Target or Ceiling while Locked re-runs it once.
//
// LIMITER (no coloration)
//   The limiter only ever multiplies the delayed signal by one smooth gain shared by both
//   channels: no clipping, saturation, filtering or per-channel processing. Peaks are found
//   on a 4x oversampled copy (the BS.1770 true-peak method, 24-tap polyphase interpolator).
//   2.9 ms look-ahead with an S-shaped gain curve into each peak, 150 ms release. It aims
//   0.05 dB under the ceiling. When nothing exceeds the ceiling the gain is exactly 1.0.
//
// METER
//   ITU-R BS.1770-4 / EBU R128: K-weighting, 400 ms momentary, 3 s short-term, integrated
//   with -70 LUFS absolute and -10 LU relative gates (400 ms windows, 75 % overlap, built
//   from 100 ms blocks). "Avg" is the plain ungated average since start or Refresh.
//
// BUTTONS
//   Reset    clears the stored measurement, the locked gain and all readouts.
//   Refresh  clears the readouts only (averages, output integrated, max true peaks, max GR).
//
// SAVED WITH THE PROJECT
//   All settings plus the locked gain, the lock's target/ceiling/prediction, the leveler's
//   gain and the stored blocks (8 bytes per 100 ms: about 290 KB per hour).

#include "mpc_plugin.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #include <arm_neon.h>
#endif

namespace {

// Parameter order is permanent (saved projects). Append new parameters at the end only.
enum ParamIndex
{
    kCeiling = 0,   // dBTP. Index 0 is nudged by the host on every load: harmless here.
    kTarget,        // preset target loudness
    kCustom,        // custom target, used when Target = Custom
    kMode,          // Bypass / Measure / Locked (switching to Locked performs the lock)
    kReset,         // momentary button
    kRefresh,       // momentary button
    // Read-only displays. Their value text comes from the meters, not from the value.
    kInMom, kInShort, kInInteg, kInAvg,
    kOutMom, kOutShort, kOutInteg, kOutAvg,
    kInTpMax, kOutTpMax,
    kGain,          // gain being applied before the limiter (leveler or locked offset)
    kGr,            // limiter gain reduction now (worst value of the last 100 ms)
    kGrMax,         // largest gain reduction since Refresh
    kPredicted,     // Locked: predicted integrated loudness of the output
    // Loudness history (display only): input loudness of the stored measurement in 5 s columns,
    // oldest first; the last one is the column being filled. Appended after the original
    // parameters so saved projects keep their parameter order.
    kHistFirst,
    // Touch-help (display only). The MASTER page's invisible help buttons set kHelp; the plugin
    // mirrors it into kHelp2. Their value text is the first / second line of the explanation.
    kHelp = kHistFirst + 48,
    kHelp2,
    kNumParams
};

constexpr int kFirstReadout = kInMom;
constexpr int kNumReadouts  = kPredicted + 1 - kFirstReadout;
constexpr int kHistCols     = kHelp - kHistFirst;

// Help text, one entry per help item (button id on the MASTER page). Max 31 characters per line
// (the VST display-text buffer).
const char* const kHelpText[16][2] = {
    {"Touch a meter or a label",       "to see what it does."},
    {"Loudness of the last 0.4 s",     "at the output, in LUFS."},
    {"Loudness of the last 3 s",       "at the output, in LUFS."},
    {"Gated average loudness of the",  "output since Refresh, LUFS."},
    {"Loudness to aim for. Double-",   "tap the target to change it."},
    {"How hard the limiter is",        "working right now, in dB."},
    {"Most gain reduction since",      "Refresh, in dB."},
    {"Highest true peak at the",       "input since Refresh, dBTP."},
    {"Gain before the limiter:",       "leveler or locked offset, dB."},
    {"Expected loudness of the",       "bounce once Locked, in LUFS."},
    {"Highest true peak at the",       "output since Refresh, dBTP."},
    {"Bypass: off. Measure: learn",    "and ride. Locked: fixed gain."},
    {"Highest true peak the limiter",  "lets through, in dBTP."},
    {"Your own target loudness,",      "used when Target is Custom."},
    {"Reset: clear measurement+lock.", "Refresh: clear readouts only."},
    {"Running integrated loudness of", "the measurement so far, LUFS."},
};
constexpr int kHistColBlocks = 50;          // 50 x 100 ms = 5 s per column -> 4 minutes on screen

enum Mode { kModeBypass = 0, kModeMeasure = 1, kModeLocked = 2 };

constexpr float kPresetTargets[4] = { -14.0f, -16.0f, -23.0f, -24.0f };   // index 4 = Custom

constexpr int    kBoxLen      = 65;                       // each of the two attack smoothing stages
constexpr int    kLookahead   = 2 * (kBoxLen - 1);        // 128 samples = 2.9 ms at 44.1 kHz
constexpr int    kTaps        = 24;                       // interpolator taps per phase
constexpr int    kInterpDelay = kTaps / 2;                // detector latency
constexpr int    kLatency     = kLookahead + kInterpDelay; // 140 samples = 3.2 ms
constexpr int    kDelaySize   = 256;                      // power of two > kLatency
constexpr int    kMaxBlocks   = 72000;                    // 2 hours of 100 ms records
constexpr float  kMarginDb    = 0.05f;                    // limiter aims this far below the ceiling
constexpr float  kLevelerRange = 12.0f;                   // leveler range, +/- dB
constexpr float  kLevelerGateLu = 20.0f;                  // hold when input is this far below target
constexpr float  kLevelerDownSec = 2.0f;                  // time constants of the gain ride
constexpr float  kLevelerUpSec   = 6.0f;
constexpr float  kLockMaxDb   = 30.0f;                    // largest locked offset either way
constexpr int    kLockIterations = 10;                    // most locks converge in 2-4
constexpr int    kLockBlocksPerFrame = 4;                 // lock calculation work per audio frame

constexpr float  kNone = -1000.0f;                        // "no value yet" (shown as --)
constexpr float  kBusy = -2000.0f;                        // "calculating" (shown as calc)

constexpr double kAbsGateEnergy = 1.1724653045822964e-7;  // 10^((-70 + 0.691) / 10)

inline double energyToLufs(double e) { return e > 1e-20 ? -0.691 + 10.0 * std::log10(e) : (double)kNone; }
inline float  dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }
inline float  linToDb(float g)  { return g > 1e-9f ? 20.0f * std::log10(g) : -180.0f; }

// ---------------------------------------------------------------------------
// K-weighting (BS.1770-4): high shelf then high pass, coefficients derived for the actual
// sample rate from the analogue prototype (same derivation as libebur128 / pyloudnorm).
// Double precision: the 38 Hz high pass needs it for an accurate meter.
// ---------------------------------------------------------------------------
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
        {   // high shelf, +4 dB above ~1.7 kHz
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan(pi * f0 / sr), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            Biquad64 b;
            b.b0 = (Vh + Vb * K / Q + K * K) / a0;
            b.b1 = 2.0 * (K * K - Vh) / a0;
            b.b2 = (Vh - Vb * K / Q + K * K) / a0;
            b.a1 = 2.0 * (K * K - 1.0) / a0;
            b.a2 = (1.0 - K / Q + K * K) / a0;
            shelf[0] = shelf[1] = b;
        }
        {   // high pass at ~38 Hz
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan(pi * f0 / sr);
            const double a0 = 1.0 + K / Q + K * K;
            Biquad64 b;
            b.b0 = 1.0; b.b1 = -2.0; b.b2 = 1.0;
            b.a1 = 2.0 * (K * K - 1.0) / a0;
            b.a2 = (1.0 - K / Q + K * K) / a0;
            hp[0] = hp[1] = b;
        }
        reset();
    }
    void reset() { for (int c = 0; c < 2; ++c) { shelf[c].reset(); hp[c].reset(); } }

    // Sum over both channels of the squared K-weighted sample.
    double energy(float l, float r)
    {
        const double yl = hp[0].process(shelf[0].process((double)l));
        const double yr = hp[1].process(shelf[1].process((double)r));
        return yl * yl + yr * yr;
    }
};

// ---------------------------------------------------------------------------
// 4x oversampled true-peak estimate: polyphase windowed-sinc interpolator (Kaiser window,
// 24 taps per phase). Returns the largest |value| over both channels for the sample
// kInterpDelay samples ago, including the three points between it and the next sample.
// The four output points (the sample itself + 3 phases) are computed as 4 independent lanes
// (h[k][lane]), which the compiler turns into one NEON/SSE vector multiply-add per tap.
// ---------------------------------------------------------------------------
struct TruePeak
{
    alignas(16) float h[kTaps][4];
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
        const double pi = 3.14159265358979323846, beta = 5.0, half = kTaps / 2.0;
        for (int p = 0; p < 3; ++p)
        {
            const double frac = (p + 1) * 0.25;
            double sum = 0.0;
            double tmp[kTaps];
            for (int k = 0; k < kTaps; ++k)
            {
                const double t = (double)k - (kInterpDelay - 1) - frac;   // distance from the interpolated point
                const double sinc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(pi * t) / (pi * t);
                const double u = t / half;
                const double w = std::fabs(u) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - u * u)) / besselI0(beta);
                tmp[k] = sinc * w;
                sum += tmp[k];
            }
            for (int k = 0; k < kTaps; ++k) h[k][p + 1] = (float)(tmp[k] / sum);
        }
        for (int k = 0; k < kTaps; ++k) h[k][0] = (k == kInterpDelay - 1) ? 1.0f : 0.0f;   // lane 0: the sample
        reset();
    }
    void reset() { std::memset(hist, 0, sizeof(hist)); pos = 0; }

    float push(float l, float r)
    {
        hist[0][pos] = hist[0][pos + kTaps] = l;
        hist[1][pos] = hist[1][pos + kTaps] = r;
        if (++pos == kTaps) pos = 0;
        const float* wl = &hist[0][pos];   // w[0] oldest ... w[kTaps-1] newest
        const float* wr = &hist[1][pos];
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        // Explicit NEON: clang does not auto-vectorise float on 32-bit ARM (the Force), and
        // flush-to-zero is already on (the framework sets it), so this is exact enough here.
        float32x4_t vl = vdupq_n_f32(0.0f), vr = vdupq_n_f32(0.0f);
        for (int k = 0; k < kTaps; ++k)
        {
            const float32x4_t hk = vld1q_f32(h[k]);
            vl = vmlaq_n_f32(vl, hk, wl[k]);
            vr = vmlaq_n_f32(vr, hk, wr[k]);
        }
        const float32x4_t m4 = vmaxq_f32(vabsq_f32(vl), vabsq_f32(vr));
        float32x2_t m2 = vpmax_f32(vget_low_f32(m4), vget_high_f32(m4));
        m2 = vpmax_f32(m2, m2);
        return vget_lane_f32(m2, 0);
#else
        alignas(16) float al[4] = {0, 0, 0, 0}, ar[4] = {0, 0, 0, 0};
        for (int k = 0; k < kTaps; ++k)
        {
            const float xl = wl[k], xr = wr[k];
            for (int lane = 0; lane < 4; ++lane) { al[lane] += h[k][lane] * xl; ar[lane] += h[k][lane] * xr; }
        }
        float peak = 0.0f;
        for (int lane = 0; lane < 4; ++lane) peak = std::fmax(peak, std::fmax(std::fabs(al[lane]), std::fabs(ar[lane])));
        return peak;
#endif
    }
};

// ---------------------------------------------------------------------------
// Moving average over kBoxLen samples (running sum, re-summed once per lap so rounding never
// drifts: an idle limiter stays at exactly 1.0).
// ---------------------------------------------------------------------------
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
        if (++pos == kBoxLen)
        {
            pos = 0;
            double s = 0.0;
            for (float v : ring) s += v;
            sum = s;
        }
        return (float)(sum * (1.0 / kBoxLen));
    }
};

// ---------------------------------------------------------------------------
// Look-ahead limiter gain computer. Input: required gain per sample (<= 1). Output: a smooth
// gain that is never above the required gain of the sample it is applied to (the sample
// kLookahead older than the newest input):
//   held = minimum of the last kLookahead+1 required gains (monotonic queue)
//   env  = instant attack to held, exponential release (150 ms)
//   out  = two cascaded kBoxLen moving averages of env: an S-shaped gain curve into each
//          peak over 2.9 ms. Every env value in the averaged span covers the target sample,
//          so the output can never exceed its required gain. The gentle curve keeps the gain
//          change itself from creating new inter-sample peaks or audible clicks.
// ---------------------------------------------------------------------------
struct LimiterGain
{
    static constexpr int kQ = 256;   // power of two > kLookahead + 1
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

// ---------------------------------------------------------------------------
// Gated loudness histogram (EBU R128 integrated): 0.1 LU bins from -70 to +10 LUFS with the
// exact energy sum per bin, so the result does not depend on the bin width except at the
// relative-gate edge. Constant memory however long the measurement runs.
// ---------------------------------------------------------------------------
struct LoudnessHistogram
{
    static constexpr int kBins = 800;
    uint32_t count[kBins];
    double   energy[kBins];
    uint64_t totalCount = 0;   // running totals: the absolute-gated mean needs no scan
    double   totalEnergy = 0.0;

    void clear() { std::memset(count, 0, sizeof(count)); std::memset(energy, 0, sizeof(energy)); totalCount = 0; totalEnergy = 0.0; }

    void add(double e)
    {
        if (!(e > kAbsGateEnergy)) return;
        const double l = energyToLufs(e);
        const int b = mpc::clamp((int)((l + 70.0) * 10.0), 0, kBins - 1);
        ++count[b];
        energy[b] += e;
        ++totalCount;
        totalEnergy += e;
    }

    // Scans only the bins above the relative gate (typically 150-350 of them).
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

struct BlockRecord { float energy; float peak; };   // one 100 ms block of the input

} // namespace

class LUFSMaster : public mpc::Plugin
{
public:
    LUFSMaster() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("ceiling", "Ceiling", -3.0f, 0.0f, -1.0f, "dBTP", 1));
        addParam(mpc::choiceParam("target", "Target", {"-14 LUFS Streaming", "-16 LUFS Apple/Podcast", "-23 LUFS EBU R128",
                                                       "-24 LUFS ATSC A/85", "Custom"}, 0));
        addParam(mpc::floatParam("custom", "Custom LUFS", -30.0f, -6.0f, -14.0f, "LUFS", 1));
        addParam(mpc::choiceParam("mode", "Mode", {"Bypass", "Measure", "Locked"}, 1));
        addParam(mpc::boolParam("reset", "Reset", false));
        addParam(mpc::boolParam("refresh", "Refresh", false));

        static const char* const ids[kNumReadouts] = {
            "in_mom", "in_short", "in_integ", "in_avg", "out_mom", "out_short", "out_integ", "out_avg",
            "in_tp_max", "out_tp_max", "gain", "gr", "gr_max", "predicted" };
        static const char* const names[kNumReadouts] = {
            "In Mom LUFS", "In ST LUFS", "In Int LUFS", "In Avg LUFS",
            "Out Mom LUFS", "Out ST LUFS", "Out Int LUFS", "Out Avg LUFS",
            "In TP Max", "Out TP Max", "Gain", "GR", "GR Max", "Predicted" };
        for (int i = 0; i < kNumReadouts; ++i)
        {
            mpc::Param p = mpc::floatParam(ids[i], names[i], 0.0f, 1.0f, 0.0f, "", 2);
            p.automatable = false;   // displays: keep them out of automation lanes
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
            histVal_[i] = kNone;
            histDisp_[i].store(kNone);
            histDirty_[i] = true;
        }

        std::vector<std::string> help1, help2;
        for (const auto& h : kHelpText) { help1.push_back(h[0]); help2.push_back(h[1]); }
        mpc::Param h1 = mpc::choiceParam("help", "Help", help1, 0);
        mpc::Param h2 = mpc::choiceParam("help2", "Help 2", help2, 0);
        h1.automatable = false;
        h2.automatable = false;
        addParam(h1);
        addParam(h2);

        // Programs set only Ceiling, Target and Custom (the first three parameters), so
        // picking one never changes the mode or touches the measurement.
        addProgram("Streaming -14", { -1.0f, 0.0f, -14.0f });
        addProgram("Apple/Podcast -16", { -1.0f, 1.0f, -16.0f });
        addProgram("EBU R128 -23", { -1.0f, 2.0f, -23.0f });
        addProgram("ATSC A/85 -24", { -2.0f, 3.0f, -24.0f });

        // All memory is allocated here, once: prepare() can run many times and must not drop
        // the measurement.
        bufA_.assign(kMaxBlocks, BlockRecord{0.0f, 0.0f});
        bufB_.assign(kMaxBlocks, BlockRecord{0.0f, 0.0f});
        active_.store(bufA_.data());
        inHist_.clear(); outHist_.clear(); stagedHist_.clear();
        tp_.design(); outTp_.design();
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sampleRate_ = sampleRate;
        blockLen_ = (int)std::lround(sampleRate * 0.1);
        if (blockLen_ < 1) blockLen_ = 1;
        inK_.prepare(sampleRate); outK_.prepare(sampleRate);
        limiter_.prepare(sampleRate);
        gainCoef_ = (float)(1.0 - std::exp(-1.0 / (0.030 * sampleRate)));   // 30 ms gain glide
        reset();
    }

    // Runs on resume and after a state/program load. Clears DSP memory, never the measurement.
    void reset() override
    {
        inK_.reset(); outK_.reset(); tp_.reset(); outTp_.reset(); limiter_.reset();
        std::memset(delay_, 0, sizeof(delay_));
        delayPos_ = 0;
        blockPos_ = 0; inAcc_ = outAcc_ = 0.0; inPeakAcc_ = outPeakAcc_ = 0.0f; grAcc_ = 1.0f;

        if (stagePending_.exchange(false, std::memory_order_acq_rel))
        {   // install a loaded project's measurement and lock
            BlockRecord* old = active_.load();
            active_.store(old == bufA_.data() ? bufB_.data() : bufA_.data());
            std::memcpy(&inHist_, &stagedHist_, sizeof(inHist_));
            count_.store(stagedCount_);
            rebuildHistory();
            lockValid_ = stagedLockValid_;
            lockedDb_.store(stagedLockedDb_);
            lockTarget_.store(stagedLockTarget_);
            lockCeiling_.store(stagedLockCeiling_);
            predicted_.store(stagedPredicted_);
            levelerDb_.store(stagedLevelerDb_);
            levelerDesired_ = stagedLevelerDb_;
            job_.active = false;
            inRingN_ = 0;
        }
        else if (job_.active)
        {
            job_.active = false;           // a resume interrupted a lock: run it again
            lockRequested_ = true;
        }

        // The current switch positions are the baseline: a load or resume never "presses" them.
        lastMode_ = getInt(kMode);
        if (getBool(kReset)) set(kReset, 0.0f);
        if (getBool(kRefresh)) set(kRefresh, 0.0f);

        appliedGain_ = gainTargetLin();
        publishCounter_ = 0;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int numFrames = ctx.numFrames;
        const int mode = getInt(kMode);
        const float target = effectiveTarget();
        const float ceiling = get(kCeiling);

        // ----- buttons and mode changes (once per block) -----------------------------------
        if (getBool(kReset))   { clearMeasurement(); clearReadouts(); set(kReset, 0.0f);   notifyHostParameterChanged(kReset); }
        if (getBool(kRefresh)) { clearReadouts();                     set(kRefresh, 0.0f); notifyHostParameterChanged(kRefresh); }
        if (getInt(kHelp2) != getInt(kHelp)) { set(kHelp2, (float)getInt(kHelp)); notifyHostParameterChanged(kHelp2); }   // help line 2 follows line 1 (display only)

        if (mode == kModeLocked && lastMode_ != kModeLocked) lockRequested_ = true;
        if (mode == kModeLocked && !job_.active && lockValid_ &&
            (std::fabs(target - lockTarget_.load()) > 1e-3f || std::fabs(ceiling - lockCeiling_.load()) > 1e-3f))
            lockRequested_ = true;
        if (mode != kModeLocked) { lockRequested_ = false; job_.active = false; }
        lastMode_ = mode;
        if (lockRequested_) { startLock(target, ceiling); lockRequested_ = false; }
        if (job_.active) runLockJob(numFrames * kLockBlocksPerFrame);

        // ----- per-block settings ---------------------------------------------------------
        const float gTarget = gainTargetLin();
        const bool  limiting = mode != kModeBypass;
        const float ceilLin = dbToLin(ceiling - kMarginDb);

        const bool haveIn = ctx.inputs != nullptr && ctx.numInputs > 0 && ctx.inputs[0] != nullptr;
        const float* inL = haveIn ? ctx.inputs[0] : nullptr;
        const float* inR = haveIn ? ((ctx.numInputs > 1 && ctx.inputs[1] != nullptr) ? ctx.inputs[1] : ctx.inputs[0]) : nullptr;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        for (int i = 0; i < numFrames; ++i)
        {
            const float l = haveIn ? inL[i] : 0.0f;          // read the frame first (in place)
            const float r = haveIn ? inR[i] : 0.0f;

            inAcc_ += inK_.energy(l, r);

            float g = appliedGain_;
            if (g != gTarget) { g += (gTarget - g) * gainCoef_; if (std::fabs(gTarget - g) < 1e-7f) g = gTarget; appliedGain_ = g; }
            const float xl = l * g, xr = r * g;

            const float peak = tp_.push(xl, xr);              // true peak of x, kInterpDelay ago
            inPeakAcc_ = std::fmax(inPeakAcc_, peak / g);
            const float required = (limiting && peak > ceilLin) ? ceilLin / peak : 1.0f;
            const float lg = limiter_.process(required);
            grAcc_ = std::fmin(grAcc_, lg);

            delay_[0][delayPos_] = xl; delay_[1][delayPos_] = xr;
            const int rd = (delayPos_ - kLatency) & (kDelaySize - 1);
            const float yl = delay_[0][rd] * lg;
            const float yr = delay_[1][rd] * lg;
            delayPos_ = (delayPos_ + 1) & (kDelaySize - 1);

            outL[i] = yl;
            if (outR != nullptr) outR[i] = yr;

            outAcc_ += outK_.energy(yl, yr);
            outPeakAcc_ = std::fmax(outPeakAcc_, outTp_.push(yl, yr));

            if (++blockPos_ >= blockLen_) finishBlock(mode, target);
        }
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        char buf[32];
        if (index == kCustom)
        {
            std::snprintf(buf, sizeof(buf), "%.1f", (double)roundHalf(plainValue));
            out = buf; return true;
        }
        if (index == kCeiling)
        {
            std::snprintf(buf, sizeof(buf), "%.1f dBTP", (double)plainValue);
            out = buf; return true;
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
        const bool isDb = index == kInTpMax || index == kOutTpMax || index == kGain || index == kGr || index == kGrMax;
        if (index == kGain) std::snprintf(buf, sizeof(buf), "%+.1f dB", (double)v);
        else                std::snprintf(buf, sizeof(buf), isDb ? "%.1f dB" : "%.1f", (double)v);
        if (std::strcmp(buf, "-0.0 dB") == 0) std::snprintf(buf, sizeof(buf), "0.0 dB");
        if (std::strcmp(buf, "+0.0 dB") == 0) std::snprintf(buf, sizeof(buf), "0.0 dB");
        out = buf;
        return true;
    }

    // ----- state: version 1 ------------------------------------------------------------------
    //   u8 version | u8 lockValid | f32 lockedDb, lockTarget, lockCeiling, predicted, levelerDb
    //   | u32 count | count x { f32 energy, f32 peak }
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        const uint32_t n = count_.load(std::memory_order_acquire);
        const BlockRecord* blocks = active_.load(std::memory_order_acquire);
        out.reserve(1 + 1 + 5 * 4 + 4 + (size_t)n * 8);
        out.push_back(1);
        out.push_back(lockValid_ ? 1 : 0);
        putF32(out, lockedDb_.load()); putF32(out, lockTarget_.load()); putF32(out, lockCeiling_.load());
        putF32(out, predicted_.load()); putF32(out, levelerDb_.load());
        putU32(out, n);
        for (uint32_t k = 0; k < n; ++k) { putF32(out, blocks[k].energy); putF32(out, blocks[k].peak); }
    }

    void loadExtraState(const uint8_t* d, size_t size) override
    {
        if (d == nullptr || size < 26 || d[0] != 1) return;   // unknown version: keep what we have
        size_t pos = 2;
        const bool lockValid = d[1] != 0;
        const float lockedDb = getF32(d, pos), lockTarget = getF32(d, pos), lockCeiling = getF32(d, pos);
        const float predicted = getF32(d, pos), levelerDb = getF32(d, pos);
        uint32_t n = 0; std::memcpy(&n, d + pos, 4); pos += 4;   // little-endian on every MPC and desktop
        if (n > (uint32_t)kMaxBlocks || pos + (size_t)n * 8 > size) return;
        if (!std::isfinite(lockedDb) || !std::isfinite(levelerDb)) return;

        BlockRecord* staging = active_.load() == bufA_.data() ? bufB_.data() : bufA_.data();
        stagedHist_.clear();
        for (uint32_t k = 0; k < n; ++k)
        {
            BlockRecord b; b.energy = getF32(d, pos); b.peak = getF32(d, pos);
            if (!std::isfinite(b.energy) || b.energy < 0.0f) b.energy = 0.0f;
            if (!std::isfinite(b.peak) || b.peak < 0.0f) b.peak = 0.0f;
            staging[k] = b;
            if (k >= 3)
                stagedHist_.add(((double)staging[k - 3].energy + staging[k - 2].energy + staging[k - 1].energy + staging[k].energy) * 0.25);
        }
        stagedCount_ = n;
        stagedLockValid_ = lockValid;
        stagedLockedDb_ = mpc::clamp(lockedDb, -kLockMaxDb, kLockMaxDb);
        stagedLockTarget_ = lockTarget; stagedLockCeiling_ = lockCeiling;
        stagedPredicted_ = predicted;
        stagedLevelerDb_ = mpc::clamp(levelerDb, -kLevelerRange, kLevelerRange);
        stagePending_.store(true, std::memory_order_release);
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name           = "LUFS Master";
        info.vendor         = "sureeel";   // must match plugin.json "manufacturer"
        info.uniqueId       = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version        = 1000;
        info.isInstrument   = false;
        info.numInputs      = 2;
        info.numOutputs     = 2;
        info.latencySamples = kLatency;
        info.tailSamples    = kLatency;
        return info;
    }
    static constexpr char kUid[5] = "S6yP";

    static float roundHalf(float v) { return std::round(v * 2.0f) * 0.5f; }
    static void  putF32(std::vector<uint8_t>& o, float f) { uint8_t b[4]; std::memcpy(b, &f, 4); o.insert(o.end(), b, b + 4); }
    static void  putU32(std::vector<uint8_t>& o, uint32_t v) { uint8_t b[4]; std::memcpy(b, &v, 4); o.insert(o.end(), b, b + 4); }
    static float getF32(const uint8_t* d, size_t& pos) { float f; std::memcpy(&f, d + pos, 4); pos += 4; return f; }

    float effectiveTarget() const
    {
        const int t = getInt(kTarget);
        return (t >= 0 && t < 4) ? kPresetTargets[t] : roundHalf(get(kCustom));
    }

    float gainTargetDb() const
    {
        switch (getInt(kMode))
        {
            case kModeMeasure: return levelerDb_.load(std::memory_order_relaxed);
            case kModeLocked:  return lockValid_ ? lockedDb_.load(std::memory_order_relaxed) : 0.0f;
            default:           return 0.0f;
        }
    }
    float gainTargetLin() const { const float db = gainTargetDb(); return db == 0.0f ? 1.0f : dbToLin(db); }

    // ----- every 100 ms -------------------------------------------------------------------
    void finishBlock(int mode, float target)
    {
        const double inE  = inAcc_ / blockLen_;
        const double outE = outAcc_ / blockLen_;
        inAcc_ = outAcc_ = 0.0; blockPos_ = 0;

        inRing_[inRingPos_] = inE; outRing_[inRingPos_] = outE;
        inRingPos_ = (inRingPos_ + 1) % 30;
        if (inRingN_ < 30) ++inRingN_;

        auto ringMean = [this](const double* ring, int len) {
            double s = 0.0; const int n = len < inRingN_ ? len : inRingN_;
            for (int k = 1; k <= n; ++k) s += ring[(inRingPos_ - k + 30) % 30];
            return n > 0 ? s / n : 0.0;
        };
        const double inMomE = ringMean(inRing_, 4),  inShortE = ringMean(inRing_, 30);
        const double outMomE = ringMean(outRing_, 4), outShortE = ringMean(outRing_, 30);
        inMom_ = (float)energyToLufs(inMomE);   inShort_ = (float)energyToLufs(inShortE);
        outMom_ = (float)energyToLufs(outMomE); outShort_ = (float)energyToLufs(outShortE);

        // stored measurement (Measure mode only)
        if (mode == kModeMeasure)
        {
            const uint32_t n = count_.load(std::memory_order_relaxed);
            if (n < (uint32_t)kMaxBlocks)
            {
                BlockRecord* blocks = active_.load(std::memory_order_relaxed);
                blocks[n].energy = (float)inE;
                blocks[n].peak   = inPeakAcc_;
                count_.store(n + 1, std::memory_order_release);
                if (n + 1 >= 4)
                    inHist_.add(((double)blocks[n - 3].energy + blocks[n - 2].energy + blocks[n - 1].energy + blocks[n].energy) * 0.25);
                historyAddBlock(n, inE);
            }
        }
        if (inRingN_ >= 4) outHist_.add(outMomE);

        inAvgSum_ += inE; outAvgSum_ += outE; ++avgN_;
        inTpMax_  = std::fmax(inTpMax_, inPeakAcc_);
        outTpMax_ = std::fmax(outTpMax_, outPeakAcc_);
        grNow_ = linToDb(grAcc_);
        grMax_ = std::fmin(grMax_, grNow_);
        inPeakAcc_ = outPeakAcc_ = 0.0f; grAcc_ = 1.0f;

        // leveler: ride toward the target from the input short-term loudness. Gate: hold the
        // gain whenever the input is more than 10 LU below the programme's own integrated
        // loudness so far (the BS.1770 relative gate), or below -60 LUFS, so breaks, fades
        // and silence are not pulled up.
        // The two integrated values are recomputed on alternate blocks (each at 5 Hz) so no
        // single audio block carries all the meter work.
        if ((publishCounter_ & 1) == 0) inInteg_ = (float)inHist_.integrated();
        else                            outInteg_ = (float)outHist_.integrated();

        if (mode == kModeMeasure)
        {
            const float integ = inInteg_;
            const float gate = integ > kNone + 1.0f ? std::fmax(-60.0f, integ - 10.0f)
                                                     : std::fmax(-60.0f, target - kLevelerGateLu);
            if (inRingN_ >= 4 && inShort_ > gate)
                levelerDesired_ = mpc::clamp(target - inShort_, -kLevelerRange, kLevelerRange);
            float lv = levelerDb_.load(std::memory_order_relaxed);
            const float d = levelerDesired_ - lv;
            lv += d * (0.1f / (d < 0.0f ? kLevelerDownSec : kLevelerUpSec));
            levelerDb_.store(lv, std::memory_order_relaxed);
        }

        publishReadouts(mode, publishCounter_ & 1);
        publishHistory();
        ++publishCounter_;
    }

    // ----- loudness history (display only) -----------------------------------------------------
    // Column c covers stored blocks [c*50, c*50+50): the ungated input loudness of 5 s of the
    // measurement. The newest column (being filled) is shown in the last slot.
    void historyAddBlock(uint32_t blockIndex, double energy)
    {
        if (blockIndex % kHistColBlocks == 0)
        {   // a new column starts: scroll left
            for (int i = 0; i + 1 < kHistCols; ++i) histVal_[i] = histVal_[i + 1];
            for (bool& d : histDirty_) d = true;
            histAccE_ = 0.0; histAccN_ = 0;
        }
        histAccE_ += energy; ++histAccN_;
        const float v = (float)energyToLufs(histAccE_ / histAccN_);
        if (v != histVal_[kHistCols - 1]) { histVal_[kHistCols - 1] = v; histDirty_[kHistCols - 1] = true; }
    }

    // Recomputes the whole history from the stored blocks (project load, Reset). O(stored blocks).
    void rebuildHistory()
    {
        const uint32_t n = count_.load(std::memory_order_relaxed);
        const BlockRecord* blocks = active_.load(std::memory_order_relaxed);
        const int cols = (int)((n + kHistColBlocks - 1) / kHistColBlocks);
        for (int i = 0; i < kHistCols; ++i)
        {
            const int c = cols - kHistCols + i;            // column shown in slot i
            histVal_[i] = kNone;
            if (c < 0) continue;
            double e = 0.0; uint32_t k = 0;
            for (uint32_t b = (uint32_t)c * kHistColBlocks; b < n && b < (uint32_t)(c + 1) * kHistColBlocks; ++b, ++k)
                e += blocks[b].energy;
            if (k > 0) histVal_[i] = (float)energyToLufs(e / k);
            if (c == cols - 1) { histAccE_ = e; histAccN_ = (int)k; }
        }
        if (n == 0) { histAccE_ = 0.0; histAccN_ = 0; }
        for (bool& d : histDirty_) d = true;
    }

    // Sends at most 8 changed history columns per 100 ms block, so a scroll (48 columns) is
    // spread over 0.6 s instead of hitting the host all at once.
    void publishHistory()
    {
        int sent = 0;
        for (int k = 0; k < kHistCols && sent < 8; ++k)
        {
            const int i = (histPublishPos_ + k) % kHistCols;
            if (!histDirty_[i]) continue;
            histDirty_[i] = false;
            float x = histVal_[i];
            if (x > kNone + 1.0f && x < -99.0f) x = kNone;
            histDisp_[i].store(x, std::memory_order_relaxed);
            setNormalised(kHistFirst + i, x <= kNone + 1.0f ? 0.0f : mpc::clamp((x + 48.0f) / 48.0f, 0.0f, 1.0f));
            notifyHostParameterChanged(kHistFirst + i);
            ++sent;
        }
        histPublishPos_ = (histPublishPos_ + 8) % kHistCols;
    }

    // Updates the display values and tells the host about the ones that changed (display only,
    // no effect on the audio). The meters (momentary, short-term, gain reduction) refresh every
    // 100 ms block (10 Hz); the other readouts alternate, even/odd, at 5 Hz. The screen is
    // therefore never asked to redraw faster than 10 Hz.
    void publishReadouts(int mode, int half)
    {
        float v[kNumReadouts];
        v[kInMom - kFirstReadout]     = inMom_;
        v[kInShort - kFirstReadout]   = inShort_;
        v[kInInteg - kFirstReadout]   = inInteg_;
        v[kInAvg - kFirstReadout]     = avgN_ ? (float)energyToLufs(inAvgSum_ / avgN_) : kNone;
        v[kOutMom - kFirstReadout]    = outMom_;
        v[kOutShort - kFirstReadout]  = outShort_;
        v[kOutInteg - kFirstReadout]  = outInteg_;
        v[kOutAvg - kFirstReadout]    = avgN_ ? (float)energyToLufs(outAvgSum_ / avgN_) : kNone;
        v[kInTpMax - kFirstReadout]   = inTpMax_ > 0.0f ? linToDb(inTpMax_) : kNone;
        v[kOutTpMax - kFirstReadout]  = outTpMax_ > 0.0f ? linToDb(outTpMax_) : kNone;
        v[kGain - kFirstReadout]      = mode == kModeLocked && job_.active ? kBusy : gainTargetDb();
        v[kGr - kFirstReadout]        = grNow_;
        v[kGrMax - kFirstReadout]     = grMax_;
        v[kPredicted - kFirstReadout] = mode != kModeLocked ? kNone : (job_.active ? kBusy : (lockValid_ ? predicted_.load() : kNone));

        for (int k = 0; k < kNumReadouts; ++k)
        {
            const int idx = kFirstReadout + k;
            const bool meter = idx == kInMom || idx == kOutMom || idx == kInShort || idx == kOutShort || idx == kGr;
            if (!meter && (k & 1) != half) continue;
            float x = v[k];
            if (x > kNone + 1.0f && x < -99.0f) x = kNone;   // below the meter range
            const int shown = (int)std::lround(x * 10.0f);
            const int index = kFirstReadout + k;
            const float norm = readoutNorm(index, x);
            if (shown == lastShown_[k] && std::fabs(norm - getNormalised(index)) < 1e-3f) continue;
            lastShown_[k] = shown;
            display_[k].store(x, std::memory_order_relaxed);
            setNormalised(index, norm);
            notifyHostParameterChanged(index);
        }
    }

    // Bar position of each display (0..1). The MASTER page's LED film strips use these directly:
    // loudness -48..0 LUFS (49 frames, 1 LU per LED), gain reduction 0..12 dB (25 frames,
    // 0.5 dB per LED). Display only.
    static float readoutNorm(int index, float v)
    {
        if (v <= kNone + 1.0f) return 0.0f;
        switch (index)
        {
            case kInTpMax: case kOutTpMax: return mpc::clamp((v + 30.0f) / 30.0f, 0.0f, 1.0f);
            case kGain:                    return mpc::clamp((v + 24.0f) / 48.0f, 0.0f, 1.0f);
            case kGr: case kGrMax:         return mpc::clamp(-v / 12.0f, 0.0f, 1.0f);
            default:                       return mpc::clamp((v + 48.0f) / 48.0f, 0.0f, 1.0f);
        }
    }

    void clearMeasurement()
    {
        count_.store(0, std::memory_order_release);
        inHist_.clear();
        inInteg_ = kNone;
        rebuildHistory();
        lockValid_ = false;
        lockedDb_.store(0.0f); predicted_.store(kNone);
        levelerDb_.store(0.0f); levelerDesired_ = 0.0f;
        job_.active = false;
        if (getInt(kMode) == kModeLocked) lockRequested_ = false;
    }

    void clearReadouts()
    {
        outHist_.clear();
        outInteg_ = kNone;
        inAvgSum_ = outAvgSum_ = 0.0; avgN_ = 0;
        inTpMax_ = outTpMax_ = 0.0f;
        grMax_ = 0.0f;
        for (int& s : lastShown_) s = INT32_MIN;   // force a refresh of every display
    }

    // ----- Lock calculation ------------------------------------------------------------------
    // Works on the stored 100 ms blocks, as 400 ms gating windows (4 blocks, 75 % overlap).
    // Evaluation 0: the input's gated integrated loudness I0 -> first offset = target - I0.
    // Evaluations 1..: predicted output loudness for an offset G, with a block-rate model of
    // the limiter:
    //   over_b    = block true peak + G - ceiling (dB)          required gain reduction
    //   gr_b      = max(over_b, gr_{b-1} * kModelReleaseKeep)   released into the next blocks
    //   through_b = S * (1 - exp(-(1 - K) * gr_b / S))          loudness that still gets through:
    //               a fraction (1-K) of small overshoots, never more than S dB (a brickwall
    //               limiter has a loudness ceiling)
    //   block energy is reduced by (gr_b - through_b) dB
    //   K, kModelReleaseKeep and S were fitted to 37 renders of this limiter (mastered songs and
    //   raw drums, 0..30 dB of gain reduction): RMS error 0.19 LU, songs within 0.3 LU.
    //   G is corrected with a secant step (the loudness gained per dB of gain shrinks as the
    //   limiter works harder) until the prediction is within 0.02 LU of the target. When one
    //   more dB of gain would add less than 0.1 LU the target is not reachable: the lock stops
    //   there and Predicted shows the loudness it will actually reach.
    // Each evaluation is two passes (absolute gate, then relative gate), spread over audio
    // blocks (kLockBlocksPerFrame stored blocks per audio frame) so it never causes a dropout.
    // A 5-minute measurement locks in well under a second; a 2-hour one in a few seconds.
    static constexpr double kModelLossK = 0.78;
    static constexpr double kModelReleaseKeep = 0.60;
    static constexpr double kModelThroughMaxDb = 1.5;

    struct LockJob
    {
        bool     active = false;
        int      eval = 0;          // 0 = input loudness, then predictions
        int      pass = 0;          // 0 = absolute-gate sum, 1 = relative-gate sum
        uint32_t b = 0, n = 0;      // block cursor, block count
        double   gr = 0.0;          // modelled gain reduction carried between blocks (dB)
        double   e1 = 0.0, e2 = 0.0, e3 = 0.0;   // modelled energies of the previous 3 blocks
        double   sum = 0.0; uint64_t cnt = 0;
        double   relThreshold = 0.0;
        double   gainDb = 0.0, prevGainDb = 0.0, prevResult = 0.0;
        double   target = -14.0, ceilDb = -1.05;
        double   lastResult = kNone;
    } job_;

    void startLock(float target, float ceiling)
    {
        job_ = LockJob{};
        job_.n = count_.load(std::memory_order_relaxed);
        job_.target = target;
        job_.ceilDb = (double)ceiling - kMarginDb;
        lockTarget_.store(target); lockCeiling_.store(ceiling);
        if (job_.n < 4) { lockValid_ = false; lockedDb_.store(0.0f); predicted_.store(kNone); return; }
        job_.active = true;
    }

    void runLockJob(int budget)
    {
        const BlockRecord* blocks = active_.load(std::memory_order_relaxed);
        const bool   model = job_.eval > 0;
        const double G2 = model ? std::pow(10.0, job_.gainDb / 10.0) : 1.0;
        const double threshold = job_.pass == 0 ? kAbsGateEnergy : job_.relThreshold;

        uint32_t end = job_.b + (uint32_t)(budget > 16 ? budget : 16);
        if (end > job_.n) end = job_.n;
        for (uint32_t i = job_.b; i < end; ++i)
        {
            double e = (double)blocks[i].energy * G2;
            if (model)
            {
                const double pk = blocks[i].peak;
                const double over = pk > 1e-9 ? 20.0 * std::log10(pk) + job_.gainDb - job_.ceilDb : -200.0;
                job_.gr = std::fmax(over, job_.gr * kModelReleaseKeep);
                if (job_.gr > 1e-4)
                {
                    const double through = kModelThroughMaxDb * (1.0 - std::exp(-(1.0 - kModelLossK) * job_.gr / kModelThroughMaxDb));
                    e *= std::exp(-0.230258509299404568 * (job_.gr - through));   // 10^(-loss/10)
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

        // pass finished
        if (job_.cnt == 0) { finishLock(false); return; }
        if (job_.pass == 0)
        {
            job_.relThreshold = std::fmax(kAbsGateEnergy, job_.sum / (double)job_.cnt * 0.1);
            job_.pass = 1;
        }
        else
        {
            const double I = energyToLufs(job_.sum / (double)job_.cnt);
            const double err = job_.target - I;
            double next;
            if (job_.eval == 0) next = err;                       // first guess: no limiting
            else
            {
                job_.lastResult = I;
                if (std::fabs(err) < 0.02 || job_.eval > kLockIterations) { finishLock(true); return; }
                double slope = 1.0;                                // dLUFS / dGain
                if (job_.eval >= 2 && std::fabs(job_.gainDb - job_.prevGainDb) > 1e-6)
                {
                    slope = (I - job_.prevResult) / (job_.gainDb - job_.prevGainDb);
                    if (err > 0.0 && slope < 0.1)
                    {   // more gain buys almost nothing: keep the smaller of the two gains
                        if (job_.prevGainDb < job_.gainDb) { job_.gainDb = job_.prevGainDb; job_.lastResult = job_.prevResult; }
                        finishLock(true);
                        return;
                    }
                    slope = mpc::clamp(slope, 0.1, 1.0);
                }
                next = job_.gainDb + err / slope;
            }
            job_.prevGainDb = job_.gainDb; job_.prevResult = I;
            next = mpc::clamp(next, -(double)kLockMaxDb, (double)kLockMaxDb);
            if (job_.eval > 0 && std::fabs(next - job_.gainDb) < 1e-4) { finishLock(true); return; }   // pinned at the limit
            job_.gainDb = next;
            ++job_.eval;
            job_.pass = 0;
        }
        job_.b = 0; job_.gr = 0.0; job_.e1 = job_.e2 = job_.e3 = 0.0; job_.sum = 0.0; job_.cnt = 0;
    }

    void finishLock(bool ok)
    {
        job_.active = false;
        lockValid_ = ok;
        lockedDb_.store(ok ? (float)job_.gainDb : 0.0f);
        predicted_.store(ok ? (float)job_.lastResult : kNone);
    }

    // ----- members ---------------------------------------------------------------------------
    double sampleRate_ = 44100.0;
    int    blockLen_ = 4410;
    float  gainCoef_ = 0.001f;
    float  appliedGain_ = 1.0f;

    KWeighting  inK_, outK_;
    TruePeak    tp_, outTp_;
    LimiterGain limiter_;
    float delay_[2][kDelaySize] = {};
    int   delayPos_ = 0;

    int    blockPos_ = 0;
    double inAcc_ = 0.0, outAcc_ = 0.0;
    float  inPeakAcc_ = 0.0f, outPeakAcc_ = 0.0f, grAcc_ = 1.0f;
    double inRing_[30] = {}, outRing_[30] = {};
    int    inRingPos_ = 0, inRingN_ = 0;
    float  inMom_ = kNone, inShort_ = kNone, outMom_ = kNone, outShort_ = kNone;
    float  inInteg_ = kNone, outInteg_ = kNone;
    double inAvgSum_ = 0.0, outAvgSum_ = 0.0; uint64_t avgN_ = 0;
    float  inTpMax_ = 0.0f, outTpMax_ = 0.0f, grNow_ = 0.0f, grMax_ = 0.0f;
    LoudnessHistogram inHist_, outHist_;

    std::vector<BlockRecord> bufA_, bufB_;
    std::atomic<BlockRecord*> active_{nullptr};
    std::atomic<uint32_t> count_{0};

    int  lastMode_ = kModeMeasure;
    bool lockRequested_ = false;
    bool lockValid_ = false;
    std::atomic<float> lockedDb_{0.0f}, lockTarget_{-14.0f}, lockCeiling_{-1.0f}, predicted_{kNone};
    std::atomic<float> levelerDb_{0.0f};
    float levelerDesired_ = 0.0f;

    // staged by loadExtraState (UI thread), installed by reset() (audio thread)
    std::atomic<bool> stagePending_{false};
    LoudnessHistogram stagedHist_;
    uint32_t stagedCount_ = 0;
    bool  stagedLockValid_ = false;
    float stagedLockedDb_ = 0.0f, stagedLockTarget_ = -14.0f, stagedLockCeiling_ = -1.0f;
    float stagedPredicted_ = kNone, stagedLevelerDb_ = 0.0f;

    std::atomic<float> display_[kNumReadouts];
    float histVal_[kHistCols];
    std::atomic<float> histDisp_[kHistCols];
    bool  histDirty_[kHistCols];
    double histAccE_ = 0.0;
    int   histAccN_ = 0;
    int   histPublishPos_ = 0;
    int lastShown_[kNumReadouts] = { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN,
                                     INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN };
    int publishCounter_ = 0;
};

MPC_PLUGIN_ENTRY(LUFSMaster)
