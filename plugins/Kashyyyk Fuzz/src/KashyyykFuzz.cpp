// Kashyyyk Fuzz - a modular fuzz pedal for MPC / Force, by Sure Eel (manufacturer "sureeel").
//
// 0.3: six circuit families (Face, Bender, Muff, Op-Amp, Octave, Dual) of 1 - 3 cascaded stages, Sag (a starved
// battery), Feedback (self-oscillation, behind a safety limiter), coupling caps, resistor types, separate Q1 / Q2
// transistors, a Buffer option and 29 factory presets. Level matching is measured per combination on a background
// thread.
//
// SIGNAL FLOW. Stereo in, MONO effect: the fuzz is one engine fed (L + R) / 2; the dry signal keeps its stereo and is
// delayed by the engine's latency, so Blend never combs.
//
//   L,R ──┬──────────────────────────────── dry (stereo, delayed 35 samples) ──────────────┐
//         └─ Input ─ (L+R)/2 ─ guitar / pot / cable / input cap ─ 4x ─ Fuzz Face stage ─      ├─ Blend ─ Output
//                    (exact network)                                (tabulated Ebers-Moll)   │
//                    ─ part roll-off ─ diode clipper ─ 4x down ─ coupling ─ tone stack ─ trim ─ Level ─┘
//
// The clipping is never bypassed: the transistor stage always clips; the diode slot adds a second clipper (or None).
// Analog models: kf_circuit.h (exact solves), tabulated offline by gen/gen_tables.cpp into kf_tables.inc;
// the engine: kf_dsp.h.
//
// GLOBAL (never changed by presets, Randomize or Undo; saved with the project): Output, Input, Guitar Vol, Vol Follow
// and its learned full-volume reference.
//
// VOL FOLLOW (Auto): the input meter stands in for the guitar's volume knob. It follows the peaks of note attacks
// (not the decay of a note), slowly (about 0.3 s), relative to the full-volume level that LEARN measured, and drives
// the same impedance model as the Guitar Vol knob - tone and clean-up only, since the real pot already lowered the
// level. The knob then sets the lowest position the follower can reach.
//
// PARAMETERS (ids are permanent; new ones are appended at the end)
//   0 Output  1 Input  2 Fuzz  3 Tone  4 Level  5 Blend  6 Guitar Vol  7 Vol Follow  8 Learn (button)
//   9 Transistors  10 Diodes  11 Diode Arr  12 Input Cap  13 Tone Stack
//   14 Next Q  15 Next D  16 Next C (tap-to-step buttons on the pedal graphic)
//   17 CPU  18 Status  19 Vol Now  20 Panel (displays)
//   ---- 0.2 ----
//   21 Random  22 Undo  23 Save  24 Cancel (buttons)  25 Name Pos  26 Letter (the name editor)
//   27 Name  28 Patch  29 Board D (displays: the name being edited, Random / Undo / Save status, the diode picture)
//   ---- 0.3 ----
//   30 Sag  31 Stages  32 Topology  33 Feedback  34 Dual Mix  35 Coupling  36 Resistors  37 Q2 (later stages' transistor)
//   38 Buffer (global)  39 Circuit (display: family, stages, battery voltage, level-match state)
//
// RANDOM makes a new sound: transistors, diodes and their arrangement, input cap, tone stack, Fuzz and Tone, in safe
// ranges (the arrangement is mostly Pair, rarely None). Never Input, Output, Guitar Vol or Vol Follow. UNDO puts back the
// sound from before the last Random.
// SAVE: the first tap starts a name ("Random 01" after a Random, else "User 01"); Name Pos and Letter edit it; the
// second tap stores it in the next of 16 user slots, listed after the factory preset in the MPC's preset list and
// kept across restarts in ".sureeel Kashyyyk Fuzz user presets.bin" next to the plugin folder (written by a
// background thread: no file work in the audio thread). CANCEL leaves the name editor.

#include "mpc_plugin.h"
#include "kf_tables_loader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

enum ParamIndex
{
    kOutput = 0, kInput, kFuzz, kTone, kLevel, kBlend, kGuitarVol, kVolFollow, kLearn,
    kTransistors, kDiodes, kDiodeArr, kInputCap, kToneStack,
    kNextQ, kNextD, kNextC,
    kCpu, kStatus, kVolNow, kPanel,
    // ---- 0.2 ----
    kRandom, kUndo, kSave, kCancel, kNamePos, kLetter, kName, kPatch, kBoardD,
    // ---- 0.3 ----
    kSag, kStages, kTopology, kFeedback, kDualMix, kCoupling, kResistors, kQ2, kBuffer, kCircuit,
    kNumParams
};
constexpr int kNameLen = 16, kNumUser = 16;
const char kCharset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-+";
constexpr int kNumChars = (int)sizeof(kCharset) - 1;
// the sound: what a preset, Random and Undo touch
const int kSoundParams[] = {kFuzz, kTone, kLevel, kBlend, kTransistors, kDiodes, kDiodeArr, kInputCap, kToneStack,
                            kSag, kStages, kTopology, kFeedback, kDualMix, kCoupling, kResistors, kQ2};
const char* const kTopologyNames[kf::kNumTopologies] = {"Face", "Bender", "Muff", "Op-Amp", "Octave", "Dual"};
void putF32(std::vector<uint8_t>& d, float v) { uint8_t b[4]; std::memcpy(b, &v, 4); d.insert(d.end(), b, b + 4); }

const kf::Tables& tables() { return kf::loadTables(); }

// the level-match key: every choice that changes the level (the knobs are measured at fixed settings)
uint64_t levelKey(const kf::Settings& s)
{   // 11 choices x 3 bits (33 bits: a 64-bit key), plus a top bit so a key is never 0
    uint64_t k = 0;
    for (int v : {s.topo, s.stages - 1, s.tr, s.q2(), s.di, s.arr, s.cap, s.tone, s.coupling, s.resistors, s.buffer ? 1 : 0}) k = k * 8u + (uint64_t)v;
    return k | (1ull << 63);
}
kf::Settings fromKey(uint64_t k)
{
    int v[11];
    for (int i = 10; i >= 0; --i) { v[i] = (int)(k & 7u); k >>= 3; }
    kf::Settings s;
    s.topo = v[0]; s.stages = v[1] + 1; s.tr = v[2]; s.tr2 = v[3]; s.di = v[4]; s.arr = v[5]; s.cap = v[6]; s.tone = v[7];
    s.coupling = v[8]; s.resistors = v[9]; s.buffer = v[10] != 0;
    return s;
}

float dbToGain(float dB) { return std::fabs(dB) < 0.005f ? 1.0f : std::pow(10.0f, dB / 20.0f); }
inline float sane(float x) { return std::isfinite(x) ? x : 0.0f; }

constexpr int kDelay = kf::Oversampler4x::kLatency;
constexpr float kDefaultRefDb = -14.0f;

} // namespace

class KashyyykFuzz : public mpc::Plugin
{
public:
    KashyyykFuzz() : mpc::Plugin(makeInfo())
    {
        tables();
        addParam(mpc::floatParam("output", "Output", -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("input", "Input", -24.0f, 24.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("fuzz", "Fuzz", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(mpc::floatParam("tone", "Tone", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::floatParam("level", "Level", -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("blend", "Blend", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::floatParam("guitar_vol", "Guitar Vol", 0.0f, 10.0f, 10.0f, "", 1));
        addParam(mpc::choiceParam("vol_follow", "Vol Follow", {"Off", "Auto"}, 0));
        addButton("learn", "Learn");
        { std::vector<std::string> n; for (const auto& p : kf::kTransistors) n.push_back(p.name); addParam(mpc::choiceParam("transistors", "Transistors", n, 0)); }
        { std::vector<std::string> n; for (const auto& p : kf::kDiodes) n.push_back(p.name); addParam(mpc::choiceParam("diodes", "Diodes", n, 0)); }
        addParam(mpc::choiceParam("diode_arr", "Diode Arr", {"Pair", "Single", "2+1 Asym", "None"}, 0));   // Pair: the diodes are in the circuit
        { std::vector<std::string> n; for (const char* c : kf::kInputCapNames) n.push_back(c); addParam(mpc::choiceParam("input_cap", "Input Cap", n, 3)); }
        addParam(mpc::choiceParam("tone_stack", "Tone Stack", {"Muff", "Flat", "Mid Boost", "Tilt", "Bypass"}, 3));
        addButton("next_q", "Next Q");
        addButton("next_d", "Next D");
        addButton("next_c", "Next C");
        for (const char* const* d : {kDispCpu, kDispStatus, kDispVol, kDispPanel}) addDisplay(d[0], d[1]);
        // ---- 0.2 ----
        addButton("random", "Random");
        addButton("undo", "Undo");
        addButton("save", "Save");
        addButton("cancel", "Cancel");
        {
            std::vector<std::string> pos, letters;
            for (int i = 1; i <= kNameLen; ++i) pos.push_back(std::to_string(i));
            for (int i = 0; i < kNumChars; ++i) letters.push_back(kCharset[i] == ' ' ? std::string("space") : std::string(1, kCharset[i]));
            mpc::Param a = mpc::choiceParam("name_pos", "Name Pos", pos, 0), b = mpc::choiceParam("letter", "Letter", letters, 0);
            a.automatable = b.automatable = false;
            addParam(a); addParam(b);
        }
        addDisplay("name", "Name");
        addDisplay("patch", "Patch");
        addDisplay("board_d", "Board D");
        // ---- 0.3 ----
        addParam(mpc::floatParam("sag", "Sag", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::choiceParam("stages", "Stages", {"1", "2", "3"}, 0));
        { std::vector<std::string> n(kTopologyNames, kTopologyNames + kf::kNumTopologies); addParam(mpc::choiceParam("topology", "Topology", n, 0)); }
        addParam(mpc::floatParam("feedback", "Feedback", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("dual_mix", "Dual Mix", 0.0f, 100.0f, 50.0f, "%", 0));
        { std::vector<std::string> n(kf::kCouplingNames, kf::kCouplingNames + kf::kNumCouplings); addParam(mpc::choiceParam("coupling", "Coupling", n, 4)); }
        addParam(mpc::choiceParam("resistors", "Resistors", {"Metal Film", "Carbon Comp"}, 0));
        { std::vector<std::string> n = {"Same"}; for (const auto& p : kf::kTransistors) n.push_back(p.name); addParam(mpc::choiceParam("q2", "Q2", n, 0)); }
        addParam(mpc::choiceParam("buffer", "Buffer", {"Off", "On"}, 0));
        addDisplay("circuit", "Circuit");
        static_assert(kCircuit + 1 == kNumParams, "parameter list");
        // the factory presets: by family, named by sound ("inspired by"; the Death By Audio models follow documented
        // behaviour, as no schematics are published)
        std::vector<float> defaults; for (int i = 0; i < numParams(); ++i) defaults.push_back(param(i).def);
        addProgram("Germanium Face", defaults);
        addFactoryPresets(defaults);
        numFactory_ = (int)numPrograms();
        for (int u = 0; u < kNumUser; ++u)
        {
            char nm[16]; std::snprintf(nm, sizeof nm, "Empty %02d", u + 1);
            addProgram(nm, defaults);
        }
        const std::string& pf = mpc::pluginFolder();
        if (pf.find(" - VST - ") != std::string::npos) userFile_ = pf.substr(0, pf.find_last_of('/')) + "/.sureeel Kashyyyk Fuzz user presets.bin";
        else userFile_ = (pf.empty() ? std::string(".") : pf) + "/user-presets.bin";      // development builds
        loadUserPresets();
        rng_ = (uint32_t)(reinterpret_cast<uintptr_t>(this) * 2654435761u) | 1u;
        worker_ = std::thread([this] { writerLoop(); });
    }
    ~KashyyykFuzz() override { quit_.store(true); if (worker_.joinable()) worker_.join(); }

    // Only the sound belongs to a preset: never the global levels, the guitar-volume settings, buttons or displays.
    bool programSetsParam(int index) const override
    {
        for (int k : kSoundParams) if (k == index) return true;
        return false;
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        fs_ = sampleRate > 1.0 ? sampleRate : 44100.0;
        fsW_.store(fs_);
        core_.prepare(fs_, &tables());
        {   // the level match for the current sound now (prepare is not realtime); later changes go to the worker
            const kf::Settings s = settings();
            levelVal_.store(kf::calibrate(&tables(), fs_, s));
            levelDone_.store(levelKey(s), std::memory_order_release);
            levelWant_.store(levelKey(s));
        }
        envFastA_ = coef(0.5); envFastR_ = coef(20.0); envSlowA_ = coef(30.0); envSlowR_ = coef(300.0);
        publishEvery_ = (int)(fs_ / 10.0);
        reset();
    }

    void reset() override
    {
        core_.reset();
        std::memset(dryL_, 0, sizeof dryL_); std::memset(dryR_, 0, sizeof dryR_); dpos_ = 0;
        gIn_ = dbToGain(get(kInput)); gOut_ = dbToGain(get(kOutput)) ; gWet_ = dbToGain(get(kLevel)); blend_ = get(kBlend) * 0.01f;
        fuzzSm_ = get(kFuzz) * 0.01f; toneSm_ = get(kTone) * 0.01f; volSm_ = get(kGuitarVol) * 0.1f;
        envFast_ = envSlow_ = 0.0f; capture_ = 0; holdoff_ = 0; peak_ = 0.0f; playDb_ = refDb_.load();
        learning_ = 0; learnN_ = 0;
        for (int b : {kLearn, kNextQ, kNextD, kNextC, kRandom, kUndo, kSave, kCancel}) btnLast_[b] = getBool(b);
        lastPos_ = getInt(kNamePos); lastLetter_ = getInt(kLetter);
        patchChanged_ = nameChanged_ = true; lastBoardD_ = -1;
        sinceShow_ = 0; cpuAcc_ = 0.0; cpuFrames_ = 0;
        lastStatusKey_ = lastVolKey_ = lastCpuKey_ = lastCircuitKey_ = -1;
        sagSm_ = get(kSag) * 0.01f; fbSm_ = get(kFeedback) * 0.01f; mixSm_ = get(kDualMix) * 0.01f;
        core_.update(settings());
        if (levelDone_.load(std::memory_order_acquire) == levelKey(settings())) core_.trim = core_.trimTarget = levelVal_.load();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const auto t0 = std::chrono::steady_clock::now();
        const int n = ctx.numFrames;
        const int numCh = mpc::clamp(ctx.numOutputs, 0, 2);

        // ----- buttons: act on every change ---------------------------------------------------------------------
        if (acted(kNextQ)) step(kTransistors, kf::kNumTransistors);
        if (acted(kNextD)) step(kDiodes, kf::kNumDiodes);
        if (acted(kNextC)) step(kInputCap, kf::kNumInputCaps);
        if (acted(kLearn)) { learning_ = (int)(4.0 * fs_); learnN_ = 0; }
        if (acted(kRandom)) randomize();
        if (acted(kUndo)) undo();
        if (acted(kSave)) { if (naming_) confirmName(); else startNaming(); }
        if (acted(kCancel) && naming_) { naming_ = false; nameMsg_ = kNmCancelled; nameChanged_ = true; }
        if (getInt(kNamePos) != lastPos_)
        {
            lastPos_ = getInt(kNamePos);
            cursor_ = mpc::clamp(lastPos_, 0, kNameLen - 1);
            if (naming_) { lastLetter_ = charIndex(name_[cursor_]); tell(kLetter, (float)lastLetter_); nameChanged_ = true; }
        }
        if (getInt(kLetter) != lastLetter_)
        {
            lastLetter_ = getInt(kLetter);
            if (naming_)
            {
                for (int i = 0; i < cursor_; ++i) if (name_[i] == 0) name_[i] = ' ';
                name_[cursor_] = kCharset[mpc::clamp(lastLetter_, 0, kNumChars - 1)];
                nameChanged_ = true;
            }
        }
        {   // a save finished: ask the MPC to re-read the preset names (it reads them only at load otherwise), as in
            // Hydra Eel, Sleepy Android and Elver
            const int sd = saveDone_.exchange(-1);
            if (sd >= 0) { nameMsg_ = kNmSaved; nameChanged_ = true; patch_ = kPtSaved; patchChanged_ = true; requestDisplayUpdate(); }
        }

        // ----- settings, smoothed per block (about 30 ms) --------------------------------------------------------
        const float k = 1.0f - std::exp(-(float)n / (0.03f * (float)fs_));
        fuzzSm_ += (get(kFuzz) * 0.01f - fuzzSm_) * k;
        toneSm_ += (get(kTone) * 0.01f - toneSm_) * k;
        sagSm_ += (get(kSag) * 0.01f - sagSm_) * k;
        fbSm_ += (get(kFeedback) * 0.01f - fbSm_) * k;
        mixSm_ += (get(kDualMix) * 0.01f - mixSm_) * k;
        const bool autoVol = getInt(kVolFollow) == 1;
        float volTarget = get(kGuitarVol) * 0.1f;
        if (autoVol) volTarget = std::max(volTarget, followVol());           // the knob is the lowest it can go
        const float kv = 1.0f - std::exp(-(float)n / ((autoVol ? 0.3f : 0.03f) * (float)fs_));
        volSm_ += (volTarget - volSm_) * kv;
        kf::Settings s = settings();
        core_.update(s);
        {   // level match: ask the worker for this combination; take its answer when it is for this combination
            const uint64_t key = levelKey(s);
            if (levelWant_.load(std::memory_order_relaxed) != key)
            {   // a new part or circuit: start at once from the offline estimate (within a few dB), the worker refines it
                levelWant_.store(key, std::memory_order_release);
                core_.trim = core_.trimTarget = kf::levelEstimate(&tables(), s);
            }
            if (levelDone_.load(std::memory_order_acquire) == key) { core_.trimTarget = levelVal_.load(); leveled_ = true; }
            else leveled_ = false;
        }

        // ----- gains: linear ramps across the block -------------------------------------------------------------
        const float gInT = dbToGain(get(kInput)), gOutT = dbToGain(get(kOutput)), gWetT = dbToGain(get(kLevel)), blT = get(kBlend) * 0.01f;
        const float inv = n > 0 ? 1.0f / (float)n : 0.0f;
        const float dIn = (gInT - gIn_) * inv, dOut = (gOutT - gOut_) * inv, dWet = (gWetT - gWet_) * inv, dBl = (blT - blend_) * inv;

        const float* inL = (ctx.inputs != nullptr && ctx.numInputs > 0) ? ctx.inputs[0] : nullptr;
        const float* inR = (ctx.inputs != nullptr && ctx.numInputs > 1 && ctx.inputs[1] != nullptr) ? ctx.inputs[1] : inL;
        bool bad = false;
        for (int i = 0; i < n; ++i)
        {
            const float l = inL ? sane(inL[i]) : 0.0f, r = inR ? sane(inR[i]) : l;
            gIn_ += dIn; gOut_ += dOut; gWet_ += dWet; blend_ += dBl;
            const float x = 0.5f * (l + r) * gIn_;                         // the mono feed of the fuzz
            meter(x);
            float wet = core_.process(x) * gWet_;
            if (!std::isfinite(wet)) { wet = 0.0f; bad = true; }
            // the dry path, delayed by the engine's latency so Blend never combs
            const float dl = dryL_[dpos_], dr = dryR_[dpos_];
            dryL_[dpos_] = l * gIn_; dryR_[dpos_] = r * gIn_;
            dpos_ = dpos_ + 1 < kDelay ? dpos_ + 1 : 0;
            const float b = blend_;
            if (numCh > 0) ctx.outputs[0][i] = (dl * (1.0f - b) + wet * b) * gOut_;
            if (numCh > 1) ctx.outputs[1][i] = (dr * (1.0f - b) + wet * b) * gOut_;
        }
        gIn_ = gInT; gOut_ = gOutT; gWet_ = gWetT; blend_ = blT;
        if (bad) core_.reset();                                             // never stay silent after a bad value

        // ----- read-outs, 10 times a second ----------------------------------------------------------------------
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        cpuAcc_ += us; cpuFrames_ += n;
        sinceShow_ += n;
        if (sinceShow_ >= publishEvery_) { sinceShow_ = 0; publish(); }
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        switch (index)
        {
            case kGuitarVol: std::snprintf(b, sizeof b, "%.1f", (double)plain); out = b; return true;
            case kCpu: std::snprintf(b, sizeof b, "CPU %.1f %%", (double)cpuShown_.load()); out = b; return true;
            case kVolNow:
                if (getInt(kVolFollow) == 1 && get(kGuitarVol) >= 9.95f) std::snprintf(b, sizeof b, "Auto: lower Guitar Vol");
                else if (getInt(kVolFollow) == 1) std::snprintf(b, sizeof b, "Vol %.1f (auto)", (double)volShown_.load());
                else std::snprintf(b, sizeof b, "Vol %.1f", (double)volShown_.load());
                out = b; return true;
            case kStatus:
            {
                const int lt = learnShown_.load();
                if (lt > 0) std::snprintf(b, sizeof b, "LEARN: play at full vol %d", lt);
                else std::snprintf(b, sizeof b, "Full vol ref %.1f dBFS", (double)refDb_.load());
                out = b; return true;
            }
            case kLearn: case kNextQ: case kNextD: case kNextC: case kRandom: case kUndo: case kSave: case kCancel: out = "tap"; return true;
            case kName: out = nameText(); return true;
            case kPatch: out = patchText(); return true;
            case kBoardD: out = ""; return true;
            case kCircuit:
            {
                const int tp = mpc::clamp(getInt(kTopology), 0, kf::kNumTopologies - 1);
                std::snprintf(b, sizeof b, "%s x%d  Batt %.1f V%s", kTopologyNames[tp], getInt(kStages) + 1, (double)vccShown_.load(), levelShown_.load() ? "" : "  lvl..");
                out = b; return true;
            }
            case kPanel: out = ""; return true;
            default: return false;
        }
    }

    bool leveledForTest() const { return leveled_; }

    // the learned full-volume reference is global state: saved with the project, never with a preset
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        out.push_back(1);
        const float r = refDb_.load(); uint8_t p[4]; std::memcpy(p, &r, 4); out.insert(out.end(), p, p + 4);
    }
    void loadExtraState(const uint8_t* data, size_t size) override
    {
        if (size >= 5 && data[0] == 1) { float r; std::memcpy(&r, data + 1, 4); if (std::isfinite(r) && r > -80.0f && r <= 6.0f) refDb_.store(r); }
    }

private:
    static constexpr const char* kDispCpu[2] = {"cpu", "CPU"};
    static constexpr const char* kDispStatus[2] = {"status", "Status"};
    static constexpr const char* kDispVol[2] = {"vol_now", "Vol Now"};
    static constexpr const char* kDispPanel[2] = {"panel", "Panel"};

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "Kashyyyk Fuzz";
        info.vendor = "sureeel";
        info.uniqueId = MPC_FOURCC('S', '5', 'K', 'w');
        info.version = 300;                 // 0.3.0 (circuit families, stages, Sag, Feedback, presets)
        info.isInstrument = false;
        info.numInputs = 2; info.numOutputs = 2;
        info.latencySamples = kDelay;       // the oversampler; the dry path is delayed to match
        info.tailSamples = 1;
        return info;
    }
    void addDisplay(const char* id, const char* name)
    {
        mpc::Param p = mpc::floatParam(id, name, 0.0f, 1.0f, 0.0f, "", 2); p.automatable = false; addParam(p);
    }
    void addButton(const char* id, const char* name)
    {
        mpc::Param p = mpc::boolParam(id, name, false); p.automatable = false; addParam(p);
    }
    bool acted(int b) { const bool v = getBool(b); if (v != btnLast_[b]) { btnLast_[b] = v; return true; } return false; }
    void step(int param, int count) { set(param, (float)((getInt(param) + 1) % count)); notifyHostParameterChanged(param); }
    float coef(double ms) const { return (float)(1.0 - std::exp(-1.0 / (ms * 0.001 * fs_))); }

    kf::Settings settings() const
    {
        kf::Settings s;
        s.tr = mpc::clamp(getInt(kTransistors), 0, kf::kNumTransistors - 1);
        s.di = mpc::clamp(getInt(kDiodes), 0, kf::kNumDiodes - 1);
        s.arr = mpc::clamp(getInt(kDiodeArr), 0, kf::kArr - 1);
        s.cap = mpc::clamp(getInt(kInputCap), 0, kf::kNumInputCaps - 1);
        s.tone = mpc::clamp(getInt(kToneStack), 0, kf::kNumTones - 1);
        s.topo = mpc::clamp(getInt(kTopology), 0, kf::kNumTopologies - 1);
        s.stages = mpc::clamp(getInt(kStages) + 1, 1, 3);
        s.coupling = mpc::clamp(getInt(kCoupling), 0, kf::kNumCouplings - 1);
        s.resistors = mpc::clamp(getInt(kResistors), 0, 1);
        s.tr2 = getInt(kQ2) <= 0 ? -1 : mpc::clamp(getInt(kQ2) - 1, 0, kf::kNumTransistors - 1);
        s.buffer = getInt(kBuffer) == 1;
        s.fuzz = fuzzSm_; s.toneKnob = toneSm_;
        s.sag = mpc::clamp(sagSm_, 0.0f, 1.0f); s.feedback = mpc::clamp(fbSm_, 0.0f, 1.0f); s.dualMix = mpc::clamp(mixSm_, 0.0f, 1.0f);
        s.guitarVol = mpc::clamp(volSm_, 0.0f, 1.0f);
        s.levelFromPot = getInt(kVolFollow) == 0;
        return s;
    }

    void tell(int p, float plain) { set(p, plain); notifyHostParameterChanged(p); }

    // ----- Random and Undo ---------------------------------------------------------------------------------------
    float rnd() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return (float)(rng_ & 0xFFFFFF) / 16777216.0f; }
    int pickW(std::initializer_list<float> w)
    {
        float s = 0.0f; for (float x : w) s += x;
        float r = rnd() * s; int i = 0;
        for (float x : w) { if (r < x) return i; r -= x; ++i; }
        return i - 1;
    }
    void randomize()
    {
        for (int k : kSoundParams) undoVal_[k] = getNormalised(k);
        haveUndo_ = true; randomized_ = true;
        tell(kTransistors, (float)(int)(rnd() * kf::kNumTransistors));
        tell(kDiodes, (float)(int)(rnd() * kf::kNumDiodes));
        tell(kDiodeArr, (float)pickW({0.5f, 0.15f, 0.25f, 0.1f}));              // Pair, Single, 2+1, None
        tell(kInputCap, (float)pickW({0.1f, 0.25f, 0.25f, 0.4f}));             // 1n5, 10n, 47n, 100uF
        tell(kToneStack, (float)(int)(rnd() * kf::kNumTones));
        tell(kFuzz, 40.0f + 60.0f * rnd());
        tell(kTone, 15.0f + 70.0f * rnd());
        tell(kTopology, (float)pickW({0.25f, 0.2f, 0.2f, 0.12f, 0.1f, 0.13f}));
        tell(kStages, (float)pickW({0.35f, 0.45f, 0.2f}));
        tell(kSag, rnd() < 0.7f ? 25.0f * rnd() : 25.0f + 55.0f * rnd());          // mostly a little, sometimes starved
        tell(kFeedback, rnd() < 0.8f ? 0.0f : 50.0f * rnd());                       // rarely, and never full squeal
        tell(kDualMix, 25.0f + 50.0f * rnd());
        tell(kCoupling, (float)(int)(rnd() * kf::kNumCouplings));
        tell(kResistors, rnd() < 0.3f ? 1.0f : 0.0f);
        tell(kQ2, rnd() < 0.5f ? 0.0f : (float)(1 + (int)(rnd() * kf::kNumTransistors)));
        patch_ = kPtRandom; patchChanged_ = true;
    }
    void undo()
    {
        if (!haveUndo_) { patch_ = kPtNoUndo; patchChanged_ = true; return; }
        for (int k : kSoundParams) if (getNormalised(k) != undoVal_[k]) { setNormalised(k, undoVal_[k]); notifyHostParameterChanged(k); }
        haveUndo_ = false;
        patch_ = kPtUndone; patchChanged_ = true;
    }
    std::string patchText() const
    {
        switch (patchDisp_.load())
        {
            case kPtRandom: return std::string("Random: ") + kTopologyNames[mpc::clamp(getInt(kTopology), 0, kf::kNumTopologies - 1)] + " " + kf::kTransistors[mpc::clamp(getInt(kTransistors), 0, 4)].name + " " + kf::kDiodes[mpc::clamp(getInt(kDiodes), 0, 4)].name;
            case kPtUndone: return "Undone";
            case kPtNoUndo: return "Nothing to undo";
            case kPtSaved: { std::string s = "Saved: "; for (int i = 0; i < kNameLen && savedNameDisp_[i].load(); ++i) s += savedNameDisp_[i].load(); return s; }
            default: return "Ready";
        }
    }

    // ----- naming and saving (the design of Hydra Eel and Sleepy Android) -------------------------------------------
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
        lastPos_ = cursor_; tell(kNamePos, (float)cursor_);
        lastLetter_ = charIndex(name_[cursor_]); tell(kLetter, (float)lastLetter_);
        nameMsg_ = kNmNaming; nameChanged_ = true;
    }
    void confirmName()
    {
        if (savePending_.load(std::memory_order_acquire) >= 0) return;
        for (int k = 0; k < kNumParams; ++k) saveVals_[k] = get(k);
        int len = kNameLen;
        while (len > 0 && (name_[len - 1] == 0 || name_[len - 1] == ' ')) --len;
        for (int i = 0; i < kNameLen; ++i) saveName_[i] = i < len ? (name_[i] ? name_[i] : ' ') : 0;
        saveName_[kNameLen] = 0;
        if (len == 0) autoName(saveName_);
        const int slot = nextSlot_;
        nextSlot_ = (nextSlot_ + 1) % kNumUser;
        savePending_.store(slot, std::memory_order_release);
        randomized_ = false; naming_ = false;
        nameMsg_ = kNmSaving; nameChanged_ = true;
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
        if (msg == kNmSaving) return "Saving...";
        if (msg == kNmSaved) { std::string s = "Saved: "; for (int i = 0; i < kNameLen && savedNameDisp_[i].load(); ++i) s += savedNameDisp_[i].load(); return s; }
        if (msg == kNmCancelled) return "Not saved";
        return "Tap SAVE to name it";
    }
    // file: "KFUP" | u8 1 | u8 next | u16 count | kNumUser x { u8 used, char name[17], f32 x count }
    void loadUserPresets()
    {
        std::vector<uint8_t> d;
        if (!mpc::readFile(userFile_, d) || d.size() < 8 || std::memcmp(d.data(), "KFUP", 4) != 0 || d[4] != 1) return;
        const int count = d[6] | (d[7] << 8);
        const size_t rec = 1 + (kNameLen + 1) + 4 * (size_t)count;
        if (count < 1 || d.size() < 8 + rec * kNumUser) return;
        nextSlot_ = d[5] % kNumUser;
        for (int u = 0; u < kNumUser; ++u)
        {
            const uint8_t* r = d.data() + 8 + rec * (size_t)u;
            userUsed_[u] = r[0];
            if (!r[0]) continue;
            char nm[kNameLen + 1] = {0};
            std::memcpy(nm, r + 1, kNameLen);
            for (char& c : nm) if (c != 0 && (c < 32 || c > 126)) c = '?';
            std::vector<float>& v = programs_[(size_t)(numFactory_ + u)].plainValues;
            for (int k = 0; k < count && k < kNumParams; ++k) { float x; std::memcpy(&x, r + 1 + kNameLen + 1 + 4 * k, 4); if (std::isfinite(x)) v[(size_t)k] = x; }
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
                for (int k = 0; k < kNumParams; ++k) v[(size_t)k] = saveVals_[k];
                programs_[(size_t)(numFactory_ + u)].name.assign(saveName_);
                for (int i = 0; i <= kNameLen; ++i) savedNameDisp_[i].store(saveName_[i]);
                writeUserFile((u + 1) % kNumUser);
                savePending_.store(-1, std::memory_order_release);
                saveDone_.store(u, std::memory_order_release);
            }
            const uint64_t want = levelWant_.load(std::memory_order_acquire);
            if (want != 0 && want != levelDone_.load(std::memory_order_acquire))
            {   // a new combination: measured once, then remembered
                float v = 0.0f;
                if (!cacheFind(want, v)) { v = kf::calibrate(&tables(), fsW_.load(), fromKey(want)); cachePut(want, v); }
                levelVal_.store(v);
                levelDone_.store(want, std::memory_order_release);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }
    // the worker's memory of measured combinations (worker thread only)
    bool cacheFind(uint64_t k, float& v) const { for (int i = 0; i < kCache; ++i) if (cacheKey_[i] == k) { v = cacheVal_[i]; return true; } return false; }
    void cachePut(uint64_t k, float v) { cacheKey_[cacheNext_] = k; cacheVal_[cacheNext_] = v; cacheNext_ = (cacheNext_ + 1) % kCache; }

    // ----- factory presets ---------------------------------------------------------------------------------------
    struct P { const char* name; int topo, stages, tr, q2, di, arr, cap, ts, coupling, res; float fuzz, tone, sag, fb, mix, blend; };
    void addFactoryPresets(const std::vector<float>& d)
    {
        // tr / q2: 0 OC44 1 2N2613 2 AC107 3 2N3904 4 2N5088 (q2 -1 = same)   di: 0 1N34A 1 1N914 2 1N6263 3 D9B 4 LED
        // arr: 0 Pair 1 Single 2 2+1 3 None   cap: 0 1n5 1 10n 2 47n 3 100uF   ts: 0 Muff 1 Flat 2 Mid 3 Tilt 4 Bypass
        // coupling: 0 10n 1 47n 2 100n (film) 3 1u 4 10u 5 100u (elec)   res: 0 metal film 1 carbon comp
        static const P presets[] = {
            // Fuzz Face
            {"Germanium Warmth",   0, 1, 0, -1, 0, 3, 3, 3, 4, 1, 80, 45,  5,  0, 50, 100},
            {"Silicon Aggression", 0, 1, 3, -1, 1, 3, 3, 3, 4, 0, 90, 60,  0,  0, 50, 100},
            {"Hendrix Clean-Up",   0, 1, 3, -1, 1, 3, 3, 3, 4, 1, 100, 55, 0,  0, 50, 100},
            // Tone Bender
            {"Mk I.5 Rasp",        0, 1, 2, -1, 0, 3, 1, 3, 2, 1, 85, 65, 10,  0, 50, 100},
            {"Supa Punch",         1, 2, 1,  0, 0, 3, 2, 3, 3, 1, 85, 55,  0,  0, 50, 100},
            {"British Buzz",       1, 3, 2,  0, 0, 3, 1, 3, 2, 1, 80, 70,  0,  0, 50, 100},
            // Maestro FZ-1
            {"Sixties Sputter",    1, 3, 2, -1, 0, 3, 0, 3, 0, 1, 75, 70, 60,  0, 50, 100},
            // Mosrite Fuzzrite
            {"Surf Saw",           1, 1, 3, -1, 1, 3, 1, 3, 1, 0, 85, 75,  0,  0, 50,  85},
            // Shin-ei / Univox Super-Fuzz
            {"Octave Scoop",       4, 2, 4, -1, 1, 0, 2, 0, 2, 0, 85, 50,  0,  0, 50, 100},
            {"Super Splat",        4, 3, 4, -1, 1, 0, 2, 0, 2, 0, 100, 35, 20, 0, 50, 100},
            // Octavia
            {"Upper Octave",       4, 1, 3, -1, 1, 3, 2, 3, 3, 0, 70, 65,  0,  0, 50, 100},
            // Big Muff
            {"Triangle",           2, 2, 4, -1, 1, 0, 2, 0, 2, 0, 80, 50,  0,  0, 50, 100},
            {"Ram's Head",         2, 2, 4, -1, 1, 0, 2, 0, 2, 1, 70, 40,  0,  0, 50, 100},
            {"Op-Amp",             3, 2, 4, -1, 1, 0, 2, 0, 2, 0, 75, 55,  0,  0, 50, 100},
            {"Russian",            2, 2, 3, -1, 1, 0, 2, 0, 4, 1, 85, 35,  0,  0, 50, 100},
            {"Nano",               2, 2, 4, -1, 1, 0, 2, 0, 1, 0, 70, 55,  0,  0, 50, 100},
            {"Lead",               2, 1, 4, -1, 1, 0, 2, 2, 2, 0, 65, 50,  0,  0, 50, 100},
            {"Shoegaze Wall",      2, 3, 4, -1, 1, 0, 3, 0, 4, 1, 100, 45, 0,  0, 50, 100},
            // ZVex Fuzz Factory
            {"Factory Squeal",     0, 1, 2, -1, 0, 3, 3, 3, 4, 1, 100, 55, 30, 70, 50, 100},
            {"Starved Gate",       0, 1, 2, -1, 0, 3, 3, 3, 4, 1, 90, 50, 85, 20, 50, 100},
            // Death By Audio Fuzz War
            {"War Wall",           3, 3, 4, -1, 1, 0, 2, 2, 2, 0, 90, 55,  0,  0, 50, 100},
            {"Scorched Earth",     3, 3, 4, -1, 4, 2, 2, 3, 2, 1, 100, 70, 0,  0, 50, 100},
            // Death By Audio Supersonic Fuzz Gun
            {"Gun Blend",          5, 2, 3, -1, 1, 0, 2, 3, 3, 0, 90, 60,  0,  0, 50, 100},
            {"Sonic Gate",         5, 3, 3, -1, 1, 0, 2, 3, 3, 0, 100, 55, 75, 10, 65, 100},
            // Death By Audio Apocalypse
            {"Apocalypse Gate",    2, 3, 4, -1, 1, 0, 2, 2, 2, 0, 100, 50, 70,  0, 50, 100},
            {"Apocalypse Octave",  4, 3, 4, -1, 1, 0, 2, 3, 2, 0, 100, 60, 30, 30, 50, 100},
            // Boutique
            {"OC44 Black Glass",   0, 1, 0,  0, 0, 3, 3, 3, 4, 1, 75, 45, 15,  0, 50, 100},
            {"2N2613 Tribute",     1, 2, 1, -1, 0, 3, 2, 3, 3, 1, 80, 50,  0,  0, 50, 100},
        };
        for (const P& p : presets)
        {
            std::vector<float> v = d;
            v[kTopology] = (float)p.topo; v[kStages] = (float)(p.stages - 1);
            v[kTransistors] = (float)p.tr; v[kQ2] = (float)(p.q2 < 0 ? 0 : p.q2 + 1);
            v[kDiodes] = (float)p.di; v[kDiodeArr] = (float)p.arr; v[kInputCap] = (float)p.cap; v[kToneStack] = (float)p.ts;
            v[kCoupling] = (float)p.coupling; v[kResistors] = (float)p.res;
            v[kFuzz] = p.fuzz; v[kTone] = p.tone; v[kSag] = p.sag; v[kFeedback] = p.fb; v[kDualMix] = p.mix; v[kBlend] = p.blend;
            addProgram(p.name, v);
        }
    }

    void writeUserFile(int next)
    {
        std::vector<uint8_t> d = {'K', 'F', 'U', 'P', 1, (uint8_t)next, (uint8_t)(kNumParams & 255), (uint8_t)(kNumParams >> 8)};
        for (int u = 0; u < kNumUser; ++u)
        {
            d.push_back(userUsed_[u]);
            char nm[kNameLen + 1] = {0};
            std::strncpy(nm, programs_[(size_t)(numFactory_ + u)].name.c_str(), kNameLen);
            d.insert(d.end(), nm, nm + kNameLen + 1);
            for (int k = 0; k < kNumParams; ++k) putF32(d, programs_[(size_t)(numFactory_ + u)].plainValues[(size_t)k]);
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

    // ----- Vol Follow: attack peaks of notes, relative to the learned full-volume level -----------------------------
    inline void meter(float x)
    {
        const float a = std::fabs(x);
        envFast_ += (a - envFast_) * (a > envFast_ ? envFastA_ : envFastR_);
        envSlow_ += (a - envSlow_) * (a > envSlow_ ? envSlowA_ : envSlowR_);
        if (holdoff_ > 0) --holdoff_;
        if (capture_ > 0)
        {   // the attack window: its peak is this note's level
            peak_ = std::max(peak_, a);
            if (--capture_ == 0)
            {
                const float db = 20.0f * std::log10(std::max(1e-6f, peak_));
                playDb_ += (db - playDb_) * 0.35f;
                if (learning_ > 0 && learnN_ < 64) learnPeaks_[learnN_++] = db;
            }
        }
        else if (holdoff_ == 0 && envFast_ > 0.003f && envFast_ > 1.6f * envSlow_ + 0.0005f)
        {   // a note starts
            capture_ = (int)(0.04 * fs_); holdoff_ = (int)(0.08 * fs_); peak_ = a;
        }
        if (learning_ > 0 && --learning_ == 0) finishLearn();
    }
    void finishLearn()
    {
        if (learnN_ < 2) return;                             // nothing played: keep the old reference
        std::vector<float> v(learnPeaks_, learnPeaks_ + learnN_);
        std::sort(v.begin(), v.end());
        double s = 0.0; int c = 0;
        for (int i = learnN_ / 2; i < learnN_; ++i) { s += v[(size_t)i]; ++c; }       // the louder half
        refDb_.store((float)(s / c));
        playDb_ = refDb_.load();
    }
    // the volume-knob position whose pot divide matches how far below full volume the attacks are
    float followVol() const
    {
        const float rDb = std::min(0.0f, playDb_ - refDb_.load());
        const double t = std::pow(10.0, rDb / 20.0), k = 4.394449;
        return (float)mpc::clamp(std::log(1.0 + t * (std::exp(k) - 1.0)) / k, 0.0, 1.0);
    }

    void show(int p, float norm) { setNormalised(p, norm); notifyHostParameterChanged(p); }
    void publish()
    {
        const float cpu = cpuFrames_ > 0 ? (float)(cpuAcc_ / (cpuFrames_ / fs_ * 1e6) * 100.0) : 0.0f;
        cpuAcc_ = 0.0; cpuFrames_ = 0;
        cpuShown_.store(cpu);
        const int ck = (int)std::lround(cpu * 10.0f);
        if (ck != lastCpuKey_) { lastCpuKey_ = ck; show(kCpu, (ck & 1) ? 0.5f : 0.51f); }
        volShown_.store(volSm_ * 10.0f);
        const int vk = ((int)std::lround(volSm_ * 100.0f) * 2 + getInt(kVolFollow)) * 2 + (get(kGuitarVol) >= 9.95f ? 1 : 0);
        if (vk != lastVolKey_) { lastVolKey_ = vk; show(kVolNow, std::min(1.0f, volSm_)); }
        const int lt = learning_ > 0 ? 1 + (int)(learning_ / fs_) : 0;
        learnShown_.store(lt);
        const int sk = lt * 10000 + (int)std::lround(refDb_.load() * 10.0f) + 5000;
        if (sk != lastStatusKey_) { lastStatusKey_ = sk; show(kStatus, (sk & 1) ? 0.5f : 0.51f); }
        if (patchChanged_) { patchChanged_ = false; patchDisp_.store(patch_); ++serialP_; show(kPatch, (serialP_ & 1) ? 0.5f : 0.51f); }
        if (nameChanged_)
        {
            nameChanged_ = false;
            for (int i = 0; i <= kNameLen; ++i) nameDisp_[i].store(name_[i]);
            nameCursorDisp_.store(cursor_); nameMsgDisp_.store(nameMsg_);
            ++serialN_; show(kName, (serialN_ & 1) ? 0.5f : 0.51f);
        }
        // the board's diode picture: the diode part, or empty sockets when the arrangement is None
        const int bd = getInt(kDiodeArr) == 3 ? kf::kNumDiodes : mpc::clamp(getInt(kDiodes), 0, kf::kNumDiodes - 1);
        if (bd != lastBoardD_) { lastBoardD_ = bd; show(kBoardD, (float)bd / kf::kNumDiodes); }
        // the circuit line: family, stages, the battery, the level match
        vccShown_.store(core_.vccLast); levelShown_.store(leveled_);
        const int ck2 = ((getInt(kTopology) * 4 + getInt(kStages)) * 200 + (int)std::lround(core_.vccLast * 10.0f)) * 2 + (leveled_ ? 1 : 0);
        if (ck2 != lastCircuitKey_) { lastCircuitKey_ = ck2; ++serialC_; show(kCircuit, (serialC_ & 1) ? 0.5f : 0.51f); }
    }

    kf::Core core_;
    double fs_ = 44100.0;
    float dryL_[kDelay] = {}, dryR_[kDelay] = {};
    int dpos_ = 0;
    float gIn_ = 1.0f, gOut_ = 1.0f, gWet_ = 1.0f, blend_ = 1.0f, fuzzSm_ = 0.7f, toneSm_ = 0.5f, volSm_ = 1.0f;
    float sagSm_ = 0.0f, fbSm_ = 0.0f, mixSm_ = 0.5f;
    // level matching (audio thread asks, worker measures)
    std::atomic<uint64_t> levelWant_{0}, levelDone_{0};
    std::atomic<float> levelVal_{1.0f};
    std::atomic<double> fsW_{44100.0};
    static constexpr int kCache = 128;
    uint64_t cacheKey_[kCache] = {}; float cacheVal_[kCache] = {}; int cacheNext_ = 0;
    bool leveled_ = true;
    std::atomic<float> vccShown_{9.0f};
    std::atomic<bool> levelShown_{true};
    int lastCircuitKey_ = -1, serialC_ = 0;
    bool btnLast_[kNumParams] = {};
    // vol follow
    float envFast_ = 0.0f, envSlow_ = 0.0f, envFastA_ = 0.1f, envFastR_ = 0.001f, envSlowA_ = 0.01f, envSlowR_ = 0.0001f;
    float peak_ = 0.0f, playDb_ = kDefaultRefDb;
    int capture_ = 0, holdoff_ = 0, learning_ = 0, learnN_ = 0;
    float learnPeaks_[64] = {};
    std::atomic<float> refDb_{kDefaultRefDb};
    // read-outs
    int publishEvery_ = 4410, sinceShow_ = 0, lastStatusKey_ = -1, lastVolKey_ = -1, lastCpuKey_ = -1;
    double cpuAcc_ = 0.0; long cpuFrames_ = 0;
    std::atomic<float> cpuShown_{0.0f}, volShown_{10.0f};
    std::atomic<int> learnShown_{0};
    // random / undo / save
    uint32_t rng_ = 0x9E3779B9u;
    float undoVal_[kNumParams] = {};
    bool haveUndo_ = false, randomized_ = false, naming_ = false, patchChanged_ = true, nameChanged_ = true;
    enum Patch { kPtReady, kPtRandom, kPtUndone, kPtNoUndo, kPtSaved };
    enum NameMsg { kNmIdle, kNmNaming, kNmSaving, kNmSaved, kNmCancelled };
    int patch_ = kPtReady, nameMsg_ = kNmIdle, cursor_ = 0, lastPos_ = -1, lastLetter_ = -1, lastBoardD_ = -1, serialP_ = 0, serialN_ = 0;
    char name_[kNameLen + 1] = {}, saveName_[kNameLen + 1] = {};
    std::atomic<char> nameDisp_[kNameLen + 1] = {}, savedNameDisp_[kNameLen + 1] = {};
    std::atomic<int> nameCursorDisp_{0}, nameMsgDisp_{kNmIdle}, patchDisp_{kPtReady};
    float saveVals_[kNumParams] = {};
    int numFactory_ = 1, nextSlot_ = 0;
    uint8_t userUsed_[kNumUser] = {};
    std::string userFile_;
    std::atomic<int> savePending_{-1}, saveDone_{-1};
    std::atomic<bool> quit_{false};
    std::thread worker_;
};

MPC_PLUGIN_ENTRY(KashyyykFuzz)
