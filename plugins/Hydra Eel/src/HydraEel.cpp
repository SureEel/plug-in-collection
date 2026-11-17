// Hydra Eel - a spaghetti western guitar rig for Akai MPC / Force, by Sure Eel (manufacturer "sureeel").
//
// Signal flow (each section has its own On switch and crossfades in/out without clicks):
//
//   in -> Input gain -> DRIVE (mono: 4 compressors / 4 fuzzes, fuzzes 2x oversampled)
//      -> MOD (tremolo <-> vibrato, or rotary <-> phaser; mono in, stereo out)
//      -> DELAY (digital or analog, ms or tempo-synced, stereo)
//      -> REVERB (plate, spring or shimmer, stereo)
//      -> Mono / Stereo -> Output gain -> -1 dBFS look-ahead limiter -> out
//   Bypass: the input, delayed by the limiter's look-ahead, bit-exact.
//
// Patch tools: Randomize (sensible ranges, never bypass / input / output / stereo / section switches),
// Undo, Rand Amount; Save Preset with a letter-picker name, stored after the factory presets in a
// file beside the plugin folder (kept across restarts and reinstalls).
// Buttons on the MPC touch screen are on/off switches; action buttons act on every change (see
// acted()), which is what works on the hardware.

#include "mpc_plugin.h"
#include "dsp/tempo.h"
#include "hydra_dsp.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

using namespace hydra;

// Parameter order is permanent (saved projects). Append new parameters at the end only.
enum ParamIndex
{
    kOutput = 0,   // dB. Parameter 0 is nudged by the host on every load: harmless here.
    kInput, kBypass, kStereo,
    kDriveOn, kDriveType, kAmount, kTone, kLevel, kBias,
    kModOn, kModMode, kBlend, kRate, kDepth, kWave, kRotor,
    kDelayOn, kDelayType, kSync, kTime, kDiv, kFeedback, kDelayMix,
    kRevOn, kRevType, kDecay, kRevTone, kRevMix,
    kRandomize, kUndo, kRandAmount, kSave, kConfirm, kCancel, kNamePos, kLetter,
    kInLevel, kOutLevel, kLimGr, kCompGr, kRotorNow, kDelayNow, kRandStatus, kPresetName,   // displays
    kHelp, kHelp2,
    kDriveMix,                                              // added later: dry blend of the drive (parallel compression)
    kRateMode,                                              // added later: mod rate in Hz or tempo-synced
    kNumParams
};
constexpr int kNumAudioParams = kRevMix + 1;   // what a preset holds
constexpr int kNameLen = 16;
const char kCharset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.#&+!";
constexpr int kNumChars = (int)sizeof(kCharset) - 1;
constexpr float kCeiling = 0.891f;            // -1 dBFS
constexpr int kDelaySize = 128;

const char* const kHelpText[][2] = {
    {"Touch a label or meter",         "to see what it does."},            // 0
    {"Level into the plugin, dB.",     "Guitar: try 0 to +12 dB."},        // 1
    {"Output level before the",        "-1 dBFS safety limiter."},         // 2
    {"Bypass: the input passes",       "through untouched."},              // 3
    {"Mono: one channel, sums the",    "output. Stereo: wide effects."},   // 4
    {"Drive: compressors (left) to",   "fuzzes (right), clean to dirty."}, // 5
    {"Comp: how hard it squeezes.",    "Fuzz: how much gain."},            // 6
    {"Tone: dark to bright.",          "Sustain: mid scoop at centre."},   // 7
    {"Drive output level, dB.",        "Match it to bypass."},             // 8
    {"Comp: noise gate threshold.",    "Fuzz: starve it to sputter."},     // 9
    {"Mod: tremolo-vibrato, or",       "rotary-phaser. Blend sweeps."},    // 10
    {"Blend: from the first to the",   "second effect; both mid-way."},    // 11
    {"Rate: Hz or Sync. Hold it at",   "the bottom: Sync; top: Hz."},      // 12
    {"How deep the modulation is.",    "0 % = none."},                     // 13
    {"Tremolo / vibrato shape:",       "sine, triangle or square."},       // 14
    {"Rotor slow / fast: it ramps",    "up and down like a real one."},    // 15
    {"Digital: clean repeats.",        "Analog: darker, wobbly."},         // 16
    {"Time: ms or Sync. Hold it at",   "the bottom: Sync; top: ms."},      // 17
    {"Feedback: how many repeats.",    "Analog repeats saturate."},        // 18
    {"Delay level against the dry",    "signal (50 % = both full)."},      // 19
    {"Plate: dense. Spring: drippy",   "boing. Shimmer: octave glow."},    // 20
    {"Reverb length.",                 "Shimmer: also the glow."},         // 21
    {"Reverb tone: dark to bright.",   ""},                                // 22
    {"Reverb level against the dry",   "signal (50 % = both full)."},      // 23
    {"New random settings within",     "musical ranges. Undo goes back."}, // 24
    {"Undo restores the settings",     "before the last Randomize."},      // 25
    {"How far Randomize moves:",       "low nudges, 100 % rerolls."},      // 26
    {"Save: name it with Name Pos",    "and Letter, then Confirm."},       // 27
    {"Levels in dBFS; GR = gain",      "reduction of the limiter, dB."},   // 28
    {"Live readout: rotor speed",      "or the actual delay time."},       // 29
    {"Shows what the last button",     "did."},                            // 30
    {"Compressor gain reduction",      "in dB (compressor types)."},       // 31
    {"Drive Mix: dry blend. Below",    "100 % = parallel compression."},  // 32
};
constexpr int kNumHelp = (int)(sizeof(kHelpText) / sizeof(kHelpText[0]));
static_assert(kNumHelp <= 40, "help items");

// Randomizer: which parameters and the musical range each may land in. kind 0 linear, 1 log, 2 choice.
struct RandSpec { int param; float lo, hi; int kind; };
const RandSpec kRandSpecs[] = {
    {kDriveType, 0, 7, 2}, {kAmount, 15, 85, 0}, {kTone, 20, 80, 0}, {kLevel, -6, 3, 0}, {kBias, 0, 35, 0},
    {kModMode, 0, 1, 2}, {kBlend, 0, 100, 0}, {kRate, 0.5f, 8.0f, 1}, {kDepth, 20, 90, 0}, {kWave, 0, 2, 2}, {kRotor, 0, 1, 2},
    {kDelayType, 0, 1, 2}, {kTime, 120, 900, 1}, {kFeedback, 10, 60, 0}, {kDelayMix, 10, 45, 0},
    {kRevType, 0, 2, 2}, {kDecay, 25, 85, 0}, {kRevTone, 20, 80, 0}, {kRevMix, 10, 50, 0}, {kDriveMix, 60, 100, 0},
};

class HydraEel : public mpc::Plugin
{
public:
    HydraEel() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("output", "Output", -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("input",  "Input",  -12.0f, 24.0f, 0.0f, "dB", 1));
        addParam(mpc::boolParam ("bypass", "Bypass", false));
        addParam(mpc::choiceParam("stereo", "Stereo", {"Mono", "Stereo"}, 1));
        // drive (compressors and fuzzes)
        addParam(mpc::boolParam ("drive_on", "Drive On", true));
        addParam(mpc::choiceParam("drive_type", "Drive Type", {"Opto", "Smooth", "FET", "Punch", "Germanium", "Gated", "Sustain", "Octave"}, 4));
        addParam(mpc::floatParam("amount", "Amount", 0.0f, 100.0f, 35.0f, "%", 0));
        addParam(mpc::floatParam("tone",   "Tone",   0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::floatParam("level",  "Level", -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("bias",   "Gate Bias", 0.0f, 100.0f, 0.0f, "%", 0));
        // modulation
        addParam(mpc::boolParam ("mod_on", "Mod On", true));
        addParam(mpc::choiceParam("mod_mode", "Mod Mode", {"Trem / Vib", "Rotary / Phaser"}, 0));
        addParam(mpc::floatParam("blend",  "Blend", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::logParam  ("rate",   "Rate",  0.1f, 12.0f, 5.5f, "Hz", 2));
        addParam(mpc::floatParam("depth",  "Depth", 0.0f, 100.0f, 60.0f, "%", 0));
        addParam(mpc::choiceParam("wave",  "Wave", {"Sine", "Triangle", "Square"}, 0));
        addParam(mpc::choiceParam("rotor", "Rotor", {"Slow", "Fast"}, 0));
        // delay
        addParam(mpc::boolParam ("delay_on", "Delay On", true));
        addParam(mpc::choiceParam("delay_type", "Delay Type", {"Digital", "Analog"}, 1));
        addParam(mpc::choiceParam("sync",  "Sync", {"ms", "Tempo"}, 0));
        addParam(mpc::logParam  ("time",   "Time", 20.0f, 2000.0f, 380.0f, "ms", 0));
        addParam(mpc::choiceParam("div",   "Division", mpc::dsp::divisionNames(), 9));     // 1/8D
        addParam(mpc::floatParam("feedback", "Feedback", 0.0f, 95.0f, 35.0f, "%", 0));
        addParam(mpc::floatParam("delay_mix", "Delay Mix", 0.0f, 100.0f, 30.0f, "%", 0));
        // reverb
        addParam(mpc::boolParam ("rev_on", "Reverb On", true));
        addParam(mpc::choiceParam("rev_type", "Reverb Type", {"Plate", "Spring", "Shimmer"}, 1));
        addParam(mpc::floatParam("decay",  "Decay", 0.0f, 100.0f, 55.0f, "%", 0));
        addParam(mpc::floatParam("rev_tone", "Rev Tone", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::floatParam("rev_mix", "Reverb Mix", 0.0f, 100.0f, 30.0f, "%", 0));
        // patch tools
        addParam(mpc::boolParam ("randomize", "Randomize", false));
        addParam(mpc::boolParam ("undo", "Undo", false));
        addParam(mpc::floatParam("rand_amount", "Rand Amount", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::boolParam ("save", "Save", false));
        addParam(mpc::boolParam ("confirm", "Confirm", false));
        addParam(mpc::boolParam ("cancel", "Cancel", false));
        std::vector<std::string> pos, letters;
        for (int i = 1; i <= kNameLen; ++i) pos.push_back(std::to_string(i));
        for (int i = 0; i < kNumChars; ++i) letters.push_back(kCharset[i] == ' ' ? std::string("space") : std::string(1, kCharset[i]));
        addParam(mpc::choiceParam("name_pos", "Name Pos", pos, 0));
        addParam(mpc::choiceParam("letter", "Letter", letters, 0));
        const char* const dispIds[] = {"in_level", "out_level", "lim_gr", "comp_gr", "rotor_now", "delay_now", "rand_status", "preset_name"};
        const char* const dispNames[] = {"In Level", "Out Level", "Limiter GR", "Comp GR", "Rotor Now", "Delay Now", "Status", "Preset Name"};
        for (int i = 0; i < 8; ++i)
        {
            mpc::Param p = mpc::floatParam(dispIds[i], dispNames[i], 0.0f, 1.0f, 0.0f, "", 2);
            p.automatable = false;
            addParam(p);
        }
        std::vector<std::string> h1, h2;
        for (const auto& h : kHelpText) { h1.push_back(h[0]); h2.push_back(h[1]); }
        mpc::Param p1 = mpc::choiceParam("help", "Help", h1, 0), p2 = mpc::choiceParam("help2", "Help 2", h2, 0);
        p1.automatable = p2.automatable = false;
        addParam(p1); addParam(p2);
        addParam(mpc::floatParam("drive_mix", "Drive Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::choiceParam("rate_mode", "Rate Mode", {"Hz", "Sync"}, 0));
        static_assert(kRateMode + 1 == kNumParams, "parameter list");

        // ---- factory presets: defaults + changes ----------------------------------------------
        using P = std::vector<std::pair<int, float>>;
        auto preset = [this](const char* name, const P& changes) {
            std::vector<float> v(kNumAudioParams);
            for (int i = 0; i < kNumAudioParams; ++i) v[(size_t)i] = param(i).def;
            float mix = 100.0f;
            for (const auto& c : changes) { if (c.first == kDriveMix) mix = c.second; else v[(size_t)c.first] = c.second; }
            addProgram(name, v);
            progMix_.push_back(mix);                     // Drive Mix and Rate Mode sit after the program table: applied in programLoaded()
        };
        enum { Opto, Smooth, FET, Punch, Germ, Gated, Sustain, Octave };
        preset("Init", {{kModOn, 0}, {kDelayOn, 0}, {kRevOn, 0}});        // just the drive: nothing hidden
        // spaghetti western
        preset("Lonesome Trail", {{kDriveType, Opto}, {kAmount, 30}, {kTone, 60}, {kRate, 5.5f}, {kDepth, 70}, {kWave, 0},
                                  {kDelayType, 1}, {kTime, 420}, {kFeedback, 30}, {kDelayMix, 28}, {kRevType, 1}, {kDecay, 62}, {kRevMix, 38}});
        preset("Showdown",       {{kDriveType, Germ}, {kAmount, 70}, {kTone, 62}, {kLevel, -2}, {kModOn, 0},
                                  {kDelayType, 1}, {kTime, 340}, {kFeedback, 40}, {kDelayMix, 30}, {kRevType, 1}, {kDecay, 70}, {kRevMix, 40}});
        preset("Desert Wind",    {{kDriveType, Opto}, {kAmount, 40}, {kTone, 55}, {kBlend, 100}, {kRate, 4.5f}, {kDepth, 40},
                                  {kDelayType, 0}, {kTime, 520}, {kFeedback, 45}, {kDelayMix, 35}, {kRevType, 0}, {kDecay, 80}, {kRevMix, 42}});
        preset("Ennio Twang",    {{kDriveType, Punch}, {kAmount, 55}, {kTone, 70}, {kRate, 6.5f}, {kDepth, 55}, {kWave, 1},
                                  {kSync, 1}, {kDiv, 9}, {kFeedback, 25}, {kDelayMix, 25}, {kRevType, 1}, {kDecay, 55}, {kRevMix, 40}});
        preset("Ghost Town",     {{kDriveOn, 0}, {kBlend, 100}, {kRate, 0.8f}, {kDepth, 30},
                                  {kDelayType, 1}, {kTime, 600}, {kFeedback, 55}, {kDelayMix, 30}, {kRevType, 2}, {kDecay, 85}, {kRevMix, 45}});
        preset("Duel at Dawn",   {{kDriveType, Gated}, {kAmount, 75}, {kBias, 40}, {kTone, 55}, {kModOn, 0},
                                  {kTime, 280}, {kFeedback, 20}, {kDelayMix, 20}, {kRevType, 1}, {kDecay, 65}, {kRevMix, 35}});
        preset("Tumbleweed",     {{kDriveType, Opto}, {kAmount, 30}, {kModMode, 1}, {kBlend, 0}, {kRate, 6.5f}, {kDepth, 70}, {kRotor, 0},
                                  {kDelayOn, 0}, {kRevType, 1}, {kDecay, 50}, {kRevMix, 30}});
        preset("Octave Outlaw",  {{kDriveType, Octave}, {kAmount, 60}, {kTone, 55}, {kModOn, 0}, {kSync, 1}, {kDiv, 10},
                                  {kFeedback, 35}, {kDelayMix, 25}, {kRevType, 1}, {kDecay, 50}, {kRevMix, 30}});
        preset("Big Sky Fuzz",   {{kDriveType, Sustain}, {kAmount, 65}, {kTone, 45}, {kModOn, 0}, {kDelayType, 1}, {kTime, 480},
                                  {kFeedback, 45}, {kDelayMix, 30}, {kRevType, 2}, {kDecay, 75}, {kRevMix, 40}});
        // funk and jazz, on the compressors
        preset("Funk Chicken",   {{kDriveType, Punch}, {kAmount, 70}, {kDriveMix, 75}, {kTone, 65}, {kModOn, 0}, {kDelayOn, 0},
                                  {kRevType, 0}, {kDecay, 30}, {kRevMix, 12}});
        preset("Jazz Box",       {{kDriveType, Opto}, {kAmount, 45}, {kDriveMix, 70}, {kTone, 35}, {kModOn, 0}, {kDelayOn, 0},
                                  {kRevType, 0}, {kDecay, 45}, {kRevMix, 18}});
        preset("Clean Strut",    {{kDriveType, FET}, {kAmount, 60}, {kDriveMix, 65}, {kTone, 55}, {kModMode, 1}, {kBlend, 100}, {kRate, 1.2f}, {kDepth, 50},
                                  {kDelayType, 0}, {kSync, 1}, {kDiv, 4}, {kFeedback, 15}, {kDelayMix, 15}, {kRevType, 0}, {kDecay, 35}, {kRevMix, 15}});
        preset("Smooth Cat",     {{kDriveType, Smooth}, {kAmount, 50}, {kModMode, 1}, {kBlend, 30}, {kRate, 6.0f}, {kDepth, 45},
                                  {kDelayOn, 0}, {kRevType, 0}, {kDecay, 50}, {kRevMix, 20}});
        numFactory_ = (int)programs_.size();
        for (int u = 0; u < kNumUser; ++u)
        {
            char nm[16]; std::snprintf(nm, sizeof nm, "Empty %02d", u + 1);
            userMix_[u] = 100.0f;
            addProgram(nm, programs_[0].plainValues);
            programs_.back().name.reserve(24);
        }
        const std::string& pf = mpc::pluginFolder();
        if (pf.find(" - VST - ") != std::string::npos) userFile_ = pf.substr(0, pf.find_last_of('/')) + "/.sureeel Hydra Eel user presets.bin";
        else userFile_ = (pf.empty() ? std::string(".") : pf) + "/user-presets.bin";   // development builds
        loadUserPresets();
        rng_.reseed((uint32_t)(reinterpret_cast<uintptr_t>(this) ^ 0xEE1u) * 2654435761u);
        worker_ = std::thread([this] { writerLoop(); });
    }

    // Silent on purpose: the compressor's noise gate is set and closed (watchdog: leave it alone)
    bool silenceIsExpected() const override { return getBool(kDriveOn) && getInt(kDriveType) < 4 && get(kBias) > 0.5f; }

    // Input and Output are global: presets (factory or saved) never change them; only you do.
    bool programSetsParam(int index) const override { return index != kInput && index != kOutput; }
    // the setting kept after the program table
    void programLoaded(int index) override
    {
        if (index >= 0 && index < numFactory_) { set(kDriveMix, progMix_[(size_t)index]); set(kRateMode, 0.0f); }
        else if (index >= numFactory_ && index < numFactory_ + kNumUser) { set(kDriveMix, userMix_[index - numFactory_]); set(kRateMode, userRate_[index - numFactory_]); }
        // tempo-synced delay: put the Time knob on the preset's division
        if (getInt(kSync) == 1) setNormalised(kTime, (float)getInt(kDiv) / (float)(mpc::dsp::kNumDivisions - 1));
    }

    ~HydraEel() override
    {
        quit_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = sampleRate;
        drive_.prepare(sampleRate);
        mod_.prepare(sampleRate); mod_.setRamps(sampleRate);
        delay_.prepare(sampleRate); delay_.setAnalogTone(sampleRate);
        driveDry_.prepare(64);
        // the reverbs run at half rate (see process): half the cost, and their tails are dark anyway
        const double hr = sampleRate * 0.5;
        plate_.prepare(hr);
        spring_.prepare(hr);
        shimmer_.prepare(hr);
        shimHp_.a = lpCoef(300.0f, hr);
        limiter_.prepare(sampleRate);
        for (Glide* g : {&gIn_, &gOut_, &gLevel_, &gBlend_, &gDepth_, &gFb_, &gDMix_, &gRMix_, &gDecay_, &gBypass_,
                         &gDriveOn_, &gModOn_, &gDelayOn_, &gRevOn_, &gDMixDrive_})
            g->setTime(20.0f, sampleRate);
        blockLen_ = (int)(sampleRate * 0.1);
        reset();
    }

    void reset() override
    {
        drive_.reset(); mod_.reset(); delay_.reset(); plate_.reset(); spring_.reset(); shimmer_.reset();
        shimHp_.reset(); shimLp_.reset(); shimFb_ = 0.0f;
        revPhase_ = 0; revIn_ = 0.0f; curWl_ = curWr_ = 0.0f;
        limiter_.reset();
        std::memset(dly_, 0, sizeof dly_); dlyPos_ = 0;
        gIn_.v = dbToLin(get(kInput)); gOut_.v = dbToLin(get(kOutput)); gLevel_.v = dbToLin(get(kLevel));
        gBlend_.v = get(kBlend) * 0.01f; gDepth_.v = get(kDepth) * 0.01f; gFb_.v = get(kFeedback) * 0.01f;
        gDMix_.v = get(kDelayMix) * 0.01f; gRMix_.v = get(kRevMix) * 0.01f; gDecay_.v = get(kDecay) * 0.01f;
        gBypass_.v = getBool(kBypass) ? 1.0f : 0.0f; gDMixDrive_.v = get(kDriveMix) * 0.01f; driveDry_.clear();
        gDriveOn_.v = getBool(kDriveOn) ? 1.0f : 0.0f; gModOn_.v = getBool(kModOn) ? 1.0f : 0.0f;
        gDelayOn_.v = getBool(kDelayOn) ? 1.0f : 0.0f; gRevOn_.v = getBool(kRevOn) ? 1.0f : 0.0f;
        mod_.hornHz = getBool(kRotor) ? get(kRate) : get(kRate) * 0.125f; mod_.drumHz = mod_.hornHz * 0.88f;
        lastType_ = getInt(kDriveType);
        for (int b : {kRandomize, kUndo, kSave, kConfirm, kCancel}) btnLast_[b] = getBool(b);
        lastPos_ = getInt(kNamePos); lastLetter_ = getInt(kLetter);
        // a load is not a knob move: nothing may flip until the knob is moved again
        disarm(rateFlip_, getNormalised(kRate)); disarm(timeFlip_, getNormalised(kTime));
        lastRateSync_ = getInt(kRateMode) == 1; lastTimeSync_ = getInt(kSync) == 1; rateHint_.store(false); timeHint_.store(false);
        lastDiv_ = getInt(kDiv); lastTimeDiv_ = timeDiv(getNormalised(kTime));
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        tempoValid_ = ctx.transport.tempoValid;
        bpm_ = ctx.transport.tempoValid ? ctx.transport.bpm : 120.0;
        handleButtons();
        handleFlips();

        // ---- per-block settings ----------------------------------------------------------------
        const float inT = dbToLin(get(kInput)), outT = dbToLin(get(kOutput));
        const bool bypass = getBool(kBypass), stereo = getInt(kStereo) == 1;
        const int type = getInt(kDriveType);
        if (type != lastType_) { drive_.reset(); lastType_ = type; }
        drive_.set(type, get(kAmount) * 0.01f, get(kTone) * 0.01f, get(kBias) * 0.01f);
        const float dMixDT = get(kDriveMix) * 0.01f;
        const int dryDelay = type >= 4 ? Oversampler4x::kLatency : 0;      // keeps the dry blend in time with the fuzz
        const float levelT = dbToLin(get(kLevel));
        const int modMode = getInt(kModMode), wave = getInt(kWave);
        const bool fast = getInt(kRotor) == 1;
        const float rate = rateHz(), blendT = get(kBlend) * 0.01f, depthT = get(kDepth) * 0.01f;
        rateNow_.store(rate);
        const bool analog = getInt(kDelayType) == 1;
        const float delayMs = delayMsNow();
        const float delaySamples = clamp(delayMs, 1.0f, 4000.0f) * 0.001f * (float)sr_;
        delayMsNow_ = clamp(delayMs, 1.0f, 4000.0f);
        const float fbT = clamp(get(kFeedback) * 0.01f, 0.0f, 0.95f);
        const float dMixT = get(kDelayMix) * 0.01f, rMixT = get(kRevMix) * 0.01f, decayT = get(kDecay) * 0.01f;
        const int revType = getInt(kRevType);
        const float driveOnT = getBool(kDriveOn) ? 1.0f : 0.0f, modOnT = getBool(kModOn) ? 1.0f : 0.0f;
        const float delayOnT = getBool(kDelayOn) ? 1.0f : 0.0f, revOnT = getBool(kRevOn) ? 1.0f : 0.0f;
        const float tone = get(kRevTone) * 0.01f;
        // reverb settings per block
        const float plateDamp = 0.75f - 0.72f * tone, plateBw = 0.25f + 0.73f * tone;
        const float springTone = lpCoef(2000.0f * std::exp2(tone * 2.0f), sr_ * 0.5);
        shimLp_.a = lpCoef(2500.0f * std::exp2(tone * 2.0f), sr_ * 0.5);

        const bool haveIn = ctx.inputs != nullptr && ctx.numInputs > 0 && ctx.inputs[0] != nullptr;
        const float* inL = haveIn ? ctx.inputs[0] : nullptr;
        const float* inR = haveIn ? ((ctx.numInputs > 1 && ctx.inputs[1]) ? ctx.inputs[1] : ctx.inputs[0]) : nullptr;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        for (int i = 0; i < n; ++i)
        {
            const float rawL = haveIn ? inL[i] : 0.0f, rawR = haveIn ? inR[i] : 0.0f;   // read first (in place)
            const float gi = gIn_.next(inT);
            float l = rawL * gi, r = rawR * gi;
            inPeak_ = std::fmax(inPeak_, std::fmax(std::fabs(l), std::fabs(r)));

            // DRIVE (mono)
            const float dOn = gDriveOn_.next(driveOnT);
            const float lv = gLevel_.next(levelT);
            const float dm = gDMixDrive_.next(dMixDT);
            if (dOn > 0.0f)
            {
                const float m = 0.5f * (l + r);
                driveDry_.push(m);
                const float wet = drive_.process(m) * lv;
                const float d = wet * dm + driveDry_.tap(dryDelay) * (1.0f - dm);
                l += (d - l) * dOn; r += (d - r) * dOn;
            }
            // MOD (mono in, stereo out)
            const float mOn = gModOn_.next(modOnT);
            const float bl = gBlend_.next(blendT), dp = gDepth_.next(depthT);
            if (mOn > 0.0f)
            {
                const float m = 0.5f * (l + r);
                float ml, mr;
                if (modMode == 0) mod_.tremVib(m, ml, mr, rate, dp, bl, wave, stereo);
                else mod_.rotPhase(m, ml, mr, rate, dp, bl, fast, stereo);
                l += (ml - l) * mOn; r += (mr - r) * mOn;
            }
            // DELAY
            const float yOn = gDelayOn_.next(delayOnT);
            const float fb = gFb_.next(fbT), dmix = gDMix_.next(dMixT);
            if (yOn > 0.0f)
            {
                float wl, wr;
                delay_.process(l, r, wl, wr, delaySamples, fb, analog, stereo);
                const float dry = std::fmin(1.0f, 2.0f * (1.0f - dmix)), wet = std::fmin(1.0f, 2.0f * dmix);
                const float ol = l * dry + wl * wet, orr = r * dry + wr * wet;
                l += (ol - l) * yOn; r += (orr - r) * yOn;
            }
            // REVERB
            const float vOn = gRevOn_.next(revOnT);
            const float rmix = gRMix_.next(rMixT), dec = gDecay_.next(decayT);
            if (vOn > 0.0f)
            {
                // half-rate reverb: average each pair of input samples, run the reverb once per
                // pair, and interpolate the output linearly back to the full rate
                const float m = 0.5f * (l + r);
                float wl, wr;
                if (revPhase_ == 0) { revIn_ = m; revPhase_ = 1; wl = curWl_; wr = curWr_; }
                else
                {
                    revPhase_ = 0;
                    const float x0 = 0.5f * (revIn_ + m);
                    float nl, nr;
                    if (revType == 1)
                    {
                        spring_.process(x0, dec, springTone, 0.55f - 0.4f * tone, stereo, nl, nr);
                    }
                    else
                    {
                        float x = x0;
                        if (revType == 2) x += shimFb_;
                        plate_.process(x, nl, nr, 0.3f + 0.66f * dec, plateDamp, plateBw);
                        if (revType == 2)
                        {   // shimmer: the reverb's output, an octave up, fed back in (gently, filtered)
                            float s = shimmer_.process(0.5f * (nl + nr));
                            s = s - shimHp_.lp(s);
                            s = shimLp_.lp(s);
                            shimFb_ = sclip(s * (0.25f + 0.35f * dec));
                        }
                    }
                    wl = 0.5f * (curWl_ + nl); wr = 0.5f * (curWr_ + nr);
                    curWl_ = nl; curWr_ = nr;
                }                const float dry = std::fmin(1.0f, 2.0f * (1.0f - rmix)), wet = std::fmin(1.0f, 2.0f * rmix);
                const float ol = l * dry + wl * wet, orr = r * dry + wr * wet;
                l += (ol - l) * vOn; r += (orr - r) * vOn;
            }
            if (!stereo) { const float m = 0.5f * (l + r); l = r = m; }

            // output gain, limiter, exact bypass
            const float go = gOut_.next(outT);
            const float pl = l * go, pr = r * go;
            const float peak = std::fmax(std::fabs(pl), std::fabs(pr));
            const float lg = limiter_.process(peak > kCeiling ? kCeiling / peak : 1.0f);
            grAcc_ = std::fmin(grAcc_, lg);
            dly_[0][dlyPos_] = pl; dly_[1][dlyPos_] = pr; dly_[2][dlyPos_] = rawL; dly_[3][dlyPos_] = rawR;
            const int rd = (dlyPos_ - kLookahead) & (kDelaySize - 1);
            dlyPos_ = (dlyPos_ + 1) & (kDelaySize - 1);
            const float wl = dly_[0][rd] * lg, wr = dly_[1][rd] * lg;
            const float bp = gBypass_.next(bypass ? 1.0f : 0.0f);
            const float ol = dly_[2][rd] * bp + wl * (1.0f - bp);
            const float orr = dly_[3][rd] * bp + wr * (1.0f - bp);
            outPeak_ = std::fmax(outPeak_, std::fmax(std::fabs(ol), std::fabs(orr)));
            outL[i] = std::isfinite(ol) ? ol : 0.0f;
            if (outR) outR[i] = std::isfinite(orr) ? orr : 0.0f;
            if (++blockPos_ >= blockLen_) { blockPos_ = 0; publish(); }
        }
        samplesNow_ += n;
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char buf[64];
        switch (index)
        {
            case kInLevel: case kOutLevel:
            {
                const float v = index == kInLevel ? dIn_.load() : dOut_.load();
                if (v < -90.0f) out = "-inf dB"; else { std::snprintf(buf, sizeof buf, "%.1f dB", (double)v); out = buf; }
                return true;
            }
            case kLimGr:   std::snprintf(buf, sizeof buf, "%.1f dB", (double)dGr_.load()); out = buf; return true;
            case kCompGr:
                if (getInt(kDriveType) >= 4) out = "fuzz: no GR";
                else { std::snprintf(buf, sizeof buf, "%.1f dB", (double)dCompGr_.load()); out = buf; }
                return true;
            case kRotorNow:
                if (getInt(kModMode) == 1) std::snprintf(buf, sizeof buf, "rotor %.2f Hz", (double)dRotor_.load());
                else std::snprintf(buf, sizeof buf, "lfo %.2f Hz", (double)rateNow_.load());
                out = buf; return true;
            case kDelayNow:
                std::snprintf(buf, sizeof buf, getInt(kSync) == 1 && tempoValid_ ? "%.0f ms (%s)" : "%.0f ms", (double)dDelayMs_.load(),
                              mpc::dsp::divisionName(getInt(kDiv)));
                out = buf; return true;
            case kRate:
            {
                const bool sync = getInt(kRateMode) == 1;
                if (rateHint_.load()) { out = sync ? "hold -> Hz" : "hold -> Sync"; return true; }
                if (sync && tempoValid_) { out = std::string("Sync ") + mpc::dsp::divisionName(rateDiv(getNormalised(kRate))); return true; }
                std::snprintf(buf, sizeof buf, sync ? "no tempo: %.2f Hz" : "Hz %.2f", (double)rateFree(getNormalised(kRate)));
                out = buf; return true;
            }
            case kTime:
            {
                const bool sync = getInt(kSync) == 1;
                if (timeHint_.load()) { out = sync ? "hold -> ms" : "hold -> Sync"; return true; }
                if (sync && tempoValid_) { out = std::string("Sync ") + mpc::dsp::divisionName(timeDiv(getNormalised(kTime))); return true; }
                std::snprintf(buf, sizeof buf, sync ? "no tempo: %.0f ms" : "ms %.0f", (double)timeFree(getNormalised(kTime)));
                out = buf; return true;
            }
            case kRandStatus:  out = statusText(); return true;
            case kPresetName:  out = nameText(); return true;
            case kBlend:
            {
                const float b = plain * 0.01f;
                const int a = (int)std::lround(100.0f * std::fmin(1.0f, 2.0f * (1.0f - b))), c = (int)std::lround(100.0f * std::fmin(1.0f, 2.0f * b));
                const bool rot = getInt(kModMode) == 1;
                if (c == 0) out = rot ? "rotary" : "tremolo";
                else if (a == 0) out = rot ? "phaser" : "vibrato";
                else { std::snprintf(buf, sizeof buf, rot ? "rot %d / pha %d" : "trem %d / vib %d", a, c); out = buf; }
                return true;
            }
            case kBias:
                if (getInt(kDriveType) < 4)
                {
                    if (plain < 1.0f) out = "gate off";
                    else { std::snprintf(buf, sizeof buf, "gate %.0f dB", (double)(-84.0f + 54.0f * plain * 0.01f)); out = buf; }
                }
                else { std::snprintf(buf, sizeof buf, "starve %.0f %%", (double)plain); out = buf; }
                return true;
            case kAmount:
                std::snprintf(buf, sizeof buf, getInt(kDriveType) < 4 ? "comp %.0f %%" : "gain %.0f %%", (double)plain);
                out = buf; return true;
            default: break;
        }
        const mpc::Param& p = param(index);
        if (p.kind == mpc::Param::Kind::Float && !p.unit.empty())
        {
            std::snprintf(buf, sizeof buf, "%.*f %s", p.decimals, (double)plain, p.unit.c_str());
            out = (std::strncmp(buf, "-0 ", 3) == 0 || std::strncmp(buf, "-0.0 ", 5) == 0) ? buf + 1 : buf;
            if (p.unit == "dB" && plain > 0.04f) out = "+" + out;
            return true;
        }
        return false;
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "Hydra Eel";
        info.vendor = "sureeel";      // must match plugin.json "manufacturer"
        info.uniqueId = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);
        info.version = 1000;
        info.isInstrument = false;
        info.numInputs = 2;
        info.numOutputs = 2;
        info.latencySamples = kLookahead;
        info.tailSamples = (int)(44100 * 4);
        return info;
    }
    static constexpr char kUid[5] = "SpPe";
    static void putF32(std::vector<uint8_t>& o, float f) { uint8_t b[4]; std::memcpy(b, &f, 4); o.insert(o.end(), b, b + 4); }

    // ---- Hz / ms <-> Sync on the Rate and Time knobs ------------------------------------------------
    // Free: Rate 0.1-12 Hz (log), Time 20-2000 ms (log). Sync: the knob runs through the tempo
    // divisions (1/64 .. 2/1, dotted and triplets): Rate from slow (bottom) to fast (top), Time from
    // short (bottom) to long (top). Without a tempo from the host, Sync falls back to the free scale
    // and says so. The flip: hold the knob at its end for 0.7 s after moving it there (a touch screen
    // has no hard stop): free flips to Sync at the bottom, Sync flips back to free at the top.
    static constexpr int kDivs = mpc::dsp::kNumDivisions;
    static float rateFree(float n) { return 0.1f * std::pow(120.0f, clamp(n, 0.0f, 1.0f)); }
    static int rateDiv(float n) { return (kDivs - 1) - (int)std::lround(clamp(n, 0.0f, 1.0f) * (kDivs - 1)); }
    static float timeFree(float n) { return 20.0f * std::pow(100.0f, clamp(n, 0.0f, 1.0f)); }
    static int timeDiv(float n) { return (int)std::lround(clamp(n, 0.0f, 1.0f) * (kDivs - 1)); }
    float divHz(int i) const { return (float)(bpm_ / 60.0) / mpc::dsp::divisionBeats(i); }
    float divMs(int i) const { return 1000.0f * mpc::dsp::beatsToSeconds(mpc::dsp::divisionBeats(i), bpm_); }
    float rateHz() const
    {
        const float n = getNormalised(kRate);
        return getInt(kRateMode) == 1 && tempoValid_ ? clamp(divHz(rateDiv(n)), 0.02f, 40.0f) : rateFree(n);
    }
    float delayMsNow() const
    {
        const float n = getNormalised(kTime);
        return getInt(kSync) == 1 && tempoValid_ ? divMs(timeDiv(n)) : timeFree(n);
    }
    struct Flip { float last = -1.0f; bool armed = false; int64_t since = 0; };
    void disarm(Flip& f, float n) { f.last = n; f.armed = false; }
    // returns true when the knob has been held in its flip zone long enough
    bool watch(Flip& f, float n, bool sync, std::atomic<bool>& hint)
    {
        if (n != f.last) { f.armed = f.last >= 0.0f; f.last = n; f.since = samplesNow_; }
        const bool zone = f.armed && (sync ? n >= 0.985f : n <= 0.015f);
        if (zone != hint.load()) { hint.store(zone); requestDisplayUpdate(); }
        return zone && samplesNow_ - f.since >= (int64_t)(0.7 * sr_);
    }
    void moveKnob(int idx, float n) { setNormalised(idx, clamp(n, 0.03f, 0.97f)); notifyHostParameterChanged(idx); }
    void handleFlips()
    {
        if (tempoValid_ != lastTempoValid_) { lastTempoValid_ = tempoValid_; requestDisplayUpdate(); }
        // mod rate
        const bool rSync = getInt(kRateMode) == 1;
        const float rn = getNormalised(kRate);
        if (rSync != lastRateSync_)
        {   // the mode changed (selector, preset, flip): keep the speed as close as possible
            if (lastRateSync_ >= 0) convertRate(rSync, rn);
            lastRateSync_ = rSync; disarm(rateFlip_, getNormalised(kRate)); requestDisplayUpdate();
        }
        else if (watch(rateFlip_, rn, rSync, rateHint_))
        {
            setNormalised(kRateMode, rSync ? 0.0f : 1.0f); notifyHostParameterChanged(kRateMode);
            convertRate(!rSync, rn); lastRateSync_ = !rSync;
            disarm(rateFlip_, getNormalised(kRate)); rateHint_.store(false); requestDisplayUpdate();
        }
        // delay time (the Sync selector is the same mode)
        const bool tSync = getInt(kSync) == 1;
        const float tn = getNormalised(kTime);
        if (tSync != lastTimeSync_)
        {
            if (lastTimeSync_ >= 0) convertTime(tSync, tn);
            lastTimeSync_ = tSync; disarm(timeFlip_, getNormalised(kTime)); requestDisplayUpdate();
        }
        else if (watch(timeFlip_, tn, tSync, timeHint_))
        {
            set(kSync, tSync ? 0.0f : 1.0f); notifyHostParameterChanged(kSync);
            convertTime(!tSync, tn); lastTimeSync_ = !tSync;
            disarm(timeFlip_, getNormalised(kTime)); timeHint_.store(false); requestDisplayUpdate();
        }
        if (getInt(kSync) == 1)
        {   // Division and the Time knob stay in step: whichever moved, the other follows
            const int dt = timeDiv(getNormalised(kTime)), dp = getInt(kDiv);
            if (dp != lastDiv_ && dt == lastTimeDiv_) { setNormalised(kTime, (float)dp / (float)(kDivs - 1)); notifyHostParameterChanged(kTime); disarm(timeFlip_, getNormalised(kTime)); }
            else if (dt != dp) { set(kDiv, (float)dt); notifyHostParameterChanged(kDiv); }
        }
        lastDiv_ = getInt(kDiv); lastTimeDiv_ = timeDiv(getNormalised(kTime));
    }
    // to Sync: the division nearest the current speed; to free: that division's speed
    void convertRate(bool toSync, float n)
    {
        if (toSync)
        {
            const float hz = rateFree(n);
            int best = 0; float bd = 1e9f;
            for (int i = 0; i < kDivs; ++i) { const float dd = std::fabs(std::log(divHz(i) / hz)); if (dd < bd) { bd = dd; best = i; } }
            moveKnob(kRate, (float)((kDivs - 1) - best) / (float)(kDivs - 1));
        }
        else moveKnob(kRate, std::log(clamp(divHz(rateDiv(n)), 0.1f, 12.0f) / 0.1f) / std::log(120.0f));
    }
    void convertTime(bool toSync, float n)
    {
        if (toSync)
        {
            const float ms = timeFree(n);
            int best = 0; float bd = 1e9f;
            for (int i = 0; i < kDivs; ++i) { const float dd = std::fabs(std::log(divMs(i) / ms)); if (dd < bd) { bd = dd; best = i; } }
            moveKnob(kTime, (float)best / (float)(kDivs - 1));
            set(kDiv, (float)best); notifyHostParameterChanged(kDiv);
        }
        else moveKnob(kTime, std::log(clamp(divMs(timeDiv(n)), 20.0f, 2000.0f) / 20.0f) / std::log(100.0f));
    }

    // ---- buttons ------------------------------------------------------------------------------
    // Touch-screen buttons are on/off switches: each tap flips the switch and the plugin acts on
    // each flip (it never writes the switch). A second flip within 0.35 s is ignored, so a
    // footswitch's press + release, or a Q-Link wobbling around halfway, counts once.
    bool acted(int idx)
    {
        const bool now = getBool(idx);
        if (now == btnLast_[idx]) return false;
        btnLast_[idx] = now;
        if (samplesNow_ - btnTime_[idx] < (int64_t)(0.35 * sr_)) return false;
        btnTime_[idx] = samplesNow_;
        return true;
    }

    void setAndTell(int p, float plain) { set(p, plain); notifyHostParameterChanged(p); }

    void handleButtons()
    {
        if (getInt(kHelp2) != getInt(kHelp)) { set(kHelp2, (float)getInt(kHelp)); notifyHostParameterChanged(kHelp2); }
        if (getInt(kSync) != lastSync_ || getInt(kModMode) != lastMode_ || (getInt(kDriveType) < 4) != lastComp_)
        {   // texts that depend on another setting
            lastSync_ = getInt(kSync); lastMode_ = getInt(kModMode); lastComp_ = getInt(kDriveType) < 4;
            requestDisplayUpdate();
        }
        if (acted(kRandomize)) { randomize(); requestDisplayUpdate(); }
        if (acted(kUndo)) { undo(); requestDisplayUpdate(); }
        if (acted(kSave)) { if (naming_) confirmName(); else startNaming(); }
        if (acted(kConfirm)) { if (naming_) confirmName(); else startNaming(); }
        if (acted(kCancel) && naming_) { naming_ = false; nameMsg_ = kNmCancelled; nameChanged_ = true; }
        if (getInt(kNamePos) != lastPos_)
        {
            lastPos_ = getInt(kNamePos);
            cursor_ = clamp(lastPos_, 0, kNameLen - 1);
            if (naming_) { lastLetter_ = charIndex(name_[cursor_]); setAndTell(kLetter, (float)lastLetter_); }
            nameChanged_ = true;
        }
        if (getInt(kLetter) != lastLetter_)
        {
            lastLetter_ = getInt(kLetter);
            if (naming_)
            {
                for (int i = 0; i < cursor_; ++i) if (name_[i] == 0) name_[i] = ' ';
                name_[cursor_] = kCharset[clamp(lastLetter_, 0, kNumChars - 1)];
                nameChanged_ = true;
            }
        }
        const int done = saveDone_.exchange(-1, std::memory_order_acq_rel);
        if (done >= 0) { status_ = kStSaved; statusChanged_ = true; nameMsg_ = kNmSaved; nameChanged_ = true; requestDisplayUpdate(); }
    }

    // ---- randomizer -----------------------------------------------------------------------------
    void randomize()
    {
        const float amount = clamp(get(kRandAmount) * 0.01f, 0.0f, 1.0f);
        for (int i = 0; i < kNumParams; ++i) undoVal_[i] = getNormalised(i);
        int changed = 0;
        for (const RandSpec& s : kRandSpecs)
        {
            const float cur = get(s.param);
            float v = cur;
            if (s.kind == 2) { if (rng_.nextFloat() < amount) v = (float)clamp((int)(s.lo + rng_.nextFloat() * (s.hi - s.lo + 1.0f)), (int)s.lo, (int)s.hi); }
            else if (s.kind == 1)
            {
                const float t = s.lo * std::pow(s.hi / s.lo, rng_.nextFloat());
                v = std::exp(std::log(std::fmax(cur, 1e-3f)) + (std::log(t) - std::log(std::fmax(cur, 1e-3f))) * amount);
            }
            else { const float t = s.lo + (s.hi - s.lo) * rng_.nextFloat(); v = cur + (t - cur) * amount; }
            if (amount > 0.0f && std::fabs(v - cur) > 1e-5f * std::fabs(s.hi - s.lo)) { setAndTell(s.param, v); ++changed; }
        }
        haveUndo_ = true; randomized_ = true;
        disarm(rateFlip_, getNormalised(kRate)); disarm(timeFlip_, getNormalised(kTime));
        status_ = kStRandom; statusCount_ = changed; statusChanged_ = true;
    }
    void undo()
    {
        if (!haveUndo_) { status_ = kStNoUndo; statusChanged_ = true; return; }
        for (const RandSpec& s : kRandSpecs)
            if (getNormalised(s.param) != undoVal_[s.param]) { setNormalised(s.param, undoVal_[s.param]); notifyHostParameterChanged(s.param); }
        haveUndo_ = false;
        disarm(rateFlip_, getNormalised(kRate)); disarm(timeFlip_, getNormalised(kTime));
        status_ = kStUndone; statusChanged_ = true;
    }
    std::string statusText() const
    {
        char buf[64];
        switch (statusDisp_.load())
        {
            case kStRandom: std::snprintf(buf, sizeof buf, "Randomized %d settings", countDisp_.load()); break;
            case kStUndone: return "Undone";
            case kStNoUndo: return "Nothing to undo";
            case kStSaving: return "Saving...";
            case kStSaved:
            {
                std::string s = "Saved: ";
                for (int i = 0; i < kNameLen && savedNameDisp_[i].load(); ++i) s += savedNameDisp_[i].load();
                return s;
            }
            default: return "Ready";
        }
        return buf;
    }

    // ---- preset naming and saving (same design as Sleepy Android) ------------------------------
    static int charIndex(char c) { for (int i = 0; i < kNumChars; ++i) if (kCharset[i] == c) return i; return 0; }
    void autoName(char* out) const
    {
        char nm[kNameLen + 8];
        std::snprintf(nm, sizeof nm, randomized_ ? "Random %02d" : "User %02d", nextSlot_ + 1);
        std::memset(out, 0, kNameLen + 1);
        std::memcpy(out, nm, std::min((size_t)kNameLen, std::strlen(nm)));
    }
    void startNaming()
    {
        autoName(name_);
        naming_ = true;
        cursor_ = std::min((int)std::strlen(name_), kNameLen - 1);
        lastPos_ = cursor_; setAndTell(kNamePos, (float)cursor_);
        lastLetter_ = charIndex(name_[cursor_]); setAndTell(kLetter, (float)lastLetter_);
        nameMsg_ = kNmNaming; nameChanged_ = true;
    }
    void confirmName()
    {
        if (savePending_.load(std::memory_order_acquire) >= 0) return;
        for (int k = 0; k < kNumAudioParams; ++k) saveVals_[k] = get(k);
        saveMix_ = get(kDriveMix); saveRate_ = (float)getInt(kRateMode);
        int len = kNameLen;
        while (len > 0 && (name_[len - 1] == 0 || name_[len - 1] == ' ')) --len;
        for (int i = 0; i < kNameLen; ++i) saveName_[i] = i < len ? (name_[i] ? name_[i] : ' ') : 0;
        saveName_[kNameLen] = 0;
        if (len == 0) autoName(saveName_);
        const int slot = nextSlot_;
        nextSlot_ = (nextSlot_ + 1) % kNumUser;
        savePending_.store(slot, std::memory_order_release);
        randomized_ = false; naming_ = false;
        nameMsg_ = kNmSaving; nameChanged_ = true; status_ = kStSaving; statusChanged_ = true;
    }
    std::string nameText() const
    {
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
                if (i == c) { s += '['; s += ch == ' ' ? '_' : ch; s += ']'; } else s += ch;
            }
            return s;
        }
        if (msg == kNmSaved) { std::string s = "Saved: "; for (int i = 0; i < kNameLen && savedNameDisp_[i].load(); ++i) s += savedNameDisp_[i].load(); return s; }
        if (msg == kNmSaving) return "Saving...";
        if (msg == kNmCancelled) return "Not saved";
        return "Tap Save to store";
    }
    // file: "HEUP" | u8 3 | u8 next | kNumUser x { u8 used, char name[17], f32 x kNumAudioParams, f32 drive mix, f32 rate mode }  (v1: neither, v2: no rate mode)
    void loadUserPresets()
    {
        std::vector<uint8_t> d;
        if (!mpc::readFile(userFile_, d) || d.size() < 6 || std::memcmp(d.data(), "HEUP", 4) != 0 || (d[4] < 1 || d[4] > 3)) return;
        const int ver = d[4];
        const size_t rec = 1 + (kNameLen + 1) + 4 * (size_t)kNumAudioParams + (ver >= 2 ? 4 : 0) + (ver >= 3 ? 4 : 0);
        if (d.size() < 6 + rec * kNumUser) return;
        nextSlot_ = d[5] % kNumUser;
        for (int u = 0; u < kNumUser; ++u)
        {
            const uint8_t* r = d.data() + 6 + rec * (size_t)u;
            userUsed_[u] = r[0];
            if (!r[0]) continue;
            char nm[kNameLen + 1] = {0};
            std::memcpy(nm, r + 1, kNameLen);
            for (char& c : nm) if (c != 0 && (c < 32 || c > 126)) c = '?';
            std::vector<float>& v = programs_[(size_t)(numFactory_ + u)].plainValues;
            for (int k = 0; k < kNumAudioParams; ++k) { float x; std::memcpy(&x, r + 1 + kNameLen + 1 + 4 * k, 4); if (std::isfinite(x)) v[(size_t)k] = x; }
            if (ver >= 2) { float x; std::memcpy(&x, r + 1 + kNameLen + 1 + 4 * kNumAudioParams, 4); if (std::isfinite(x)) userMix_[u] = clamp(x, 0.0f, 100.0f); }
            if (ver >= 3) { float x; std::memcpy(&x, r + 1 + kNameLen + 1 + 4 * kNumAudioParams + 4, 4); if (std::isfinite(x)) userRate_[u] = x > 0.5f ? 1.0f : 0.0f; }
            programs_[(size_t)(numFactory_ + u)].name.assign(nm[0] ? nm : "User");
        }
    }
    void writerLoop()
    {
        while (!quit_.load(std::memory_order_acquire))
        {
            const int u = savePending_.load(std::memory_order_acquire);
            if (u >= 0)
            {
                userUsed_[u] = 1;
                std::vector<float>& v = programs_[(size_t)(numFactory_ + u)].plainValues;
                for (int k = 0; k < kNumAudioParams; ++k) v[(size_t)k] = saveVals_[k];
                userMix_[u] = saveMix_; userRate_[u] = saveRate_;
                programs_[(size_t)(numFactory_ + u)].name.assign(saveName_);
                for (int i = 0; i <= kNameLen; ++i) savedNameDisp_[i].store(saveName_[i]);
                writeUserFile((u + 1) % kNumUser);
                savePending_.store(-1, std::memory_order_release);
                saveDone_.store(u, std::memory_order_release);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }
    void writeUserFile(int next)
    {
        std::vector<uint8_t> d = {'H', 'E', 'U', 'P', 3, (uint8_t)next};
        for (int u = 0; u < kNumUser; ++u)
        {
            d.push_back(userUsed_[u]);
            char nm[kNameLen + 1] = {0};
            std::strncpy(nm, programs_[(size_t)(numFactory_ + u)].name.c_str(), kNameLen);
            d.insert(d.end(), nm, nm + kNameLen + 1);
            for (int k = 0; k < kNumAudioParams; ++k) putF32(d, programs_[(size_t)(numFactory_ + u)].plainValues[(size_t)k]);
            putF32(d, userMix_[u]); putF32(d, userRate_[u]);
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

    // ---- displays, every 100 ms; the host is told only about values that changed ----------------
    void push(int idx, float norm) { if (std::fabs(norm - getNormalised(idx)) < 0.004f) return; setNormalised(idx, norm); notifyHostParameterChanged(idx); }
    void bump(int idx, int& serial) { setNormalised(idx, (float)((serial++ % 50) + 1) / 100.0f); notifyHostParameterChanged(idx); }
    void publish()
    {
        const float in = inPeak_ > 1e-5f ? linToDb(inPeak_) : -100.0f, out = outPeak_ > 1e-5f ? linToDb(outPeak_) : -100.0f;
        inPeak_ = outPeak_ = 0.0f;
        dIn_.store(in); dOut_.store(out);
        push(kInLevel, clamp((in + 48.0f) / 48.0f, 0.0f, 1.0f));
        push(kOutLevel, clamp((out + 48.0f) / 48.0f, 0.0f, 1.0f));
        const float gr = linToDb(grAcc_); grAcc_ = 1.0f; dGr_.store(gr);
        push(kLimGr, clamp(-gr / 12.0f, 0.0f, 1.0f));
        const float cgr = linToDb(drive_.takeGr()); dCompGr_.store(cgr);
        push(kCompGr, clamp(-cgr / 24.0f, 0.0f, 1.0f));
        dRotor_.store(mod_.hornHz);
        if (std::fabs(mod_.hornHz - lastRotor_) > 0.02f) { lastRotor_ = mod_.hornHz; bump(kRotorNow, serialR_); }
        dDelayMs_.store(delayMsNow_);
        if (std::fabs(delayMsNow_ - lastDelay_) > 0.5f) { lastDelay_ = delayMsNow_; bump(kDelayNow, serialD_); }
        if (statusChanged_)
        {
            statusChanged_ = false; statusDisp_.store(status_); countDisp_.store(statusCount_);
            bump(kRandStatus, serialS_);
        }
        if (nameChanged_)
        {
            nameChanged_ = false;
            for (int i = 0; i <= kNameLen; ++i) nameDisp_[i].store(name_[i]);
            nameCursorDisp_.store(cursor_); nameMsgDisp_.store(nameMsg_);
            bump(kPresetName, serialN_);
        }
    }

    // ---- members ----------------------------------------------------------------------------------
    double sr_ = 44100.0, bpm_ = 120.0;
    bool tempoValid_ = true, lastTempoValid_ = true;
    Flip rateFlip_, timeFlip_;
    int lastRateSync_ = -1, lastTimeSync_ = -1, lastDiv_ = -1, lastTimeDiv_ = -1;
    std::atomic<bool> rateHint_{false}, timeHint_{false};
    std::atomic<float> rateNow_{5.5f};
    Drive drive_;
    Mod mod_;
    Delay delay_;
    Plate plate_;
    Spring spring_;
    OctaveUp shimmer_;
    OnePole shimHp_, shimLp_;
    float shimFb_ = 0.0f, revIn_ = 0.0f, curWl_ = 0.0f, curWr_ = 0.0f;
    int revPhase_ = 0;
    PeakLimiter limiter_;
    float dly_[4][kDelaySize] = {};
    int dlyPos_ = 0;
    Ring driveDry_;
    Glide gDMixDrive_;
    std::vector<float> progMix_;
    float userMix_[16] = {}, userRate_[16] = {};
    Glide gIn_, gOut_, gLevel_, gBlend_, gDepth_, gFb_, gDMix_, gRMix_, gDecay_, gBypass_, gDriveOn_, gModOn_, gDelayOn_, gRevOn_;
    int lastType_ = -1, lastSync_ = -1, lastMode_ = -1;
    bool lastComp_ = false;
    float delayMsNow_ = 380.0f, inPeak_ = 0.0f, outPeak_ = 0.0f, grAcc_ = 1.0f, lastRotor_ = -1.0f, lastDelay_ = -1.0f;
    int blockLen_ = 4410, blockPos_ = 0, serialR_ = 0, serialD_ = 0, serialS_ = 0, serialN_ = 0;
    std::atomic<float> dIn_{-100.0f}, dOut_{-100.0f}, dGr_{0.0f}, dCompGr_{0.0f}, dRotor_{0.0f}, dDelayMs_{0.0f};

    bool btnLast_[kNumParams] = {};
    int64_t btnTime_[kNumParams] = {};
    int64_t samplesNow_ = 1 << 30;

    mpc::dsp::Xorshift32 rng_;
    float undoVal_[kNumParams] = {};
    bool haveUndo_ = false, randomized_ = false;
    enum Status { kStReady, kStRandom, kStUndone, kStNoUndo, kStSaving, kStSaved };
    int status_ = kStReady, statusCount_ = 0;
    bool statusChanged_ = true;
    std::atomic<int> statusDisp_{kStReady}, countDisp_{0};

    enum NameMsg { kNmIdle, kNmNaming, kNmSaving, kNmSaved, kNmCancelled };
    bool naming_ = false, nameChanged_ = true;
    char name_[kNameLen + 1] = {};
    int cursor_ = 0, lastPos_ = -1, lastLetter_ = -1, nameMsg_ = kNmIdle;
    std::atomic<char> nameDisp_[kNameLen + 1] = {}, savedNameDisp_[kNameLen + 1] = {};
    std::atomic<int> nameCursorDisp_{0}, nameMsgDisp_{kNmIdle};

    static constexpr int kNumUser = 16;
    int numFactory_ = 0, nextSlot_ = 0;
    uint8_t userUsed_[kNumUser] = {};
    float saveVals_[kNumAudioParams] = {}, saveMix_ = 100.0f, saveRate_ = 0.0f;
    char saveName_[kNameLen + 1] = {};
    std::string userFile_;
    std::atomic<int> savePending_{-1}, saveDone_{-1};
    std::atomic<bool> quit_{false};
    std::thread worker_;
};

} // namespace

MPC_PLUGIN_ENTRY(HydraEel)
