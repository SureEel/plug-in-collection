// Elver - a clean, simple, low-memory synthesizer for Akai MPC / Force, by Sure Eel (manufacturer "sureeel").
//
// Per voice: an A side and a B side, each with three oscillators (sine, triangle, saw, square/pulse,
// noise; level, octave, semitone, fine; unison up to 4 detuned, spread copies), its own filter
// (24 dB LP, 12 dB LP, BP, HP, notch; cutoff, resonance, envelope amount, key tracking) and its own
// amp and filter envelopes (ADSR; releases up to 20 s). Blend (also the XY pad's X) crossfades A and B;
// the pad's Y drives cutoff, morph, resonance or drive. The two filters run in parallel (A osc ->
// A filter, B osc -> B filter) or in series (both sides -> A filter -> B filter). Two LFOs (pitch,
// filter, amp or blend), slow random drift, ring modulation, drive and bit-crush grit. No effects.
//
// Shape (per side): a single-cycle wave built from a library wave ("Start From") and / or a wave drawn
// with the finger on the DRAW page, then reshaped (Bright, Odd/Even, Formant, Fold, Bend, Sync, Mutate).
// It is rebuilt as band-limited tables on the worker thread whenever a setting moves, and crossfaded in
// (20 ms), so turning a knob never clicks and high notes never alias. Any oscillator can play it.
//
// Voices: up to 8, inside a budget of 48 running oscillators (unison copies count), so heavy patches
// play fewer voices instead of overloading the CPU. Output: master Volume, Output level, soft limit.
// Volume, Output and the Skin are global: factory presets never change them (saved presets restore
// their skin); randomize never touches Volume / Output.

#include "mpc_plugin.h"
#include "elver_dsp.h"
#include "elver_wave.h"

#include <memory>
#include <mutex>

#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

using namespace elver;

// ---- parameters -------------------------------------------------------------------------------------
// Side block (A then B), offsets inside it:
constexpr int kO = 0;            // oscillators: 3 x {wave, level, octave, semi, fine, pw}
constexpr int kUni = 18, kDet = 19, kSpr = 20;
constexpr int kFType = 21, kCut = 22, kRes = 23, kFEnv = 24, kKeyTrk = 25;
constexpr int kFA = 26, kFD = 27, kFS = 28, kFR = 29, kAA = 30, kAD = 31, kAS = 32, kAR = 33;
constexpr int kSideN = 34;

// Shaper block (A then B), appended after the 1.0 parameters, offsets inside it:
enum ShOff { kShStart = 0, kShBright, kShOddEven, kShFormant, kShFold, kShBend, kShSync, kShMutate, kShSeed,
             kShDrawMix, kShSmooth, kShCopy, kShClear, kShUndo, kShDrawX, kShDrawY,
             kShPts, kShScope = kShPts + kDrawPoints, kShN = kShScope + kScopePoints };

enum ParamIndex
{
    kVolume = 0,    // dB; parameter 0 is nudged by the host on every load: harmless here
    kOutput, kSkin, kBlend, kPadY, kPadDest, kRouting, kVoices, kDrift, kDrive, kGrit, kRing,
    kA, kB = kA + kSideN,
    kL1Rate = kB + kSideN, kL1Depth, kL1Shape, kL1Dest, kL2Rate, kL2Depth, kL2Shape, kL2Dest,
    kNumSound,                                   // everything above is the sound (presets hold it)
    kCategory = kNumSound, kRandomize, kUndo, kRandAmount, kLockA, kLockB, kLockFilter, kLockEnv, kLockMod, kLockSkin,
    kBrowseCat, kBrowseSlot, kLoad, kSave, kSaveCat, kConfirm, kCancel,
    kKey0,                                       // the letter grid: kNumKeys on/off keys
    kCpu = kKey0 + 42, kVoicesNow, kStatus, kNameEdit, kHelp, kHelp2,
    kShA, kShB = kShA + kShN,                    // 1.1: the two shapers (appended: saved projects keep their order)
    kPresetPick = kShB + kShN,                   // 1.1: a drop-down of all presets (kept for saved projects; not on screen)
    kSlotRow0,                                   // 1.2: the PRESET page's list: the browsed category's 16 preset names
    kNumParams = kSlotRow0 + 16
};
// "the sound": what presets, randomize and undo cover. The 1.0 block plus each shaper's settings and
// drawing (not its buttons, the drawing pad's position or the scope).
constexpr bool isSound(int i)
{
    return i < kNumSound || (i >= kShA && i < kShB + kShN && ((i - kShA) % kShN <= kShSmooth ||
           ((i - kShA) % kShN >= kShPts && (i - kShA) % kShN < kShScope)));
}
constexpr int shOff(int i) { return i >= kShA && i < kShB + kShN ? (i - kShA) % kShN : -1; }
static_assert(!isSound(kPresetPick), "the preset drop-down is not part of the sound");
constexpr int kNumKeys = 42;
const char kKeyChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -";   // 38 characters, then 4 editing keys
enum EditKey { kKeyBack = 38, kKeyLeft, kKeyRight, kKeyClear };
constexpr int kNameLen = 16;

constexpr int kCats = 8;                      // 1.2: Funk added (after the seven moods)
const char* const kCatNames[kCats] = {"Ambient", "Pads & Winds", "Keys & Plucks", "Leads & Basses", "Lo-Fi Psych", "Industrial", "Noise & Chaos", "Funk"};
const char* const kCatTags[kCats] = {"AMB", "PAD", "KEY", "LEAD", "LOFI", "IND", "NOIS", "FUNK"};
constexpr int kFactoryPer = 8, kUserPer = 8, kSlotsPer = kFactoryPer + kUserPer;
const char* const kFactoryNames[kCats][kFactoryPer] = {
    {"Deep Tide", "Glass Fog", "Slow Comet", "Silt Bloom", "Low Halo", "Still Water", "Drift Ice", "Night Pollen"},
    {"Reed Wind", "Warm Choir", "Night Ferry", "Soft Gale", "Choir Loft", "Brass Haze", "Wind Organ", "Velvet Air"},
    {"Glass Keys", "Pluck Drop", "Toy Bell", "Wood Tap", "Ice Mallet", "Harp Wire", "Kalimba Glow", "Felt Keys"},
    {"Eel Lead", "Sub Current", "Acid Fin", "Saw Wire", "Rubber Bass", "Fat Solo", "Sub Pluck", "Siren Lead"},
    {"Dusty Tape", "Melt Glow", "Warm Static", "Moth Light", "VHS Choir", "Warped Poly", "Cassette Sun", "Hazy Organ"},
    {"Rust Gears", "Clang Yard", "Piston", "Sheet Metal", "Steam Press", "Rivet Gun", "Iron Lung", "Foundry"},
    {"Static Swarm", "Broken Radio", "Grit Storm", "Ion Storm", "Shortwave", "Hornet Nest", "Static Rain", "Data Storm"},
    // Funk: set by hand (funkPreset), inspired by Minneapolis-sound brass stabs, James Brown horn hits and
    // clavinet, prog-rock leads and organ, and early disco strings and octave bass
    {"Royal Stab", "Brass Poly", "Clav Strut", "Horn Hit", "Prog Lead", "Cathedral Organ", "Disco Strings", "Octave Bass"},
};
const char* const kSkinNames[4] = {"Neon Rain", "Capsule Red", "Monolith", "Derelict"};

const char* const kHelpText[][2] = {
    {"Touch a name or a meter",       "to see what it does."},            // 0
    {"XY pad: X blends A and B,",     "Y drives the Pad Y target."},      // 1
    {"Blend: A side to B side.",      "Also the pad's X."},               // 2
    {"Pad Y target: cutoff, morph",   "(osc 1 to osc 3), reso, drive."},  // 3
    {"Wave: sine, triangle, saw,",    "square (pulse width), noise."},    // 4
    {"Level of this oscillator.",     "0 = off (saves CPU)."},            // 5
    {"Octave, semitone and fine",     "tuning (cents)."},                 // 6
    {"Pulse width of the square.",    "50 % = square."},                  // 7
    {"Unison: detuned copies of",     "each osc, spread in stereo."},     // 8
    {"Filter type: 24 / 12 dB LP,",   "band, high-pass, notch."},         // 9
    {"Cutoff and resonance.",         "Reso stops short of a scream."},   // 10
    {"Env amount: how far the",       "filter envelope moves cutoff."},   // 11
    {"Key tracking: cutoff follows",  "the note (100 % = fully)."},       // 12
    {"Parallel: A osc > A filter,",   "B > B. Series: A > B filters."},   // 13
    {"Amp envelope: attack, decay,",  "sustain, release (to 20 s)."},     // 14
    {"Filter envelope (ADSR)",        "for this side's filter."},         // 15
    {"LFO: rate, depth, shape and",   "where it goes."},                  // 16
    {"Drift: slow random pitch and",  "filter wander, like tape."},       // 17
    {"Drive: saturation. Grit:",      "bit-crush and rate reduction."},   // 18
    {"Ring: osc 1 x osc 2 per side,", "clangy, metallic tones."},         // 19
    {"Voices: most notes at once.",   "Big unison patches use fewer."},   // 20
    {"Volume and Output: global,",    "presets never change them."},      // 21
    {"Skin: the panel's colours.",    "Randomize can pick one."},         // 22
    {"Category: the mood Randomize",  "uses: calm to noisy, and Funk."},  // 23
    {"Randomize: a new sound in the", "category. Undo goes back."},       // 24
    {"Amount: low nudges, 100 %",     "rerolls."},                        // 25
    {"Locks keep a section as it",    "is when you randomize."},          // 26
    {"Pick a category, then tap a",   "preset in the list to load it."},  // 27
    {"Save: type a name, pick a",     "category, Confirm."},              // 28
    {"CPU: Elver's own load per",     "audio block. Voices: playing."},   // 29
    {"Shape: a wave you sculpt. Pick", "Shape as an oscillator's wave."}, // 30
    {"Start From: the library wave",  "the shape begins with."},          // 31
    {"Bright tilts the harmonics.",   "Odd/Even: hollow or organ-like."}, // 32
    {"Formant: a vocal resonance",    "moving through the harmonics."},   // 33
    {"Fold, Bend (phase distortion)", "and Sync reshape the cycle."},     // 34
    {"Mutate: seeded variation.",     "Same Seed = same wave."},          // 35
    {"Drag to draw: osc 1 plays the",  "drawn wave (Shape) from then."},   // 36
    {"Copy Start: draw over the",     "start wave. Undo: last stroke."},  // 37
    {"The wave the oscillators set",  "to Shape play (one cycle)."},      // 38
};
constexpr int kNumHelp = (int)(sizeof(kHelpText) / sizeof(kHelpText[0]));
static_assert(kNumHelp <= 40, "help items");

inline float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }

// ---- voices -------------------------------------------------------------------------------------------
constexpr int kMaxVoices = 8, kMaxUni = 4;
// CPU budget per instance, in units: each sounding side of a voice costs 1 (filter, envelopes) plus 1 per
// running oscillator copy. The Force (32-bit, Cortex-A17) is about half as fast as the MPC Key 37's A55.
#if defined(__arm__)
constexpr int kBudget = 24;
#else
constexpr int kBudget = 48;
#endif

struct SideState
{
    float ph[3][kMaxUni] = {};
    StereoFilter filt;
    ChunkEnv amp, fenv;
    void reset() { std::memset(ph, 0, sizeof ph); filt.reset(); amp.reset(); fenv.reset(); }
};
struct Voice
{
    bool active = false, released = false, pedalHeld_ = false;
    int note = 60; float vel = 1.0f; uint32_t age = 0;
    SideState s[2];
    Drift drift;
    mpc::dsp::Xorshift32 rng;
    void reset() { active = released = false; s[0].reset(); s[1].reset(); }
};

// per-block settings of one side (read once per block)
struct SideSet
{
    int wave[3]; float level[3]; float ratio[3]; float pw[3];
    int uni; float det; float spr;
    int ftype; float cut, res, fenvAmt, keyTrk;
    bool on;                    // blend gain > 0
    bool mono;                  // no unison spread: one filter channel, copied
};

class Elver : public mpc::Plugin
{
public:
    Elver() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("volume", "Volume", -48.0f, 6.0f, -8.0f, "dB", 1));
        addParam(mpc::floatParam("output", "Output", -24.0f, 12.0f, 0.0f, "dB", 1));
        { mpc::Param p = mpc::choiceParam("skin", "Skin", {kSkinNames[0], kSkinNames[1], kSkinNames[2], kSkinNames[3]}, 0); addParam(p); }
        addParam(mpc::floatParam("blend", "Blend", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("pad_y", "Pad Y", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(mpc::choiceParam("pad_dest", "Pad Y To", {"Cutoff", "Morph", "Resonance", "Drive"}, 0));
        addParam(mpc::choiceParam("routing", "Routing", {"Parallel", "Series"}, 0));
        addParam(mpc::intParam("voices", "Voices", 1, kMaxVoices, kMaxVoices));
        addParam(mpc::floatParam("drift", "Drift", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("drive", "Drive", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("grit", "Grit", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("ring", "Ring", 0.0f, 100.0f, 0.0f, "%", 0));
        for (int side = 0; side < 2; ++side)
        {
            const char S = side ? 'B' : 'A';
            const char s = side ? 'b' : 'a';
            for (int o = 0; o < 3; ++o)
            {
                auto id = [&](const char* f) { char b[32]; std::snprintf(b, sizeof b, "%c_o%d_%s", s, o + 1, f); return std::string(b); };
                auto nm = [&](const char* f) { char b[32]; std::snprintf(b, sizeof b, "%c%d %s", S, o + 1, f); return std::string(b); };
                addParam(mpc::choiceParam(id("wave").c_str(), nm("Wave").c_str(), {"Sine", "Triangle", "Saw", "Square", "Noise", "Shape"}, o == 0 ? 2 : 0));
                addParam(mpc::floatParam(id("level").c_str(), nm("Level").c_str(), 0.0f, 100.0f, o == 0 ? 80.0f : 0.0f, "%", 0));
                addParam(mpc::intParam(id("oct").c_str(), nm("Octave").c_str(), -3, 3, 0));
                addParam(mpc::intParam(id("semi").c_str(), nm("Semi").c_str(), -12, 12, 0));
                addParam(mpc::floatParam(id("fine").c_str(), nm("Fine").c_str(), -50.0f, 50.0f, 0.0f, "ct", 0));
                addParam(mpc::floatParam(id("pw").c_str(), nm("PW").c_str(), 5.0f, 95.0f, 50.0f, "%", 0));
            }
            auto id = [&](const char* f) { char b[32]; std::snprintf(b, sizeof b, "%c_%s", s, f); return std::string(b); };
            auto nm = [&](const char* f) { char b[32]; std::snprintf(b, sizeof b, "%c %s", S, f); return std::string(b); };
            addParam(mpc::intParam(id("unison").c_str(), nm("Unison").c_str(), 1, kMaxUni, 1));
            addParam(mpc::floatParam(id("detune").c_str(), nm("Detune").c_str(), 0.0f, 100.0f, 15.0f, "ct", 0));
            addParam(mpc::floatParam(id("spread").c_str(), nm("Spread").c_str(), 0.0f, 100.0f, 60.0f, "%", 0));
            addParam(mpc::choiceParam(id("ftype").c_str(), nm("Filter").c_str(), {"LP24", "LP12", "BP", "HP", "Notch"}, 0));
            addParam(mpc::logParam(id("cutoff").c_str(), nm("Cutoff").c_str(), 30.0f, 18000.0f, 2500.0f, "Hz", 0));
            addParam(mpc::floatParam(id("reso").c_str(), nm("Reso").c_str(), 0.0f, 100.0f, 15.0f, "%", 0));
            addParam(mpc::floatParam(id("fenv").c_str(), nm("Env Amt").c_str(), -100.0f, 100.0f, 30.0f, "%", 0));
            addParam(mpc::floatParam(id("keytrk").c_str(), nm("Key Trk").c_str(), 0.0f, 100.0f, 50.0f, "%", 0));
            const char* envF[8] = {"f_att", "f_dec", "f_sus", "f_rel", "a_att", "a_dec", "a_sus", "a_rel"};
            const char* envN[8] = {"F.Attack", "F.Decay", "F.Sustain", "F.Release", "Attack", "Decay", "Sustain", "Release"};
            const float envDef[8] = {5, 400, 30, 600, 5, 300, 80, 400};
            for (int k = 0; k < 8; ++k)
            {
                if (k % 4 == 2) addParam(mpc::floatParam(id(envF[k]).c_str(), nm(envN[k]).c_str(), 0.0f, 100.0f, envDef[k], "%", 0));
                else addParam(mpc::logParam(id(envF[k]).c_str(), nm(envN[k]).c_str(), 1.0f, k % 4 == 3 ? 20000.0f : 10000.0f, envDef[k], "ms", 0));
            }
        }
        for (int l = 0; l < 2; ++l)
        {
            char a[16], b[16];
            std::snprintf(a, sizeof a, "lfo%d_rate", l + 1); std::snprintf(b, sizeof b, "LFO%d Rate", l + 1);
            addParam(mpc::logParam(a, b, 0.02f, 20.0f, l ? 0.2f : 5.0f, "Hz", 2));
            std::snprintf(a, sizeof a, "lfo%d_depth", l + 1); std::snprintf(b, sizeof b, "LFO%d Depth", l + 1);
            addParam(mpc::floatParam(a, b, 0.0f, 100.0f, 0.0f, "%", 0));
            std::snprintf(a, sizeof a, "lfo%d_shape", l + 1); std::snprintf(b, sizeof b, "LFO%d Shape", l + 1);
            addParam(mpc::choiceParam(a, b, {"Sine", "Triangle", "Saw", "Square", "S&H"}, 0));
            std::snprintf(a, sizeof a, "lfo%d_dest", l + 1); std::snprintf(b, sizeof b, "LFO%d To", l + 1);
            addParam(mpc::choiceParam(a, b, {"Pitch", "Filter", "Amp", "Blend"}, l ? 1 : 0));
        }
        static_assert(kL2Dest + 1 == kNumSound, "sound parameters");
        std::vector<std::string> cats(kCatNames, kCatNames + kCats);
        addParam(mpc::choiceParam("category", "Category", cats, 0));
        addParam(mpc::boolParam("randomize", "Randomize", false));
        addParam(mpc::boolParam("undo", "Undo", false));
        addParam(mpc::floatParam("rand_amount", "Rand Amount", 0.0f, 100.0f, 100.0f, "%", 0));
        for (const char* l : {"Lock A", "Lock B", "Lock Filter", "Lock Env", "Lock Mod", "Lock Skin"})
        {
            std::string i(l); for (char& c : i) c = c == ' ' ? '_' : (char)std::tolower(c);
            addParam(mpc::boolParam(i.c_str(), l, false));
        }
        addParam(mpc::choiceParam("browse_cat", "Browse", cats, 0));
        {
            std::vector<std::string> slots; for (int i = 1; i <= kSlotsPer; ++i) slots.push_back(std::to_string(i));
            slots.push_back("-");                                        // none picked yet (a tap on any row loads)
            addParam(mpc::choiceParam("browse_slot", "Browse Slot", slots, 0));
        }
        addParam(mpc::boolParam("load", "Load", false));
        addParam(mpc::boolParam("save", "Save", false));
        addParam(mpc::choiceParam("save_cat", "Save In", cats, 0));
        addParam(mpc::boolParam("confirm", "Confirm", false));
        addParam(mpc::boolParam("cancel", "Cancel", false));
        for (int k = 0; k < kNumKeys; ++k)
        {
            char id[16], nm[16];
            std::snprintf(id, sizeof id, "key_%d", k);
            if (k < kKeyBack) std::snprintf(nm, sizeof nm, "Key %s", kKeyChars[k] == ' ' ? "Space" : std::string(1, kKeyChars[k]).c_str());
            else std::snprintf(nm, sizeof nm, "%s", (const char*[]){"Backspace", "Left", "Right", "Clear"}[k - kKeyBack]);
            mpc::Param p = mpc::boolParam(id, nm, false); p.automatable = false; addParam(p);
        }
        for (const char* d : {"CPU", "Voices Now", "Status", "Name"})
        {
            std::string i(d); for (char& c : i) c = c == ' ' ? '_' : (char)std::tolower(c);
            mpc::Param p = mpc::floatParam(("disp_" + i).c_str(), d, 0.0f, 1.0f, 0.0f, "", 2); p.automatable = false; addParam(p);
        }
        std::vector<std::string> h1, h2;
        for (const auto& h : kHelpText) { h1.push_back(h[0]); h2.push_back(h[1]); }
        mpc::Param p1 = mpc::choiceParam("help", "Help", h1, 0), p2 = mpc::choiceParam("help2", "Help 2", h2, 0);
        p1.automatable = p2.automatable = false;
        addParam(p1); addParam(p2);
        // ---- 1.1: shapers ----
        std::vector<std::string> lib; for (int i = 0; i < kLibSize; ++i) lib.push_back(kLibrary[i].name);
        for (int side = 0; side < 2; ++side)
        {
            const char S = side ? 'B' : 'A', s = side ? 'b' : 'a';
            auto id = [&](const char* f) { char b[32]; std::snprintf(b, sizeof b, "%c_sh_%s", s, f); return std::string(b); };
            auto nm = [&](const char* f) { char b[32]; std::snprintf(b, sizeof b, "%c %s", S, f); return std::string(b); };
            addParam(mpc::choiceParam(id("start").c_str(), nm("Start From").c_str(), lib, 2));
            addParam(mpc::floatParam(id("bright").c_str(), nm("Bright").c_str(), -100.0f, 100.0f, 0.0f, "%", 0));
            addParam(mpc::floatParam(id("oddeven").c_str(), nm("Odd/Even").c_str(), -100.0f, 100.0f, 0.0f, "%", 0));
            addParam(mpc::floatParam(id("formant").c_str(), nm("Formant").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
            addParam(mpc::floatParam(id("fold").c_str(), nm("Fold").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
            addParam(mpc::floatParam(id("bend").c_str(), nm("Bend").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
            addParam(mpc::floatParam(id("sync").c_str(), nm("Sync").c_str(), 1.0f, 8.0f, 1.0f, "x", 2));
            addParam(mpc::floatParam(id("mutate").c_str(), nm("Mutate").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
            addParam(mpc::intParam(id("seed").c_str(), nm("Seed").c_str(), 0, 999, 1));
            addParam(mpc::floatParam(id("drawmix").c_str(), nm("Draw Mix").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
            addParam(mpc::choiceParam(id("smooth").c_str(), nm("Smooth").c_str(), {"Smooth", "Linear", "Steps"}, 0));
            addParam(mpc::boolParam(id("copy").c_str(), nm("Copy Start").c_str(), false));
            addParam(mpc::boolParam(id("clear").c_str(), nm("Clear Draw").c_str(), false));
            addParam(mpc::boolParam(id("undo").c_str(), nm("Undo Draw").c_str(), false));
            addParam(mpc::floatParam(id("draw_x").c_str(), nm("Draw X").c_str(), 0.0f, 1.0f, 0.5f, "", 2));
            addParam(mpc::floatParam(id("draw_y").c_str(), nm("Draw Y").c_str(), 0.0f, 1.0f, 0.5f, "", 2));
            for (int k = 0; k < kDrawPoints; ++k)
            {
                char pid[24], pnm[24];
                std::snprintf(pid, sizeof pid, "%c_pt_%02d", s, k); std::snprintf(pnm, sizeof pnm, "%c Pt %02d", S, k + 1);
                mpc::Param p = mpc::floatParam(pid, pnm, -1.0f, 1.0f, (float)std::sin(6.283185307179586 * k / kDrawPoints), "", 2);
                p.automatable = false; addParam(p);
            }
            for (int k = 0; k < kScopePoints; ++k)
            {
                char pid[24], pnm[24];
                std::snprintf(pid, sizeof pid, "%c_sc_%02d", s, k); std::snprintf(pnm, sizeof pnm, "%c Scope %02d", S, k + 1);
                mpc::Param p = mpc::floatParam(pid, pnm, -1.0f, 1.0f, 0.0f, "", 2);
                p.automatable = false; addParam(p);
            }
        }
        {   // the drop-down: the list the MPC shows is fixed in the skin, so user slots are listed as
            // "TAG User n"; the value shown on the page is always the preset's real name
            std::vector<std::string> names;
            for (int c = 0; c < kCats; ++c)
                for (int s = 0; s < kSlotsPer; ++s)
                    names.push_back(s < kFactoryPer ? std::string(kCatTags[c]) + " " + kFactoryNames[c][s]
                                                    : std::string(kCatTags[c]) + " User " + std::to_string(s - kFactoryPer + 1));
            addParam(mpc::choiceParam("preset_pick", "Preset", names, 0));
        }
        for (int k = 0; k < 16; ++k)
        {   // the list rows (display only): name of the browsed category's preset k, "> " when it is loaded
            char id[16], nm[16]; std::snprintf(id, sizeof id, "slot_%02d", k); std::snprintf(nm, sizeof nm, "Slot %02d", k + 1);
            mpc::Param p = mpc::floatParam(id, nm, 0.0f, 1.0f, 0.0f, "", 2); p.automatable = false; addParam(p);
        }
        static_assert(kSlotRow0 + 16 == kNumParams && kSlotsPer == 16, "parameter list");
        for (int i = 0; i < kNumParams; ++i) if (isSound(i)) soundList_.push_back(i);
        // shape engine: three table buffers per side (current, fading-out, being built)
        for (int side = 0; side < 2; ++side)
            for (int b = 0; b < 3; ++b) sh_[side].buf[b].reset(new WaveTable());

        buildPrograms();
        for (int side = 0; side < 2; ++side) { sh_[side].builtSig = shapeSig(side); buildSide(side, 0); sh_[side].cur = 0; sh_[side].inUse.store(1); sh_[side].scopeDirty = true; }
        const std::string& pf = mpc::pluginFolder();
        if (pf.find(" - VST - ") != std::string::npos) userFile_ = pf.substr(0, pf.find_last_of('/')) + "/.sureeel Elver user presets.bin";
        else userFile_ = (pf.empty() ? std::string(".") : pf) + "/user-presets.bin";
        loadUserPresets();
        skinFile_ = pf.find(" - VST - ") != std::string::npos ? pf + "/Plugin Skins/TUI.json" : (pf.empty() ? std::string() : pf + "/Plugin Skins/TUI.json");
        rng_.reseed((uint32_t)(reinterpret_cast<uintptr_t>(this) ^ 0xE1FEu) * 2654435761u);
        worker_ = std::thread([this] { writerLoop(); });
    }
    ~Elver() override { quit_.store(true); if (worker_.joinable()) worker_.join(); }

    // global settings: no preset changes them (a saved preset restores its skin in programLoaded)
    bool programSetsParam(int index) const override { return isSound(index) && index != kVolume && index != kOutput && index != kSkin; }
    void programLoaded(int index) override
    {
        progLoaded_.store(true);                                 // the preset list follows (a project restore does not call this)
        // a loaded preset's shapes are built right away (on the loading thread, never the audio thread), so
        // its first note already plays them; they are swapped in without a crossfade
        for (int side = 0; side < 2; ++side) buildNow(side);
        const int cat = index / kSlotsPer, slot = index % kSlotsPer;
        if (cat < kCats && slot >= kFactoryPer && userUsed_[cat][slot - kFactoryPer]) set(kSkin, (float)userSkin_[cat][slot - kFactoryPer]);
    }
    // an instrument is silent between notes: the framework's input-based watchdog does not apply;
    // Elver watches "notes held but output exactly zero" itself (see process)
    bool silenceIsExpected() const override { return true; }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        envDirty_ = true;
        crushRel_ = (float)std::exp(-1.0 / (0.2 * sampleRate));            // 200 ms release
        reset();
    }
    void reset() override
    {
        for (Voice& v : voices_) v.reset();
        lfoPh_[0] = lfoPh_[1] = 0.0f; lfoS_[0] = lfoS_[1] = 0.0f; ampS_ = 1.0f; shL_[0] = shL_[1] = 0.0f; crushHold_ = 0; crushL_ = crushR_ = 0.0f; crushEnv_ = 0.0f;
        gBlend_ = get(kBlend) * 0.01f; gAprev_ = std::cos(gBlend_ * kPi * 0.5f); gBprev_ = std::sin(gBlend_ * kPi * 0.5f); gVol_ = dbToLin(get(kVolume)) * dbToLin(get(kOutput));
        for (int b = kRandomize; b < kNumParams; ++b) btnLast_[b] = getBool(b);
        pickLast_ = getInt(kPresetPick); slotLast_ = getInt(kBrowseSlot); lastBrowseCat_ = getInt(kBrowseCat);   // a restore keeps the browse position (progLast_ is not reset: a load's reset must not hide the new preset number)
        silentHeld_ = 0;
        for (int side = 0; side < 2; ++side)
        {   // the drawing pad's current position is the baseline: a load never "draws"
            const int b = side ? kShB : kShA;
            sh_[side].lastX = get(b + kShDrawX); sh_[side].lastY = get(b + kShDrawY); sh_[side].lastDrawAt = -((int64_t)1 << 40);
        }
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const auto t0 = std::chrono::steady_clock::now();
        handleUi();
        if (statusChanged_ || nameChanged_ || slotsDirty_) publish();          // readouts follow taps at once
        // ---- MIDI -----------------------------------------------------------------------------------
        int ev = 0;
        const int n = ctx.numFrames;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        readSettings();
        for (int start = 0; start < n;)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= start) handleMidi(ctx.midi[ev++]);
            int end = n;
            if (ev < ctx.numMidiEvents) end = std::max(start + 1, std::min(n, ctx.midi[ev].sampleOffset));
            // render in chunks of at most 16 samples (the modulation rate)
            for (int c = start; c < end; c += 16) render(outL + c, outR ? outR + c : nullptr, std::min(16, end - c));
            start = end;
        }
        while (ev < ctx.numMidiEvents) handleMidi(ctx.midi[ev++]);

        // ---- watchdog: notes are sounding but the output is exactly zero for 1.5 s -> reset the engine,
        //      keeping the held notes
        bool anyNonZero = false;
        for (int i = 0; i < n && !anyNonZero; ++i) anyNonZero = outL[i] != 0.0f;
        int held = 0; for (const Voice& v : voices_) held += v.active && !v.released;
        if (held > 0 && !anyNonZero && expectSound_) silentHeld_ += n; else silentHeld_ = 0;
        if (silentHeld_ > (int)(1.5 * sr_)) { silentHeld_ = 0; softReset(); }

        // ---- CPU meter: our own time as a share of the block's real-time duration ----------------
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        const double share = us / (1e6 * n / sr_);
        cpu_ += (float)((share - cpu_) * 0.05);
        samples_ += n;
        if (++pubCount_ * n >= (int)(sr_ * 0.25)) { pubCount_ = 0; publish(); }
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char buf[64];
        switch (index)
        {
            case kCpu: std::snprintf(buf, sizeof buf, "%.1f %%", (double)(100.0f * cpuDisp_.load())); out = buf; return true;
            case kVoicesNow: std::snprintf(buf, sizeof buf, "%d / %d", voicesDisp_.load(), limitDisp_.load()); out = buf; return true;
            case kStatus: out = statusText(); return true;
            case kNameEdit: out = nameText(); return true;
            case kPresetPick: out = programName(clamp((int)std::lround(plain), 0, kCats * kSlotsPer - 1)); return true;
            default: break;
        }
        if (index >= kSlotRow0 && index < kSlotRow0 + 16)
        {
            const int k = index - kSlotRow0, cat = clamp(getInt(kBrowseCat), 0, kCats - 1), prog = cat * kSlotsPer + k;
            const bool empty = k >= kFactoryPer && !userUsed_[cat][k - kFactoryPer];
            std::string n = programName(prog);
            n = empty ? "(empty)" : n.substr(std::min(n.size(), std::strlen(kCatTags[cat]) + 1));
            out = (prog == currentProgram() ? "> " : "   ") + std::to_string(k + 1) + "  " + n;
            return true;
        }
        switch (index)
        {
            case kBrowseSlot:
            {
                if ((int)std::lround(plain) >= kSlotsPer) { out = "-"; return true; }
                const int prog = getInt(kBrowseCat) * kSlotsPer + (int)std::lround(plain);
                const std::string s = programName(prog);
                out = s.size() > 5 ? s.substr(s.find(' ') + 1) : s; return true;
            }
            default: break;
        }
        if (index == kBlend) { std::snprintf(buf, sizeof buf, "A %.0f / B %.0f", (double)(100 - plain), (double)plain); out = buf; return true; }
        const mpc::Param& p = param(index);
        if (p.kind == mpc::Param::Kind::Float && !p.unit.empty())
        {
            if (p.unit == "ms" && plain >= 1000.0f) std::snprintf(buf, sizeof buf, "%.2f s", (double)plain * 0.001);
            else if (p.unit == "Hz" && plain >= 1000.0f) std::snprintf(buf, sizeof buf, "%.2f kHz", (double)plain * 0.001);
            else std::snprintf(buf, sizeof buf, "%.*f %s", p.decimals, (double)plain, p.unit.c_str());
            out = (std::strncmp(buf, "-0 ", 3) == 0) ? buf + 1 : buf;
            return true;
        }
        return false;
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "Elver"; info.vendor = "sureeel";
        info.uniqueId = MPC_FOURCC('S', 'q', 'y', 'C');
        info.version = 1200; info.isInstrument = true; info.numInputs = 0; info.numOutputs = 2;
        return info;
    }

    // ---- per-block settings -------------------------------------------------------------------------
    void readSettings()
    {
        for (int side = 0; side < 2; ++side)
        {
            const int b = side ? kB : kA;
            SideSet& s = set_[side];
            for (int o = 0; o < 3; ++o)
            {
                s.wave[o] = getInt(b + kO + o * 6);
                s.level[o] = get(b + kO + o * 6 + 1) * 0.01f;
                s.ratio[o] = std::exp2((float)getInt(b + kO + o * 6 + 2) + (float)getInt(b + kO + o * 6 + 3) / 12.0f + get(b + kO + o * 6 + 4) / 1200.0f);
                s.pw[o] = get(b + kO + o * 6 + 5) * 0.01f;
            }
            s.uni = clamp(getInt(b + kUni), 1, kMaxUni); s.det = get(b + kDet); s.spr = get(b + kSpr) * 0.01f;
            s.mono = s.uni == 1 || s.spr <= 0.0f;
            s.ftype = getInt(b + kFType); s.cut = get(b + kCut); s.res = get(b + kRes) * 0.01f;
            s.fenvAmt = get(b + kFEnv) * 0.01f; s.keyTrk = get(b + kKeyTrk) * 0.01f;
            // envelope coefficients cost several exp / log each: recompute only when a setting moved
            float e[8];
            for (int k = 0; k < 8; ++k) e[k] = get(b + kFA + k);
            if (std::memcmp(e, envCache_[side], sizeof e) != 0 || envDirty_)
            {
                std::memcpy(envCache_[side], e, sizeof e);
                for (Voice& v : voices_)
                {
                    v.s[side].fenv.set(e[0], e[1], e[2] * 0.01f, e[3], sr_);
                    v.s[side].amp.set(e[4], e[5], e[6] * 0.01f, e[7], sr_);
                }
            }
        }
        envDirty_ = false;
        // shapes: a changed setting asks the worker for new tables; a finished build is swapped in
        for (int side = 0; side < 2; ++side)
        {
            ShapeSide& sh = sh_[side];
            const int b = side ? kShB : kShA;
            sh.wantSig.store(shapeSig(side));                    // the worker rebuilds when this differs from what it built
            const int p = sh.pending.load();
            if (p >= 0 && sh.prev < 0)
            {
                sh.prev = sh.cur; sh.cur = p; sh.fade = 0.0f; sh.pending.store(-1);
                sh.inUse.store((1 << sh.cur) | (1 << sh.prev));
                if (sh.snap.exchange(false)) { sh.prev = -1; sh.fade = 1.0f; sh.inUse.store(1 << sh.cur); }   // a loaded preset: no fade
                sh.scopeDirty = true;
            }
            if (sh.scopeDirty)
            {
                sh.scopeDirty = false;
                const WaveTable& w = *sh.buf[sh.cur];
                for (int k = 0; k < kScopePoints; ++k) { setNormalised(b + kShScope + k, clamp(0.5f + 0.5f * w.scope[k], 0.0f, 1.0f)); notifyHostParameterChanged(b + kShScope + k); }
            }
        }
        // pad Y target
        padDest_ = getInt(kPadDest); padY_ = get(kPadY) * 0.01f - 0.5f;   // -0.5 .. 0.5, centre = neutral
        if (padDest_ == 1)
        {   // morph: osc 1 -> osc 3 emphasis within each side
            const float m = padY_ * 2.0f;
            morph_[0] = clamp(1.0f - m, 0.0f, 2.0f); morph_[1] = 1.0f; morph_[2] = clamp(1.0f + m, 0.0f, 2.0f);
        }
        else morph_[0] = morph_[1] = morph_[2] = 1.0f;
        routing_ = getInt(kRouting);
        drift_ = get(kDrift) * 0.01f;
        drive_ = get(kDrive) * 0.01f + (padDest_ == 3 ? std::fmax(0.0f, padY_ * 2.0f) : 0.0f);
        grit_ = get(kGrit) * 0.01f; ring_ = get(kRing) * 0.01f;
        for (int l = 0; l < 2; ++l)
        {
            lfoRate_[l] = get(l ? kL2Rate : kL1Rate); lfoDepth_[l] = get(l ? kL2Depth : kL1Depth) * 0.01f;
            lfoShape_[l] = getInt(l ? kL2Shape : kL1Shape); lfoDest_[l] = getInt(l ? kL2Dest : kL1Dest);
        }
        // voice budget: units per voice (each sounding side 1, plus its running oscillator copies)
        int per = 0, oscOn = 0;
        const float bl = get(kBlend) * 0.01f;
        for (int side = 0; side < 2; ++side)
        {
            if ((side == 0 && bl >= 0.999f) || (side == 1 && bl <= 0.001f)) continue;
            ++per;
            for (int o = 0; o < 3; ++o) if (set_[side].level[o] > 0.0f) { per += set_[side].wave[o] == kNoise ? 1 : set_[side].uni; ++oscOn; }
        }
        limit_ = clamp(kBudget / std::max(1, per), 1, getInt(kVoices));
        expectSound_ = oscOn > 0 && get(kVolume) > -47.0f;
        volT_ = dbToLin(get(kVolume)) * dbToLin(get(kOutput));
        blendT_ = bl;
    }

    // ---- MIDI ------------------------------------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn()) noteOn(e.note(), e.velocity() / 127.0f);
        else if (e.isNoteOff()) noteOff(e.note());
        else if (e.isController())
        {
            if (e.controller() == 64) { sustain_ = e.data[2] >= 64; if (!sustain_) for (Voice& v : voices_) if (v.active && v.pedalHeld_) releaseVoice(v); }
            if (e.controller() == 123 || e.controller() == 120) { for (Voice& v : voices_) if (v.active) releaseVoice(v); if (e.controller() == 120) for (Voice& v : voices_) v.reset(); }
        }
        else if (e.isPitchBend()) bend_ = ((e.data[1] | (e.data[2] << 7)) - 8192) / 8192.0f;
    }
    void noteOn(int note, float vel)
    {
        int active = 0; for (const Voice& v : voices_) active += v.active;
        Voice* pick = nullptr;
        // the same note held again: retrigger that voice
        for (Voice& v : voices_) if (v.active && v.note == note) { pick = &v; break; }
        if (!pick && active < limit_) for (Voice& v : voices_) if (!v.active) { pick = &v; break; }
        if (!pick)
        {   // steal: the oldest released voice, else the oldest
            uint32_t best = 0xffffffff;
            for (Voice& v : voices_) if (v.active && v.released && v.age < best) { best = v.age; pick = &v; }
            if (!pick) for (Voice& v : voices_) if (v.active && v.age < best) { best = v.age; pick = &v; }
            if (!pick) pick = &voices_[0];
        }
        Voice& v = *pick;
        const bool fresh = !v.active;
        v.active = true; v.released = false; v.pedalHeld_ = false; v.note = note; v.vel = vel; v.age = ++ageCounter_;
        v.rng.reseed((uint32_t)(note * 2654435761u) ^ ageCounter_);
        for (int side = 0; side < 2; ++side)
        {
            SideState& s = v.s[side];
            if (fresh)
            {
                s.filt.reset();
                for (int o = 0; o < 3; ++o) for (int u = 0; u < kMaxUni; ++u) s.ph[o][u] = v.rng.nextFloat();   // free-running phases
            }
            s.amp.noteOn(); s.fenv.noteOn();
        }
        // too many voices for the budget: release the oldest beyond the limit
        int count = 0; for (const Voice& w : voices_) count += w.active;
        while (count > limit_)
        {
            Voice* old = nullptr; uint32_t best = 0xffffffff;
            for (Voice& w : voices_) if (w.active && &w != &v && w.age < best) { best = w.age; old = &w; }
            if (!old) break;
            old->reset(); --count;
        }
    }
    void noteOff(int note)
    {
        for (Voice& v : voices_)
            if (v.active && !v.released && v.note == note) { if (sustain_) v.pedalHeld_ = true; else releaseVoice(v); }
    }
    void releaseVoice(Voice& v) { v.released = true; v.pedalHeld_ = false; for (SideState& s : v.s) { s.amp.noteOff(); s.fenv.noteOff(); } }

    // ---- rendering (at most 16 samples: modulation is updated once per chunk) ------------------------
    float lfoValue(int l)
    {
        const float p = lfoPh_[l];
        switch (lfoShape_[l])
        {
            case 1: return 1.0f - 4.0f * std::fabs(p - 0.5f);
            case 2: return 2.0f * p - 1.0f;
            case 3: return p < 0.5f ? 1.0f : -1.0f;
            case 4: return shL_[l];
            default: return sin01(p);
        }
    }
    void render(float* L, float* R, int n)
    {
        // LFOs (global), advanced per chunk
        float pitchSemi = bend_ * 2.0f, filtOct = 0.0f, ampMod = 1.0f, blendMod = 0.0f;
        for (int l = 0; l < 2; ++l)
        {
            const float before = lfoPh_[l];
            lfoPh_[l] += lfoRate_[l] * (float)n / (float)sr_;
            if (lfoPh_[l] >= 1.0f) { lfoPh_[l] -= std::floor(lfoPh_[l]); shL_[l] = lfoRng_.nextBipolar(); }
            (void)before;
            // ~1.5 ms slew: square and S&H steps move smoothly instead of clicking
            lfoS_[l] += (lfoValue(l) - lfoS_[l]) * std::fmin(1.0f, (float)n / 64.0f);
            const float lv = lfoS_[l], v = lv * lfoDepth_[l];
            switch (lfoDest_[l])
            {
                case 0: pitchSemi += 2.0f * v; break;
                case 1: filtOct += 3.0f * v; break;
                case 2: ampMod *= 1.0f - 0.5f * lfoDepth_[l] * (1.0f - lv); break;
                default: blendMod += 0.5f * v; break;
            }
        }
        gBlend_ += (blendT_ - gBlend_) * std::fmin(1.0f, 0.004f * n);
        const float b = clamp(gBlend_ + blendMod, 0.0f, 1.0f);
        const float gA = std::cos(b * kPi * 0.5f), gB = std::sin(b * kPi * 0.5f);
        // the blend gains ramp sample by sample from the last chunk's values (an LFO on Blend never zips);
        // a side keeps rendering while it fades out
        const float gA0 = gAprev_, gB0 = gBprev_, dA = (gA - gA0) / (float)n, dB = (gB - gB0) / (float)n;
        gAprev_ = gA; gBprev_ = gB;
        set_[0].on = std::fmax(gA, gA0) > 1e-4f; set_[1].on = std::fmax(gB, gB0) > 1e-4f;
        const float cutPad = padDest_ == 0 ? padY_ * 6.0f : 0.0f;          // +-3 octaves
        const float resPad = padDest_ == 2 ? padY_ * 0.8f : 0.0f;
        float tmpL[16], tmpR[16];
        for (int i = 0; i < n; ++i) { tmpL[i] = 0.0f; tmpR[i] = 0.0f; }
        int sounding = 0;
        for (Voice& v : voices_)
        {
            if (!v.active) continue;
            ++sounding;
            const float drift = drift_ > 0.0f ? v.drift.next(v.rng, n) * drift_ : 0.0f;
            const float noteSemi = (float)(v.note - 69) + pitchSemi + drift * 0.3f;
            const float baseHz = 440.0f * std::exp2(noteSemi / 12.0f);
            const float vel = 0.4f + 0.6f * v.vel;
            float sideL[2][16], sideR[2][16];
            float envA[2], envF[2];
            for (int side = 0; side < 2; ++side)
            {
                SideState& s = v.s[side];
                const SideSet& st = set_[side];
                // envelopes: one step per chunk; the amp is ramped linearly across the chunk
                float ampEnv[16];
                const float a0 = s.amp.lvl, a1 = s.amp.advance(n), fe = s.fenv.advance(n);
                { const float d = (a1 - a0) / (float)n; float a = a0; for (int i = 0; i < n; ++i) { a += d; ampEnv[i] = a; } }
                envA[side] = a1; envF[side] = fe;
                // filter coefficients for this chunk - also for a side Blend has switched off: in series
                // routing its filter keeps running, and stale coefficients would jump when it comes back
                const float kt = st.keyTrk * (float)(v.note - 60) / 12.0f;
                const float oct = kt + st.fenvAmt * 6.0f * fe + filtOct + cutPad + drift * 0.5f;
                s.filt.set(st.ftype, st.cut * std::exp2(oct), clamp(st.res + resPad, 0.0f, 1.0f), sr_);
                if (!st.on)
                {   // in parallel routing an unused filter restarts from silence when the side comes back
                    if (routing_ == 0) s.filt.reset();
                    for (int i = 0; i < n; ++i) { sideL[side][i] = sideR[side][i] = 0.0f; }
                    continue;
                }
                // oscillators
                // a side without unison spread is mono: one channel is summed and copied
                float* accL = sideL[side];
                float* accR = st.mono ? nullptr : sideR[side];
                for (int i = 0; i < n; ++i) accL[i] = 0.0f;
                if (accR) for (int i = 0; i < n; ++i) accR[i] = 0.0f;
                float o1[16] = {}, o2[16] = {};
                const bool ringOn = ring_ > 0.0f;
                const float monoG = 0.70710678f;                                    // centre pan gain
                for (int o = 0; o < 3; ++o)
                {
                    const float lvl = st.level[o] * morph_[o];
                    if (lvl <= 0.0f) continue;
                    float* tap = ringOn && o < 2 ? (o == 0 ? o1 : o2) : nullptr;
                    if (st.wave[o] == kNoise)
                    {
                        const float g = lvl * 0.6f * monoG;
                        for (int i = 0; i < n; ++i) { const float y = v.rng.nextBipolar(); accL[i] += y * g; if (accR) accR[i] += y * g; if (tap) tap[i] = y * 0.6f; }
                        continue;
                    }
                    const int U = st.uni;
                    const float norm = lvl / std::sqrt((float)U);
                    for (int u = 0; u < U; ++u)
                    {
                        const float pos = U > 1 ? (float)u / (float)(U - 1) * 2.0f - 1.0f : 0.0f;   // -1 .. 1
                        const float hz = baseHz * st.ratio[o] * (U > 1 ? std::exp2(pos * st.det / 1200.0f) : 1.0f);
                        const float inc = clamp(hz / (float)sr_, 0.0f, 0.45f);
                        float gl = norm * monoG, gr = gl;
                        if (accR) { const float pan = pos * st.spr; gl = norm * std::sqrt(0.5f * (1.0f - pan)); gr = norm * std::sqrt(0.5f * (1.0f + pan)); }
                        if (st.wave[o] == kShape)
                        {
                            const ShapeSide& sh = sh_[side];
                            tableRun(s.ph[o][u], inc, sh.buf[sh.cur].get(), sh.prev >= 0 ? sh.buf[sh.prev].get() : nullptr, sh.fade, kShapeFadeStep,
                                     gl, gr, accL, accR, u == 0 ? tap : nullptr, n);
                        }
                        else oscRun(st.wave[o], s.ph[o][u], inc, st.pw[o], gl, gr, accL, accR, u == 0 ? tap : nullptr, n);
                    }
                }
                if (ringOn)
                {
                    const float keep = 1.0f - 0.5f * ring_, rg = ring_ * 0.8f * monoG;
                    for (int i = 0; i < n; ++i) accL[i] = accL[i] * keep + o1[i] * o2[i] * rg;
                    if (accR) for (int i = 0; i < n; ++i) accR[i] = accR[i] * keep + o1[i] * o2[i] * rg;
                }
                if (!accR) for (int i = 0; i < n; ++i) sideR[side][i] = sideL[side][i];
                // parallel: each side through its own filter, then its amp envelope
                if (routing_ == 0 && st.mono)
                {
                    for (int i = 0; i < n; ++i) sideL[side][i] = sideR[side][i] = s.filt.run(0, st.ftype, sideL[side][i]) * ampEnv[i];
                    s.filt.syncRight();
                }
                else if (routing_ == 0)
                    for (int i = 0; i < n; ++i)
                    {
                        sideL[side][i] = s.filt.run(0, st.ftype, sideL[side][i]) * ampEnv[i];
                        sideR[side][i] = s.filt.run(1, st.ftype, sideR[side][i]) * ampEnv[i];
                    }
                else
                    for (int i = 0; i < n; ++i) { sideL[side][i] *= ampEnv[i]; sideR[side][i] *= ampEnv[i]; }
            }
            const bool seriesMono = (set_[0].mono || !set_[0].on) && (set_[1].mono || !set_[1].on);
            for (int i = 0; i < n; ++i)
            {
                const float ga = gA0 + dA * (float)(i + 1), gb = gB0 + dB * (float)(i + 1);
                float l = sideL[0][i] * ga + sideL[1][i] * gb, r = sideR[0][i] * ga + sideR[1][i] * gb;
                if (routing_ == 1)
                {   // series: both sides -> A filter -> B filter (one channel when neither side is spread)
                    l = v.s[0].filt.run(0, set_[0].ftype, l); l = v.s[1].filt.run(0, set_[1].ftype, l);
                    if (seriesMono) r = l;
                    else { r = v.s[0].filt.run(1, set_[0].ftype, r); r = v.s[1].filt.run(1, set_[1].ftype, r); }
                }
                tmpL[i] += l * vel; tmpR[i] += r * vel;
            }
            if (routing_ == 1 && seriesMono) { v.s[0].filt.syncRight(); v.s[1].filt.syncRight(); }
            // the voice ends when both amp envelopes are idle
            if (!v.s[0].amp.isActive() && !v.s[1].amp.isActive()) v.reset();
        }
        voicesNow_ = sounding;
        for (ShapeSide& sh : sh_)
            if (sh.prev >= 0) { sh.fade += kShapeFadeStep * (float)n; if (sh.fade >= 1.0f) { sh.fade = 1.0f; sh.prev = -1; sh.inUse.store(1 << sh.cur); } }
        // master: drive, grit, volume, soft limit
        const float dg = 1.0f + 9.0f * drive_ * drive_;
        const int hold = grit_ > 0.0f ? 1 + (int)(grit_ * 7.0f) : 1;
        // Grit crushes relative to the signal's own level (a peak follower), so it sounds the same loud or quiet
        // and never rounds a quiet sound away to silence: 9 bits at a touch of grit, under 3 bits at full
        const float q = grit_ > 0.0f ? std::exp2(9.0f - 6.5f * grit_) : 0.0f;
        for (int i = 0; i < n; ++i)
        {
            gVol_ += (volT_ - gVol_) * 0.002f;
            ampS_ += (ampMod - ampS_) * 0.01f;
            float l = tmpL[i] * ampS_ * 0.35f, r = tmpR[i] * ampS_ * 0.35f;
            if (drive_ > 0.0f) { l = sclip(l * dg) / std::sqrt(dg) * 1.3f; r = sclip(r * dg) / std::sqrt(dg) * 1.3f; }
            if (grit_ > 0.0f)
            {
                crushEnv_ = std::fmax(std::fmax(std::fabs(l), std::fabs(r)), crushEnv_ * crushRel_);
                if (++crushHold_ >= hold)
                {
                    crushHold_ = 0;
                    const float e = std::fmax(crushEnv_, 1e-5f);
                    crushL_ = std::round(l / e * q) / q * e; crushR_ = std::round(r / e * q) / q * e;
                }
                l = crushL_; r = crushR_;
            }
            l *= gVol_; r *= gVol_;
            // soft limit: transparent below about -6 dBFS, never above -0.5 dBFS
            l = 0.944f * sclip(l / 0.944f * 1.0f); r = 0.944f * sclip(r / 0.944f);
            L[i] = l; if (R) R[i] = r;
        }
    }
    void softReset()
    {   // clear every filter and phase, keep the held notes playing
        for (Voice& v : voices_) for (SideState& s : v.s) { s.filt.reset(); }
        crushL_ = crushR_ = 0.0f;
        ++watchdogResets_;
    }

    // ---- UI: buttons act on every change of their switch (what works on the MPC touch screen) ------
    bool acted(int idx)
    {
        const bool now = getBool(idx);
        if (now == btnLast_[idx]) return false;
        btnLast_[idx] = now;
        if (samples_ - btnTime_[idx] < (int64_t)(0.25 * sr_)) return false;
        btnTime_[idx] = samples_;
        return true;
    }
    void setAndTell(int p, float plain) { set(p, plain); notifyHostParameterChanged(p); }
    void handleUi()
    {
        if (getInt(kHelp2) != getInt(kHelp)) { set(kHelp2, (float)getInt(kHelp)); notifyHostParameterChanged(kHelp2); }
        if (acted(kRandomize)) randomize();
        if (acted(kUndo)) undo();
        if (getInt(kBrowseCat) != lastBrowseCat_)
        {   // a new category: nothing picked in it yet, so a tap on any row loads
            lastBrowseCat_ = getInt(kBrowseCat);
            if (getInt(kBrowseSlot) != kSlotsPer) setAndTell(kBrowseSlot, (float)kSlotsPer);
            slotLast_ = kSlotsPer; slotsDirty_ = true;
        }
        if (getInt(kBrowseSlot) != slotLast_)
        {
            slotLast_ = getInt(kBrowseSlot);
            if (slotLast_ < kSlotsPer)
            {
                const int cat = getInt(kBrowseCat), prog = cat * kSlotsPer + slotLast_;
                if (slotLast_ >= kFactoryPer && !userUsed_[cat][slotLast_ - kFactoryPer]) status_ = kStEmpty;
                else { setProgram(prog); progLast_ = prog; status_ = kStLoaded; loadedProg_ = prog; requestDisplayUpdate(); }
                statusChanged_ = true; slotsDirty_ = true;
            }
        }
        if (acted(kLoad))
        {
            const int slot = std::min(getInt(kBrowseSlot), kSlotsPer - 1);
            const int prog = getInt(kBrowseCat) * kSlotsPer + slot;
            if (slot >= kFactoryPer && !userUsed_[getInt(kBrowseCat)][slot - kFactoryPer]) { status_ = kStEmpty; }
            else { setProgram(prog); status_ = kStLoaded; loadedProg_ = prog; requestDisplayUpdate(); }
            statusChanged_ = true;
        }
        // the preset drop-down: a new pick loads that preset; a preset loaded any other way moves the pick
        if (progLoaded_.exchange(false))
        {
            progLast_ = currentProgram();
            if (getInt(kPresetPick) != progLast_) setAndTell(kPresetPick, (float)progLast_);
            pickLast_ = progLast_;
            // the list shows the loaded preset's category, with it marked
            const int cat = progLast_ / kSlotsPer, slot = progLast_ % kSlotsPer;
            if (getInt(kBrowseCat) != cat) setAndTell(kBrowseCat, (float)cat);
            if (getInt(kBrowseSlot) != slot) setAndTell(kBrowseSlot, (float)slot);
            lastBrowseCat_ = cat; slotLast_ = slot; slotsDirty_ = true;
        }
        if (getInt(kPresetPick) != pickLast_)
        {
            pickLast_ = getInt(kPresetPick);
            const int cat = pickLast_ / kSlotsPer, slot = pickLast_ % kSlotsPer;
            if (slot >= kFactoryPer && !userUsed_[cat][slot - kFactoryPer]) status_ = kStEmpty;
            else { setProgram(pickLast_); progLast_ = pickLast_; status_ = kStLoaded; loadedProg_ = pickLast_; requestDisplayUpdate(); }
            statusChanged_ = true;
        }
        if (acted(kSave)) startNaming();
        if (acted(kConfirm)) { if (naming_) confirmSave(); else startNaming(); }
        if (acted(kCancel) && naming_) { naming_ = false; status_ = kStCancelled; statusChanged_ = true; nameChanged_ = true; }
        for (int k = 0; k < kNumKeys; ++k) if (acted(kKey0 + k)) typeKey(k);
        for (int side = 0; side < 2; ++side) drawUi(side);
        const int done = saveDone_.exchange(-1);
        if (done >= 0) { status_ = kStSaved; savedProg_ = done; statusChanged_ = true; nameChanged_ = true; slotsDirty_ = true; requestDisplayUpdate(); }
    }

    // ---- drawing: the DRAW page's XY pad paints the 64 points; strokes are filled in between touches ----
    void setPoint(int side, int k, float v)
    {
        const int p = (side ? kShB : kShA) + kShPts + k;
        if (std::fabs(get(p) - v) > 1e-4f) { set(p, v); notifyHostParameterChanged(p); }
    }
    // drawing must be heard: if none of this side's sounding oscillators plays Shape, oscillator 1 is set to
    // Shape (and given a level if it had none), and Draw Mix goes to 100 %
    void makeDrawingAudible(int side)
    {
        const int b = side ? kShB : kShA, o0 = side ? kB : kA;
        if (get(b + kShDrawMix) <= 0.0f) setAndTell(b + kShDrawMix, 100.0f);
        for (int o = 0; o < 3; ++o)
            if (getInt(o0 + kO + o * 6) == kShape && get(o0 + kO + o * 6 + 1) > 0.0f) return;
        setAndTell(o0 + kO, (float)kShape);
        if (get(o0 + kO + 1) <= 0.0f) setAndTell(o0 + kO + 1, 80.0f);
    }
    void pushDrawUndo(int side)
    {
        ShapeSide& sh = sh_[side];
        const int b = side ? kShB : kShA;
        if (sh.undoN == kDrawUndo) { std::memmove(sh.undo[0], sh.undo[1], sizeof(sh.undo[0]) * (kDrawUndo - 1)); --sh.undoN; }
        for (int k = 0; k < kDrawPoints; ++k) sh.undo[sh.undoN][k] = get(b + kShPts + k);
        ++sh.undoN;
    }
    void drawUi(int side)
    {
        ShapeSide& sh = sh_[side];
        const int b = side ? kShB : kShA;
        const float x = get(b + kShDrawX), y = get(b + kShDrawY);
        if (x != sh.lastX || y != sh.lastY)
        {
            sh.lastX = x; sh.lastY = y;
            const int k = clamp((int)std::lround(x * (kDrawPoints - 1)), 0, kDrawPoints - 1);
            const float v = clamp(y * 2.0f - 1.0f, -1.0f, 1.0f);
            if (samples_ - sh.lastDrawAt > (int64_t)(0.3 * sr_))
            {   // a new stroke
                pushDrawUndo(side);
                makeDrawingAudible(side);                                // drawing takes over, and is heard
                setPoint(side, k, v);
            }
            else
            {   // fill every point between the last touch and this one
                const int dir = k >= sh.lastIdx ? 1 : -1;
                for (int j = sh.lastIdx; ; j += dir)
                {
                    const float t = k == sh.lastIdx ? 1.0f : (float)(j - sh.lastIdx) / (float)(k - sh.lastIdx);
                    setPoint(side, j, sh.lastVal + (v - sh.lastVal) * t);
                    if (j == k) break;
                }
            }
            sh.lastIdx = k; sh.lastVal = v; sh.lastDrawAt = samples_;
        }
        if (acted(b + kShCopy))
        {
            pushDrawUndo(side);
            const WaveTable& w = *sh.buf[sh.cur];
            for (int k = 0; k < kDrawPoints; ++k) setPoint(side, k, w.start[k]);
            makeDrawingAudible(side);
        }
        if (acted(b + kShClear)) { pushDrawUndo(side); for (int k = 0; k < kDrawPoints; ++k) setPoint(side, k, 0.0f); }
        if (acted(b + kShUndo) && sh.undoN > 0)
        {
            --sh.undoN;
            for (int k = 0; k < kDrawPoints; ++k) setPoint(side, k, sh.undo[sh.undoN][k]);
        }
    }

    // ---- shapes: built on the worker thread -------------------------------------------------------------
    void readShape(int side, ShapeSettings& s) const
    {
        const int b = side ? kShB : kShA;
        s.start = getInt(b + kShStart); s.bright = get(b + kShBright) * 0.008f; s.oddEven = get(b + kShOddEven) * 0.01f;
        s.formant = get(b + kShFormant) * 0.01f; s.fold = get(b + kShFold) * 0.01f; s.bend = get(b + kShBend) * 0.01f;
        s.sync = get(b + kShSync); s.mutate = get(b + kShMutate) * 0.01f; s.seed = getInt(b + kShSeed);
        s.drawMix = get(b + kShDrawMix) * 0.01f; s.smooth = getInt(b + kShSmooth);
        for (int k = 0; k < kDrawPoints; ++k) s.points[k] = clamp(get(b + kShPts + k), -1.0f, 1.0f);
    }
    void buildSide(int side, int into)
    {
        ShapeSide& sh = sh_[side];
        ShapeSettings s; readShape(side, s);
        if (s.start != sh.baseIdx) { libraryCycle(s.start, fft_, sh.base); sh.baseIdx = s.start; }
        WaveTable& w = *sh.buf[into];
        buildShape(s, sh.base, fft_, w);
        for (int k = 0; k < kDrawPoints; ++k) w.start[k] = (float)sh.base[k * kWaveN / kDrawPoints];
    }
    // a fingerprint of a side's shape settings (any thread: the parameters are atomic)
    uint64_t shapeSig(int side) const
    {
        const int b = side ? kShB : kShA;
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](float x) { uint32_t u; std::memcpy(&u, &x, 4); h = (h ^ u) * 1099511628211ull; };
        for (int k = kShStart; k <= kShSmooth; ++k) mix(getNormalised(b + k));
        for (int k = 0; k < kDrawPoints; ++k) mix(getNormalised(b + kShPts + k));
        return h;
    }
    // builds a side's tables now (loading thread) and hands them to the audio thread to swap in at once
    void buildNow(int side)
    {
        std::lock_guard<std::mutex> lk(buildMu_);
        ShapeSide& sh = sh_[side];
        if (sh.pending.load() >= 0) return;                       // one is already waiting: the audio thread takes it first
        const int used = sh.inUse.load();
        int into = 0; while (into < 3 && (used & (1 << into))) ++into;
        if (into >= 3) return;
        sh.builtSig = shapeSig(side);
        buildSide(side, into);
        sh.snap.store(true);
        sh.pending.store(into);
    }
    void shapeWork()
    {
        std::lock_guard<std::mutex> lk(buildMu_);
        for (int side = 0; side < 2; ++side)
        {
            ShapeSide& sh = sh_[side];
            if (sh.wantSig.load() == sh.builtSig || sh.pending.load() >= 0) continue;
            const int used = sh.inUse.load();
            int into = 0; while (into < 3 && (used & (1 << into))) ++into;
            if (into >= 3) continue;
            sh.builtSig = shapeSig(side);                          // what is read now is what gets built
            buildSide(side, into);
            sh.pending.store(into);
        }
    }

    // ---- randomize ------------------------------------------------------------------------------------
    // Each mood sets every sound parameter inside its own ranges; Randomize Amount moves the current
    // sound that far toward it. Locks skip sections; Volume and Output are never touched.
    struct Target { float v[kNumParams]; };
    float rnd(float a, float b) { return a + (b - a) * rng_.nextFloat(); }
    float rlog(float a, float b) { return a * std::pow(b / a, rng_.nextFloat()); }
    int pick(std::initializer_list<int> c) { const int n = (int)c.size(); return *(c.begin() + std::min(n - 1, (int)(rng_.nextFloat() * n))); }
    void moodTarget(int cat, Target& t)
    {
        for (int i : soundList_) t.v[i] = get(i);
        // oscillators
        static const int kIntervals[] = {0, 0, 0, 7, 12, -12, 5, 3, 4};
        static const int kHarsh[] = {1, 6, 11, -6, 13, 18, -11};
        for (int side = 0; side < 2; ++side)
        {
            const int b = side ? kB : kA;
            const bool harsh = cat == 5 || cat == 6;
            const int nOsc = cat == 2 || cat == 3 || cat == 7 ? 1 + (int)(rng_.nextFloat() * 2) : 1 + (int)(rng_.nextFloat() * 3);
            for (int o = 0; o < 3; ++o)
            {
                const int p = b + kO + o * 6;
                int w;
                switch (cat)
                {
                    case 0: w = pick({kSine, kSine, kTri, kTri, kSaw}); break;
                    case 1: w = pick({kSaw, kTri, kSaw, kNoise, kSquare}); break;
                    case 2: w = pick({kTri, kSaw, kSquare, kSine}); break;
                    case 3: w = pick({kSaw, kSaw, kSquare, kSquare, kTri}); break;
                    case 4: w = pick({kTri, kSaw, kSquare, kSine}); break;
                    case 5: w = pick({kSquare, kSaw, kNoise, kSquare}); break;
                    case 7: w = pick({kSaw, kSaw, kSquare, kSquare, kTri, kSaw}); break;
                    default: w = pick({kNoise, kSquare, kSaw, kNoise, kSine}); break;
                }
                t.v[p] = (float)w;
                t.v[p + 1] = o < nOsc ? rnd(o == 0 ? 60.0f : 25.0f, o == 0 ? 90.0f : 70.0f) : 0.0f;
                int oc = cat == 3 && side == 1 ? pick({-2, -1, -1, 0}) : pick({0, 0, 0, -1, 1});
                if (cat == 0) oc = pick({0, 0, -1, 1});
                if (cat == 7) oc = side == 1 ? pick({-1, -1, 0}) : pick({0, 0, 1});   // funk: octave basses
                t.v[p + 2] = (float)oc;
                t.v[p + 3] = (float)(harsh ? kHarsh[(int)(rng_.nextFloat() * 7) % 7] * (o > 0) : (o == 0 ? 0 : kIntervals[(int)(rng_.nextFloat() * 9) % 9]));
                t.v[p + 4] = cat == 4 ? rnd(-35.0f, 35.0f) : (harsh ? rnd(-50.0f, 50.0f) : rnd(-6.0f, 6.0f));
                t.v[p + 5] = harsh ? rnd(8.0f, 92.0f) : (cat == 7 ? rnd(12.0f, 50.0f) : rnd(25.0f, 75.0f));
            }
            const float uniMax = cat == 2 || cat == 3 ? 2.0f : (cat == 7 ? 3.0f : 4.0f);
            t.v[b + kUni] = std::floor(rnd(1.0f, uniMax + 0.99f));
            t.v[b + kDet] = cat == 4 ? rnd(20.0f, 60.0f) : (cat == 6 ? rnd(0.0f, 100.0f) : rnd(4.0f, 25.0f));
            t.v[b + kSpr] = rnd(30.0f, 100.0f);
            // filter
            switch (cat)
            {
                case 0: t.v[b + kFType] = (float)pick({kLP24, kLP12, kLP12}); t.v[b + kCut] = rlog(300, 3000); t.v[b + kRes] = rnd(0, 35); t.v[b + kFEnv] = rnd(0, 30); break;
                case 1: t.v[b + kFType] = (float)pick({kLP24, kLP12, kBP}); t.v[b + kCut] = rlog(400, 4000); t.v[b + kRes] = rnd(10, 45); t.v[b + kFEnv] = rnd(5, 35); break;
                case 2: t.v[b + kFType] = (float)pick({kLP24, kLP12}); t.v[b + kCut] = rlog(400, 3000); t.v[b + kRes] = rnd(0, 40); t.v[b + kFEnv] = rnd(35, 80); break;
                case 3: t.v[b + kFType] = (float)pick({kLP24, kLP24, kLP12}); t.v[b + kCut] = rlog(200, 2200); t.v[b + kRes] = rnd(20, 70); t.v[b + kFEnv] = rnd(30, 85); break;
                case 4: t.v[b + kFType] = (float)pick({kLP12, kLP24}); t.v[b + kCut] = rlog(300, 1600); t.v[b + kRes] = rnd(10, 40); t.v[b + kFEnv] = rnd(-10, 30); break;
                case 5: t.v[b + kFType] = (float)pick({kBP, kHP, kNotch, kLP24}); t.v[b + kCut] = rlog(300, 6000); t.v[b + kRes] = rnd(60, 95); t.v[b + kFEnv] = rnd(-60, 80); break;
                case 7: t.v[b + kFType] = (float)pick({kLP24, kLP24, kLP12}); t.v[b + kCut] = rlog(500, 3500); t.v[b + kRes] = rnd(10, 45); t.v[b + kFEnv] = rnd(30, 80); break;
                default: t.v[b + kFType] = (float)pick({kLP24, kLP12, kBP, kHP, kNotch}); t.v[b + kCut] = rlog(60, 15000); t.v[b + kRes] = rnd(0, 95); t.v[b + kFEnv] = rnd(-100, 100); break;
            }
            t.v[b + kKeyTrk] = rnd(20, 80);
            // envelopes: amp and filter
            float aA, aD, aS, aR, fA, fD, fS, fR;
            switch (cat)
            {
                case 0: aA = rlog(1500, 8000); aD = rlog(1000, 6000); aS = rnd(60, 100); aR = rlog(5000, 20000); fA = rlog(1000, 8000); fD = rlog(1000, 8000); fS = rnd(30, 80); fR = rlog(3000, 15000); break;
                case 1: aA = rlog(300, 2500); aD = rlog(400, 3000); aS = rnd(60, 95); aR = rlog(1000, 6000); fA = rlog(200, 2500); fD = rlog(500, 3000); fS = rnd(30, 70); fR = rlog(800, 5000); break;
                case 2: aA = rlog(1, 10); aD = rlog(100, 800); aS = rnd(0, 40); aR = rlog(100, 800); fA = rlog(1, 8); fD = rlog(80, 500); fS = rnd(0, 30); fR = rlog(80, 600); break;
                case 3: aA = rlog(1, 20); aD = rlog(100, 600); aS = rnd(40, 90); aR = rlog(50, 300); fA = rlog(1, 30); fD = rlog(100, 400); fS = rnd(10, 50); fR = rlog(50, 300); break;
                case 4: aA = rlog(5, 200); aD = rlog(200, 1500); aS = rnd(40, 90); aR = rlog(300, 2000); fA = rlog(5, 300); fD = rlog(200, 1500); fS = rnd(20, 70); fR = rlog(300, 2000); break;
                case 5: aA = rlog(1, 10); aD = rlog(50, 300); aS = rnd(20, 60); aR = rlog(50, 400); fA = rlog(1, 10); fD = rlog(30, 250); fS = rnd(0, 40); fR = rlog(50, 300); break;
                case 7: aA = rlog(1, 12); aD = rlog(120, 700); aS = rnd(20, 80); aR = rlog(60, 400); fA = rlog(1, 10); fD = rlog(80, 500); fS = rnd(10, 50); fR = rlog(60, 400); break;
                default: aA = rlog(1, 3000); aD = rlog(20, 4000); aS = rnd(0, 100); aR = rlog(20, 8000); fA = rlog(1, 3000); fD = rlog(20, 4000); fS = rnd(0, 100); fR = rlog(20, 8000); break;
            }
            t.v[b + kAA] = aA; t.v[b + kAD] = aD; t.v[b + kAS] = aS; t.v[b + kAR] = aR;
            t.v[b + kFA] = fA; t.v[b + kFD] = fD; t.v[b + kFS] = fS; t.v[b + kFR] = fR;
        }
        // blend, pad, routing
        t.v[kBlend] = rnd(15, 85); t.v[kPadY] = rnd(30, 70); t.v[kPadDest] = (float)pick({0, 0, 1, 2, 3});
        t.v[kRouting] = (float)(cat == 5 || cat == 6 ? pick({0, 1}) : pick({0, 0, 0, 1}));
        // keep the result audible: two filters in series must overlap, band / high-pass not too low,
        // and a negative envelope may not close a filter below about 80 Hz
        if (t.v[kRouting] > 0.5f && (int)t.v[kA + kFType] <= kLP12)
        {   // series: the first filter, if a low-pass, stays open enough for the second to have something
            t.v[kA + kCut] = std::fmax(t.v[kA + kCut], 400.0f);
            t.v[kA + kFEnv] = std::fmax(t.v[kA + kFEnv], -std::log2(t.v[kA + kCut] / 200.0f) / 6.0f * 100.0f);
        }
        if (t.v[kRouting] > 0.5f)
        {
            t.v[kB + kFType] = (float)pick({kLP12, kLP24});
            t.v[kB + kCut] = clamp(t.v[kA + kCut] * rlog(2.0f, 8.0f), 400.0f, 18000.0f);
            t.v[kB + kFEnv] = std::fabs(t.v[kB + kFEnv]);
        }
        for (int b : {kA, kB})
        {
            const int ft = (int)t.v[b + kFType];
            // a high-pass above about 1.2 kHz (or a band-pass far up) can remove a low note's whole sound
            if (ft == kHP) t.v[b + kCut] = clamp(t.v[b + kCut], 120.0f, 1200.0f);
            if (ft == kBP) t.v[b + kCut] = clamp(t.v[b + kCut], 250.0f, 4000.0f);
            if (t.v[b + kFEnv] < 0.0f)
            {   // cut * 2^(6 * amt) >= 80 Hz
                const float minAmt = -std::log2(std::fmax(1.0f, t.v[b + kCut] / 80.0f)) / 6.0f * 100.0f;
                t.v[b + kFEnv] = std::fmax(t.v[b + kFEnv], minAmt);
            }
        }
        // modulation and character
        float l1r, l1d, l2r, l2d; int l1s, l2s, l1t, l2t;
        switch (cat)
        {
            case 0: l1r = rlog(0.02f, 0.3f); l1d = rnd(20, 60); l1s = pick({0, 1}); l1t = pick({1, 3, 0}); l2r = rlog(0.02f, 0.2f); l2d = rnd(10, 50); l2s = pick({0, 1, 4}); l2t = pick({3, 1}); break;
            case 1: l1r = rlog(0.1f, 1.0f); l1d = rnd(10, 40); l1s = pick({0, 1}); l1t = pick({1, 3}); l2r = rlog(0.05f, 0.5f); l2d = rnd(0, 30); l2s = 0; l2t = pick({0, 2}); break;
            case 2: l1r = rlog(3.0f, 7.0f); l1d = rnd(0, 15); l1s = 0; l1t = 0; l2r = rlog(0.2f, 2.0f); l2d = rnd(0, 20); l2s = 0; l2t = 1; break;
            case 3: l1r = rlog(4.0f, 7.0f); l1d = rnd(0, 20); l1s = 0; l1t = 0; l2r = rlog(0.2f, 4.0f); l2d = rnd(10, 50); l2s = pick({0, 1, 2}); l2t = 1; break;
            case 4: l1r = rlog(0.1f, 0.8f); l1d = rnd(5, 25); l1s = pick({0, 1}); l1t = 0; l2r = rlog(0.05f, 0.5f); l2d = rnd(10, 40); l2s = pick({0, 4}); l2t = 1; break;
            case 7: l1r = rlog(4.5f, 6.5f); l1d = rnd(0, 15); l1s = 0; l1t = 0; l2r = rlog(0.1f, 1.5f); l2d = rnd(0, 25); l2s = 0; l2t = 1; break;
            case 5: l1r = rlog(2.0f, 12.0f); l1d = rnd(40, 90); l1s = pick({3, 4, 2}); l1t = pick({1, 2}); l2r = rlog(1.0f, 8.0f); l2d = rnd(30, 80); l2s = pick({3, 4}); l2t = pick({1, 2, 3}); break;
            default: l1r = rlog(0.5f, 20.0f); l1d = rnd(30, 100); l1s = pick({4, 3, 2}); l1t = pick({0, 1, 2, 3}); l2r = rlog(0.5f, 20.0f); l2d = rnd(30, 100); l2s = pick({4, 3}); l2t = pick({0, 1, 2, 3}); break;
        }
        t.v[kL1Rate] = l1r; t.v[kL1Depth] = l1d; t.v[kL1Shape] = (float)l1s; t.v[kL1Dest] = (float)l1t;
        t.v[kL2Rate] = l2r; t.v[kL2Depth] = l2d; t.v[kL2Shape] = (float)l2s; t.v[kL2Dest] = (float)l2t;
        const float dr[kCats][2] = {{30, 80}, {5, 30}, {0, 10}, {0, 15}, {50, 100}, {0, 20}, {40, 100}, {0, 15}};
        const float dv[kCats][2] = {{0, 15}, {0, 20}, {0, 15}, {10, 45}, {20, 60}, {40, 90}, {20, 100}, {5, 35}};
        const float gr[kCats][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {25, 70}, {0, 30}, {30, 100}, {0, 0}};
        const float rg[kCats][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 10}, {0, 15}, {40, 100}, {20, 100}, {0, 0}};
        t.v[kDrift] = rnd(dr[cat][0], dr[cat][1]); t.v[kDrive] = rnd(dv[cat][0], dv[cat][1]);
        t.v[kGrit] = rnd(gr[cat][0], gr[cat][1]); t.v[kRing] = rnd(rg[cat][0], rg[cat][1]);
        shapeTarget(cat, t);
    }
    // the shapers (drawn after everything else, so the rest of a mood's draws stay as they were).
    // Draw Mix, Smooth and the drawing itself are never randomized.
    void shapeTarget(int cat, Target& t)
    {
        static const int kPool[kCats][8] = {
            {0, 1, 10, 13, 28, 30, 44, 51}, {3, 5, 10, 12, 15, 26, 31, 33}, {18, 20, 22, 23, 24, 26, 29, 46},
            {2, 4, 6, 8, 9, 25, 35, 41}, {3, 8, 15, 17, 30, 37, 40, 52}, {21, 23, 34, 36, 38, 39, 42, 57},
            {54, 55, 56, 57, 58, 59, 36, 38}, {26, 29, 31, 2, 6, 20, 24, 8}};
        static const float kRange[kCats][7][2] = {   // bright, odd/even, formant, fold, bend, sync, mutate
            {{-60, 0}, {-30, 30}, {10, 40}, {0, 5}, {0, 15}, {1, 1}, {0, 20}},
            {{-40, 20}, {-40, 40}, {20, 70}, {0, 10}, {0, 20}, {1, 1}, {0, 25}},
            {{-20, 40}, {-50, 50}, {10, 50}, {0, 15}, {0, 45}, {1, 1}, {0, 15}},
            {{0, 60}, {-60, 60}, {20, 60}, {0, 30}, {0, 30}, {1, 3}, {0, 15}},
            {{-60, -10}, {-40, 40}, {20, 70}, {0, 10}, {0, 30}, {1, 1}, {20, 60}},
            {{0, 80}, {-100, 100}, {30, 90}, {20, 80}, {10, 70}, {1, 4}, {0, 40}},
            {{-100, 100}, {-100, 100}, {0, 100}, {0, 100}, {0, 100}, {1, 6}, {30, 100}},
            {{-10, 50}, {-40, 40}, {10, 50}, {0, 15}, {0, 30}, {1, 2}, {0, 15}}};
        static const float kFormantP[kCats] = {0.3f, 0.6f, 0.2f, 0.25f, 0.6f, 0.3f, 0.4f, 0.25f};
        static const float kShapeP[kCats] = {0.35f, 0.35f, 0.3f, 0.25f, 0.4f, 0.45f, 0.5f, 0.3f};
        for (int side = 0; side < 2; ++side)
        {
            const int b = side ? kShB : kShA, o0 = side ? kB : kA;
            const float* r[7]; for (int k = 0; k < 7; ++k) r[k] = kRange[cat][k];
            t.v[b + kShStart] = (float)kPool[cat][(int)(rng_.nextFloat() * 8) % 8];
            t.v[b + kShBright] = rnd(r[0][0], r[0][1]);
            t.v[b + kShOddEven] = rnd(r[1][0], r[1][1]);
            t.v[b + kShFormant] = rng_.nextFloat() < kFormantP[cat] ? rnd(r[2][0], r[2][1]) : 0.0f;
            t.v[b + kShFold] = rnd(r[3][0], r[3][1]);
            t.v[b + kShBend] = rnd(r[4][0], r[4][1]);
            t.v[b + kShSync] = rnd(r[5][0], r[5][1]);
            t.v[b + kShMutate] = rnd(r[6][0], r[6][1]);
            t.v[b + kShSeed] = std::floor(rnd(0.0f, 999.99f));
            for (int o = 0; o < 3; ++o)
                if (t.v[o0 + kO + o * 6 + 1] > 0.0f && rng_.nextFloat() < kShapeP[cat]) t.v[o0 + kO + o * 6] = (float)kShape;
        }
    }
    static int sectionOf(int p)
    {   // 0 = A osc, 1 = B osc, 2 = filters, 3 = envelopes, 4 = mod / character, 5 = always
        if (p >= kShA && p < kShB + kShN) return (p - kShA) / kShN;   // a shaper belongs to its side
        if (p == kRouting || p == kPadDest) return 2;
        if (p >= kA && p < kB + kSideN)
        {
            const int off = (p - kA) % kSideN, side = (p - kA) / kSideN;
            if (off <= kSpr) return side;
            if (off <= kKeyTrk) return 2;
            return 3;
        }
        if (p >= kL1Rate || p == kDrift || p == kDrive || p == kGrit || p == kRing) return 4;
        return 5;
    }
    void randomize()
    {
        const int cat = getInt(kCategory);
        const float amount = clamp(get(kRandAmount) * 0.01f, 0.0f, 1.0f);
        for (int i : soundList_) undo_[i] = getNormalised(i);
        Target t; moodTarget(cat, t);
        const bool lock[5] = {getBool(kLockA), getBool(kLockB), getBool(kLockFilter), getBool(kLockEnv), getBool(kLockMod)};
        int changed = 0;
        for (int i : soundList_)
        {
            if (i == kVolume || i == kOutput || i == kSkin || i == kVoices) continue;
            if (shOff(i) >= kShDrawMix) continue;                  // the drawing, Draw Mix and Smooth are yours
            const int sec = sectionOf(i);
            if (sec < 5 && lock[sec]) continue;
            const mpc::Param& p = param(i);
            const float cur = getNormalised(i), tgt = p.toNormalised(t.v[i]);
            float nv;
            if (p.kind == mpc::Param::Kind::Choice || p.kind == mpc::Param::Kind::Int || p.kind == mpc::Param::Kind::Bool)
                nv = rng_.nextFloat() < amount ? tgt : cur;
            else nv = cur + (tgt - cur) * amount;
            if (nv != cur) { setNormalised(i, nv); notifyHostParameterChanged(i); ++changed; }
        }
        if (!getBool(kLockSkin))
        {   // the skin follows the mood (or any skin, at random)
            static const int kMoodSkin[kCats] = {2, 0, 2, 1, 0, 3, 3, 0};
            const int s = rng_.nextFloat() < 0.6f ? kMoodSkin[cat] : (int)(rng_.nextFloat() * 4) % 4;
            if (s != getInt(kSkin)) { setAndTell(kSkin, (float)s); ++changed; }
        }
        haveUndo_ = true; lastRandCat_ = cat;
        status_ = kStRandom; statusCount_ = changed; statusChanged_ = true;
        requestDisplayUpdate();
    }
    void undo()
    {
        if (!haveUndo_) { status_ = kStNoUndo; statusChanged_ = true; return; }
        for (int i : soundList_)
            if (i != kVolume && i != kOutput && getNormalised(i) != undo_[i]) { setNormalised(i, undo_[i]); notifyHostParameterChanged(i); }
        haveUndo_ = false; status_ = kStUndone; statusChanged_ = true; requestDisplayUpdate();
    }

    // ---- presets: 7 categories x (4 factory + 8 user slots), in the drop-down as "TAG Name" ----------
    void buildPrograms()
    {
        // factory presets: each mood's generator with a fixed seed, at full amount, then named
        for (int c = 0; c < kCats; ++c)
            for (int s = 0; s < kSlotsPer; ++s)
            {
                std::vector<float> v((size_t)kNumParams);
                for (int i = 0; i < kNumParams; ++i) v[(size_t)i] = param(i).def;
                char nm[32];
                if (s < kFactoryPer)
                {
                    // seeds re-rolled where the first draw came out too quiet (levels checked by elver_test)
                    static const uint32_t kReroll[kCats][kFactoryPer] = {   // seeds re-rolled where a draw came out too quiet
                        {0,0,0,0, 0,0,0,0}, {0,0,0,0, 0,0,0,0}, {0,0,0,0, 0,0,2,0}, {0,0,0,0, 0,0,0,0},
                        {0,0,0,0, 0,0,0,0}, {0,2,0,0, 0,0,0,1}, {2,2,1,1, 0,0,4,0}, {0,0,0,0, 0,0,0,0}};
                    rng_.reseed(0xE1E0u + (uint32_t)(c * 97 + s * 13) + kReroll[c][s] * 7919u);
                    Target t; moodTargetFrom(c, v, t);
                    for (int i : soundList_) v[(size_t)i] = t.v[i];
                    if (c == 7) funkPreset(s, v);
                    std::snprintf(nm, sizeof nm, "%s %s", kCatTags[c], kFactoryNames[c][s]);
                }
                else std::snprintf(nm, sizeof nm, "%s (empty)", kCatTags[c]);
                addProgram(nm, v);
                programs_.back().name.reserve(28);
            }
        for (int i : soundList_) set(i, param(i).def);                  // back to the defaults (the host then selects program 0)
    }
    // Funk, by hand: side A only (Blend 0), no grit or ring. Plain values.
    void funkPreset(int s, std::vector<float>& v)
    {
        auto P = [&](int i, float x) { v[(size_t)i] = x; };
        auto osc = [&](int o, int wave, float lvl, int oct, int semi, float fine, float pw) {
            const int b = kA + kO + o * 6; P(b, (float)wave); P(b + 1, lvl); P(b + 2, (float)oct); P(b + 3, (float)semi); P(b + 4, fine); P(b + 5, pw); };
        auto filt = [&](int type, float cut, float res, float env, float kt) { P(kA + kFType, (float)type); P(kA + kCut, cut); P(kA + kRes, res); P(kA + kFEnv, env); P(kA + kKeyTrk, kt); };
        auto fenv = [&](float a, float d, float sus, float r) { P(kA + kFA, a); P(kA + kFD, d); P(kA + kFS, sus); P(kA + kFR, r); };
        auto amp = [&](float a, float d, float sus, float r) { P(kA + kAA, a); P(kA + kAD, d); P(kA + kAS, sus); P(kA + kAR, r); };
        auto uni = [&](int n, float det, float spr) { P(kA + kUni, (float)n); P(kA + kDet, det); P(kA + kSpr, spr); };
        auto lfo1 = [&](float rate, float depth, int dest) { P(kL1Rate, rate); P(kL1Depth, depth); P(kL1Shape, 0); P(kL1Dest, (float)dest); };
        P(kBlend, 0); P(kPadY, 50); P(kPadDest, 0); P(kRouting, 0); P(kGrit, 0); P(kRing, 0); P(kDrift, 5); P(kDrive, 10);
        P(kL1Depth, 0); P(kL2Depth, 0);
        for (int o = 0; o < 3; ++o) osc(o, kSaw, 0, 0, 0, 0, 50);
        uni(1, 10, 60);
        switch (s)
        {
            case 0:  // Royal Stab: bright detuned saw brass, a quick filter snap
                osc(0, kSaw, 85, 0, 0, 0, 50); osc(1, kSaw, 60, 0, 0, 9, 50); uni(3, 14, 70);
                filt(kLP24, 900, 25, 70, 50); fenv(2, 260, 20, 200); amp(3, 350, 55, 220); P(kDrive, 15); lfo1(5.5f, 4, 0); break;
            case 1:  // Brass Poly: softer swell, a slow filter
                osc(0, kSaw, 80, 0, 0, 0, 50); osc(1, kSaw, 70, 0, 0, 8, 50); uni(2, 12, 80);
                filt(kLP24, 1400, 15, 45, 50); fenv(60, 500, 40, 400); amp(40, 400, 80, 350); break;
            case 2:  // Clav Strut: narrow pulse, plucked, tracking the keys
                osc(0, kSquare, 85, 0, 0, 0, 20); osc(1, kSaw, 40, 1, 0, 0, 50);
                filt(kLP12, 1800, 40, 60, 70); fenv(1, 160, 10, 80); amp(1, 220, 25, 90); P(kDrive, 20); break;
            case 3:  // Horn Hit: a short stab
                osc(0, kSaw, 85, 0, 0, 0, 50); osc(1, kSquare, 60, 0, 0, 0, 40); uni(2, 10, 60);
                filt(kLP24, 700, 30, 85, 50); fenv(4, 180, 15, 120); amp(4, 300, 25, 150); break;
            case 4:  // Prog Lead: saw over a square an octave down, vibrato, a little drive
                osc(0, kSaw, 85, 0, 0, 0, 50); osc(1, kSquare, 50, -1, 0, 0, 50);
                filt(kLP24, 1200, 55, 50, 60); fenv(5, 400, 50, 300); amp(2, 300, 85, 250); lfo1(5.5f, 12, 0); P(kDrive, 25); break;
            case 5:  // Cathedral Organ: the full drawbar organ from the Shape library
                osc(0, kShape, 85, 0, 0, 0, 50); P(kShA + kShStart, 29);
                filt(kLP12, 6000, 5, 0, 30); fenv(5, 300, 100, 300); amp(15, 100, 100, 300); lfo1(6.0f, 8, 0); P(kDrive, 8); break;
            case 6:  // Disco Strings: wide unison saws, slow attack, gentle chorus wobble
                osc(0, kSaw, 80, 0, 0, 0, 50); osc(1, kSaw, 70, 0, 0, 10, 50); uni(4, 22, 100);
                filt(kLP12, 2500, 10, 10, 40); fenv(300, 800, 60, 900); amp(350, 500, 90, 900); lfo1(5.0f, 6, 0); P(kDrift, 15); P(kDrive, 0); break;
            default: // Octave Bass: saw an octave down with a square on the note
                osc(0, kSaw, 85, -1, 0, 0, 50); osc(1, kSquare, 60, 0, 0, 0, 50);
                filt(kLP24, 450, 35, 65, 40); fenv(1, 180, 20, 100); amp(1, 250, 70, 80); P(kDrive, 15); break;
        }
    }
    // the generator, starting from given values (used before the parameters' atomics matter)
    void moodTargetFrom(int c, const std::vector<float>& base, Target& t)
    {
        for (int i : soundList_) set(i, base[(size_t)i]);
        moodTarget(c, t);
    }

    static int charIndex(char c) { for (int i = 0; i < kKeyBack; ++i) if (kKeyChars[i] == c) return i; return 36; }
    void startNaming()
    {
        naming_ = true;
        const int cat = lastRandCat_ >= 0 ? lastRandCat_ : getInt(kCategory);
        setAndTell(kSaveCat, (float)cat);
        int n = 1; for (int u = 0; u < kUserPer; ++u) n += userUsed_[cat][u];
        std::snprintf(name_, sizeof name_, "%s %02d", cat == 3 ? "Lead" : (cat == 1 ? "Pad" : (cat == 2 ? "Keys" : (cat == 4 ? "Lofi" : (cat == 5 ? "Metal" : (cat == 6 ? "Noise" : (cat == 7 ? "Funk" : "Ambient")))))), n);
        cursor_ = (int)std::strlen(name_);
        status_ = kStNaming; statusChanged_ = true; nameChanged_ = true;
    }
    void typeKey(int k)
    {
        if (!naming_) startNaming();
        int len = (int)std::strlen(name_);
        if (k < kKeyBack)
        {
            if (len >= kNameLen) return;
            std::memmove(name_ + cursor_ + 1, name_ + cursor_, (size_t)(len - cursor_ + 1));
            name_[cursor_++] = kKeyChars[k];
        }
        else if (k == kKeyBack && cursor_ > 0) { std::memmove(name_ + cursor_ - 1, name_ + cursor_, (size_t)(len - cursor_ + 1)); --cursor_; }
        else if (k == kKeyLeft && cursor_ > 0) --cursor_;
        else if (k == kKeyRight && cursor_ < len) ++cursor_;
        else if (k == kKeyClear) { name_[0] = 0; cursor_ = 0; }
        nameChanged_ = true;
    }
    void confirmSave()
    {
        if (savePending_.load() >= 0) return;
        const int cat = getInt(kSaveCat);
        int slot = -1;
        for (int u = 0; u < kUserPer && slot < 0; ++u) if (!userUsed_[cat][u]) slot = u;
        if (slot < 0) { slot = nextOverwrite_[cat]; nextOverwrite_[cat] = (nextOverwrite_[cat] + 1) % kUserPer; }
        for (int i : soundList_) saveVals_[i] = get(i);
        saveSkin_ = getInt(kSkin);
        std::snprintf(saveName_, sizeof saveName_, "%s", name_[0] ? name_ : "Untitled");
        savePending_.store(cat * kSlotsPer + kFactoryPer + slot);
        naming_ = false; status_ = kStSaving; statusChanged_ = true; nameChanged_ = true;
    }
    std::string nameText() const
    {
        if (!namingDisp_.load()) return "tap Save to name a preset";
        std::string s; const int c = cursorDisp_.load();
        for (int i = 0; i <= kNameLen; ++i)
        {
            const char ch = nameDisp_[i].load();
            if (i == c) s += '|';
            if (!ch) break;
            s += ch;
        }
        return s;
    }
    enum Status { kStReady, kStRandom, kStUndone, kStNoUndo, kStNaming, kStSaving, kStSaved, kStCancelled, kStLoaded, kStEmpty };
    std::string statusText() const
    {
        char b[64];
        switch (statusDisp_.load())
        {
            case kStRandom: std::snprintf(b, sizeof b, "%s: %d changed", kCatNames[catDisp_.load()], countDisp_.load()); return b;
            case kStUndone: return "Undone";
            case kStNoUndo: return "Nothing to undo";
            case kStNaming: return "Type a name, then Confirm";
            case kStSaving: return "Saving...";
            case kStSaved: return std::string("Saved: ") + savedNameDisp_;
            case kStCancelled: return "Not saved";
            case kStLoaded: return std::string("Loaded: ") + programName(loadedDisp_.load());
            case kStEmpty: return "That slot is empty";
            default: return "Ready";
        }
    }
    // user presets file, version 2: "ELUP" | u8 2 | u16 count | kCats x kUserPer x { u8 used, u8 skin,
    // char name[17], f32 x count } with the values in sound-list order. Version 1 (Elver 1.0) is
    // "ELUP" | u8 1 | the same records with 88 values; it still loads, the shapers at their defaults.
    void loadUserPresets()
    {
        std::vector<uint8_t> d;
        if (!mpc::readFile(userFile_, d) || d.size() < 5 || std::memcmp(d.data(), "ELUP", 4) != 0 || (d[4] != 1 && d[4] != 2)) return;
        size_t head = 5, count = kNumSound;
        if (d[4] == 2) { if (d.size() < 7) return; count = (size_t)d[5] | ((size_t)d[6] << 8); head = 7; }
        const size_t rec = 2 + (kNameLen + 1) + 4 * count;
        const int fileCats = (int)std::min<size_t>((size_t)kCats, (d.size() - head) / (rec * kUserPer));
        if (fileCats < 1) return;
        const size_t use = std::min(count, soundList_.size());
        for (int c = 0; c < fileCats; ++c)
            for (int u = 0; u < kUserPer; ++u)
            {
                const uint8_t* r = d.data() + head + rec * (size_t)(c * kUserPer + u);
                if (!r[0]) continue;
                userUsed_[c][u] = 1; userSkin_[c][u] = r[1] % 4;
                char nm[kNameLen + 1] = {0}; std::memcpy(nm, r + 2, kNameLen);
                for (char& ch : nm) if (ch != 0 && (ch < 32 || ch > 126)) ch = '?';
                mpc::Program& pr = programs_[(size_t)(c * kSlotsPer + kFactoryPer + u)];
                for (size_t j = 0; j < use; ++j) { float x; std::memcpy(&x, r + 2 + kNameLen + 1 + 4 * j, 4); if (std::isfinite(x)) pr.plainValues[(size_t)soundList_[j]] = x; }
                char full[32]; std::snprintf(full, sizeof full, "%s %s", kCatTags[c], nm);
                pr.name.assign(full);
            }
    }
    void writerLoop()
    {
        while (!quit_.load())
        {
            const int prog = savePending_.load();
            if (prog >= 0)
            {
                const int c = prog / kSlotsPer, u = prog % kSlotsPer - kFactoryPer;
                mpc::Program& pr = programs_[(size_t)prog];
                for (int i : soundList_) pr.plainValues[(size_t)i] = saveVals_[i];
                char full[32]; std::snprintf(full, sizeof full, "%s %s", kCatTags[c], saveName_);
                pr.name.assign(full);
                userUsed_[c][u] = 1; userSkin_[c][u] = (uint8_t)saveSkin_;
                std::snprintf(savedNameDisp_, sizeof savedNameDisp_, "%s", full);
                writeUserFile();
                savePending_.store(-1);
                saveDone_.store(prog);
            }
            shapeWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }
    // The MPC's drop-down list is fixed text in the skin (Plugin Skins/TUI.json). After a save (and at
    // start-up) Elver rewrites that one list with the current names: factory presets, saved user presets,
    // "TAG empty n" for empty slots. Worker thread only; only that list changes; written to a temporary
    // file and renamed; left alone if the file is not as expected.
    std::string listEntry(int prog) const
    {
        const int c = prog / kSlotsPer, s = prog % kSlotsPer;
        if (s >= kFactoryPer && !userUsed_[c][s - kFactoryPer]) return std::string(kCatTags[c]) + " empty " + std::to_string(s - kFactoryPer + 1);
        return programName(prog);
    }
    void syncSkinList()
    {
        if (skinFile_.empty()) return;
        std::vector<uint8_t> raw;
        if (!mpc::readFile(skinFile_, raw) || raw.empty()) return;
        std::string j(raw.begin(), raw.end());
        const std::string key = "\"Parameter " + std::to_string(kPresetPick) + "\"";
        size_t k = j.find("\"replacementStrings\"");
        if (k == std::string::npos) return;
        k = j.find(key, k);
        if (k == std::string::npos) return;
        const size_t open = j.find('[', j.find("\"value\"", k));
        if (open == std::string::npos) return;
        const size_t close = j.find(']', open);
        if (close == std::string::npos) return;
        std::string list = "[";
        for (int i = 0; i < kCats * kSlotsPer; ++i)
        {
            std::string n = listEntry(i), esc;
            for (char ch : n) { if (ch == '"' || ch == '\\') esc += '\\'; if ((unsigned char)ch >= 32) esc += ch; }
            list += (i ? ", \"" : "\"") + esc + "\"";
        }
        list += "]";
        if (j.compare(open, close - open + 1, list) == 0) return;        // already right
        j.replace(open, close - open + 1, list);
        const std::string tmp = skinFile_ + ".tmp";
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return;
        const bool ok = std::fwrite(j.data(), 1, j.size(), f) == j.size();
        std::fclose(f);
        if (!ok) { std::remove(tmp.c_str()); return; }
        std::remove(skinFile_.c_str()); std::rename(tmp.c_str(), skinFile_.c_str());
    }
    void writeUserFile()
    {
        std::vector<uint8_t> d = {'E', 'L', 'U', 'P', 2, (uint8_t)(soundList_.size() & 0xFF), (uint8_t)(soundList_.size() >> 8)};
        for (int c = 0; c < kCats; ++c)
            for (int u = 0; u < kUserPer; ++u)
            {
                const mpc::Program& pr = programs_[(size_t)(c * kSlotsPer + kFactoryPer + u)];
                d.push_back(userUsed_[c][u]); d.push_back(userSkin_[c][u]);
                char nm[kNameLen + 1] = {0};
                const std::string& full = pr.name;
                std::strncpy(nm, full.size() > std::strlen(kCatTags[c]) + 1 ? full.c_str() + std::strlen(kCatTags[c]) + 1 : "", kNameLen);
                d.insert(d.end(), nm, nm + kNameLen + 1);
                for (int i : soundList_) { float f = pr.plainValues[(size_t)i]; uint8_t b[4]; std::memcpy(b, &f, 4); d.insert(d.end(), b, b + 4); }
            }
        const std::string tmp = userFile_ + ".tmp";
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return;
        const bool ok = std::fwrite(d.data(), 1, d.size(), f) == d.size();
        std::fclose(f);
        if (!ok) { std::remove(tmp.c_str()); return; }
        std::remove(userFile_.c_str()); std::rename(tmp.c_str(), userFile_.c_str());
    }

    // ---- displays (4 times a second; the host is told only what changed) -----------------------------
    void bump(int idx, int& s) { setNormalised(idx, (float)((s++ % 50) + 1) / 100.0f); notifyHostParameterChanged(idx); }
    void publish()
    {
        cpuDisp_.store(cpu_);
        if (std::fabs(cpu_ - lastCpu_) > 0.002f) { lastCpu_ = cpu_; setNormalised(kCpu, clamp(cpu_ * 4.0f, 0.0f, 1.0f)); notifyHostParameterChanged(kCpu); }
        if (voicesNow_ != voicesDisp_.load() || limit_ != limitDisp_.load()) { voicesDisp_.store(voicesNow_); limitDisp_.store(limit_); bump(kVoicesNow, serialV_); }
        if (slotsDirty_)
        {   // the list's 16 rows redraw (their text comes from formatValue)
            slotsDirty_ = false;
            for (int k = 0; k < 16; ++k) bump(kSlotRow0 + k, serialSlot_);
        }
        if (statusChanged_)
        {
            statusChanged_ = false; statusDisp_.store(status_); countDisp_.store(statusCount_); catDisp_.store(clamp(lastRandCat_, 0, kCats - 1)); loadedDisp_.store(loadedProg_);
            bump(kStatus, serialS_);
        }
        if (nameChanged_)
        {
            nameChanged_ = false;
            for (int i = 0; i <= kNameLen; ++i) nameDisp_[i].store(name_[i]);
            cursorDisp_.store(cursor_); namingDisp_.store(naming_);
            bump(kNameEdit, serialN_);
        }
    }

    // ---- members ---------------------------------------------------------------------------------------
    double sr_ = 44100.0;
    Voice voices_[kMaxVoices];
    SideSet set_[2];
    float envCache_[2][8] = {};
    bool envDirty_ = true;
    uint32_t ageCounter_ = 0;
    int limit_ = kMaxVoices, voicesNow_ = 0, routing_ = 0, padDest_ = 0;
    float padY_ = 0.0f, morph_[3] = {1, 1, 1}, drift_ = 0.0f, drive_ = 0.0f, grit_ = 0.0f, ring_ = 0.0f, bend_ = 0.0f;
    float lfoS_[2] = {}, ampS_ = 1.0f;
    float lfoRate_[2] = {}, lfoDepth_[2] = {}, lfoPh_[2] = {}, shL_[2] = {};
    int lfoShape_[2] = {}, lfoDest_[2] = {};
    mpc::dsp::Xorshift32 lfoRng_{0xA11CEu}, rng_;
    float gAprev_ = 1.0f, gBprev_ = 0.0f;
    float gBlend_ = 0.0f, blendT_ = 0.0f, gVol_ = 0.3f, volT_ = 0.3f, crushL_ = 0.0f, crushR_ = 0.0f;
    int crushHold_ = 0;
    float crushEnv_ = 0.0f, crushRel_ = 0.9999f;
    bool sustain_ = false, expectSound_ = false;
    int silentHeld_ = 0, watchdogResets_ = 0;
    float cpu_ = 0.0f, lastCpu_ = -1.0f;
    int pickLast_ = 0, progLast_ = 0, slotLast_ = -1, serialSlot_ = 0;
    bool slotsDirty_ = true;
    std::atomic<bool> progLoaded_{false};
    int pubCount_ = 0, serialV_ = 0, serialS_ = 0, serialN_ = 0, lastBrowseCat_ = -1;
    std::atomic<float> cpuDisp_{0.0f};
    std::atomic<int> voicesDisp_{-1}, limitDisp_{-1}, statusDisp_{kStReady}, countDisp_{0}, catDisp_{0}, loadedDisp_{0};
    // buttons
    bool btnLast_[kNumParams] = {};
    int64_t btnTime_[kNumParams] = {};
    int64_t samples_ = 1 << 30;
    // randomize / undo
    float undo_[kNumParams] = {};
    bool haveUndo_ = false;
    int lastRandCat_ = -1, status_ = kStReady, statusCount_ = 0, loadedProg_ = 0, savedProg_ = -1;
    bool statusChanged_ = true;
    // naming / saving
    bool naming_ = false, nameChanged_ = true;
    char name_[kNameLen + 2] = {};
    int cursor_ = 0;
    std::atomic<char> nameDisp_[kNameLen + 1] = {};
    std::atomic<int> cursorDisp_{0};
    std::atomic<bool> namingDisp_{false};
    char savedNameDisp_[32] = {};
    uint8_t userUsed_[kCats][kUserPer] = {}, userSkin_[kCats][kUserPer] = {};
    int nextOverwrite_[kCats] = {};
    float saveVals_[kNumParams] = {};
    int saveSkin_ = 0;
    char saveName_[kNameLen + 1] = {};
    std::string userFile_, skinFile_;
    std::atomic<int> savePending_{-1}, saveDone_{-1};
    std::atomic<bool> quit_{false};
    // shapes
    static constexpr int kDrawUndo = 8;
    static constexpr float kShapeFadeStep = 1.0f / 900.0f;          // about 20 ms
    struct ShapeSide
    {
        std::unique_ptr<WaveTable> buf[3];
        int cur = 0, prev = -1; float fade = 1.0f;                   // audio thread
        std::atomic<int> pending{-1}, inUse{1};
        std::atomic<bool> snap{false};
        std::atomic<uint64_t> wantSig{0}; uint64_t builtSig = 0;   // builtSig: worker / loading thread, under buildMu_
        double base[kWaveN] = {}; int baseIdx = -1;                  // worker thread
        bool scopeDirty = false;
        float lastX = -1.0f, lastY = -1.0f, lastVal = 0.0f; int lastIdx = 0; int64_t lastDrawAt = 0;
        float undo[kDrawUndo][kDrawPoints] = {}; int undoN = 0;
    } sh_[2];
    Fft fft_;
    std::mutex buildMu_;                                           // the worker and a preset load never build at once
    std::vector<int> soundList_;
    std::thread worker_;
};

} // namespace

MPC_PLUGIN_ENTRY(Elver)
