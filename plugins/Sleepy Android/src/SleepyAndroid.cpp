// Sleepy Android - envelope filter + looping stutter for guitar, for the MPC / Force. By sureeel.
//
// SIGNAL FLOW
//
//   in -> Input Gain -> [ Routing ]                                   -> Mix -> Output -> limiter -> out
//                         Filter > Stutter : stutter(filter(x))          (dry/wet of the whole plugin)
//                         Stutter > Filter : filter(stutter(x))
//                         Parallel         : x + (filter(x) - x) + (stutter(x) - x)
//   Bypass crossfades to the input, delayed by the limiter look-ahead so timing never jumps.
//
// ANDROID FILTER, modelled on the behaviour of a classic stereo envelope/LFO filter, in four
// sections like the original:
//   ENVELOPE FOLLOWER  Sensitivity (Env: 0..+54 dB drive; Gate: threshold -6..-60 dB), Attack
//                      1 ms..2 s, Release 5 ms..5 s, Env / Gate mode. Gate: fully on past the
//                      threshold, fully off below, shaped by Attack and Release. Env LED display.
//   LFO                0.1..100 Hz, Triangle / Square / Ramp Up / Ramp Down, depth in octaves,
//                      to the Filter, the Volume or Both. Stereo: the right channel's LFO is
//                      inverted, so the two cutoffs (and tremolos) move in opposite directions.
//   FILTER             Cutoff = resting position; Sweep -6..+6 octaves (centre zero, + up, - down);
//                      four-pole 24 dB/oct ladder, LP / BP / HP; Resonance from fattening through
//                      twang to soft self-oscillation.
//   OUTPUT             VCA: the same envelope drives the volume (with Gate mode, a noise gate);
//                      Filter Mix (wet/dry); Filter On.
//   cutoff = Cutoff * 2^(Sweep * envelope + Depth * lfo). Modulation runs every 16 samples.
//
// SLEEPY STUTTER (capture-and-loop, a delay with feedback)
//   When triggered it captures a slice of the input (Slice length), then loops it for the Loop
//   duration. Every pass writes back  buffer = buffer * Feedback + input * (1 - Feedback):
//   100 % freezes the slice, lower values let new playing seep in like a delay. Each loop end
//   starts a new capture while Hold is on; Trigger plays one cycle. Fixed mode uses Slice and
//   Loop; Random mode picks new lengths between the Min and Max knobs every cycle. Sync switches
//   all six length knobs between milliseconds and note values at the MPC tempo. Dry Mute
//   silences the dry signal while the loop plays. Short fades at the slice edges avoid clicks.
//
// SAFETY LIMITER
//   Sample-peak look-ahead limiter at -1 dBFS (1.5 ms look-ahead, 100 ms release) on the output,
//   so full feedback and high resonance can never get dangerously loud.
//
// PEDAL MORPH
//   Effects receive no MIDI on the MPC, so the expression pedal drives the Pedal parameter (map
//   it on the MPC; it is also Q-Link 1). Up to 8 knobs can follow it: Assign on, turn the knob
//   you want, set it to the heel value, press Set Heel, set the toe value, press Set Toe, press
//   Save. While assigned, the knob's sound follows heel + (toe - heel) * Pedal. Clear removes all
//   assignments. Assignments are saved with the project and with user presets.
//
// HELP
//   The touch page's invisible help buttons set the Help parameter; its value text (and Help 2,
//   which the plugin keeps in step) is the explanation shown in the help window.

#include "mpc_plugin.h"
#include "dsp/lfo.h"
#include "dsp/noise.h"
#include "dsp/tempo.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

// Parameter order is permanent (saved projects). Append new parameters at the end only.
enum ParamIndex
{
    kOutput = 0,    // dB. Index 0 is nudged by the host on every load: harmless here.
    kInGain, kMix, kBypass, kRouting, kPedal,
    // Android filter, in the four sections of the original: envelope follower, LFO, filter, output
    kFiltOn, kSens, kAttack, kRelease, kFiltMode,
    kLfoRate, kLfoShape, kLfoDepth, kLfoDest, kStereo,
    kCutoff, kEnvAmt, kReso, kFiltType,
    kVca, kFiltMix,
    kStutOn, kStutMode, kSync, kSlice, kLoop, kSliceMin, kSliceMax, kLoopMin, kLoopMax,
    kFeedback, kDryMute, kHold, kTrigger, kStutMix,
    kAssign, kSetHeel, kSetToe, kSaveMorph, kClearMorph,
    kInLevel, kFiltPos, kLimGr, kStutNow, kMorphStatus,     // displays
    kHelp, kHelp2,                                          // help window text
    // Patch randomizer (appended later; order permanent)
    kRandomize, kUndo, kRandAmount, kLockFilter, kLockStutter, kRandStatus,
    kEnvLevel,                                              // display: envelope activity LED
    kRandWidth,                                             // stutter: how wide Random mode strays
    kStutLit,                                               // display: stutter active LED
    // filter auto sweep + touch (input level) control
    kAutoSweep, kSweepRate, kSweepMax, kSweepRange, kSweepDir, kSweepTouch, kSweepTouchAmt,
    kEnvTouch, kEnvTouchAmt,
    // stutter re-roll speed + touch
    kRandRate, kRandRateMax, kStutTouch, kStutTouchAmt,
    kTouchLevel, kSweepNow,                                 // displays
    // preset naming (letter picker)
    kNamePos, kNameLetter, kNameConfirm, kNameCancel, kPresetName,
    kNumParams
};
constexpr int kNumAudioParams = kStutMix + 1;   // what the built-in program table sets directly
// Settings added later; presets carry them too (applied in programLoaded()).
constexpr int kExtParams[] = {kRandWidth, kAutoSweep, kSweepRate, kSweepMax, kSweepRange, kSweepDir,
                              kSweepTouch, kSweepTouchAmt, kEnvTouch, kEnvTouchAmt,
                              kRandRate, kRandRateMax, kStutTouch, kStutTouchAmt};
constexpr int kNumExt = (int)(sizeof(kExtParams) / sizeof(kExtParams[0]));
constexpr int kPresetVals = kNumAudioParams + kNumExt;   // one user preset
static_assert(kNumExt == 14, "preset extension arrays are sized 14");
constexpr int kNameLen = 16;
const char kCharset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.#&+!";
constexpr int kNumChars = (int)sizeof(kCharset) - 1;

enum Routing { kFilterFirst = 0, kStutterFirst = 1, kParallel = 2 };

constexpr int   kMaxSlots      = 8;
constexpr int   kLookahead     = 64;            // limiter look-ahead = plugin latency (1.45 ms)
constexpr int   kDelaySize     = 128;
constexpr float kCeiling       = 0.891f;        // -1 dBFS
constexpr float kMaxSliceSec   = 2.0f;
constexpr float kMaxLoopSec    = 16.0f;
constexpr float kNone          = -1000.0f;

// Length knobs: 0..1 -> milliseconds (log) or a note value from dsp/tempo.h.
constexpr float kSliceMsMin = 10.0f,  kSliceMsMax = 2000.0f;
constexpr float kLoopMsMin  = 50.0f,  kLoopMsMax  = 8000.0f;
constexpr int   kSliceDivLo = 0,  kSliceDivHi = 13;    // 1/64 .. 1/2
constexpr int   kLoopDivLo  = 4,  kLoopDivHi  = 16;    // 1/16 .. 2 bars

// Help text, one entry per help button on the touch pages. Max 31 characters per line.
const char* const kHelpText[][2] = {
    {"Touch a label or meter",         "to see what it does."},            // 0
    {"Level into the plugin in dB.",   "Guitar: try +6 to +12 dB."},       // 1
    {"Output level before the",        "safety limiter, in dB."},          // 2
    {"Dry/wet of the whole plugin,",   "in %."},                           // 3
    {"Order: filter then stutter,",    "stutter then filter, or both."},   // 4
    {"Master bypass: the input",       "passes through untouched."},       // 5
    {"Pedal position, 0-100 %.",       "Map the expression pedal here."},  // 6
    {"Assign on, turn a knob, then",   "Set Heel, Set Toe: it is kept."},  // 7
    {"Clear removes every pedal",      "assignment."},                     // 8
    {"Env: drive into the follower.",  "Gate: the on/off threshold."},     // 9
    {"Sweep: octaves the envelope",    "moves the cutoff, + up - down."},  // 10
    {"Fattens, then twangs; at the",   "top it sings, softly."},           // 11
    {"Filter shape LP / BP / HP,",     "24 dB per octave."},               // 12
    {"Envelope speed: attack up to",   "2 s, release up to 5 s."},         // 13
    {"Filter on/off and its",          "wet/dry mix, in %."},              // 14
    {"Input level in dBFS; filter",    "position as cutoff in Hz."},       // 15
    {"Trigger: tap on / tap off.",     "Hold: latch. LED = looping."},     // 16
    {"Fixed: your lengths. Random:",   "new lengths every loop."},         // 17
    {"ms: lengths in milliseconds.",   "Tempo: note values at tempo."},    // 18
    {"Slice: length captured.",        "Loop: how long it repeats."},      // 19
    {"How much of the loop feeds",     "back: 100 % freezes it."},         // 20
    {"Dry Mute silences the dry",      "signal while the loop plays."},    // 21
    {"Random mode picks slice and",    "loop lengths in these ranges."},   // 22
    {"Stutter state and lengths;",     "GR = output limiter in dB."},      // 23
    {"Stutter wet/dry mix, in %.",     "Dry Mute overrides the dry."},     // 24
    {"New random patch within safe",   "ranges. Undo goes back."},         // 25
    {"Undo restores the settings",     "from before the last Randomize."}, // 26
    {"How far Randomize moves: low",   "nudges, 100 % rerolls."},          // 27
    {"Locked sections are left",       "alone by Randomize."},             // 28
    {"Resting cutoff: where the",      "filter sits between notes, Hz."},  // 29
    {"Env: follows your level.",       "Gate: fully on past threshold."},  // 30
    {"LFO 0.1-100 Hz: rate, shape",    "and depth in octaves."},           // 31
    {"VCA: the envelope sets the",     "volume. With Gate: a gate."},      // 32
    {"LFO To: filter cutoff,",         "volume, or both."},                // 33
    {"Stereo: right LFO inverted,",    "cutoffs move opposite."},          // 34
    {"Envelope LED: set Sensitivity",  "so it peaks on loud notes."},      // 35
    {"Random width: 0 % = Slice and",  "Loop exactly, 100 % = Min-Max."}, // 36
    {"Save: stores this patch as a",   "user preset (Random/User NN)."},  // 37
    {"Stutter LED: lit while the",     "stutter records or loops."},      // 38
    {"Auto sweep: moves the cutoff",   "by Range: up, down, up-down."},   // 39
    {"Sweep speed: Rate = slowest,",   "Rate Max = fastest (touch)."},    // 40
    {"Touch: your playing level sets", "the speed. Amount = how much."},  // 41
    {"Env Touch: dynamics shape the",  "sweep. Off: every note equal."},  // 42
    {"How often random lengths",       "re-roll: Rate .. Rate Max."},     // 43
    {"Touch level: your playing",      "level, smoothed (soft..hard)."},  // 44
    {"Name: Position picks a letter,", "Letter changes it. Confirm."},    // 45
};

// Patch randomizer: which parameters, and the musical range each may land in (plain values).
// kind: 0 = linear, 1 = logarithmic, 2 = choice (index range). Section 0 = filter, 1 = stutter.
struct RandSpec { int param; float lo, hi; int kind; int section; };
const RandSpec kRandSpecs[] = {
    {kSens,     40.0f,  85.0f, 0, 0}, {kCutoff,  80.0f, 1200.0f, 1, 0}, {kEnvAmt, -4.0f, 5.0f, 0, 0},
    {kReso,     10.0f,  85.0f, 0, 0}, {kFiltType, 0.0f,   2.0f, 2, 0}, {kFiltMode, 0.0f,  1.0f, 2, 0},
    {kAttack,    2.0f, 200.0f, 1, 0}, {kRelease, 50.0f, 1500.0f, 1, 0}, {kFiltMix, 60.0f, 100.0f, 0, 0},
    {kLfoRate,   0.2f,   8.0f, 1, 0}, {kLfoDepth, 0.0f,   1.5f, 0, 0}, {kLfoShape, 0.0f,  3.0f, 2, 0},
    {kLfoDest,   0.0f,   2.0f, 2, 0}, {kStereo,   0.0f,   1.0f, 2, 0},
    {kFeedback, 20.0f,  95.0f, 0, 1}, {kStutMode, 0.0f,   1.0f, 2, 1}, {kStutMix, 40.0f, 100.0f, 0, 1},
    {kSlice,     0.25f,  0.75f, 0, 1}, {kLoop,     0.25f,  0.75f, 0, 1},
    {kSliceMin,  0.15f,  0.45f, 0, 1}, {kSliceMax, 0.45f,  0.75f, 0, 1},
    {kLoopMin,   0.20f,  0.50f, 0, 1}, {kLoopMax,  0.50f,  0.80f, 0, 1},
};
constexpr int kNumRandSpecs = (int)(sizeof(kRandSpecs) / sizeof(kRandSpecs[0]));
constexpr int kNumHelp = (int)(sizeof(kHelpText) / sizeof(kHelpText[0]));

inline float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }
inline float linToDb(float g)  { return g > 1e-9f ? 20.0f * std::log10(g) : -180.0f; }
inline float logMap(float t, float lo, float hi) { return lo * std::pow(hi / lo, mpc::clamp(t, 0.0f, 1.0f)); }

bool isMorphable(int i)
{
    switch (i)
    {
        case kOutput: case kInGain: case kMix:
        case kSens: case kCutoff: case kEnvAmt: case kReso: case kAttack: case kRelease: case kFiltMix:
        case kLfoRate: case kLfoDepth:
        case kSlice: case kLoop: case kSliceMin: case kSliceMax: case kLoopMin: case kLoopMax:
        case kFeedback: case kStutMix:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Sample-peak look-ahead limiter gain (same construction as LUFS Master's, without oversampling):
// minimum of the required gains over the look-ahead window, instant attack + exponential release,
// then a moving average over the window so the gain ramps into each peak.
// ---------------------------------------------------------------------------
struct PeakLimiter
{
    static constexpr int kQ = 128;
    float   qVal[kQ];
    int64_t qIdx[kQ];
    int64_t head = 0, tail = 0, n = 0;
    float   env = 1.0f, rel = 0.0f;
    float   ring[kLookahead];
    int     pos = 0;
    double  sum = kLookahead;

    void prepare(double sr) { rel = (float)(1.0 - std::exp(-1.0 / (0.100 * sr))); reset(); }
    void reset() { head = tail = n = 0; env = 1.0f; for (float& v : ring) v = 1.0f; pos = 0; sum = kLookahead; }

    float process(float required)
    {
        while (tail > head && qVal[(tail - 1) & (kQ - 1)] >= required) --tail;
        qVal[tail & (kQ - 1)] = required; qIdx[tail & (kQ - 1)] = n; ++tail;
        while (qIdx[head & (kQ - 1)] < n - kLookahead) ++head;
        const float held = qVal[head & (kQ - 1)];
        ++n;
        if (held < env) env = held;
        else { env += (held - env) * rel; if (held - env < 1e-6f) env = held; }
        sum += (double)env - (double)ring[pos];
        ring[pos] = env;
        if (++pos == kLookahead) { pos = 0; double s = 0.0; for (float v : ring) s += v; sum = s; }
        return (float)(sum * (1.0 / kLookahead));
    }
};

// Smooths toward a target with a fixed time constant; snaps when close (exact values at rest).
struct Glide
{
    float v = 0.0f, coef = 0.01f;
    void setTime(float ms, double sr) { coef = (float)(1.0 - std::exp(-1.0 / (ms * 0.001 * sr))); }
    float next(float target) { v += (target - v) * coef; if (std::fabs(target - v) < 1e-6f) v = target; return v; }
};

// ---------------------------------------------------------------------------
// Four-pole (24 dB/oct) ladder filter, zero-delay-feedback form (Zavalishin's TPT ladder).
// Four one-pole stages in series with global negative feedback k * y4. The feedback equation is
// solved for the current sample, so it stays stable and in tune under fast cutoff modulation.
// Modes by mixing the stage outputs (pole mixing):
//   LP 24 dB/oct = y4,  BP (12 dB/oct each side) = 4 y2 - 8 y3 + 4 y4,
//   HP 24 dB/oct = u - 4 y1 + 6 y2 - 4 y3 + y4.
// Resonance: k from 0 to 3.85. Low settings fatten, the top of the knob gives a strong, clear
// twang, and it stops short of self-oscillation (k = 4). The feedback signal also passes through a
// soft saturator, so even hard playing at full resonance cannot squeal (the output limiter is there
// as well). With no resonance the filter is
// purely linear (no aliasing). The low-pass passband loss that feedback causes is partly made up.
// ---------------------------------------------------------------------------
struct Ladder
{
    float s[4] = {0, 0, 0, 0};
    float G = 0.5f, b = 0.5f, G2 = 0.25f, G3 = 0.125f, G4 = 0.0625f, k = 0.0f, comp = 1.0f;

    void set(float cutoffHz, float reso01, float sr)
    {
        cutoffHz = mpc::clamp(cutoffHz, 20.0f, 0.45f * sr);
        const float g = std::tan(3.14159265f * cutoffHz / sr);
        G = g / (1.0f + g); b = 1.0f - G;
        G2 = G * G; G3 = G2 * G; G4 = G3 * G;
        k = 3.85f * mpc::clamp(reso01, 0.0f, 1.0f);
        comp = 1.0f + 0.4f * k;
    }
    void reset() { s[0] = s[1] = s[2] = s[3] = 0.0f; }

    static float soft(float x) { return x / (1.0f + std::fabs(x) * 0.5f); }   // gentle, cheap saturator

    float process(float x, int mode)
    {
        const float S = G3 * b * s[0] + G2 * b * s[1] + G * b * s[2] + b * s[3];
        const float y4est = (G4 * x + S) / (1.0f + k * G4);
        const float u = x - soft(k * y4est);     // saturate only the resonance feedback: linear at low resonance
        float in = u, y[4];
        for (int i = 0; i < 4; ++i)
        {
            const float v = (in - s[i]) * G;
            y[i] = v + s[i];
            s[i] = y[i] + v;
            in = y[i];
        }
        switch (mode)
        {
            case 1:  return 4.0f * y[1] - 8.0f * y[2] + 4.0f * y[3];
            case 2:  return u - 4.0f * y[0] + 6.0f * y[1] - 4.0f * y[2] + y[3];
            default: return y[3] * comp;
        }
    }
};

struct Slot { int param; float heel, toe; };

} // namespace

class SleepyAndroid : public mpc::Plugin
{
public:
    SleepyAndroid() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("output",   "Output",      -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("in_gain",  "Input Gain",  -12.0f, 24.0f, 6.0f, "dB", 1));
        addParam(mpc::floatParam("mix",      "Mix",           0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::boolParam ("bypass",   "Bypass", false));
        addParam(mpc::choiceParam("routing", "Routing", {"Filt>Stut", "Stut>Filt", "Parallel"}, 0));
        addParam(mpc::floatParam("pedal",    "Pedal",         0.0f, 100.0f, 0.0f, "%", 0));

        addParam(mpc::boolParam ("filt_on",  "Filter On", true));
        // envelope follower
        addParam(mpc::floatParam("sens",     "Sensitivity",   0.0f, 100.0f, 15.0f, "%", 0));
        addParam(mpc::logParam  ("attack",   "Attack",        1.0f, 2000.0f, 5.0f, "ms", 0));
        addParam(mpc::logParam  ("release",  "Release",       5.0f, 5000.0f, 300.0f, "ms", 0));
        addParam(mpc::choiceParam("env_mode","Env Mode", {"Env", "Gate"}, 0));
        // LFO
        addParam(mpc::logParam  ("lfo_rate", "LFO Rate",      0.1f, 100.0f, 2.0f, "Hz", 2));
        addParam(mpc::choiceParam("lfo_shape","LFO Shape", {"Tri", "Square", "Ramp Up", "Ramp Dn"}, 0));
        addParam(mpc::floatParam("lfo_depth","LFO Depth",     0.0f, 4.0f, 0.0f, "oct", 1));
        addParam(mpc::choiceParam("lfo_dest","LFO To", {"Filter", "Volume", "Both"}, 0));
        addParam(mpc::choiceParam("stereo",  "Stereo", {"Mono", "Stereo"}, 0));
        // filter
        addParam(mpc::logParam  ("cutoff",   "Cutoff",       40.0f, 8000.0f, 180.0f, "Hz", 0));
        addParam(mpc::floatParam("sweep",    "Sweep",        -6.0f, 6.0f, 4.5f, "oct", 1));
        addParam(mpc::floatParam("reso",     "Resonance",     0.0f, 100.0f, 70.0f, "%", 0));   // tops out at a strong twang
        addParam(mpc::choiceParam("ftype",   "Filter Type", {"LP", "BP", "HP"}, 0));
        // output
        addParam(mpc::boolParam ("vca",      "VCA", false));
        addParam(mpc::floatParam("filt_mix", "Filter Mix",    0.0f, 100.0f, 100.0f, "%", 0));

        addParam(mpc::boolParam ("stut_on",  "Stutter On", true));
        addParam(mpc::choiceParam("stut_mode", "Stut Mode", {"Fixed", "Random"}, 0));
        addParam(mpc::choiceParam("sync",    "Sync", {"ms", "Tempo"}, 0));
        addParam(mpc::floatParam("slice",    "Slice",         0.0f, 1.0f, 0.45f, "", 3));
        addParam(mpc::floatParam("loop",     "Loop",          0.0f, 1.0f, 0.40f, "", 3));
        addParam(mpc::floatParam("slice_min","Slice Min",     0.0f, 1.0f, 0.20f, "", 3));
        addParam(mpc::floatParam("slice_max","Slice Max",     0.0f, 1.0f, 0.60f, "", 3));
        addParam(mpc::floatParam("loop_min", "Loop Min",      0.0f, 1.0f, 0.25f, "", 3));
        addParam(mpc::floatParam("loop_max", "Loop Max",      0.0f, 1.0f, 0.55f, "", 3));
        addParam(mpc::floatParam("feedback", "Feedback",      0.0f, 100.0f, 70.0f, "%", 0));
        addParam(mpc::boolParam ("dry_mute", "Dry Mute", false));
        addParam(mpc::boolParam ("hold",     "Hold", false));
        addParam(mpc::boolParam ("trigger",  "Trigger", false));
        addParam(mpc::floatParam("stut_mix", "Stutter Mix",   0.0f, 100.0f, 100.0f, "%", 0));

        addParam(mpc::boolParam ("assign",   "Assign", false));
        addParam(mpc::boolParam ("set_heel", "Set Heel", false));
        addParam(mpc::boolParam ("set_toe",  "Set Toe", false));
        addParam(mpc::boolParam ("save_morph", "Save", false));
        addParam(mpc::boolParam ("clear_morph", "Clear", false));

        const char* const dispIds[]   = {"in_level", "filt_pos", "lim_gr", "stut_now", "morph_status"};
        const char* const dispNames[] = {"Input Level", "Filter Pos", "Limiter GR", "Stutter Now", "Pedal Status"};
        for (int i = 0; i < 5; ++i)
        {
            mpc::Param p = mpc::floatParam(dispIds[i], dispNames[i], 0.0f, 1.0f, 0.0f, "", 2);
            p.automatable = false;
            addParam(p);
        }
        std::vector<std::string> h1, h2;
        for (const auto& h : kHelpText) { h1.push_back(h[0]); h2.push_back(h[1]); }
        mpc::Param p1 = mpc::choiceParam("help", "Help", h1, 0), p2 = mpc::choiceParam("help2", "Help 2", h2, 0);
        p1.automatable = p2.automatable = false;
        addParam(p1);
        addParam(p2);

        addParam(mpc::boolParam ("randomize",   "Randomize", false));      // momentary, Q-Link / automation
        addParam(mpc::boolParam ("undo",        "Undo", false));           // momentary
        addParam(mpc::floatParam("rand_amount", "Rand Amount", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::boolParam ("lock_filter", "Lock Filter", false));
        addParam(mpc::boolParam ("lock_stutter","Lock Stutter", false));
        mpc::Param rs = mpc::floatParam("rand_status", "Rand Status", 0.0f, 1.0f, 0.0f, "", 2);
        rs.automatable = false;
        addParam(rs);
        rng2_.reseed((uint32_t)(reinterpret_cast<uintptr_t>(this) ^ 0x5EEDu) * 2654435761u);

        // Built-in programs (all audio parameters, in order; assignments are not part of them).
        // filter (16): on | sens attack release mode | lfoRate shape depth dest stereo | cutoff sweep reso type | vca mix
        //   out in mix byp rout ped | filter | son mode sync slice loop smin smax lmin lmax fb dmut hold trig smix
        const std::vector<float> stIdle  = {0, 0, 0, 0.45f, 0.40f, 0.20f, 0.60f, 0.25f, 0.55f, 70, 0, 0, 0, 100};   // stutter off
        const std::vector<float> stReady = {1, 0, 0, 0.45f, 0.40f, 0.20f, 0.60f, 0.25f, 0.55f, 70, 0, 0, 0, 100};   // on, waiting for Hold / Trigger
        using Ext = std::vector<std::pair<int, float>>;       // later settings that differ from their defaults
        auto prog = [this](const char* name, std::vector<float> filter, const std::vector<float>& stut, float routing = 0,
                           const Ext& ext = {}) {
            std::vector<float> v = {0, 6, 100, 0, routing, 0};
            v.insert(v.end(), filter.begin(), filter.end());
            v.insert(v.end(), stut.begin(), stut.end());
            addProgram(name, v);
            progExtOv_.push_back(ext);
        };
        prog("Init",            {1,  15,   5,  300, 0,   2.0f, 0, 0.0f, 0, 0,   180,  4.5f, 70, 0,  0, 100}, stReady);
        prog("Quack Funk",      {1,  30,   2,  180, 0,   2.0f, 0, 0.0f, 0, 0,   250,  4.0f, 80, 1,  0, 100}, stIdle);
        prog("Down Sweep",      {1,  25,   5,  600, 0,   2.0f, 0, 0.0f, 0, 0,  2500, -4.0f, 60, 0,  0, 100}, stIdle);
        prog("Gate Wah",        {1,  55,   5,  300, 1,   2.0f, 0, 0.0f, 0, 0,   220,  3.5f, 72, 1,  0, 100}, stIdle);
        prog("Sleepy Glitch",   {0,  25,   5,  400, 0,   2.0f, 0, 0.0f, 0, 0,   180,  4.5f, 70, 0,  0, 100},
             {1, 1, 0, 0.45f, 0.40f, 0.15f, 0.55f, 0.20f, 0.50f, 60, 1, 1, 0, 100});
        prog("Android Stutter", {1,  55,   5,  250, 1,   2.0f, 0, 0.0f, 0, 0,   220,  4.0f, 70, 1,  0, 100},
             {1, 1, 1, 0.45f, 0.40f, 0.20f, 0.55f, 0.30f, 0.70f, 50, 0, 1, 0, 100});
        prog("Frozen Pad",      {1,  20,  50, 1500, 0,   0.3f, 0, 1.5f, 0, 1,   600,  2.0f, 50, 0,  0,  60},
             {1, 0, 1, 0.70f, 1.00f, 0.20f, 0.60f, 0.25f, 0.55f, 100, 1, 1, 0, 80}, 2);
        prog("Volume Swell",    {1,  60, 600, 1200, 0,   2.0f, 0, 0.0f, 0, 0,  8000,  0.0f,  0, 0,  1, 100}, stIdle);
        prog("Stereo Wobble",   {1,  20,  15,  400, 0,   3.0f, 0, 1.5f, 0, 1,   500,  2.0f, 65, 0,  0, 100}, stIdle);
        prog("Tremolo Gate",    {1,  55,   5,  150, 1,   6.0f, 1, 4.0f, 1, 0,  8000,  0.0f,  0, 0,  1, 100}, stIdle);
        // Inspired by Radiohead and Adrian Belew: our own settings in the spirit of those players'
        // sounds, not copies of their rigs or tones (see the README).
        //  RH Paranoid Wah: Greenwood reportedly ran his Paranoid Android solos through a Mutronics
        //  Mutator after recording them. Here: the Mutator's classic 24 dB low-pass, high resonance,
        //  1 ms attack, short release, opening wide on every pick (no published settings exist).
        prog("RH Paranoid Wah",  {1,  25,   1,  130, 0,   2.0f, 0, 0.0f, 0, 0,   150,  5.8f, 90, 0,  0, 100}, stIdle);
        //  RH Go To Sleep: Greenwood's randomised-stutter Max/MSP patch on the Go To Sleep outro - no
        //  wah: random, tempo-synced short chops (1/32 .. 1/8) that re-roll quickly and replace the dry
        //  guitar while they loop; low feedback keeps each chop clear.
        prog("RH Go To Sleep",   {0,  25,   5,  300, 0,   2.0f, 0, 0.0f, 0, 0,   180,  4.5f, 70, 0,  0, 100},
             {1, 1, 1, 0.45f, 0.40f, 0.20f, 0.60f, 0.00f, 0.25f, 30, 1, 1, 0, 100}, 0,
             {{kRandWidth, 100}, {kRandRate, 4.0f}, {kRandRateMax, 10.0f}, {kStutTouch, 1}, {kStutTouchAmt, 60}});
        //  RH Paranoid Sleep: the two together - the random stutter into the Paranoid wah (Stut>Filt),
        //  so every chop is re-filtered; harder playing re-rolls faster
        prog("RH Paranoid Sleep", {1, 25,   1,  130, 0,   2.0f, 0, 0.0f, 0, 0,   150,  5.8f, 90, 0,  0, 100},
             {1, 1, 1, 0.45f, 0.40f, 0.20f, 0.60f, 0.00f, 0.25f, 30, 1, 1, 0, 100}, 1,
             {{kRandWidth, 100}, {kRandRate, 4.0f}, {kRandRateMax, 10.0f}, {kStutTouch, 1}, {kStutTouchAmt, 60}});
        //  RH Kill Switch: the Paranoid wah plus a square-wave volume chop, after the kill-switch
        //  stutter in Greenwood's Paranoid Android solo
        prog("RH Kill Switch",   {1,  25,   1,  130, 0,  12.0f, 1, 4.0f, 1, 0,   150,  5.8f, 90, 0,  0, 100}, stIdle);        //  AB Wild Envelope: huge upward sweep, near-maximum resonance, instant attack
        prog("AB Wild Envelope", {1,  35,   1,  220, 0,   2.0f, 0, 0.0f, 0, 0,    90,  6.0f, 95, 0,  0, 100}, stIdle);
        //  AB Animal Noises: band-pass squawks from a high resting point sweeping down, plus a fast
        //  LFO wobble on top of the envelope (try bends and slides)
        prog("AB Animal Noises", {1,  35,   1,  450, 0,   7.0f, 0, 0.6f, 0, 0,  3500, -5.5f, 100, 1, 0, 100}, stIdle);
        //  AB Stutter Squawk: short fixed stutter into the squawking filter; hold Trigger to loop
        prog("AB Stutter Squawk", {1, 35,   1,  200, 0,   2.0f, 0, 0.0f, 0, 0,   150,  5.5f, 95, 1,  0, 100},
             {1, 0, 0, 0.30f, 0.30f, 0.20f, 0.60f, 0.25f, 0.55f, 45, 0, 0, 0, 100}, 1);
        // Extreme: settings pushed to the edges (the -1 dBFS limiter still holds every one).
        //  XT Laser Zap: high-resonance band-pass diving 6 octaves down from 8 kHz on every pick
        prog("XT Laser Zap",     {1,  45,   1,   90, 0,   2.0f, 0, 0.0f, 0, 0,  8000, -6.0f, 100, 1, 0, 100}, stIdle);
        //  XT Ring Tremor: 100 Hz square LFO on cutoff and volume - near audio-rate, ring-mod-like
        prog("XT Ring Tremor",   {1,  20,   5,  300, 0, 100.0f, 1, 3.0f, 2, 1,   600,  2.0f, 85, 0,  0, 100}, stIdle);
        //  XT Gate Stab: Gate + VCA, instant on/off, full-depth downward sweep: only hard hits speak
        prog("XT Gate Stab",     {1,  70,   1,    5, 1,   2.0f, 0, 0.0f, 0, 0,  8000, -5.0f, 100, 0, 1, 100}, stIdle);
        //  XT Chaos Glitch: random stutter re-rolling 10 times a second over the widest ranges, near-frozen
        //  feedback, dry muted, into a 6-octave touch-speed auto sweep at full resonance
        prog("XT Chaos Glitch",  {1,  30,   1,  100, 0,   2.0f, 0, 0.0f, 0, 1,   120,  2.0f, 96, 1,  0, 100},
             {1, 1, 0, 0.30f, 0.30f, 0.00f, 1.00f, 0.00f, 1.00f, 95, 1, 1, 0, 100}, 1,
             {{kRandWidth, 100}, {kRandRate, 10.0f}, {kRandRateMax, 10.0f}, {kAutoSweep, 1}, {kSweepRange, 6.0f},
              {kSweepDir, 2}, {kSweepRate, 2.0f}, {kSweepMax, 10.0f}, {kSweepTouchAmt, 100}});
        //  XT Frozen Swarm: a 2 s slice frozen at 100 % feedback, wobbled by a 40 Hz stereo LFO
        prog("XT Frozen Swarm",  {1,  20,  50, 2000, 0,  40.0f, 0, 2.0f, 2, 1,   700,  1.0f, 90, 1,  0, 100},
             {1, 0, 0, 1.00f, 1.00f, 0.20f, 0.60f, 0.25f, 0.55f, 100, 1, 1, 0, 100}, 2);

        for (int i = 0; i < kMaxSlots; ++i) { mParam_[i].store(-1); mHeel_[i].store(0.0f); mToe_[i].store(0.0f); }
        static_assert(kNumAudioParams == 36, "programs list 36 values");
        static_assert(kNumHelp <= 48, "help items");
        static_assert(kNumRandSpecs == 23, "randomizer specs");
        for (const mpc::Program& pr : programs_) if (pr.plainValues.size() != (size_t)kNumAudioParams) std::abort();   // table sanity

        mpc::Param el = mpc::floatParam("env_level", "Env Level", 0.0f, 1.0f, 0.0f, "", 2);   // envelope LED
        el.automatable = false;
        addParam(el);
        addParam(mpc::floatParam("rand_width", "Rand Width", 0.0f, 100.0f, 40.0f, "%", 0));  // stutter random spread
        mpc::Param sl = mpc::boolParam("stut_lit", "Stutter LED", false);                     // display
        sl.automatable = false;
        addParam(sl);

        addParam(mpc::boolParam ("auto_sweep",   "Auto Sweep", false));
        addParam(mpc::logParam  ("sweep_rate",   "Sweep Rate",      0.05f, 10.0f, 0.5f, "Hz", 2));   // slowest
        addParam(mpc::logParam  ("sweep_max",    "Sweep Rate Max",  0.05f, 10.0f, 4.0f, "Hz", 2));   // fastest
        addParam(mpc::floatParam("sweep_range",  "Sweep Range",     0.0f, 6.0f, 3.0f, "oct", 1));
        addParam(mpc::choiceParam("sweep_dir",   "Sweep Dir", {"Up", "Down", "Up-Down"}, 2));
        addParam(mpc::boolParam ("sweep_touch",  "Sweep Touch", true));
        addParam(mpc::floatParam("sweep_tamt",   "Sweep Touch Amt", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(mpc::boolParam ("env_touch",    "Env Touch", true));
        addParam(mpc::floatParam("env_tamt",     "Env Touch Amt",   0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::logParam  ("rand_rate",    "Rand Rate",       0.1f, 10.0f, 1.0f, "Hz", 2));    // slowest
        addParam(mpc::logParam  ("rand_rate_max","Rand Rate Max",   0.1f, 10.0f, 6.0f, "Hz", 2));    // fastest
        addParam(mpc::boolParam ("stut_touch",   "Stut Touch", false));
        addParam(mpc::floatParam("stut_tamt",    "Stut Touch Amt",  0.0f, 100.0f, 60.0f, "%", 0));
        for (const auto& d : {std::pair<const char*, const char*>{"touch_level", "Touch Level"}, {"sweep_now", "Sweep Now"}})
        {
            mpc::Param p = mpc::floatParam(d.first, d.second, 0.0f, 1.0f, 0.0f, "", 2);
            p.automatable = false;
            addParam(p);
        }
        std::vector<std::string> pos, letters;
        for (int i = 1; i <= kNameLen; ++i) pos.push_back(std::to_string(i));
        for (int i = 0; i < kNumChars; ++i) letters.push_back(kCharset[i] == ' ' ? std::string("space") : std::string(1, kCharset[i]));
        addParam(mpc::choiceParam("name_pos",    "Name Pos", pos, 0));
        addParam(mpc::choiceParam("name_letter", "Letter", letters, 0));
        addParam(mpc::boolParam ("name_confirm", "Confirm", false));
        addParam(mpc::boolParam ("name_cancel",  "Cancel", false));
        mpc::Param pn = mpc::floatParam("preset_name", "Preset Name", 0.0f, 1.0f, 0.0f, "", 2);
        pn.automatable = false;
        addParam(pn);
        static_assert(kPresetName + 1 == kNumParams, "parameter list");

        // the later settings of each factory program: defaults + that program's overrides
        for (const Ext& ov : progExtOv_)
        {
            std::array<float, kNumExt> e{};
            for (int k = 0; k < kNumExt; ++k) e[(size_t)k] = param(kExtParams[k]).def;
            for (const auto& o : ov) for (int k = 0; k < kNumExt; ++k) if (kExtParams[k] == o.first) e[(size_t)k] = o.second;
            progExt_.push_back(e);
        }

        // User presets: kNumUser slots after the factory programs, filled by Save and kept in a file
        // beside the plugin folder (so reinstalling the plugin does not erase them).
        numFactory_ = (int)programs_.size();
        const std::vector<float> init = programs_[0].plainValues;
        for (int u = 0; u < kNumUser; ++u) userExt_[u] = progExt_[0];
        for (int u = 0; u < kNumUser; ++u)
        {
            char nm[16]; std::snprintf(nm, sizeof(nm), "Empty %02d", u + 1);
            addProgram(nm, init);
            programs_.back().name.reserve(24);
        }
        const std::string& pf = mpc::pluginFolder();
        if (pf.find(" - VST - ") != std::string::npos) userFile_ = pf.substr(0, pf.find_last_of('/')) + "/.sureeel Sleepy Android user presets.bin";
        else userFile_ = (pf.empty() ? std::string(".") : pf) + "/user-presets.bin";      // development builds
        loadUserPresets();
        worker_ = std::thread([this] { writerLoop(); });
    }

    ~SleepyAndroid() override
    {
        quit_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = sampleRate;
        fastDecay_ = (float)std::exp(-1.0 / (0.030 * sampleRate));     // gate detector: 30 ms decay
        tchUp_     = (float)(1.0 - std::exp(-16.0 / (0.010 * sampleRate)));   // touch: per 16-sample tick
        tchDown_   = (float)(1.0 - std::exp(-16.0 / (0.200 * sampleRate)));
        tchSmooth_ = (float)(1.0 - std::exp(-16.0 / (0.150 * sampleRate)));
        for (Glide& g : gVol_) g.setTime(1.0f, sampleRate);
        const size_t n = (size_t)(kMaxSliceSec * sampleRate) + 8;
        if (buf_[0].size() != n) { buf_[0].assign(n, 0.0f); buf_[1].assign(n, 0.0f); }
        limiter_.prepare(sampleRate);
        for (Glide* g : {&gIn_, &gOut_, &gMix_, &gFiltMix_, &gStutMix_, &gFb_, &gBypass_, &gPlay_, &gDryMute_})
            g->setTime(20.0f, sampleRate);
        gPlay_.setTime(5.0f, sampleRate);
        blockLen_ = (int)(sampleRate * 0.1);
        reset();
    }

    // Silent on purpose: the VCA / gate closes between notes, or the stutter plays with the dry muted
    // (a frozen loop of silence is still the player's choice): the watchdog leaves these alone.
    bool silenceIsExpected() const override
    {
        return getBool(kVca) || (getBool(kDryMute) && (getBool(kHold) || getBool(kTrigger))) || get(kMix) < 0.5f;
    }

    // A program was applied (UI thread): set the later settings it carries.
    void programLoaded(int index) override
    {
        const std::array<float, kNumExt>* e = nullptr;
        if (index >= 0 && index < numFactory_ && index < (int)progExt_.size()) e = &progExt_[(size_t)index];
        else if (index >= numFactory_ && index < numFactory_ + kNumUser) e = &userExt_[index - numFactory_];
        if (e) for (int k = 0; k < kNumExt; ++k) set(kExtParams[k], (*e)[(size_t)k]);
    }

    // Resume / state load / program change: clear DSP memory, never the pedal assignments.
    void reset() override
    {
        for (auto& f : ladder_) f.reset();
        lfoPhase_ = 0.0f;
        envFast_ = 0.0f; posGate_ = 0.0f; gateOpen_ = false;
        touchPk_ = touchEnv_ = touch01_ = 0.0f; sweepPhase_ = 0.0f; rerollAcc_ = 0.0f; rerollDue_ = true;
        for (int c = 0; c < 2; ++c) { volT_[c] = 1.0f; volGain_[c] = 1.0f; gVol_[c].v = 1.0f; }
        for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0f);
        limiter_.reset();
        std::memset(delay_, 0, sizeof(delay_));
        delayPos_ = 0;
        env_ = 0.0f; ctrl_ = 0; pos01_ = 0.0f;
        stState_ = kIdle; stOneShot_ = false;
        if (morphPending_.exchange(false, std::memory_order_acq_rel))
        {   // install a loaded project's / preset's assignments
            nSlots_ = 0;
            for (int i = 0; i < stagedCount_ && i < kMaxSlots; ++i) slots_[nSlots_++] = staged_[i];
            publishSlots();
        }
        aState_ = kAOff;
        gIn_.v = dbToLin(eff(kInGain)); gOut_.v = dbToLin(eff(kOutput));
        gMix_.v = eff(kMix) * 0.01f; gFiltMix_.v = eff(kFiltMix) * 0.01f; gStutMix_.v = eff(kStutMix) * 0.01f;
        gFb_.v = eff(kFeedback) * 0.01f; gBypass_.v = getBool(kBypass) ? 1.0f : 0.0f; gPlay_.v = 0.0f;
        gDryMute_.v = getBool(kDryMute) ? 1.0f : 0.0f;
        // Action buttons act on every change of their switch (see acted()); a load or resume takes
        // their current position as the baseline, so loading never "presses" one.
        for (int b : {kSetHeel, kSetToe, kSaveMorph, kClearMorph, kRandomize, kUndo}) btnLast_[b] = getBool(b);
        lastActive_ = false; lastTrig_ = false;
        lastSync_ = getInt(kSync);
        lastFiltMode_ = getInt(kFiltMode);
        lastHold_ = false;          // a preset or project with Hold on starts looping right away
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int numFrames = ctx.numFrames;
        bpm_ = ctx.transport.tempoValid ? ctx.transport.bpm : 120.0;

        handleButtons();

        // ----- per-block settings (pedal morph applied through eff()) -------------------------
        const float inGainT   = dbToLin(eff(kInGain));
        const float outGainT  = dbToLin(eff(kOutput));
        const float mixT      = eff(kMix) * 0.01f;
        const bool  bypass    = getBool(kBypass);
        const int   routing   = getInt(kRouting);
        const bool  filtOn    = getBool(kFiltOn);
        // envelope follower section
        const float sens01    = mpc::clamp(eff(kSens) * 0.01f, 0.0f, 1.0f);
        const float driveDb   = sens01 * 48.0f;                                       // Env: 0..+48 dB of drive
        const float threshold = std::pow(10.0f, (-6.0f - 54.0f * sens01) / 20.0f);   // Gate: -6..-60 dB
        const bool  gateMode  = getInt(kFiltMode) == 1;
        const float attCoef   = 1.0f - std::exp(-1.0f / (mpc::clamp(eff(kAttack), 1.0f, 2000.0f) * 0.001f * (float)sr_));
        const float relCoef   = 1.0f - std::exp(-1.0f / (mpc::clamp(eff(kRelease), 5.0f, 5000.0f) * 0.001f * (float)sr_));
        // LFO section (advanced once per 16 samples)
        const float lfoInc    = mpc::clamp(eff(kLfoRate), 0.1f, 100.0f) * 16.0f / (float)sr_;
        const int   lfoShape  = getInt(kLfoShape);
        const float lfoDepth  = mpc::clamp(eff(kLfoDepth), 0.0f, 4.0f);
        const int   lfoDest   = getInt(kLfoDest);
        const bool  lfoToFilt = lfoDest != 1, lfoToVol = lfoDest != 0;
        const bool  stereo    = getInt(kStereo) == 1;
        // filter section
        const float baseHz    = mpc::clamp(eff(kCutoff), 40.0f, 8000.0f);
        const float sweepOct  = mpc::clamp(eff(kEnvAmt), -6.0f, 6.0f);
        const float reso      = mpc::clamp(eff(kReso) * 0.01f, 0.0f, 1.0f);
        const int   ftype     = getInt(kFiltType);
        // output section
        const bool  vca       = getBool(kVca);
        const float filtMixT  = eff(kFiltMix) * 0.01f;
        const bool  stutOn    = getBool(kStutOn);
        const float fbT       = mpc::clamp(eff(kFeedback) * 0.01f, 0.0f, 1.0f);
        const float stutMixT  = eff(kStutMix) * 0.01f;
        const float dryMuteT  = getBool(kDryMute) ? 1.0f : 0.0f;
        // auto sweep + touch
        const bool  autoSweep = getBool(kAutoSweep);
        const float swMin     = mpc::clamp(eff(kSweepRate), 0.05f, 10.0f);
        const float swMax     = std::fmax(swMin, mpc::clamp(eff(kSweepMax), 0.05f, 10.0f));
        const float swRange   = mpc::clamp(eff(kSweepRange), 0.0f, 6.0f);
        const int   swDir     = getInt(kSweepDir);
        const float swTouch   = getBool(kSweepTouch) ? mpc::clamp(eff(kSweepTouchAmt) * 0.01f, 0.0f, 1.0f) : 0.0f;
        const float swLogSpan = std::log2(swMax / swMin);
        // Env Touch: how much playing dynamics shape the envelope sweep (exponent 2 = full, 0 = none)
        const float envExp    = getBool(kEnvTouch) ? 2.0f * mpc::clamp(eff(kEnvTouchAmt) * 0.01f, 0.0f, 1.0f) : 0.0f;
        // stutter re-roll speed (random mode): from Rand Rate (soft / touch off) to Rand Rate Max (hard)
        randMode_ = getInt(kStutMode) == 1;
        {
            const float rMin = mpc::clamp(eff(kRandRate), 0.1f, 10.0f);
            const float rMax = std::fmax(rMin, mpc::clamp(eff(kRandRateMax), 0.1f, 10.0f));
            const float st = getBool(kStutTouch) ? mpc::clamp(eff(kStutTouchAmt) * 0.01f, 0.0f, 1.0f) : 0.0f;
            rerollRate_ = rMin * std::exp2(std::log2(rMax / rMin) * touch01_ * st);
            rerollAcc_ += rerollRate_ * (float)numFrames / (float)sr_;
            if (rerollAcc_ >= 1.0f) { rerollDue_ = true; rerollAcc_ -= std::floor(rerollAcc_); }
        }
        // Hold starts the loop engine; releasing it lets the current cycle finish.
        // Trigger: the stutter loops while Trigger is on and stops when it goes off. On the touch
        // screen that is tap on / tap off; on a Q-Link (past halfway) or a footswitch it is a
        // momentary hold. It runs the stutter even when Stutter On is off. Hold is the latch for
        // Q-Link / footswitch users (needs Stutter On). The Stutter LED shows when it is active.
        const bool trig  = getBool(kTrigger);
        const bool latch = getBool(kHold) && stutOn;
        const bool engine = stutOn || trig;
        activeNow_ = trig || latch;
        if (activeNow_ && !lastActive_ && stState_ == kIdle) startCycle(false);
        if (lastTrig_ && !trig && !latch) stState_ = kIdle;          // released: stop now
        if (!engine && stState_ != kIdle) stState_ = kIdle;
        lastActive_ = activeNow_;
        lastTrig_ = trig;

        const bool haveIn = ctx.inputs != nullptr && ctx.numInputs > 0 && ctx.inputs[0] != nullptr;
        const float* inL = haveIn ? ctx.inputs[0] : nullptr;
        const float* inR = haveIn ? ((ctx.numInputs > 1 && ctx.inputs[1]) ? ctx.inputs[1] : ctx.inputs[0]) : nullptr;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        for (int i = 0; i < numFrames; ++i)
        {
            const float rawL = haveIn ? inL[i] : 0.0f;          // read first (in place)
            const float rawR = haveIn ? inR[i] : 0.0f;

            const float gi = gIn_.next(inGainT);
            float x[2] = { rawL * gi, rawR * gi };
            inPeak_ = std::fmax(inPeak_, std::fmax(std::fabs(x[0]), std::fabs(x[1])));

            // Env mode: a peak follower with Attack / Release smoothing; Sensitivity = drive.
            // Gate mode: a fast peak detector opens the gate when it crosses the threshold (3 dB
            //   hysteresis); the envelope then goes fully on at the Attack speed and fully off at the
            //   Release speed. Both run all the time so the LED and meters work with the filter off.
            const float lvl = std::fmax(std::fabs(x[0]), std::fabs(x[1]));
            touchPk_ = std::fmax(touchPk_, lvl);
            env_ += (lvl - env_) * (lvl > env_ ? attCoef : relCoef);
            envFast_ = lvl > envFast_ ? lvl : envFast_ * fastDecay_;
            if (!gateOpen_ && envFast_ > threshold) gateOpen_ = true;
            else if (gateOpen_ && envFast_ < threshold * 0.7f) gateOpen_ = false;
            const float gateT = gateOpen_ ? 1.0f : 0.0f;
            posGate_ += (gateT - posGate_) * (gateT > posGate_ ? attCoef : relCoef);

            if ((ctrl_++ & 15) == 0)
            {   // control rate: two exp2 and two tan per 16 samples
                // Env: the driven level in dB, mapped over a 30 dB window: a soft note sits low in the
                // window and (squared) barely moves the filter, a hard pick attack reaches the top and sweeps it
                // all the way. One log10 per 16 samples.
                const float lvlDb = 20.0f * std::log10(env_ + 1e-9f) + driveDb;
                const float x = mpc::clamp((lvlDb + 30.0f) / 30.0f, 0.0f, 1.0f);
                // Env Touch at 100 %: x^2 (progressive, the low end barely moves). Lower amounts flatten
                // the curve toward "every note sweeps the same"; a soft ramp near the floor keeps it smooth.
                float envPos = envExp >= 1.999f ? x * x : (x > 0.0f ? std::pow(x, envExp) * std::fmin(1.0f, x * 8.0f) : 0.0f);
                pos01_ = gateMode ? posGate_ : envPos;

                // Touch level: short-window peak (10 ms up, 200 ms down), in dB, -50..-6 dBFS -> 0..1,
                // then smoothed over 150 ms so rates glide with the playing instead of jumping. Shared by the
                // sweep and the stutter.
                touchEnv_ += (touchPk_ - touchEnv_) * (touchPk_ > touchEnv_ ? tchUp_ : tchDown_);
                touchPk_ = 0.0f;
                const float tdb = 20.0f * std::log10(touchEnv_ + 1e-9f);
                touch01_ += (mpc::clamp((tdb + 50.0f) / 44.0f, 0.0f, 1.0f) - touch01_) * tchSmooth_;

                // Auto sweep: its own phase; speed from Sweep Rate (soft / touch off) to Sweep Rate Max
                float autoOct = 0.0f;
                sweepHz_ = swMin * std::exp2(swLogSpan * touch01_ * swTouch);
                if (autoSweep)
                {
                    sweepPhase_ += sweepHz_ * 16.0f / (float)sr_;
                    sweepPhase_ -= std::floor(sweepPhase_);
                    const float shape = swDir == 0 ? sweepPhase_ : (swDir == 1 ? 1.0f - sweepPhase_
                                                    : (sweepPhase_ < 0.5f ? 2.0f * sweepPhase_ : 2.0f - 2.0f * sweepPhase_));
                    autoOct = swRange * shape;
                }
                envPeak_ = std::fmax(envPeak_, pos01_);
                lfoPhase_ += lfoInc; lfoPhase_ -= std::floor(lfoPhase_);
                float lfo;
                switch (lfoShape)
                {
                    case 1:  lfo = lfoPhase_ < 0.5f ? 1.0f : -1.0f; break;         // square
                    case 2:  lfo = 2.0f * lfoPhase_ - 1.0f; break;                  // ramp up
                    case 3:  lfo = 1.0f - 2.0f * lfoPhase_; break;                  // ramp down
                    default: lfo = 1.0f - 4.0f * std::fabs(lfoPhase_ - 0.5f); break; // triangle
                }
                const float lfoL = lfo, lfoR = stereo ? -lfo : lfo;              // Stereo: right LFO inverted
                const float fL = lfoToFilt ? lfoDepth * lfoL : 0.0f, fR = lfoToFilt ? lfoDepth * lfoR : 0.0f;
                cutoff_ = mpc::clamp(baseHz * std::exp2(sweepOct * pos01_ + fL + autoOct), 20.0f, 18000.0f);
                ladder_[0].set(cutoff_, reso, (float)sr_);
                ladder_[1].set(mpc::clamp(baseHz * std::exp2(sweepOct * pos01_ + fR + autoOct), 20.0f, 18000.0f), reso, (float)sr_);
                // output: VCA (same envelope) and LFO tremolo
                const float env01 = vca ? pos01_ : 1.0f;
                const float tDepth = lfoToVol ? lfoDepth * 0.25f : 0.0f;          // depth 4 oct = full tremolo
                volT_[0] = env01 * (1.0f - tDepth * (0.5f - 0.5f * lfoL));
                volT_[1] = env01 * (1.0f - tDepth * (0.5f - 0.5f * lfoR));
            }
            volGain_[0] = gVol_[0].next(filtOn ? volT_[0] : 1.0f);
            volGain_[1] = gVol_[1].next(filtOn ? volT_[1] : 1.0f);

            const float fm = gFiltMix_.next(filtOn ? filtMixT : 0.0f);
            const float sm = gStutMix_.next(stutMixT);
            const float fb = gFb_.next(fbT);
            const float dm = gDryMute_.next(dryMuteT);

            float y[2];
            if (routing == kFilterFirst)
            {
                float a[2]; filterFrame(x, a, ftype, fm);
                stutterFrame(a, y, engine, sm, fb, dm);
            }
            else if (routing == kStutterFirst)
            {
                float b[2]; stutterFrame(x, b, engine, sm, fb, dm);
                filterFrame(b, y, ftype, fm);
            }
            else
            {
                float a[2], b[2];
                filterFrame(x, a, ftype, fm);
                stutterFrame(x, b, engine, sm, fb, dm);
                y[0] = a[0] + b[0] - x[0];
                y[1] = a[1] + b[1] - x[1];
            }

            const float mix = gMix_.next(mixT);
            const float go = gOut_.next(outGainT);
            const float pl = (x[0] + (y[0] - x[0]) * mix) * go;
            const float pr = (x[1] + (y[1] - x[1]) * mix) * go;

            // look-ahead peak limiter
            const float peak = std::fmax(std::fabs(pl), std::fabs(pr));
            const float lg = limiter_.process(peak > kCeiling ? kCeiling / peak : 1.0f);
            grAcc_ = std::fmin(grAcc_, lg);
            delay_[0][delayPos_] = pl; delay_[1][delayPos_] = pr;
            delay_[2][delayPos_] = rawL; delay_[3][delayPos_] = rawR;
            const int rd = (delayPos_ - kLookahead) & (kDelaySize - 1);
            delayPos_ = (delayPos_ + 1) & (kDelaySize - 1);
            const float wl = delay_[0][rd] * lg, wr = delay_[1][rd] * lg;

            const float bp = gBypass_.next(bypass ? 1.0f : 0.0f);
            const float ol = delay_[2][rd] * bp + wl * (1.0f - bp);     // exact input when bypassed
            const float orr = delay_[3][rd] * bp + wr * (1.0f - bp);
            outL[i] = std::isfinite(ol) ? ol : 0.0f;
            if (outR) outR[i] = std::isfinite(orr) ? orr : 0.0f;

            if (++blockPos_ >= blockLen_) { blockPos_ = 0; publish(); }
        }
        samplesNow_ += numFrames;
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char buf[48];
        const int sync = getInt(kSync);
        switch (index)
        {
            case kSlice: case kSliceMin: case kSliceMax:
                if (sync) out = mpc::dsp::divisionName(divIndex(plain, kSliceDivLo, kSliceDivHi));
                else { std::snprintf(buf, sizeof(buf), "%.0f ms", (double)logMap(plain, kSliceMsMin, kSliceMsMax)); out = buf; }
                return true;
            case kLoop: case kLoopMin: case kLoopMax:
                if (sync) out = mpc::dsp::divisionName(divIndex(plain, kLoopDivLo, kLoopDivHi));
                else { std::snprintf(buf, sizeof(buf), "%.0f ms", (double)logMap(plain, kLoopMsMin, kLoopMsMax)); out = buf; }
                return true;
            case kInLevel:
            {
                const float v = dInLevel_.load();
                if (v <= kNone + 1.0f) out = "-inf dB"; else { std::snprintf(buf, sizeof(buf), "%.1f dB", (double)v); out = buf; }
                return true;
            }
            case kFiltPos:   std::snprintf(buf, sizeof(buf), "%.0f Hz", (double)dCutoff_.load()); out = buf; return true;
            case kLimGr:     std::snprintf(buf, sizeof(buf), "%.1f dB", (double)dGr_.load()); out = buf; return true;
            case kStutNow:   out = stutterText(); return true;
            case kMorphStatus: out = morphText(); return true;
            case kRandStatus:  out = randText(); return true;
            case kSens:
                if (getInt(kFiltMode) == 1) std::snprintf(buf, sizeof(buf), "%.0f dB thr", (double)(-6.0f - 54.0f * plain * 0.01f));
                else std::snprintf(buf, sizeof(buf), "+%.0f dB", (double)(48.0f * plain * 0.01f));   // drive
                out = buf; return true;
            case kEnvAmt:   std::snprintf(buf, sizeof(buf), "%+.1f oct", (double)plain); out = buf; return true;
            case kEnvLevel: std::snprintf(buf, sizeof(buf), "%.0f %%", (double)(100.0f * dEnv_.load())); out = buf; return true;
            case kTouchLevel: std::snprintf(buf, sizeof(buf), "%.0f %%", (double)(100.0f * dTouch_.load())); out = buf; return true;
            case kSweepNow:
                if (getBool(kAutoSweep)) std::snprintf(buf, sizeof(buf), "%.2f Hz", (double)dSweepHz_.load());
                else std::snprintf(buf, sizeof(buf), "off (%.2f Hz)", (double)dSweepHz_.load());
                out = buf; return true;
            case kPresetName: out = nameText(); return true;
            default: break;
        }
        // Every other number shows its unit (the MPC does not append the unit label itself).
        const mpc::Param& p = param(index);
        if (p.kind == mpc::Param::Kind::Float && !p.unit.empty())
        {
            std::snprintf(buf, sizeof(buf), "%.*f %s", p.decimals, (double)plain, p.unit.c_str());
            if (std::strncmp(buf, "-0 ", 3) == 0 || std::strncmp(buf, "-0.0 ", 5) == 0) out = buf + 1; else out = buf;
            return true;
        }
        return false;
    }

    // ----- state: pedal assignments ----------------------------------------------------------
    //   u8 version(1) | u8 count | count x { u8 param, f32 heel, f32 toe }
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        const int n = mCount_.load(std::memory_order_acquire);
        out.push_back(1);
        out.push_back((uint8_t)n);
        for (int i = 0; i < n && i < kMaxSlots; ++i)
        {
            out.push_back((uint8_t)mParam_[i].load());
            putF32(out, mHeel_[i].load());
            putF32(out, mToe_[i].load());
        }
    }

    void loadExtraState(const uint8_t* d, size_t size) override
    {
        if (d == nullptr || size < 2 || d[0] != 1) return;
        const int n = d[1];
        if (n > kMaxSlots || size < 2 + (size_t)n * 9) return;
        int k = 0;
        for (int i = 0; i < n; ++i)
        {
            const uint8_t* e = d + 2 + i * 9;
            Slot s; s.param = e[0];
            std::memcpy(&s.heel, e + 1, 4); std::memcpy(&s.toe, e + 5, 4);
            if (!isMorphable(s.param) || !std::isfinite(s.heel) || !std::isfinite(s.toe)) continue;
            staged_[k++] = s;
        }
        stagedCount_ = k;
        morphPending_.store(true, std::memory_order_release);
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Sleepy Android";
        info.vendor       = "sureeel";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version = 1001;
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.latencySamples = kLookahead;
        info.tailSamples  = 0;
        return info;
    }
    static constexpr char kUid[5] = "SP1o";

    static void putF32(std::vector<uint8_t>& o, float f) { uint8_t b[4]; std::memcpy(b, &f, 4); o.insert(o.end(), b, b + 4); }
    static int divIndex(float t, int lo, int hi) { return lo + (int)std::lround(mpc::clamp(t, 0.0f, 1.0f) * (float)(hi - lo)); }

    // Parameter value as heard: the pedal blend for assigned knobs (except the one being assigned).
    float eff(int i) const
    {
        for (int s = 0; s < nSlots_; ++s)
            if (slots_[s].param == i && !(aState_ >= kAHeel && aTarget_ == i))
                return slots_[s].heel + (slots_[s].toe - slots_[s].heel) * mpc::clamp(get(kPedal) * 0.01f, 0.0f, 1.0f);
        return get(i);
    }

    // ----- Android filter ------------------------------------------------------------------------
    void filterFrame(const float* in, float* out, int ftype, float mix)
    {
        for (int c = 0; c < 2; ++c)
        {
            const float yf = ladder_[c].process(in[c], ftype);
            out[c] = (in[c] + (yf - in[c]) * mix) * volGain_[c];
        }
    }

    // ----- Sleepy stutter ------------------------------------------------------------------------
    enum StState { kIdle, kCapture, kPlay };

    float lengthSamples(float t, bool loop, int sync) const
    {
        float sec;
        if (sync) sec = mpc::dsp::beatsToSeconds(mpc::dsp::divisionBeats(loop ? divIndex(t, kLoopDivLo, kLoopDivHi)
                                                                              : divIndex(t, kSliceDivLo, kSliceDivHi)), bpm_);
        else sec = 0.001f * (loop ? logMap(t, kLoopMsMin, kLoopMsMax) : logMap(t, kSliceMsMin, kSliceMsMax));
        return sec * (float)sr_;
    }

    float randomBetween(float a, float b)
    {
        if (a > b) std::swap(a, b);
        return a + (b - a) * rng_.nextFloat();
    }

    void startCycle(bool oneShot)
    {
        const int sync = getInt(kSync);
        float s, l;
        if (getInt(kStutMode) == 1)
        {   // random: positions are drawn between Min and Max; with Sync they snap to note values
            // Rand Width pulls the draw toward the Slice / Loop knobs: 0 % = exactly those lengths,
            // 100 % = anywhere between Min and Max. Keeps random mode as predictable as you want.
            // New lengths are drawn only when the re-roll clock (Rand Rate .. Rand Rate Max) is due;
            // otherwise the last random lengths repeat.
            if (rerollDue_)
            {
                const float w = mpc::clamp(eff(kRandWidth) * 0.01f, 0.0f, 1.0f);
                const float sc = eff(kSlice), lc = eff(kLoop);
                rSlice_ = sc + w * (randomBetween(eff(kSliceMin), eff(kSliceMax)) - sc);
                rLoop_  = lc + w * (randomBetween(eff(kLoopMin), eff(kLoopMax)) - lc);
                rerollDue_ = false;
            }
            s = lengthSamples(rSlice_, false, sync);
            l = lengthSamples(rLoop_, true, sync);
        }
        else
        {
            s = lengthSamples(eff(kSlice), false, sync);
            l = lengthSamples(eff(kLoop), true, sync);
        }
        sliceLen_ = (int)std::lround(mpc::clamp(s, 64.0f, (float)(buf_[0].size() - 4)));
        loopLen_ = (int)std::lround(mpc::clamp(l, (float)sliceLen_, kMaxLoopSec * (float)sr_));
        fadeLen_ = std::max(1, std::min(96, sliceLen_ / 4));
        stPos_ = 0; stCount_ = 0;
        stState_ = kCapture;
        stOneShot_ = oneShot;
    }

    void stutterFrame(const float* in, float* out, bool on, float mix, float fb, float dryMute)
    {
        float wet[2] = {0.0f, 0.0f};
        bool playing = false;
        if (on && stState_ == kCapture)
        {
            buf_[0][(size_t)stPos_] = in[0];
            buf_[1][(size_t)stPos_] = in[1];
            if (++stPos_ >= sliceLen_) { stState_ = kPlay; stPos_ = 0; stCount_ = 0; }
        }
        else if (on && stState_ == kPlay)
        {
            playing = true;
            const int p = stPos_;
            const float w = std::min(1.0f, std::min((float)(p + 1) / fadeLen_, (float)(sliceLen_ - p) / fadeLen_));
            for (int c = 0; c < 2; ++c)
            {
                float& b = buf_[c][(size_t)p];
                wet[c] = b * w;
                b = b * fb + in[c] * (1.0f - fb);            // the loop feeds back into itself
            }
            if (++stPos_ >= sliceLen_)
            {
                stPos_ = 0;
                if (randMode_ && rerollDue_ && activeNow_) { startCycle(false); stCount_ = -1; }   // re-roll at a slice boundary
            }
            if (stState_ == kPlay && ++stCount_ >= loopLen_)
            {
                if (activeNow_) startCycle(false);          // held / latched: capture the next slice
                else stState_ = kIdle;
            }
        }
        const float pl = gPlay_.next(playing ? 1.0f : 0.0f);
        // While looping the dry signal is mixed down by the stutter mix, or fully muted by Dry Mute.
        const float dryGain = 1.0f - pl * (mix + (1.0f - mix) * dryMute);
        for (int c = 0; c < 2; ++c) out[c] = in[c] * dryGain + wet[c] * mix;
    }

    // ----- buttons and pedal assignment ----------------------------------------------------------
    enum AState { kAOff, kAKnob, kAHeel, kAToe, kAReady };

    // An action button acts on every change of its switch, on or off. The MPC touch screen can only
    // make on/off switches (its spring-back option leaves the button dead on the hardware), so each
    // tap flips the switch and each flip is one action; the plugin never writes the switch itself.
    // A second change within 0.35 s of an action is ignored, so a footswitch's press + release, or a
    // Q-Link wobbling around halfway, counts once. A Q-Link turned through the middle acts once.
    bool acted(int idx)
    {
        const bool now = getBool(idx);
        if (now == btnLast_[idx]) return false;
        btnLast_[idx] = now;
        if (samplesNow_ - btnTime_[idx] < (int64_t)(0.35 * sr_)) return false;
        btnTime_[idx] = samplesNow_;
        return true;
    }

    void snapshot() { for (int i = 0; i < kNumParams; ++i) snap_[i] = getNormalised(i); }

    void handleButtons()
    {
        lastMsgDisp_.store(lastMsg_); heelDisp_.store(aHeel_); toeDisp_.store(aToe_); lastSavedDisp_.store(lastSaved_);
        if (getInt(kHelp2) != getInt(kHelp)) { set(kHelp2, (float)getInt(kHelp)); notifyHostParameterChanged(kHelp2); }

        if (getInt(kSync) != lastSync_) { lastSync_ = getInt(kSync); requestDisplayUpdate(); }   // length texts change unit
        if (getInt(kFiltMode) != lastFiltMode_) { lastFiltMode_ = getInt(kFiltMode); requestDisplayUpdate(); }   // Sensitivity text


        const bool assign = getBool(kAssign);
        if (assign && aState_ == kAOff) { aState_ = kAKnob; snapshot(); statusChanged_ = true; }
        if (!assign && aState_ != kAOff) { aState_ = kAOff; statusChanged_ = true; }
        if (aState_ == kAKnob)
        {
            for (int i = 0; i < kNumParams; ++i)
                if (isMorphable(i) && std::fabs(getNormalised(i) - snap_[i]) > 0.003f)
                { aTarget_ = i; aState_ = kAHeel; statusChanged_ = true; break; }
        }
        // Set Heel stores the heel value; Set Toe stores the toe value and keeps the assignment
        // (no separate save step). Each shows what it stored in the pedal status line.
        if (acted(kSetHeel))
        {
            if (aState_ >= kAHeel) { aHeel_ = get(aTarget_); aState_ = kAToe; lastMsg_ = kMsgHeel; }
            else lastMsg_ = kMsgNeedKnob;
            statusChanged_ = true;
        }
        if (acted(kSetToe))
        {
            if (aState_ == kAToe)
            {
                aToe_ = get(aTarget_);
                int s = 0;
                while (s < nSlots_ && slots_[s].param != aTarget_) ++s;
                if (s < kMaxSlots)
                {
                    slots_[s] = Slot{aTarget_, aHeel_, aToe_};
                    if (s == nSlots_) ++nSlots_;
                    publishSlots();
                    lastSaved_ = aTarget_;
                    lastMsg_ = kMsgKept;
                }
                else lastMsg_ = kMsgFull;
                aState_ = kAKnob; snapshot();
            }
            else lastMsg_ = aState_ == kAHeel ? kMsgNeedHeel : kMsgNeedKnob;
            statusChanged_ = true;
        }
        // Save: the first tap opens naming with an auto-name; Save again or Confirm stores it,
        // Cancel drops it. Name Pos picks the character, Letter changes it.
        if (acted(kSaveMorph)) { if (naming_) confirmName(); else startNaming(); }
        if (acted(kNameConfirm)) { if (naming_) confirmName(); else startNaming(); }
        if (acted(kNameCancel) && naming_) { naming_ = false; nameMsg_ = kNmCancelled; nameChanged_ = true; }
        if (getInt(kNamePos) != lastPos_)
        {
            lastPos_ = getInt(kNamePos);
            cursor_ = mpc::clamp(lastPos_, 0, kNameLen - 1);
            if (naming_) { lastLetter_ = charIndex(name_[cursor_]); setAndTell(kNameLetter, (float)lastLetter_); }
            nameChanged_ = true;
        }
        if (getInt(kNameLetter) != lastLetter_)
        {
            lastLetter_ = getInt(kNameLetter);
            if (naming_)
            {
                for (int i = 0; i < cursor_; ++i) if (name_[i] == 0) name_[i] = ' ';     // typing past the end pads
                name_[cursor_] = kCharset[mpc::clamp(lastLetter_, 0, kNumChars - 1)];
                nameChanged_ = true;
            }
        }
        if (acted(kClearMorph))
        {
            nSlots_ = 0; publishSlots(); lastSaved_ = -1; lastMsg_ = kMsgCleared;
            if (aState_ != kAOff) { aState_ = kAKnob; snapshot(); }
            statusChanged_ = true;
        }
        if (acted(kRandomize)) { randomizePatch(); requestDisplayUpdate(); }   // redraw so the new values show
        if (acted(kUndo))      { undoRandomize();  requestDisplayUpdate(); }
        const int done = saveDone_.exchange(-1, std::memory_order_acq_rel);    // the writer finished
        if (done >= 0) { randState_ = 4; randCount_ = done % 100; randChanged_ = true; nameMsg_ = kNmSaved; nameChanged_ = true; requestDisplayUpdate(); }

        aStateDisp_.store(aState_);
        aTargetDisp_.store(aTarget_);
        lastMsgDisp_.store(lastMsg_); heelDisp_.store(aHeel_); toeDisp_.store(aToe_); lastSavedDisp_.store(lastSaved_);
    }

    // ----- patch randomizer (separate from the stutter's own random lengths) --------------------
    bool isPedalAssigned(int p) const
    {
        for (int s = 0; s < nSlots_; ++s) if (slots_[s].param == p) return true;
        return false;
    }

    void setAndTell(int p, float plain) { set(p, plain); notifyHostParameterChanged(p); }

    // new = current + (random target in [lo, hi] - current) * amount. Choices switch to a random
    // choice in range with probability = amount. Locked sections and pedal-assigned knobs (their
    // heel / toe stay as set, so the pedal blend keeps working) are skipped. Bypass, input gain,
    // output, master mix and routing are never part of it; the output limiter is always on.
    void randomizePatch()
    {
        const float amount = mpc::clamp(get(kRandAmount) * 0.01f, 0.0f, 1.0f);
        const bool lockF = getBool(kLockFilter), lockS = getBool(kLockStutter);
        for (int i = 0; i < kNumParams; ++i) undoVal_[i] = getNormalised(i);   // raw knob positions: exact restore
        int changed = 0;
        for (const RandSpec& r : kRandSpecs)
        {
            if ((r.section == 0 && lockF) || (r.section == 1 && lockS) || isPedalAssigned(r.param)) continue;
            const float cur = get(r.param);
            float v = cur;
            if (r.kind == 2)
            {
                if (rng2_.nextFloat() < amount)
                    v = (float)mpc::clamp((int)(r.lo + rng2_.nextFloat() * (r.hi - r.lo + 1.0f)), (int)r.lo, (int)r.hi);
            }
            else if (r.kind == 1)
            {   // logarithmic: move in log space
                const float target = r.lo * std::pow(r.hi / r.lo, rng2_.nextFloat());
                v = std::exp(std::log(std::fmax(cur, 1e-3f)) + (std::log(target) - std::log(std::fmax(cur, 1e-3f))) * amount);
            }
            else
            {
                const float target = r.lo + (r.hi - r.lo) * rng2_.nextFloat();
                v = cur + (target - cur) * amount;
            }
            // apply only real moves (a log round trip alone shifts the last bit)
            if (std::fabs(v - cur) > 1e-5f * std::fabs(r.hi - r.lo) && amount > 0.0f) { setAndTell(r.param, v); ++changed; }
        }
        // keep each random range the right way round
        for (const auto& mm : {std::pair<int, int>{kSliceMin, kSliceMax}, std::pair<int, int>{kLoopMin, kLoopMax}})
            if (get(mm.first) > get(mm.second))
            {
                const float a = get(mm.first), b = get(mm.second);
                setAndTell(mm.first, b); setAndTell(mm.second, a);
            }
        haveUndo_ = true;
        randomized_ = true;
        randState_ = 1; randCount_ = changed;
        if (aState_ == kAKnob) snapshot();      // our own changes are not "a knob you turned"
        randChanged_ = true;
    }

    void undoRandomize()
    {
        if (!haveUndo_) { randState_ = 3; randChanged_ = true; return; }
        for (const RandSpec& r : kRandSpecs)
            if (getNormalised(r.param) != undoVal_[r.param])
            { setNormalised(r.param, undoVal_[r.param]); notifyHostParameterChanged(r.param); }
        haveUndo_ = false;
        randState_ = 2;
        if (aState_ == kAKnob) snapshot();
        randChanged_ = true;
    }

    std::string randText() const
    {
        char buf[48];
        switch (randStateDisp_.load())
        {
            case 1:  std::snprintf(buf, sizeof(buf), "Randomized %d settings", randCountDisp_.load()); break;
            case 2:  std::snprintf(buf, sizeof(buf), "Undone"); break;
            case 3:  std::snprintf(buf, sizeof(buf), "Nothing to undo"); break;
            case 4:
            {
                std::string s = "Saved: ";
                for (int i = 0; i < kNameLen && savedNameDisp_[i].load(); ++i) s += savedNameDisp_[i].load();
                std::snprintf(buf, sizeof(buf), "%s", s.c_str());
                break;
            }
            case 5:  std::snprintf(buf, sizeof(buf), "Saved as Random %02d", randCountDisp_.load() + 1); break;
            case 6:  std::snprintf(buf, sizeof(buf), "Saving..."); break;
            default: std::snprintf(buf, sizeof(buf), "Ready");
        }
        return buf;
    }

    void publishSlots()
    {
        for (int i = 0; i < kMaxSlots; ++i)
        {
            mParam_[i].store(i < nSlots_ ? slots_[i].param : -1);
            mHeel_[i].store(i < nSlots_ ? slots_[i].heel : 0.0f);
            mToe_[i].store(i < nSlots_ ? slots_[i].toe : 0.0f);
        }
        mCount_.store(nSlots_, std::memory_order_release);
        statusChanged_ = true;
    }

    std::string valueText(int idx, float plain)
    {
        std::string s;
        if (idx < 0 || idx >= kNumParams) return s;
        if (!formatValue(idx, plain, s)) s = param(idx).display(plain);
        return s;
    }

    std::string morphText()
    {
        char buf[64];
        const int st = aStateDisp_.load(), tg = aTargetDisp_.load(), n = mCount_.load(), msg = lastMsgDisp_.load();
        const char* name = (tg >= 0 && tg < kNumParams) ? param(tg).name.c_str() : "";
        const int ls = lastSavedDisp_.load();
        switch (st)
        {
            case kAKnob:
                if (msg == kMsgKept && ls >= 0)
                    std::snprintf(buf, sizeof(buf), "%s kept: %s > %s", param(ls).name.c_str(),
                                  valueText(ls, heelDisp_.load()).c_str(), valueText(ls, toeDisp_.load()).c_str());
                else if (msg == kMsgNeedKnob) std::snprintf(buf, sizeof(buf), "Turn a knob first");
                else if (msg == kMsgFull) std::snprintf(buf, sizeof(buf), "Pedal full (8 knobs)");
                else std::snprintf(buf, sizeof(buf), "Turn a knob (%d set)", n);
                break;
            case kAHeel:
                std::snprintf(buf, sizeof(buf), msg == kMsgNeedHeel ? "%s: Set Heel first" : "%s: Set Heel", name); break;
            case kAToe:
                std::snprintf(buf, sizeof(buf), "Heel %s stored: Set Toe", valueText(tg, heelDisp_.load()).c_str()); break;
            default:
                if (msg == kMsgCleared) std::snprintf(buf, sizeof(buf), "Pedal cleared");
                else if (n == 0) std::snprintf(buf, sizeof(buf), "No pedal knobs");
                else std::snprintf(buf, sizeof(buf), "%d knob%s on pedal", n, n == 1 ? "" : "s");
        }
        return buf;
    }

    // ----- user presets ----------------------------------------------------------------------------
    // Audio thread: copy the patch and hand it to the writer thread (no I/O, no allocation here).
    void requestSave()
    {
        if (savePending_.load(std::memory_order_acquire) >= 0) return;      // still writing the last one
        for (int k = 0; k < kNumAudioParams; ++k) saveVals_[k] = get(k);
        for (int k = 0; k < kNumExt; ++k) saveVals_[kNumAudioParams + k] = get(kExtParams[k]);
        int len = kNameLen;
        while (len > 0 && (name_[len - 1] == 0 || name_[len - 1] == ' ')) --len;   // trim the end
        for (int i = 0; i < kNameLen; ++i) saveName_[i] = i < len ? (name_[i] ? name_[i] : ' ') : 0;
        saveName_[kNameLen] = 0;
        if (len == 0) autoName(saveName_);
        const int slot = nextSlot_;
        nextSlot_ = (nextSlot_ + 1) % kNumUser;
        savePending_.store(slot + (randomized_ ? 100 : 0), std::memory_order_release);
        randomized_ = false;
        randState_ = 6; randChanged_ = true;
    }

    void autoName(char* out) const
    {
        char nm[kNameLen + 8];
        std::snprintf(nm, sizeof(nm), randomized_ ? "Random %02d" : "User %02d", nextSlot_ + 1);
        std::memset(out, 0, kNameLen + 1);
        std::memcpy(out, nm, std::min((size_t)kNameLen, std::strlen(nm)));
    }

    static int charIndex(char c)
    {
        for (int i = 0; i < kNumChars; ++i) if (kCharset[i] == c) return i;
        return 0;                                                          // space
    }

    void startNaming()
    {
        autoName(name_);
        naming_ = true;
        cursor_ = std::min((int)std::strlen(name_), kNameLen - 1);         // just after the auto-name
        lastPos_ = cursor_; setAndTell(kNamePos, (float)cursor_);
        lastLetter_ = charIndex(name_[cursor_]); setAndTell(kNameLetter, (float)lastLetter_);
        nameMsg_ = kNmNaming; nameChanged_ = true;
    }

    void confirmName()
    {
        requestSave();
        naming_ = false;
        nameMsg_ = kNmSaving; nameChanged_ = true;
    }

    // Preset Name read-out: the draft with the cursor in [brackets] while naming, else the result.
    std::string nameText() const
    {
        char buf[64];
        const int msg = nameMsgDisp_.load();
        if (msg == kNmNaming)
        {
            const int c = nameCursorDisp_.load();
            std::string s;
            for (int i = 0; i < kNameLen; ++i)
            {
                char ch = nameDisp_[i].load();
                const bool end = ch == 0;
                if (end && i > c) break;
                if (end) ch = ' ';
                if (i == c) { s += '['; s += ch == ' ' ? '_' : ch; s += ']'; }
                else s += ch;
            }
            return s;
        }
        if (msg == kNmSaved)
        {
            std::string s = "Saved: ";
            for (int i = 0; i < kNameLen && savedNameDisp_[i].load(); ++i) s += savedNameDisp_[i].load();
            return s;
        }
        if (msg == kNmSaving) return "Saving...";
        if (msg == kNmCancelled) return "Not saved";
        std::snprintf(buf, sizeof(buf), "Tap Save to store");
        return buf;
    }

    // File v2: "SAUP" | u8 2 | u8 next slot | kNumUser x { u8 used, char name[17], f32 x kPresetVals }
    //   (v1, written by the previous version: { u8 used, u8 random, f32 x kNumAudioParams } - still read)
    void loadUserPresets()
    {
        std::vector<uint8_t> d;
        if (!mpc::readFile(userFile_, d) || d.size() < 6 || std::memcmp(d.data(), "SAUP", 4) != 0) return;
        const int ver = d[4];
        const size_t rec = ver == 1 ? 2 + 4 * (size_t)kNumAudioParams : 1 + (kNameLen + 1) + 4 * (size_t)kPresetVals;
        if ((ver != 1 && ver != 2) || d.size() < 6 + rec * kNumUser) return;
        nextSlot_ = d[5] % kNumUser;
        for (int u = 0; u < kNumUser; ++u)
        {
            const uint8_t* r = d.data() + 6 + rec * (size_t)u;
            userUsed_[u] = r[0];
            if (!r[0]) continue;
            std::vector<float>& v = programs_[(size_t)(numFactory_ + u)].plainValues;
            char nm[kNameLen + 8] = {0};
            const uint8_t* vals;
            if (ver == 1) { std::snprintf(nm, sizeof(nm), r[1] ? "Random %02d" : "User %02d", u + 1); vals = r + 2; }
            else { std::memcpy(nm, r + 1, kNameLen); vals = r + 1 + kNameLen + 1; }
            for (int k = 0; k < kNumAudioParams; ++k) { float x; std::memcpy(&x, vals + 4 * k, 4); if (std::isfinite(x)) v[(size_t)k] = x; }
            if (ver == 2)
                for (int k = 0; k < kNumExt; ++k) { float x; std::memcpy(&x, vals + 4 * (kNumAudioParams + k), 4); if (std::isfinite(x)) userExt_[u][(size_t)k] = x; }
            for (char& c : nm) if (c != 0 && (c < 32 || c > 126)) c = '?';
            if (nm[0] == 0) std::snprintf(nm, sizeof(nm), "User %02d", u + 1);
            programs_[(size_t)(numFactory_ + u)].name.assign(nm);
        }
    }

    // Writer thread: applies a pending save to the program list and writes the file.
    void writerLoop()
    {
        while (!quit_.load(std::memory_order_acquire))
        {
            const int p = savePending_.load(std::memory_order_acquire);
            if (p >= 0)
            {
                const int u = p % 100;
                userUsed_[u] = 1;
                std::vector<float>& v = programs_[(size_t)(numFactory_ + u)].plainValues;
                for (int k = 0; k < kNumAudioParams; ++k) v[(size_t)k] = saveVals_[k];
                for (int k = 0; k < kNumExt; ++k) userExt_[u][(size_t)k] = saveVals_[kNumAudioParams + k];
                programs_[(size_t)(numFactory_ + u)].name.assign(saveName_);      // fits the reserved capacity
                for (int i = 0; i <= kNameLen; ++i) savedNameDisp_[i].store(saveName_[i]);
                writeUserFile((u + 1) % kNumUser);
                savePending_.store(-1, std::memory_order_release);
                saveDone_.store(p, std::memory_order_release);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }

    void writeUserFile(int next)
    {
        std::vector<uint8_t> d = {'S', 'A', 'U', 'P', 2, (uint8_t)next};
        for (int u = 0; u < kNumUser; ++u)
        {
            d.push_back(userUsed_[u]);
            char nm[kNameLen + 1] = {0};
            std::strncpy(nm, programs_[(size_t)(numFactory_ + u)].name.c_str(), kNameLen);
            d.insert(d.end(), nm, nm + kNameLen + 1);
            for (int k = 0; k < kNumAudioParams; ++k) putF32(d, programs_[(size_t)(numFactory_ + u)].plainValues[(size_t)k]);
            for (int k = 0; k < kNumExt; ++k) putF32(d, userExt_[u][(size_t)k]);
        }
        const std::string tmp = userFile_ + ".tmp";
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (f == nullptr) return;
        const bool ok = std::fwrite(d.data(), 1, d.size(), f) == d.size();
        std::fclose(f);
        if (!ok) { std::remove(tmp.c_str()); return; }
        std::remove(userFile_.c_str());
        std::rename(tmp.c_str(), userFile_.c_str());
    }
    std::string stutterText() const
    {
        char buf[48];
        const int st = dStState_.load();
        const float s = dSliceMs_.load(), l = dLoopMs_.load();
        if (st == kCapture)   std::snprintf(buf, sizeof(buf), "Rec %.0f ms", (double)s);
        else if (st == kPlay) std::snprintf(buf, sizeof(buf), "Loop %.0f/%.0f ms", (double)s, (double)l);
        else                  std::snprintf(buf, sizeof(buf), "Idle");
        return buf;
    }

    // Displays: refreshed every 100 ms, the host is told only about values that changed.
    void publish()
    {
        const float lvl = inPeak_ > 1e-6f ? linToDb(inPeak_) : kNone;
        inPeak_ = 0.0f;
        const float gr = linToDb(grAcc_);
        grAcc_ = 1.0f;
        dInLevel_.store(lvl);
        dCutoff_.store(cutoff_);
        dGr_.store(gr);
        dStState_.store(stState_);
        dSliceMs_.store(1000.0f * sliceLen_ / (float)sr_);
        dLoopMs_.store(1000.0f * loopLen_ / (float)sr_);

        pushDisplay(kInLevel, lvl <= kNone + 1.0f ? 0.0f : mpc::clamp((lvl + 48.0f) / 48.0f, 0.0f, 1.0f));
        dEnv_.store(envPeak_);                       // envelope LED: the peak of the last 100 ms
        pushDisplay(kEnvLevel, envPeak_);
        envPeak_ = 0.0f;
        pushDisplay(kFiltPos, getBool(kFiltOn) ? pos01_ : 0.0f);
        pushDisplay(kLimGr, mpc::clamp(-gr / 12.0f, 0.0f, 1.0f));
        pushDisplay(kStutLit, stState_ != kIdle ? 1.0f : 0.0f);
        dTouch_.store(touch01_);
        pushDisplay(kTouchLevel, touch01_);
        dSweepHz_.store(sweepHz_);
        if (std::fabs(sweepHz_ - lastSweepShown_) > 0.01f * lastSweepShown_ + 0.005f)
        {   // a new value so the host re-reads the text
            lastSweepShown_ = sweepHz_;
            setNormalised(kSweepNow, mpc::clamp(std::log2(sweepHz_ / 0.05f) / std::log2(200.0f), 0.0f, 1.0f));
            notifyHostParameterChanged(kSweepNow);
        }
        if (nameChanged_)
        {
            nameChanged_ = false;
            for (int i = 0; i <= kNameLen; ++i) nameDisp_[i].store(name_[i]);
            nameCursorDisp_.store(cursor_); nameMsgDisp_.store(nameMsg_);
            setNormalised(kPresetName, (float)((nameSerial_++ % 50) + 1) / 100.0f);
            notifyHostParameterChanged(kPresetName);
        }
        const float stNorm = stState_ == kIdle ? 0.0f : (stState_ == kCapture ? 0.5f : 1.0f);
        if (stNorm != getNormalised(kStutNow) || stState_ != lastStState_ || sliceLen_ != lastSlice_)
        {
            lastStState_ = stState_; lastSlice_ = sliceLen_;
            setNormalised(kStutNow, stNorm); notifyHostParameterChanged(kStutNow);
        }
        if (statusChanged_)
        {
            statusChanged_ = false;
            setNormalised(kMorphStatus, (float)(aState_ * 16 + nSlots_) / 100.0f);
            notifyHostParameterChanged(kMorphStatus);
        }
        if (randChanged_)
        {
            randChanged_ = false;
            randStateDisp_.store(randState_); randCountDisp_.store(randCount_);
            setNormalised(kRandStatus, (float)((randSerial_++ % 50) + 1) / 100.0f);   // a new value so the host re-reads the text
            notifyHostParameterChanged(kRandStatus);
        }
    }

    void pushDisplay(int idx, float norm)
    {
        if (std::fabs(norm - getNormalised(idx)) < 0.005f) return;     // redraw only on change
        setNormalised(idx, norm);
        notifyHostParameterChanged(idx);
    }

    // ----- members -------------------------------------------------------------------------------
    double sr_ = 44100.0, bpm_ = 120.0;
    Ladder ladder_[2];
    float lfoPhase_ = 0.0f, envFast_ = 0.0f, fastDecay_ = 0.999f, posGate_ = 0.0f, envPeak_ = 0.0f;
    float volT_[2] = {1.0f, 1.0f}, volGain_[2] = {1.0f, 1.0f};
    bool  gateOpen_ = false;
    Glide gVol_[2];
    float env_ = 0.0f, pos01_ = 0.0f, cutoff_ = 120.0f;
    unsigned ctrl_ = 0;
    std::vector<float> buf_[2];
    int stState_ = kIdle, sliceLen_ = 4410, loopLen_ = 44100, fadeLen_ = 64, stPos_ = 0, stCount_ = 0;
    bool stOneShot_ = false, lastHold_ = false;
    bool btnLast_[kNumParams] = {};                      // action buttons: last seen switch position
    int64_t btnTime_[kNumParams] = {};                   // ... and when each last acted (samples)
    int64_t samplesNow_ = 1 << 30;
    bool activeNow_ = false, lastActive_ = false, lastTrig_ = false;

    // pedal status messages
    enum Msg { kMsgNone, kMsgHeel, kMsgKept, kMsgNeedKnob, kMsgNeedHeel, kMsgFull, kMsgCleared };
    int lastMsg_ = kMsgNone;
    std::atomic<int> lastMsgDisp_{kMsgNone}, lastSavedDisp_{-1};
    std::atomic<float> heelDisp_{0.0f}, toeDisp_{0.0f};

    // touch, auto sweep, stutter re-roll
    float touchPk_ = 0.0f, touchEnv_ = 0.0f, touch01_ = 0.0f, tchUp_ = 0.1f, tchDown_ = 0.01f, tchSmooth_ = 0.02f;
    float sweepPhase_ = 0.0f, sweepHz_ = 0.5f, lastSweepShown_ = -1.0f;
    float rerollRate_ = 1.0f, rerollAcc_ = 0.0f, rSlice_ = 0.45f, rLoop_ = 0.40f;
    bool  rerollDue_ = true, randMode_ = false;
    std::atomic<float> dTouch_{0.0f}, dSweepHz_{0.5f};

    // preset naming
    enum NameMsg { kNmIdle, kNmNaming, kNmSaving, kNmSaved, kNmCancelled };
    bool naming_ = false, nameChanged_ = true;
    char name_[kNameLen + 1] = {};
    int  cursor_ = 0, lastPos_ = -1, lastLetter_ = -1, nameMsg_ = kNmIdle, nameSerial_ = 0;
    std::atomic<char> nameDisp_[kNameLen + 1] = {}, savedNameDisp_[kNameLen + 1] = {};
    std::atomic<int> nameCursorDisp_{0}, nameMsgDisp_{kNmIdle};
    char saveName_[kNameLen + 1] = {};

    // presets: later settings per factory program and per user slot
    std::vector<std::vector<std::pair<int, float>>> progExtOv_;
    std::vector<std::array<float, 14>> progExt_;

    // user presets
    static constexpr int kNumUser = 16;
    std::array<float, 14> userExt_[kNumUser] = {};
    int numFactory_ = 0, nextSlot_ = 0;
    bool randomized_ = false;
    uint8_t userUsed_[kNumUser] = {};
    float saveVals_[kPresetVals] = {};
    std::string userFile_;
    std::atomic<int> savePending_{-1}, saveDone_{-1};
    std::atomic<bool> quit_{false};
    std::thread worker_;
    mpc::dsp::Xorshift32 rng_;
    PeakLimiter limiter_;
    float delay_[4][kDelaySize] = {};
    int delayPos_ = 0;
    Glide gIn_, gOut_, gMix_, gFiltMix_, gStutMix_, gFb_, gBypass_, gPlay_, gDryMute_;
    int blockLen_ = 4410, blockPos_ = 0, lastSync_ = 0, lastFiltMode_ = 0;
    float inPeak_ = 0.0f, grAcc_ = 1.0f;
    int lastStState_ = -1, lastSlice_ = -1;

    // pedal assignment (audio thread) + mirror for the UI thread
    Slot slots_[kMaxSlots] = {};
    int nSlots_ = 0;
    int aState_ = kAOff, aTarget_ = -1, lastSaved_ = -1;
    float aHeel_ = 0.0f, aToe_ = 0.0f;
    float snap_[kNumParams] = {};
    bool statusChanged_ = true;
    std::atomic<int> mParam_[kMaxSlots];
    std::atomic<float> mHeel_[kMaxSlots], mToe_[kMaxSlots];
    std::atomic<int> mCount_{0}, aStateDisp_{kAOff}, aTargetDisp_{-1};
    Slot staged_[kMaxSlots] = {};
    int stagedCount_ = 0;
    std::atomic<bool> morphPending_{false};

    // patch randomizer
    mpc::dsp::Xorshift32 rng2_;
    float undoVal_[kNumParams] = {};
    bool  haveUndo_ = false, randChanged_ = false;
    int   randState_ = 0, randCount_ = 0, randSerial_ = 0;
    std::atomic<int> randStateDisp_{0}, randCountDisp_{0};

    // display values (written by the audio thread, read by the UI thread)
    std::atomic<float> dEnv_{0.0f};
    std::atomic<float> dInLevel_{kNone}, dCutoff_{120.0f}, dGr_{0.0f}, dSliceMs_{0.0f}, dLoopMs_{0.0f};
    std::atomic<int> dStState_{kIdle};
};

MPC_PLUGIN_ENTRY(SleepyAndroid)
