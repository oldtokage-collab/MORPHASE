// MORPHASE - patch.cpp
// Defaults, factory patches, SD storage (settings / patches / patterns).
//
// Files are simple one-key-per-line JSON, parsed line by line - the same
// approach C.P.S. uses, so no JSON library and no heap churn. Enum values
// are stored as numbers, which is why every persisted enum is append-only
// and every range check on load uses the enum's *_COUNT (spec 9.6).

#include "patch.h"
#include <nvs.h>
#include "morph.h"
#include "fx.h"
#include "voice.h"
#include "arp.h"
#include "theremin.h"
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <esp_system.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp

extern bool uiDirty;   // ui_common.cpp: a save notice asks for a redraw (v0.72)

Patch          patch;
SystemSettings sys;
bool loadingPatch = false;
bool savingPatch  = false;

// ---------------------------------------------------------------------------
// Name tables
// ---------------------------------------------------------------------------
const char *WAVEFORM_NAMES[NUM_WAVEFORMS] = {
    "Sine", "Squeeze", "HalfSine", "Parabolic", "ESquare", "Saw2", "ESaw", "Square2"
};
const char *ALGORITHM_NAMES[NUM_ALGORITHMS] = {
    "4>3>2>1", "(4,3)>2>1", "4>2>1+3>1", "(4,3,2)>1", "4>3 + 2>1", "4>3>2 + 1", "4>(3,2,1)", "1 2 3 4",
    // v0.7: 9 and 10 share 1's shape, so these name the sound instead of
    // the routing (the diagram shows the routing and the feedback marks)
    "SerSaw", "Growl", "DualFB", "StkSaw", "Pair+2", "Diamond"
};
const char *MOD_TARGET_NAMES[(int)ModTarget::MOD_TARGET_COUNT] = {
    "Off", "GMorph", "Feedbk", "Pitch", "Volume", "OpMorph", "OpLevel", "OpRatio", "OpDetun",
    "-",                                        // MOD_AUDIO_END, never shown
    "ArpTmpo", "ArpSwng", "ArpGate", "Glide",
    "ChDepth", "ChMix", "DlFdbk", "DlMix", "RvSize", "RvMix", "DrAmt",   // DrAmt was LmDrive (v0.72)
    "PtchMrph",
    "CrBits", "CrDown", "CrMix",
    "GlProb",
    "PEnvAmt",                                  // v0.71
    "DrMix"                                     // v0.72
};
const char *SWITCH_ACTION_NAMES[(int)SwitchAction::SWITCH_ACTION_COUNT] = {
    "Off", "Arp", "Latch", "Hold", "Glide", "Pch+", "Pch-", "Pat+", "Pat-",
    "Tap", "RndPch", "RndPat", "Panic",
    "BypCho", "BypDly", "BypRvb", "BypCrs", "BypDrv",   // BypDrv was BypLim (v0.72)
    "Glitch",
    "Theremin",                                         // v0.81
    "Chain+", "Chain-"                                  // v0.84
};
const char *SWITCH_MODE_NAMES[(int)SwitchMode::SWITCH_MODE_COUNT] = { "Toggle", "Moment" };
const char *LFO_WAVE_NAMES[(int)LfoWave::LFO_WAVE_COUNT] = { "Sine", "Triangle", "Saw", "Square", "S&H" };
const char *ARP_PATTERN_NAMES[(int)ArpPattern::ARP_PATTERN_COUNT] = { "Up", "Down", "UpDown", "Random", "AsPlayed", "STEP" };
const char *ARP_RATE_NAMES[(int)ArpRate::ARP_RATE_COUNT] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
const float ARP_RATE_MULT[(int)ArpRate::ARP_RATE_COUNT] = { 1.0f, 0.5f, 1.f / 3.f, 0.25f, 1.f / 6.f, 0.125f };
const char *CHORD_WRAP_NAMES[(int)ChordWrap::CHORD_WRAP_COUNT] = { "Rest", "Wrap", "Octave" };
const char *GLIDE_MODE_NAMES[(int)GlideMode::GLIDE_MODE_COUNT] = { "Always", "Legato" };
const char *UI_THEME_NAMES[(int)UiTheme::UI_THEME_COUNT] = { "Green", "Amber", "Cyan", "Violet", "Red" };
const char *IMU_CURVE_NAMES[(int)ImuCurve::IMU_CURVE_COUNT] = { "Linear", "Exp" };
const char *SHIFT_MODE_NAMES[(int)ShiftMode::SHIFT_MODE_COUNT] = { "Input", "Output" };
const char *PC_MODE_NAMES[(int)PcMode::PC_MODE_COUNT] = { "Patch", "Morph" };
const char *TH_BUS_NAMES[(int)ThBus::TH_BUS_COUNT] = { "Cap", "Grove" };
const char *TH_MODE_NAMES[(int)ThMode::TH_MODE_COUNT] = { "Mod", "Play" };
const char *TH_RELEASE_NAMES[(int)ThRelease::TH_RELEASE_COUNT] = { "Hold", "Return" };
const char *TH_PITCH_NAMES[(int)ThPitch::TH_PITCH_COUNT] = { "Smooth", "Semitone" };
const char *TH_VOLAXIS_NAMES[(int)ThVolAxis::TH_VOLAXIS_COUNT] = { "Off", "X", "Y" };
const char *GL_RATE_NAMES[(int)GlRate::GL_RATE_COUNT] = { "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T" };
const char *GL_BUF_NAMES[(int)GlBuf::GL_BUF_COUNT] = { "Off", "Stutter", "Reverse", "Gate", "Random" };
const char *GL_CHAOS_NAMES[(int)GlChaos::GL_CHAOS_COUNT] = { "Off", "Ratio", "Timbre", "Wild" };
const char *GL_LEN_NAMES[(int)GlLen::GL_LEN_COUNT] = { "1/32", "1/16", "1/8" };
const char *GL_ARP_NAMES[(int)GlArp::GL_ARP_COUNT] = { "Off", "Ratchet", "Octave", "Reverse", "Rest", "Random" };
const char *LATCH_MODE_NAMES[(int)LatchMode::LATCH_MODE_COUNT] = { "Add", "Chord" };

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------
static void opDefaults(OpParams &o, float level) {
    o.morph = 0.f; o.ratio = 1.f; o.detune = 0.f; o.level = level;
    o.attack = 5.f; o.decay = 40.f; o.sustain = 70.f; o.release = 25.f;
    o.velSens = 50.f; o.keyScale = 30.f;
    o.fixed = false; o.fixNote = FIX_NOTE_DEFAULT;   // v0.71: Ratio, so older patches load unchanged
}

void patchInitDefaults(Patch &p) {
    strncpy(p.name, "Init", PATCH_NAME_LEN); p.name[PATCH_NAME_LEN] = 0;
    for (int o = 0; o < NUM_OPS; o++) opDefaults(p.op[o], o == 0 ? 100.f : 0.f);
    p.algorithm = 4;
    p.feedback = 0.f;
    p.globalMorph = 0.f;
    p.volume = 80.f;
    p.voices = PATCH_MAX_VOICES;   // same Init in both builds (v0.77)
    p.glideMs = 0.f;
    p.glideEnabled = false;
    p.glideMode = GlideMode::LEGATO;
    p.lfoWave = LfoWave::SINE;
    p.lfoRateHz = 5.f;
    p.lfoDepth = 0.f;
    p.lfoTarget = ModTarget::GMORPH;
    p.lfoDelayMs = 0.f;
    // v0.87: 50% on both axes (was X 0% and Y Off - turning the IMU on did
    // nothing on most patches). GMorph and Feedbk are one-directional, so
    // 50% reaches their whole range at full tilt (about 35 degrees).
    p.imuXTarget = ModTarget::GMORPH;   p.imuXDepth = 50.f;
    p.imuYTarget = ModTarget::FEEDBACK; p.imuYDepth = 50.f;
    p.chorusRateHz = 0.8f; p.chorusDepthMs = 8.f; p.chorusMix = 0.f;
    p.delayTimeMs = 300.f; p.delayFeedback = 35.f; p.delayMix = 0.f;
    p.reverbSize = 50.f; p.reverbDamp = 50.f; p.reverbMix = 0.f;
    // v0.72: Drive (the old Limiter curve) is a sound now, not a safety net -
    // the output limiter protects the level - so a new sound starts clean,
    // with the amount ready for when the pad or t switches it in. Patch files
    // always carry both keys, so loading an existing patch is unaffected.
    p.limiterDrive = 2.f; p.limiterMix = 0.f;
    p.crushBits = 8.f; p.crushDown = 4.f; p.crushMix = 0.f;   // Off: old patches load the same as v0.54
    p.penvAmt = 0.f; p.penvDecay = 30.f; p.penvVel = 0.f;    // Off: old patches load the same as v0.7
}

void sysInitDefaults() {
    sys.midiChannel = MIDI_CHANNEL_OMNI;       // Omni by default (spec 7.2)
    sys.bendRange = 2;
    for (int i = 0; i < NUM_CC_SLOTS; i++) { sys.ccSlot[i].number = CC_OFF; sys.ccSlot[i].target = ModTarget::OFF; sys.ccSlot[i].depth = 100; }
    for (int i = 0; i < NUM_SW_SLOTS; i++) { sys.swSlot[i].number = CC_OFF; sys.swSlot[i].action = SwitchAction::OFF; sys.swSlot[i].mode = SwitchMode::TOGGLE; }
    sys.patternBaseCc = CC_OFF;
    sys.pcMode = PcMode::PATCH;
    sys.imuXEnabled = false; sys.imuYEnabled = false;
    sys.imuXHeld = false; sys.imuYHeld = false;
    sys.imuXHeldNorm = 0.f; sys.imuYHeldNorm = 0.f;
    sys.theme = UiTheme::GREEN;
    sys.brightness = 80;
    sys.patternSlot = 0;
    sys.masterVolume = 80;
    sys.imuCalibrated = false;
    sys.imuXCalDeg = 0.f;
    sys.imuYCalDeg = 0.f;
    sys.imuXSens = 100; sys.imuYSens = 100;
    sys.imuXInvert = false; sys.imuYInvert = false;
    sys.imuXCurve = ImuCurve::LINEAR; sys.imuYCurve = ImuCurve::LINEAR;
    sys.imuXDeadzone = 5; sys.imuYDeadzone = 5;   // the fixed 5% used before v0.4
    sys.octave = 0;
    sys.transpose = 0;
    sys.morphEnabled = false;
    sys.morphPosBase = 0;
    sys.morphTimeMs = 1000;
    sys.morphPaused = false;
    sys.scopeGmFb = true; sys.scopeMorph = true; sys.scopeFx = true; sys.scopeGl = true;
    sys.glOn = false; sys.glRate = GlRate::R16; sys.glProb = 25; sys.glHold = 100;
    sys.glBuf = GlBuf::RANDOM; sys.glChaos = GlChaos::RATIO; sys.glLen = GlLen::L16; sys.glAmt = 50;
    sys.glArp = GlArp::OFF;
    sys.factoryVersion = 0;          // v0.72: a card that never had the versioned set
    // v0.77: On in the BLE build - it was chosen for BLE. A settings.json
    // without the key (from the standard build) leaves it On.
    sys.bleMidiEnabled = true;
    // Theremin (v0.81): Off; the C.P.S. values for the rest
    sys.thOn = false; sys.thBus = ThBus::CAP; sys.thMode = ThMode::MOD;
    sys.thNearMm = 60; sys.thFarMm = 250;
    sys.thTarget = ModTarget::GMORPH; sys.thDepth = 60.f; sys.thRelease = ThRelease::RETURN;
    sys.thPitch = ThPitch::SMOOTH; sys.thScale = 0; sys.thTopSemis = 12; sys.thSpan = 2;
    sys.thVolAxis = ThVolAxis::AXIS_Y;
}

// Reset covers EVERY patch field (spec 9.4): the whole struct is rebuilt
// from patchInitDefaults, so nothing can be missed, and the FX lines are
// cleared through audioTask.
void patchResetTone() {
    morphStopForPatchChange();
    patchInitDefaults(patch);
    requestFxClear();
}

static float frand(float lo, float hi) {
    return lo + (hi - lo) * ((float)(esp_random() & 0xFFFF) / 65535.f);
}
static int irand(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }

void patchRandomize() {
    morphStopForPatchChange();
    static const float RATIOS[] = { 0.5f, 1.f, 1.f, 1.f, 2.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 1.41f, 3.14f, 2.01f, 0.99f, 5.19f };
    const int NR = sizeof(RATIOS) / sizeof(RATIOS[0]);
    strncpy(patch.name, "Random", PATCH_NAME_LEN); patch.name[PATCH_NAME_LEN] = 0;
    patch.algorithm = irand(1, NUM_ALGORITHMS);
    for (int o = 0; o < NUM_OPS; o++) {
        OpParams &p = patch.op[o];
        bool carrier = algoIsCarrier(patch.algorithm, o);
        p.morph   = frand(0.f, carrier ? 40.f : 70.f);
        p.ratio   = RATIOS[irand(0, NR - 1)];
        p.detune  = (irand(0, 3) == 0) ? frand(-12.f, 12.f) : 0.f;
        p.level   = (o == 0) ? 100.f : (carrier ? frand(40.f, 100.f) : frand(0.f, 70.f));
        p.attack  = (irand(0, 2) == 0) ? frand(0.f, 60.f) : frand(0.f, 15.f);
        p.decay   = frand(20.f, 80.f);
        p.sustain = frand(0.f, 90.f);
        p.release = frand(10.f, 70.f);
        p.velSens = carrier ? frand(0.f, 40.f) : frand(40.f, 100.f);
        p.keyScale = frand(20.f, 70.f);
        p.fixed = false;
    }
    // v0.71 (update spec 5, 6.3). 10%: one MODULATOR goes fixed - a fixed
    // carrier would ignore the keys and sound broken, a fixed modulator
    // sounds metallic. 20%: a short pitch blip (a long one makes every
    // patch wobble).
    if (irand(0, 9) == 0) {
        int mods[NUM_OPS], nm = 0;
        for (int o = 0; o < NUM_OPS; o++) if (!algoIsCarrier(patch.algorithm, o)) mods[nm++] = o;
        if (nm > 0) {
            OpParams &p = patch.op[mods[irand(0, nm - 1)]];
            p.fixed = true;
            p.fixNote = (float)irand(24, 96);
        }
    }
    if (irand(0, 4) == 0) { patch.penvAmt = (float)irand(-12, 12); patch.penvDecay = frand(0.f, 40.f); }
    else                  { patch.penvAmt = 0.f; patch.penvDecay = 30.f; }
    patch.penvVel = 0.f;
    patch.feedback = (irand(0, 1) == 0) ? frand(0.f, 60.f) : 0.f;
    patch.globalMorph = 0.f;
    patch.lfoDepth = (irand(0, 2) == 0) ? frand(5.f, 40.f) : 0.f;
    patch.lfoRateHz = frand(0.2f, 6.f);
    patch.lfoTarget = (ModTarget)irand(1, 3);
    patch.lfoDelayMs = (irand(0, 1) == 0) ? frand(0.f, 800.f) : 0.f;
    patch.chorusMix = (irand(0, 2) == 0) ? frand(20.f, 50.f) : 0.f;
    patch.delayMix  = (irand(0, 2) == 0) ? frand(10.f, 35.f) : 0.f;
    patch.reverbMix = (irand(0, 1) == 0) ? frand(15.f, 45.f) : 0.f;
    patch.crushMix  = (irand(0, 4) == 0) ? frand(40.f, 100.f) : 0.f;   // rarely: it dominates the sound
    patch.crushBits = frand(3.f, 12.f);
    patch.crushDown = frand(1.f, 12.f);
    requestFxClear();
}

// ---------------------------------------------------------------------------
// Factory patches (spec 9.7). Every modulator carries velSens.
// ---------------------------------------------------------------------------
static void setOp(Patch &p, int i, float morph, float ratio, float detune, float level,
                  float a, float d, float s, float r, float vel, float ks) {
    OpParams &o = p.op[i];
    o.morph = morph; o.ratio = ratio; o.detune = detune; o.level = level;
    o.attack = a; o.decay = d; o.sustain = s; o.release = r; o.velSens = vel; o.keyScale = ks;
}

// The factory set, in list order. Must match the names factoryPatch() gives
// each index. These names are always listed first, in this order, so
// Program Change numbers stay fixed. Adding a patch: give it the next index
// in factoryPatch(), add its name here, raise FACTORY_SET_VERSION.
// Set 1 (v0.74): 01-08 keys and bells / mallets. Set 2 (v0.75): 09-16 bass
// and lead, all mono. Set 3 (v0.76): 17-24 brass / strings / pads and
// additive / morph, all poly. Set 4 (v0.79): 25-28 drums, 29-32 FX - the
// 32 are complete. The old patches stayed at the end until their own group was
// redone; E.Piano and Bell left the table with set 1, Bass and MorphLead with
// set 2, Brass, Pad and Additive with set 3, Metallic with set 4 (a card that
// has them keeps them as the player's patches).
static const char *const FACTORY_NAMES[] = {
    "01_EPiano", "02_EPBright", "03_Clav", "04_Organ",
    "05_Bell", "06_Gong", "07_Marimba", "08_Glock",
    "09_Bass", "10_Growl", "11_SawBass", "12_SubBass",
    "13_MorphLd", "14_SawLead", "15_Whistle", "16_DirtyLd",
    "17_Brass", "18_Strings", "19_WarmPad", "20_Choir",
    "21_Additive", "22_Evolve", "23_TiltPad", "24_Shimmer",
    "25_Kick", "26_Tom", "27_Snare", "28_HiHat",
    "29_Metallic", "30_CrushLd", "31_NoiseSwl", "32_Drone"
};
constexpr int FACTORY_COUNT = sizeof(FACTORY_NAMES) / sizeof(FACTORY_NAMES[0]);
constexpr int FACTORY_NEW_COUNT = 32;      // the voiced-for-v0.72 patches - since set 4 (v0.79), all of them

// The demo chain (v0.76, set 3 decision 4): neighbours share an algorithm,
// so the morph flows - 1-3 on algorithm 5 (e-piano > marimba > bell), 4-7 on
// algorithm 7 (the additive / morph group). It steps once, between 3 and 4.
// Written to a card with no morph.txt, and loaded on demand from
// SETTING > Patch > Morph > Demo.
static const char *const DEMO_CHAIN[] = {
    "01_EPiano", "07_Marimba", "05_Bell",
    "21_Additive", "22_Evolve", "23_TiltPad", "24_Shimmer"
};
// v0.79: the drum pair - the same algorithm, so the morph is smooth all the
// way: the pitch drop, the body's decay and its tone move from kick to tom.
static const char *const DEMO_DRUMS[] = { "25_Kick", "26_Tom" };
static const char *const DEMO_LABELS[DEMO_CHAIN_COUNT] = { "Pads", "Drums" };
int factoryDemoChain(const char *const **names, int which) {
    if (which == 1) { *names = DEMO_DRUMS; return (int)(sizeof(DEMO_DRUMS) / sizeof(DEMO_DRUMS[0])); }
    *names = DEMO_CHAIN;
    return (int)(sizeof(DEMO_CHAIN) / sizeof(DEMO_CHAIN[0]));
}
const char *factoryDemoLabel(int which) { return DEMO_LABELS[which == 1 ? 1 : 0]; }

// The mono parts of set 2: one voice, legato glide (a held key slides to the
// next instead of restarting the envelopes or the pitch envelope).
static void monoLegato(Patch &p, float glideMs) {
    p.voices = 1; p.glideEnabled = true; p.glideMs = glideMs; p.glideMode = GlideMode::LEGATO;
}

// Factory voicing notes (set 1). Levels: velocity 100, a 4-note chord around
// C4 lands at about -6dB going into the output limiter, with no limiting
// (checked with tools/host/morphase_render). Drive is Off in every one;
// Amount holds the grit `t` adds. Modulators carry the velocity, so playing
// harder brightens the sound. Key scaling is kept low - a high one was part
// of why the old E.Piano sounded dull up the keyboard.
static void factoryPatch(int index, Patch &p) {
    patchInitDefaults(p);                  // Drive: Mix 0, Amount 2.0
    if (index >= FACTORY_NEW_COUNT) {
        // The old patches were voiced with the pre-v0.72 defaults (Drive
        // 1.5, Mix 100). Kept so they sound the same until they are redone.
        p.limiterDrive = 1.5f; p.limiterMix = 100.f;
    }
    switch (index) {
    case 0: // 01_EPiano - algorithm 5, the DX7 E.PIANO 1 way: two pairs.
        // OP2->OP1 is the body (1:1, gentle), OP4->OP3 the tine (modulator at
        // ratio 14 that dies within ~0.2s: the "ding" of the strike).
        strcpy(p.name, "01_EPiano"); p.algorithm = 5;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 0.f, 55.f, 20.f, 32.f, 25.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f,  32.f, 0.f, 45.f, 10.f, 30.f, 75.f, 10.f);
        setOp(p, 2, 0.f, 1.f,   3.f,  60.f, 0.f, 45.f,  0.f, 30.f, 40.f, 0.f);
        setOp(p, 3, 0.f, 14.f,  0.f,  30.f, 0.f, 15.f,  0.f, 15.f, 85.f, 15.f);
        p.chorusMix = 18.f; p.chorusRateHz = 0.6f; p.chorusDepthMs = 5.f;
        p.reverbMix = 15.f;
        p.volume = 50.f;
        break;
    case 1: // 02_EPBright - 01 with a louder tine and more velocity on both
        // modulators: soft notes stay round, hard ones bark. Cuts through a band.
        strcpy(p.name, "02_EPBright"); p.algorithm = 5;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 0.f, 50.f, 20.f, 30.f, 25.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f,  48.f, 0.f, 40.f, 15.f, 30.f, 90.f, 10.f);
        setOp(p, 2, 0.f, 1.f,   4.f,  70.f, 0.f, 40.f,  0.f, 30.f, 40.f, 0.f);
        setOp(p, 3, 0.f, 14.f,  0.f,  52.f, 0.f, 18.f,  0.f, 15.f, 95.f, 10.f);
        p.chorusMix = 22.f; p.chorusRateHz = 0.8f; p.chorusDepthMs = 5.f;
        p.reverbMix = 12.f;
        p.volume = 50.f;
        break;
    case 2: // 03_Clav - algorithm 3 (4>2>1 + 3>1). The carrier is morphed
        // towards the square end, OP3 at ratio 3 adds the nasal odd partial,
        // OP4 at ratio 5 snaps on the attack only. Short decay, quick release.
        strcpy(p.name, "03_Clav"); p.algorithm = 3;
        setOp(p, 0, 45.f, 1.f,  0.f, 100.f, 0.f, 32.f,  0.f, 10.f, 30.f, 0.f);
        setOp(p, 1,  0.f, 1.f,  0.f,  38.f, 0.f, 25.f,  0.f, 10.f, 80.f, 10.f);
        setOp(p, 2,  0.f, 3.f,  0.f,  30.f, 0.f, 28.f,  0.f, 10.f, 70.f, 10.f);
        setOp(p, 3,  0.f, 5.f,  0.f,  25.f, 0.f, 10.f,  0.f,  8.f, 85.f, 10.f);
        p.chorusMix = 10.f; p.chorusRateHz = 1.2f; p.chorusDepthMs = 3.f;
        p.volume = 39.f;
        break;
    case 3: // 04_Organ - algorithm 8, four sine drawbars: 16' 8' 4' 2 2/3'
        // (ratios 0.5 / 1 / 2 / 3). No velocity, gate-like envelope, the
        // chorus as a slow rotor.
        strcpy(p.name, "04_Organ"); p.algorithm = 8;
        setOp(p, 0, 0.f, 0.5f,  0.f,  65.f, 1.f, 0.f, 100.f, 6.f, 0.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f, 100.f, 1.f, 0.f, 100.f, 6.f, 0.f, 0.f);
        setOp(p, 2, 0.f, 2.f,   0.f,  70.f, 1.f, 0.f, 100.f, 6.f, 0.f, 0.f);
        setOp(p, 3, 0.f, 3.f,   0.f,  45.f, 1.f, 0.f, 100.f, 6.f, 0.f, 0.f);
        p.chorusMix = 35.f; p.chorusRateHz = 4.5f; p.chorusDepthMs = 2.5f;
        p.reverbMix = 15.f;
        p.volume = 50.f;
        break;
    case 4: // 05_Bell - algorithm 5: two pairs, ONE modulator per carrier.
        // The first voicing put three inharmonic modulators into one carrier
        // (algorithm 4); their sums and differences made ~60 partials, many
        // 16Hz apart (a rough beat) and some below the note (heavy) - heard
        // as noisy and distorted though nothing clipped. A pair cannot
        // intermodulate: 10 clean partials across the keyboard.
        // OP2->OP1 (3.5:1) is the bell body; OP4->OP3 (7:4, +4 cents) the
        // sparkle on top, with a slow shimmer against the body.
        strcpy(p.name, "05_Bell"); p.algorithm = 5;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 0.f, 72.f, 0.f, 62.f, 20.f, 0.f);
        setOp(p, 1, 0.f, 3.5f,  0.f,  42.f, 0.f, 55.f, 0.f, 50.f, 70.f, 15.f);
        setOp(p, 2, 0.f, 4.f,   4.f,  68.f, 0.f, 50.f, 0.f, 45.f, 40.f, 0.f);
        setOp(p, 3, 0.f, 7.f,   0.f,  40.f, 0.f, 30.f, 0.f, 25.f, 85.f, 15.f);
        p.reverbMix = 30.f; p.reverbSize = 65.f;
        p.volume = 52.f;
        break;
    case 5: // 06_Gong - algorithm 14 (diamond). One source drives two paths
        // into the carrier; OP4 swells in a little later, so the spectrum
        // keeps moving after the strike. The paths sit on one grid (3.5 and
        // 7.0, the source at 1.75) and there is no feedback: the first
        // voicing (1.63 / 2.37 / 3.17 + feedback) scattered ~100-200
        // partials and put a 16-37Hz rumble under the note ("jurujuru" in
        // the bass). Now ~25 partials, the lowest at 0.74x the note.
        strcpy(p.name, "06_Gong"); p.algorithm = 14;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 0.f, 85.f, 0.f, 75.f, 20.f, 0.f);
        setOp(p, 1, 0.f, 7.f,   0.f,  18.f, 0.f, 75.f, 20.f, 70.f, 60.f, 10.f);
        setOp(p, 2, 0.f, 3.5f,  0.f,  28.f, 0.f, 70.f, 10.f, 70.f, 60.f, 10.f);
        setOp(p, 3, 0.f, 1.75f, 0.f,  18.f, 22.f, 80.f, 30.f, 70.f, 50.f, 10.f);
        p.feedback = 0.f;
        p.reverbMix = 45.f; p.reverbSize = 80.f; p.reverbDamp = 40.f;
        p.volume = 76.f;   // v0.86: 57 -> 76 (+2.5dB, sounded small in the run-through)
        break;
    case 6: // 07_Marimba - algorithm 5 like 01_EPiano, so the two morph into
        // each other without a jump (the morph.txt pair). A short body, a
        // 4x bar partial, a mallet click and a 2-semitone pitch drop for the
        // attack.
        strcpy(p.name, "07_Marimba"); p.algorithm = 5;
        // The mallet modulators are strong and velocity-heavy, so a soft hit
        // is woody and a hard one clacks (renders: centroid ~2x from vel 40 to 127).
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 0.f, 36.f, 0.f, 26.f, 30.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f,  45.f, 0.f, 12.f, 0.f, 12.f, 90.f, 10.f);
        setOp(p, 2, 0.f, 4.f,   0.f,  32.f, 0.f, 16.f, 0.f, 14.f, 60.f, 0.f);
        setOp(p, 3, 0.f, 10.f,  0.f,  32.f, 0.f,  8.f, 0.f,  8.f, 95.f, 10.f);
        p.penvAmt = 2.f; p.penvDecay = 8.f; p.penvVel = 40.f;
        p.reverbMix = 15.f;
        p.volume = 50.f;
        break;
    case 7: // 08_Glock - algorithm 13 (Pair+2, feedback on OP2): the bar's
        // partials as two plain oscillators (2.76x and 5.4x the pair's pitch),
        // the pair an octave up with a short metallic strike. Bright, short.
        strcpy(p.name, "08_Glock"); p.algorithm = 13;
        // The strike (OP2) is what velocity opens; the two partials follow
        // the velocity in level, so soft notes are also less glassy.
        setOp(p, 0, 0.f, 2.f,   0.f, 100.f, 0.f, 50.f, 0.f, 45.f, 25.f, 0.f);
        setOp(p, 1, 0.f, 3.5f,  0.f,  45.f, 0.f, 10.f, 0.f, 10.f, 95.f, 10.f);
        setOp(p, 2, 0.f, 5.52f, 0.f,  38.f, 0.f, 40.f, 0.f, 35.f, 60.f, 0.f);
        setOp(p, 3, 0.f, 10.8f, 0.f,  20.f, 0.f, 25.f, 0.f, 22.f, 70.f, 0.f);
        p.feedback = 10.f;
        p.reverbMix = 25.f; p.reverbSize = 55.f;
        p.volume = 54.f;
        break;
    // ---- Set 2 (v0.75): bass and lead, all mono with legato glide. Levels:
    // velocity 100, one note (C2 for the basses, C4 for the leads), about
    // -6dB into the output limiter. Feedback raises the partial count, so
    // the three patches that use it were kept under noise 0.01.
    case 8: // 09_Bass - algorithm 5. Body pair 1:1 with a quick pluck in the
        // modulator, plus a sub-octave pair (carrier at 0.5) for weight.
        strcpy(p.name, "09_Bass"); p.algorithm = 5;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 0.f, 45.f, 55.f, 14.f, 30.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f,  45.f, 0.f, 22.f, 20.f, 14.f, 85.f, 0.f);
        setOp(p, 2, 0.f, 0.5f,  0.f,  75.f, 0.f, 50.f, 70.f, 14.f, 20.f, 0.f);
        setOp(p, 3, 0.f, 1.f,   0.f,  18.f, 0.f, 18.f,  0.f, 10.f, 70.f, 0.f);
        monoLegato(p, 60.f);
        p.volume = 89.f;
        break;
    case 9: // 10_Growl - algorithm 10 (4>3>2>1, feedback on OP2, mid-chain).
        // The feedback is the growl: the LFO wobbles it, and tilt X (when
        // the IMU is on) opens it further.
        strcpy(p.name, "10_Growl"); p.algorithm = 10;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 0.f, 40.f, 80.f, 14.f, 30.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f,  45.f, 0.f, 35.f, 60.f, 14.f, 70.f, 0.f);
        setOp(p, 2, 0.f, 2.f,   0.f,  22.f, 0.f, 30.f, 40.f, 14.f, 60.f, 0.f);
        setOp(p, 3, 0.f, 1.f,   0.f,  15.f, 0.f, 25.f, 30.f, 14.f, 60.f, 0.f);
        p.feedback = 30.f;
        p.lfoTarget = ModTarget::FEEDBACK; p.lfoWave = LfoWave::TRIANGLE; p.lfoRateHz = 3.5f; p.lfoDepth = 25.f; p.lfoDelayMs = 250.f;
        p.imuXTarget = ModTarget::FEEDBACK; p.imuXDepth = 60.f;
        monoLegato(p, 50.f);
        p.volume = 71.f;   // v0.86: 95 -> 71 (-2.5dB)
        break;
    case 10: // 11_SawBass - algorithm 12 (3>2>1 + 4, feedback on OP4, a
        // carrier). OP4 fed back is a saw; the FM stack under it gives the
        // attack. OP4 +5 cents for a slow analog beat against the stack.
        strcpy(p.name, "11_SawBass"); p.algorithm = 12;
        setOp(p, 0, 0.f, 1.f,   0.f,  80.f, 0.f, 40.f, 60.f, 14.f, 30.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f,  35.f, 0.f, 30.f, 30.f, 14.f, 75.f, 0.f);
        setOp(p, 2, 0.f, 2.f,   0.f,  18.f, 0.f, 25.f, 10.f, 14.f, 70.f, 0.f);
        setOp(p, 3, 0.f, 1.f,   5.f,  85.f, 0.f, 60.f, 70.f, 14.f, 30.f, 0.f);
        p.feedback = 45.f;
        monoLegato(p, 50.f);
        p.volume = 83.f;
        break;
    case 11: // 12_SubBass - algorithm 4 (proposed): an almost pure sine body
        // and a 2:1 click that is gone in ~30ms; OP3 / OP4 are silent. One
        // carrier on purpose: the carrier-count scaling would take 3dB from
        // a 2-carrier algorithm even with the second carrier at level 0.
        // Mostly felt rather than heard on the speaker: judge on earphones.
        strcpy(p.name, "12_SubBass"); p.algorithm = 4;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 1.f, 60.f, 90.f, 12.f, 25.f, 0.f);
        setOp(p, 1, 0.f, 2.f,   0.f,  25.f, 0.f,  8.f,  0.f,  8.f, 90.f, 0.f);
        setOp(p, 2, 0.f, 1.f,   0.f,   0.f, 0.f, 40.f, 50.f, 12.f, 0.f, 0.f);
        setOp(p, 3, 0.f, 1.f,   0.f,   0.f, 0.f, 40.f, 50.f, 12.f, 0.f, 0.f);
        monoLegato(p, 40.f);
        p.volume = 94.f;
        break;
    case 12: // 13_MorphLd - algorithm 7 (proposed): one modulator into three
        // carriers (1x, 2x, 1x), each at a different point of the waveform
        // chain and slightly detuned. GMorph sweeps all three at once - tilt
        // X drives it (the MORPHASE signature), the LFO adds a slow drift.
        strcpy(p.name, "13_MorphLd"); p.algorithm = 7;
        setOp(p, 0, 10.f, 1.f, -6.f, 100.f, 2.f, 40.f, 85.f, 20.f, 25.f, 0.f);
        setOp(p, 1, 30.f, 2.f,  0.f,  70.f, 2.f, 40.f, 80.f, 20.f, 25.f, 0.f);
        setOp(p, 2, 20.f, 1.f,  6.f,  90.f, 2.f, 40.f, 85.f, 20.f, 25.f, 0.f);
        setOp(p, 3,  0.f, 1.f,  0.f,  25.f, 0.f, 40.f, 50.f, 20.f, 70.f, 10.f);
        p.lfoTarget = ModTarget::GMORPH; p.lfoWave = LfoWave::TRIANGLE; p.lfoRateHz = 0.4f; p.lfoDepth = 12.f;
        p.imuXTarget = ModTarget::GMORPH; p.imuXDepth = 80.f;
        p.delayMix = 18.f; p.delayTimeMs = 330.f;
        monoLegato(p, 40.f);
        p.volume = 100.f;
        break;
    case 13: // 14_SawLead - algorithm 9 (4>3>2>1, feedback on OP1, the
        // carrier): the carrier fed back is the saw; the chain above adds
        // a little edge on the attack. Delayed vibrato from the LFO.
        strcpy(p.name, "14_SawLead"); p.algorithm = 9;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 1.f, 40.f, 85.f, 18.f, 25.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f,  20.f, 0.f, 35.f, 50.f, 18.f, 70.f, 0.f);
        setOp(p, 2, 0.f, 2.f,   0.f,  12.f, 0.f, 30.f, 40.f, 18.f, 60.f, 0.f);
        setOp(p, 3, 0.f, 3.f,   0.f,  10.f, 0.f, 25.f, 30.f, 18.f, 60.f, 0.f);
        // Feedback 40, not 50: at 50 the saw's upper partials folded back
        // (aliased) into the top partials from about C6 up.
        p.feedback = 40.f;
        p.lfoTarget = ModTarget::PITCH; p.lfoRateHz = 5.5f; p.lfoDepth = 18.f; p.lfoDelayMs = 600.f;
        p.delayMix = 15.f; p.delayTimeMs = 300.f;
        monoLegato(p, 40.f);
        p.volume = 100.f;
        break;
    case 14: // 15_Whistle - algorithm 4 (proposed): one near-sine carrier,
        // two faint integer-ratio modulators (1x for a breathy edge on the
        // attack, 2x for a touch of upper partial while it sounds), a soft
        // attack, and vibrato that only comes in after ~0.7s. Integer ratios,
        // so two modulators on one carrier stay clean (set 1 lesson).
        strcpy(p.name, "15_Whistle"); p.algorithm = 4;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 12.f, 30.f, 90.f, 20.f, 20.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f,   8.f,  0.f, 20.f,  5.f, 20.f, 60.f, 0.f);
        setOp(p, 2, 0.f, 2.f,   0.f,   8.f, 12.f, 30.f, 70.f, 20.f, 20.f, 0.f);
        setOp(p, 3, 0.f, 1.f,   0.f,   0.f,  0.f, 30.f, 50.f, 20.f, 0.f, 0.f);
        p.lfoTarget = ModTarget::PITCH; p.lfoRateHz = 5.5f; p.lfoDepth = 25.f; p.lfoDelayMs = 700.f;
        p.reverbMix = 18.f;
        monoLegato(p, 60.f);
        p.volume = 75.f;   // v0.86: 100 -> 75 (-2.5dB)
        break;
    case 15: // 16_DirtyLd - algorithm 2 (proposed): a bright FM lead with
        // the Drive pad ON - the example of grit added on purpose. `t` takes
        // the Drive out; the level then rises (the curve also squashes), so
        // the Volume was set to keep the output limiter in range both ways.
        strcpy(p.name, "16_DirtyLd"); p.algorithm = 2;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 1.f, 40.f, 80.f, 18.f, 25.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f,  45.f, 0.f, 35.f, 60.f, 18.f, 70.f, 0.f);
        setOp(p, 2, 0.f, 2.f,   0.f,  25.f, 0.f, 30.f, 40.f, 18.f, 60.f, 0.f);
        setOp(p, 3, 0.f, 1.f,   7.f,  25.f, 0.f, 30.f, 50.f, 18.f, 60.f, 0.f);
        p.feedback = 15.f;
        p.limiterDrive = 3.f; p.limiterMix = 100.f;
        p.delayMix = 20.f; p.delayTimeMs = 300.f;
        monoLegato(p, 30.f);
        p.volume = 100.f;
        break;
    // ---- Set 3 (v0.76): 17-20 brass / strings / pads, 21-24 additive /
    // morph, all poly (velocity 100, a 4-note chord at C4, about -6dB into
    // the output limiter). 21-24 share algorithm 7 (one modulator, three
    // carriers): the waveform morph moves three waveforms at once, and the
    // four chain into each other without a jump (the demo chain).
    case 16: // 17_Brass - the old factory Brass, kept as it was (the author
        // found it the most convincing patch of the old set; a softer
        // two-pair voicing tried first sounded like a muted trumpet).
        // Algorithm 2 stacks two modulators onto one, and the Drive curve
        // is ON (1.5 / 100): its saturation is the brassy edge - that grit
        // reads as noise 0.04 on the analysis and is wanted here.
        // Level: a squashed sound peaks low but averages high, so it is
        // matched by RMS (about -17.5dB, a touch above the pads), not by
        // the -6dB peak target, which Drive at Mix 100 cannot reach.
        strcpy(p.name, "17_Brass"); p.algorithm = 2;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 15.f, 30.f, 80.f, 25.f, 30.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f,  60.f, 20.f, 40.f, 60.f, 25.f, 85.f, 30.f);
        setOp(p, 2, 0.f, 1.f,   7.f,  40.f, 20.f, 40.f, 60.f, 25.f, 85.f, 30.f);
        setOp(p, 3, 0.f, 2.f,   0.f,  25.f, 12.f, 40.f, 50.f, 25.f, 90.f, 40.f);
        p.chorusMix = 15.f;
        p.limiterDrive = 1.5f; p.limiterMix = 100.f;
        p.volume = 65.f;
        break;
    case 17: // 18_Strings - algorithm 7. Three saw-leaning carriers (waveform
        // morph 72 / 72 / 66, detuned -7 / +7 cents and an octave up at -4)
        // for the section, under one light, STEADY modulator. v0.79 (the
        // author: "too blurred, more like a wind band than strings"): an
        // attack-only modulator that followed the velocity had made the tone
        // brighten as it swelled - the FM brass recipe - so OP4 now holds its
        // level and barely follows the velocity; the octave carrier is lower
        // and comes in a little later; the chorus (8ms -> 4ms, Mix 50 -> 25)
        // and reverb (30 -> 15) were blurring the image. Morph stays at 72,
        // not the ESaw end (85+): ESaw has a jump in its cycle, so its
        // partials never stop and the ones past Nyquist fold back as a haze.
        strcpy(p.name, "18_Strings"); p.algorithm = 7;
        setOp(p, 0, 72.f, 1.f, -7.f, 100.f, 22.f, 35.f, 85.f, 45.f, 25.f, 0.f);
        setOp(p, 1, 72.f, 1.f,  7.f,  90.f, 22.f, 35.f, 85.f, 45.f, 25.f, 0.f);
        setOp(p, 2, 66.f, 2.f, -4.f,  30.f, 32.f, 35.f, 85.f, 45.f, 25.f, 0.f);
        setOp(p, 3,  0.f, 1.f,  0.f,  10.f, 15.f, 30.f, 100.f, 45.f, 30.f, 10.f);
        p.lfoTarget = ModTarget::PITCH; p.lfoRateHz = 5.2f; p.lfoDepth = 6.f; p.lfoDelayMs = 600.f;
        p.chorusMix = 25.f; p.chorusRateHz = 0.6f; p.chorusDepthMs = 4.f;
        p.reverbMix = 15.f; p.reverbSize = 55.f;
        p.volume = 55.f;
        break;
    case 18: // 19_WarmPad - algorithm 5, low-index pairs (the second an
        // octave up, +6 cents), a ~1s attack, a ~3s release, and a slow
        // waveform drift from the LFO.
        strcpy(p.name, "19_WarmPad"); p.algorithm = 5;
        setOp(p, 0, 15.f, 1.f,  0.f, 100.f, 55.f, 60.f, 85.f, 62.f, 25.f, 0.f);
        setOp(p, 1,  0.f, 1.f,  0.f,  25.f, 65.f, 60.f, 60.f, 62.f, 60.f, 10.f);
        setOp(p, 2, 10.f, 2.f,  6.f,  45.f, 60.f, 60.f, 80.f, 62.f, 25.f, 0.f);
        setOp(p, 3,  0.f, 1.f,  0.f,  18.f, 65.f, 60.f, 60.f, 62.f, 60.f, 10.f);
        p.lfoTarget = ModTarget::GMORPH; p.lfoWave = LfoWave::TRIANGLE; p.lfoRateHz = 0.25f; p.lfoDepth = 15.f;
        p.chorusMix = 40.f; p.reverbMix = 40.f; p.reverbSize = 75.f;
        p.volume = 100.f;  // v0.86: 93 -> 100 (+0.6dB, the most Volume gives)
        break;
    case 19: // 20_Choir - algorithm 5. A soft 1:1 body and a second pair
        // whose carrier sits on the 3rd harmonic with its own 1:1
        // sidebands - a cluster around 2-4x that reads as an "ah" vowel in
        // the middle of the keyboard. Vibrato, a wide chorus, a big room.
        strcpy(p.name, "20_Choir"); p.algorithm = 5;
        setOp(p, 0, 5.f, 1.f,   0.f, 100.f, 40.f, 50.f, 85.f, 50.f, 25.f, 0.f);
        setOp(p, 1, 0.f, 1.f,   0.f,  20.f, 45.f, 50.f, 50.f, 50.f, 50.f, 10.f);
        setOp(p, 2, 0.f, 3.f,   0.f,  75.f, 40.f, 50.f, 85.f, 50.f, 30.f, 0.f);
        setOp(p, 3, 0.f, 1.f,   0.f,  22.f, 45.f, 50.f, 60.f, 50.f, 50.f, 10.f);
        p.lfoTarget = ModTarget::PITCH; p.lfoRateHz = 5.f; p.lfoDepth = 10.f; p.lfoDelayMs = 400.f;
        p.chorusMix = 45.f; p.chorusRateHz = 0.6f; p.chorusDepthMs = 8.f;
        p.reverbMix = 45.f; p.reverbSize = 75.f;
        p.volume = 100.f;
        break;
    case 20: // 21_Additive - algorithm 7. Three carriers at 1x / 2x / 3x, each
        // at its own point of the waveform chain and each decaying faster
        // than the one below, so the upper partials fade first - an
        // evolving tone, unlike 04_Organ's steady sines. OP4 adds a hint of FM.
        strcpy(p.name, "21_Additive"); p.algorithm = 7;
        setOp(p, 0, 10.f, 1.f,  0.f, 100.f, 5.f, 50.f, 70.f, 40.f, 30.f, 0.f);
        setOp(p, 1, 30.f, 2.f,  0.f,  65.f, 5.f, 40.f, 50.f, 35.f, 40.f, 0.f);
        setOp(p, 2, 50.f, 3.f,  0.f,  45.f, 5.f, 30.f, 30.f, 30.f, 50.f, 0.f);
        setOp(p, 3,  0.f, 1.f,  0.f,  10.f, 5.f, 40.f, 40.f, 30.f, 60.f, 10.f);
        p.reverbMix = 20.f;
        p.volume = 56.f;
        break;
    case 21: // 22_Evolve - algorithm 7. A very slow LFO (0.1Hz) sweeps GMorph,
        // so a held chord keeps changing colour for as long as it is held.
        strcpy(p.name, "22_Evolve"); p.algorithm = 7;
        setOp(p, 0, 10.f, 1.f, -5.f, 100.f, 50.f, 60.f, 90.f, 60.f, 25.f, 0.f);
        setOp(p, 1, 40.f, 2.f,  0.f,  55.f, 50.f, 60.f, 90.f, 60.f, 25.f, 0.f);
        setOp(p, 2, 60.f, 1.f,  5.f,  75.f, 50.f, 60.f, 90.f, 60.f, 25.f, 0.f);
        setOp(p, 3,  0.f, 1.f,  0.f,  15.f, 50.f, 60.f, 60.f, 60.f, 50.f, 10.f);
        p.lfoTarget = ModTarget::GMORPH; p.lfoWave = LfoWave::TRIANGLE; p.lfoRateHz = 0.1f; p.lfoDepth = 45.f;
        p.chorusMix = 30.f; p.reverbMix = 40.f; p.reverbSize = 70.f;
        p.volume = 69.f;
        break;
    case 22: // 23_TiltPad - algorithm 7, 13_MorphLd made polyphonic: tilt X
        // sweeps the three carriers' waveforms (Depth 90). The LFO is Off -
        // the tilt is the whole performance.
        strcpy(p.name, "23_TiltPad"); p.algorithm = 7;
        setOp(p, 0, 10.f, 1.f, -6.f, 100.f, 30.f, 50.f, 85.f, 50.f, 25.f, 0.f);
        setOp(p, 1, 30.f, 2.f,  0.f,  60.f, 30.f, 50.f, 85.f, 50.f, 25.f, 0.f);
        setOp(p, 2, 20.f, 1.f,  6.f,  85.f, 30.f, 50.f, 85.f, 50.f, 25.f, 0.f);
        setOp(p, 3,  0.f, 1.f,  0.f,  22.f, 30.f, 50.f, 60.f, 50.f, 70.f, 10.f);
        p.lfoTarget = ModTarget::OFF; p.lfoDepth = 0.f;
        p.imuXTarget = ModTarget::GMORPH; p.imuXDepth = 90.f;
        p.chorusMix = 30.f; p.reverbMix = 35.f; p.reverbSize = 70.f;
        p.volume = 100.f;
        break;
    case 23: // 24_Shimmer - algorithm 7. Carriers at 1x, 4x and 8x, the upper
        // two detuned +5 / -5 cents so they shimmer against each other;
        // ONE modulator (3x) feeds all three, so nothing intermodulates
        // (the set 1 lesson). A slow GMorph drift and a long, large reverb.
        strcpy(p.name, "24_Shimmer"); p.algorithm = 7;
        setOp(p, 0, 0.f, 1.f,   0.f, 100.f, 35.f, 60.f, 85.f, 65.f, 25.f, 0.f);
        setOp(p, 1, 0.f, 4.f,   5.f,  45.f, 40.f, 60.f, 70.f, 65.f, 40.f, 0.f);
        setOp(p, 2, 0.f, 8.f,  -5.f,  30.f, 45.f, 60.f, 60.f, 65.f, 40.f, 0.f);
        setOp(p, 3, 0.f, 3.f,   3.f,  14.f, 30.f, 60.f, 60.f, 65.f, 60.f, 10.f);
        p.lfoTarget = ModTarget::GMORPH; p.lfoWave = LfoWave::TRIANGLE; p.lfoRateHz = 0.3f; p.lfoDepth = 15.f;
        p.chorusMix = 30.f; p.reverbMix = 55.f; p.reverbSize = 85.f;
        p.volume = 100.f;
        break;
    // ---- Set 4 (v0.79): 25-28 drums, all poly (a fast repeat must not cut
    // the last hit's decay), one hit at velocity 100 lands near -9dB; 29-32
    // FX. Low drums are judged on earphones - the built-in speaker hardly
    // reaches them.
    case 24: // 25_Kick - algorithm 4 (one carrier): a sine body with a big,
        // fast pitch drop (+30 semitones over ~0.12s, deeper the harder it
        // is hit) and a fixed-pitch modulator for an instant: the beater's
        // click. Follows the keys (C1-C2 for a kick). Morph pair with 26_Tom
        // (the same algorithm).
        strcpy(p.name, "25_Kick"); p.algorithm = 4;
        setOp(p, 0, 0.f, 1.f, 0.f, 100.f, 0.f, 40.f, 0.f, 30.f, 50.f, 0.f);
        setOp(p, 1, 0.f, 1.f, 0.f,  45.f, 0.f,  4.f, 0.f,  4.f, 70.f, 0.f);
        setOp(p, 2, 0.f, 1.f, 0.f,   0.f, 0.f,  0.f, 0.f,  0.f, 0.f, 0.f);
        setOp(p, 3, 0.f, 1.f, 0.f,   0.f, 0.f,  0.f, 0.f,  0.f, 0.f, 0.f);
        p.op[1].fixed = true; p.op[1].fixNote = 100.f;   // ~2.6kHz, whatever key
        p.penvAmt = 30.f; p.penvDecay = 12.f; p.penvVel = 50.f;
        p.volume = 100.f;
        break;
    case 25: // 26_Tom - algorithm 4 like 25_Kick: higher, a smaller drop,
        // a little 1:1 body tone; it follows the keys, so toms can be laid
        // out as a scale.
        strcpy(p.name, "26_Tom"); p.algorithm = 4;
        setOp(p, 0, 0.f, 1.f, 0.f, 100.f, 0.f, 32.f, 0.f, 30.f, 50.f, 0.f);
        setOp(p, 1, 0.f, 1.f, 0.f,  30.f, 0.f,  4.f, 0.f,  4.f, 70.f, 0.f);
        setOp(p, 2, 0.f, 1.f, 0.f,  18.f, 0.f, 20.f, 0.f, 20.f, 50.f, 0.f);
        setOp(p, 3, 0.f, 1.f, 0.f,   0.f, 0.f,  0.f, 0.f,  0.f, 0.f, 0.f);
        p.op[1].fixed = true; p.op[1].fixNote = 100.f;
        p.penvAmt = 10.f; p.penvDecay = 16.f; p.penvVel = 40.f;
        p.volume = 100.f;
        break;
    case 26: // 27_Snare - algorithm 6 (4>3>2 + 1): OP1 the drum head (follows
        // the keys, a small pitch drop) is the lead; the chain 4>3>2 is the
        // snare wires. v0.79T2 (the author: "metallic, hard on the ears - a
        // struck head should be softer"): the chain had sat at 2-9kHz, where
        // its sidebands stand far apart - a cluster of metal tones, not
        // noise. Now the carrier is mid (1.6kHz) and the modulators LOW and
        // deep (1.0kHz / 0.7kHz with the feedback), so the sidebands pack
        // closely: a soft band over 0.5-3.5kHz, under the head's level.
        // v0.79T3 (the author: "still sounds high, even at C3"): the whole
        // chain a further 7 semitones down (carrier 1.2kHz) and a little
        // shorter.
        strcpy(p.name, "27_Snare"); p.algorithm = 6;
        setOp(p, 0, 0.f, 1.f, 0.f, 100.f, 0.f, 18.f, 0.f, 18.f, 50.f, 0.f);
        setOp(p, 1, 0.f, 1.f, 0.f,  70.f, 0.f, 15.f, 0.f, 15.f, 50.f, 0.f);
        setOp(p, 2, 0.f, 1.f, 0.f, 100.f, 0.f, 17.f, 0.f, 17.f, 30.f, 0.f);
        setOp(p, 3, 0.f, 1.f, 0.f, 100.f, 0.f, 17.f, 0.f, 17.f, 30.f, 0.f);
        p.op[1].fixed = true; p.op[1].fixNote = 84.f;
        p.op[2].fixed = true; p.op[2].fixNote = 76.f;
        p.op[3].fixed = true; p.op[3].fixNote = 70.f;
        p.feedback = 100.f;
        p.penvAmt = 7.f; p.penvDecay = 8.f; p.penvVel = 30.f;
        p.volume = 84.f;
        break;
    case 27: // 28_HiHat - algorithm 1 (one carrier), every operator fixed, so
        // it never follows the keys: a deep chain of high pitches (8.4 / 5.0
        // / 3.3 / 6.6kHz) with a slightly squared waveform, very short.
        // v0.79T2 (the author: "a hard steel rod with a thick core - a hat
        // is a thin plate"): the pitches had been close together, and close
        // pitches leave difference tones down at 0.1-1kHz - the core. Spread
        // wide apart, the energy under 3kHz fell from -8.9dB to -11.3dB and
        // the sound got noisier (flatness 0.10 -> 0.13). (A light modulation
        // was tried too: it rang like a bell.)
        strcpy(p.name, "28_HiHat"); p.algorithm = 1;
        setOp(p, 0, 60.f, 1.f, 0.f, 100.f, 0.f, 8.f, 0.f, 8.f, 50.f, 0.f);
        setOp(p, 1, 60.f, 1.f, 0.f, 100.f, 0.f, 9.f, 0.f, 9.f, 30.f, 0.f);
        setOp(p, 2, 60.f, 1.f, 0.f, 100.f, 0.f, 9.f, 0.f, 9.f, 30.f, 0.f);
        setOp(p, 3, 60.f, 1.f, 0.f, 100.f, 0.f, 9.f, 0.f, 9.f, 30.f, 0.f);
        p.op[0].fixed = true; p.op[0].fixNote = 120.f;
        p.op[1].fixed = true; p.op[1].fixNote = 111.f;
        p.op[2].fixed = true; p.op[2].fixNote = 104.f;
        p.op[3].fixed = true; p.op[3].fixNote = 116.f;
        p.feedback = 100.f;
        // v0.79T3 (the author: the attack hurt in earphones; wanted a noisy
        // tail): quieter, and a small bright room for the tail (chosen by
        // ear in an 8-beat loop over a longer decay).
        p.reverbMix = 20.f; p.reverbSize = 35.f; p.reverbDamp = 30.f;
        p.volume = 74.f;
        break;
    case 28: // 29_Metallic - the old Metallic redone. Algorithm 11 (4>3 +
        // 2>1, feedback on OP4 and OP2): carriers 1 and 3, modulators 3.5 and
        // 4.5 - one non-integer modulator per carrier, every ratio on one
        // 0.5 grid, so the partials (1.5, 2.5, 4.5 ...) are metallic but
        // nothing beats or falls under the note. (A first try at carriers
        // 1 : 2.76 with a 1.41 modulator put a sideband at 0.06x: a 16Hz
        // throb.) The Delay shows off the clean echoes of v0.78.
        strcpy(p.name, "29_Metallic"); p.algorithm = 11;
        setOp(p, 0, 0.f, 1.f,  0.f, 100.f, 0.f, 55.f,  0.f, 50.f,  0.f, 0.f);
        setOp(p, 1, 0.f, 3.5f, 0.f,  45.f, 0.f, 45.f, 20.f, 45.f, 60.f, 20.f);
        setOp(p, 2, 0.f, 3.f,  0.f,  55.f, 0.f, 45.f,  0.f, 45.f,  0.f, 0.f);
        setOp(p, 3, 0.f, 4.5f, 0.f,  40.f, 0.f, 40.f, 20.f, 40.f, 60.f, 20.f);
        p.feedback = 25.f; p.delayMix = 20.f; p.reverbMix = 20.f;
        p.volume = 40.f;   // v0.86: 50 -> 40 (-2dB)
        break;
    case 29: // 30_CrushLd - a mono lead, algorithm 9 (feedback on the carrier
        // OP1), crushed hard (5 bits, 1/6 the rate): the grit is the point.
        // The aliasing of the Crush is wanted; with the Crush out the C6
        // partials are clean (the feedback does not fold back).
        strcpy(p.name, "30_CrushLd"); p.algorithm = 9;
        setOp(p, 0, 60.f, 1.f, 0.f, 100.f, 0.f, 30.f, 80.f, 25.f,  0.f, 0.f);
        setOp(p, 1,  0.f, 1.f, 0.f,  30.f, 0.f, 30.f, 60.f, 25.f, 50.f, 20.f);
        setOp(p, 2,  0.f, 2.f, 0.f,  20.f, 0.f, 30.f, 50.f, 25.f, 50.f, 20.f);
        setOp(p, 3,  0.f, 1.f, 0.f,   0.f, 0.f,  0.f,  0.f,  0.f,  0.f, 0.f);
        p.feedback = 30.f;
        p.crushBits = 5.f; p.crushDown = 6.f; p.crushMix = 80.f;
        monoLegato(p, 60.f);
        p.volume = 71.f;   // v0.86: 90 -> 71 (-2dB)
        break;
    case 30: // 31_NoiseSwl - algorithm 1: the carrier follows the keys, the
        // three modulators are fixed high pitches with the feedback on top,
        // so the noise has a pitch to play (chords work). It swells in over
        // ~1.5s; the LFO breathes the feedback, tilt X moves the waveforms.
        strcpy(p.name, "31_NoiseSwl"); p.algorithm = 1;
        setOp(p, 0, 0.f, 1.f, 0.f, 100.f, 70.f, 50.f, 90.f, 60.f, 0.f, 0.f);
        // v0.86 (the author: too much noise, no sense of pitch): OP2, the one
        // on the carrier, 80 -> 20. The pitch leads and the noise rides on
        // it like breath (the share of energy on the note's harmonics, C4:
        // -9.3 -> -2.9dB). Not 40-45: there the carrier itself all but
        // vanishes (FM index near the first zero of J0).
        setOp(p, 1, 0.f, 1.f, 0.f,  20.f, 70.f, 50.f, 85.f, 60.f, 0.f, 0.f);
        setOp(p, 2, 0.f, 1.f, 0.f,  85.f, 70.f, 50.f, 85.f, 60.f, 0.f, 0.f);
        setOp(p, 3, 0.f, 1.f, 0.f,  90.f, 60.f, 50.f, 90.f, 60.f, 0.f, 0.f);
        p.op[1].fixed = true; p.op[1].fixNote = 99.f;
        p.op[2].fixed = true; p.op[2].fixNote = 106.f;
        p.op[3].fixed = true; p.op[3].fixNote = 112.f;
        p.feedback = 100.f;
        p.lfoTarget = ModTarget::FEEDBACK; p.lfoWave = LfoWave::TRIANGLE; p.lfoRateHz = 0.3f; p.lfoDepth = 30.f;
        p.imuXTarget = ModTarget::GMORPH; p.imuXDepth = 60.f;
        p.reverbMix = 35.f; p.reverbSize = 80.f;
        p.volume = 37.f;   // v0.86: 46 -> 37 (-2dB)
        break;
    case 31: // 32_Drone - algorithm 7: three carriers (1x, 1x +7 cents, 2x
        // -5 cents) under one modulator, slow in and out; the LFO drifts the
        // waveforms for as long as the keys are held. Tilt X raises the
        // GLITCH probability (GlProb works with Glitch off; GLITCH itself is
        // a device setting, not saved in a patch): tilt it and it breaks up.
    default:
        strcpy(p.name, "32_Drone"); p.algorithm = 7;
        setOp(p, 0, 20.f, 1.f,  0.f, 100.f, 60.f, 60.f, 100.f, 70.f, 0.f, 0.f);
        setOp(p, 1, 35.f, 1.f,  7.f,  80.f, 65.f, 60.f, 100.f, 70.f, 0.f, 0.f);
        setOp(p, 2, 10.f, 2.f, -5.f,  40.f, 70.f, 60.f, 100.f, 70.f, 0.f, 0.f);
        setOp(p, 3,  0.f, 1.f,  0.f,  14.f, 60.f, 60.f, 100.f, 70.f, 0.f, 10.f);
        p.lfoTarget = ModTarget::GMORPH; p.lfoWave = LfoWave::TRIANGLE; p.lfoRateHz = 0.12f; p.lfoDepth = 25.f;
        p.imuXTarget = ModTarget::GLITCH_PROB; p.imuXDepth = 80.f;
        p.chorusMix = 30.f; p.reverbMix = 40.f; p.reverbSize = 85.f;
        p.volume = 100.f;
        break;
    }
}

// ---------------------------------------------------------------------------
// SD
// ---------------------------------------------------------------------------
static bool sdMounted = false;
bool sdReady() { return sdMounted; }

void (*bootProgress)(int phase) = nullptr;

// A delay that keeps the boot splash moving (v0.92T2): the waits between
// the SD attempts are spent drawing frames. A plain delay after boot.
static void bootWait(uint32_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) { if (bootProgress) bootProgress(1); delay(2); }
}

// ---------------------------------------------------------------------------
// The splash colour (v0.92T2). settings.json on the card holds the theme;
// a copy of its number lives in NVS (namespace "morphase", key "theme") so
// the splash can start at power-on, before the card is even looked for.
// Opened and closed on each use, written only when the number differs.
// Others in NVS: NimBLE "nimble_bond", M5GFX, M5Unified - all their own.
// ---------------------------------------------------------------------------
// ESP-IDF's NVS calls directly: Arduino's Preferences would put its error
// string table in DRAM (.data +52) for nothing.
static uint8_t nvsThemePlus1 = 0;   // what NVS holds, +1 (0 = nothing / unread)

int nvsReadTheme() {
    nvs_handle_t h;
    uint8_t v = 255;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, NVS_KEY_THEME, &v); nvs_close(h); }
    if (v >= (uint8_t)UiTheme::UI_THEME_COUNT) { nvsThemePlus1 = 0; return -1; }
    nvsThemePlus1 = v + 1;
    return v;
}

bool nvsThemeStale() { return (int)sys.theme + 1 != (int)nvsThemePlus1; }

void nvsWriteTheme() {
    if (!nvsThemeStale()) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    bool ok = nvs_set_u8(h, NVS_KEY_THEME, (uint8_t)sys.theme) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) { nvsThemePlus1 = (uint8_t)sys.theme + 1; Serial.printf("[NVS] theme %d\n", (int)sys.theme); }
}

bool sdInit() {
    pinMode(CAP_LORA_CS_PIN, OUTPUT);
    digitalWrite(CAP_LORA_CS_PIN, HIGH);
    if (esp_reset_reason() == ESP_RST_POWERON) bootWait(150);   // cold-boot rail settle (C.P.S. v0.99913)
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    const uint32_t rates[] = { 25000000, 16000000, 4000000, 1000000 };
    for (int i = 0; i < 4; i++) {
        uint32_t t0 = millis();
        if (SD.begin(SD_SPI_CS_PIN, SPI, rates[i])) {
            if (i > 0) Serial.printf("[SD] mounted at %u Hz (attempt %d)\n", (unsigned)rates[i], i + 1);
            sdMounted = true;
            return true;
        }
        Serial.printf("[SD] attempt %d at %u Hz failed (%lu ms)\n", i + 1, (unsigned)rates[i], (unsigned long)(millis() - t0));
        SD.end();
        bootWait(80);
    }
    sdMounted = false;
    return false;
}

void ensureFolders() {
    if (!sdMounted) return;
    if (!SD.exists(MORPHASE_FOLDER)) SD.mkdir(MORPHASE_FOLDER);
    if (!SD.exists(PATCH_FOLDER))    SD.mkdir(PATCH_FOLDER);
    if (!SD.exists(PATTERN_FOLDER))  SD.mkdir(PATTERN_FOLDER);
    if (!SD.exists(CHAIN_FOLDER))    SD.mkdir(CHAIN_FOLDER);       // v0.83
}

// ---------------------------------------------------------------------------
// Text buffer for one-shot writes (a hundred small f.printf() calls each
// reach the FAT layer separately and pushed audioTask over budget in
// C.P.S.; one write does not).
// ---------------------------------------------------------------------------
// v0.77: 6144 -> 4096 (2KB of RAM). Since v0.72 the current patch lives in
// current.json, so the largest file written here is settings.json at about
// 3,400 bytes with every MIDI slot filled (2,910 on a fresh card); a patch
// is about 1,900. A file that would not fit is refused and reported
// (commitBuffer), never cut short.
constexpr int SETTINGS_BUF_SIZE = 4096;
static char settingsBuf[SETTINGS_BUF_SIZE];
static int  settingsBufLen = 0;
static bool settingsBufOverflow = false;

static void sbReset() { settingsBufLen = 0; settingsBuf[0] = 0; settingsBufOverflow = false; }
static void sbAppend(const char *fmt, ...) {
    if (settingsBufLen >= SETTINGS_BUF_SIZE - 1) { settingsBufOverflow = true; return; }
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(settingsBuf + settingsBufLen, SETTINGS_BUF_SIZE - settingsBufLen, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= SETTINGS_BUF_SIZE - settingsBufLen) { settingsBufOverflow = true; settingsBufLen = SETTINGS_BUF_SIZE - 1; }
    else settingsBufLen += n;
}

static void writePatchFields(const Patch &P) {
    sbAppend("  \"morphase_format\": %d,\n", MORPHASE_PATCH_FORMAT);
    sbAppend("  \"name\": \"%s\",\n", P.name);
    for (int o = 0; o < NUM_OPS; o++) {
        const OpParams &p = P.op[o]; int n = o + 1;
        sbAppend("  \"op%d_morph\": %.2f,\n", n, p.morph);
        sbAppend("  \"op%d_ratio\": %.3f,\n", n, p.ratio);
        sbAppend("  \"op%d_detune\": %.2f,\n", n, p.detune);
        sbAppend("  \"op%d_level\": %.2f,\n", n, p.level);
        sbAppend("  \"op%d_attack\": %.2f,\n", n, p.attack);
        sbAppend("  \"op%d_decay\": %.2f,\n", n, p.decay);
        sbAppend("  \"op%d_sustain\": %.2f,\n", n, p.sustain);
        sbAppend("  \"op%d_release\": %.2f,\n", n, p.release);
        sbAppend("  \"op%d_velsens\": %.2f,\n", n, p.velSens);
        sbAppend("  \"op%d_keyscale\": %.2f,\n", n, p.keyScale);
        sbAppend("  \"op%d_fixed\": %d,\n", n, p.fixed ? 1 : 0);        // v0.71
        sbAppend("  \"op%d_fixnote\": %.0f,\n", n, p.fixNote);
    }
    sbAppend("  \"algorithm\": %d,\n", P.algorithm);
    sbAppend("  \"feedback\": %.2f,\n", P.feedback);
    sbAppend("  \"global_morph\": %.2f,\n", P.globalMorph);
    sbAppend("  \"volume\": %.2f,\n", P.volume);
    sbAppend("  \"voices\": %d,\n", P.voices);
    sbAppend("  \"glide_ms\": %.1f,\n", P.glideMs);
    sbAppend("  \"glide_enabled\": %d,\n", P.glideEnabled ? 1 : 0);
    sbAppend("  \"glide_mode\": %d,\n", (int)P.glideMode);
    sbAppend("  \"lfo_wave\": %d,\n", (int)P.lfoWave);
    sbAppend("  \"lfo_rate\": %.2f,\n", P.lfoRateHz);
    sbAppend("  \"lfo_depth\": %.2f,\n", P.lfoDepth);
    sbAppend("  \"lfo_target\": %d,\n", (int)P.lfoTarget);
    sbAppend("  \"lfo_delay_ms\": %.1f,\n", P.lfoDelayMs);
    sbAppend("  \"imu_x_target\": %d,\n", (int)P.imuXTarget);
    sbAppend("  \"imu_x_depth\": %.2f,\n", P.imuXDepth);
    sbAppend("  \"imu_y_target\": %d,\n", (int)P.imuYTarget);
    sbAppend("  \"imu_y_depth\": %.2f,\n", P.imuYDepth);
    sbAppend("  \"fx_chorus_rate\": %.2f,\n", P.chorusRateHz);
    sbAppend("  \"fx_chorus_depth\": %.2f,\n", P.chorusDepthMs);
    sbAppend("  \"fx_chorus_mix\": %.2f,\n", P.chorusMix);
    sbAppend("  \"fx_delay_time\": %.1f,\n", P.delayTimeMs);
    sbAppend("  \"fx_delay_feedback\": %.2f,\n", P.delayFeedback);
    sbAppend("  \"fx_delay_mix\": %.2f,\n", P.delayMix);
    sbAppend("  \"fx_reverb_size\": %.2f,\n", P.reverbSize);
    sbAppend("  \"fx_reverb_damp\": %.2f,\n", P.reverbDamp);
    sbAppend("  \"fx_reverb_mix\": %.2f,\n", P.reverbMix);
    sbAppend("  \"fx_limiter_drive\": %.2f,\n", P.limiterDrive);
    sbAppend("  \"fx_limiter_mix\": %.2f,\n", P.limiterMix);
    sbAppend("  \"fx_crush_bits\": %.2f,\n", P.crushBits);
    sbAppend("  \"fx_crush_down\": %.2f,\n", P.crushDown);
    sbAppend("  \"fx_crush_mix\": %.2f,\n", P.crushMix);
    sbAppend("  \"penv_amt\": %.0f,\n", P.penvAmt);                 // v0.71
    sbAppend("  \"penv_decay\": %.2f,\n", P.penvDecay);
    sbAppend("  \"penv_vel\": %.2f,\n", P.penvVel);
}

static void writePatternFields() {
    sbAppend("  \"arp_length\": %d,\n", arp.length);
    sbAppend("  \"arp_chord_wrap\": %d,\n", (int)arp.chordWrap);
    for (int i = 0; i < ARP_STEPS; i++) {
        sbAppend("  \"step%d_note\": %d,\n", i, (int)arp.step[i].note);
        sbAppend("  \"step%d_oct\": %d,\n", i, (int)arp.step[i].oct);
        sbAppend("  \"step%d_vel\": %d,\n", i, (int)arp.step[i].vel);
        sbAppend("  \"step%d_gate\": %d,\n", i, (int)arp.step[i].gate);
    }
}

static void writeSessionFields() {
    sbAppend("  \"arp_on\": %d,\n", arp.on ? 1 : 0);
    sbAppend("  \"arp_pattern\": %d,\n", (int)arp.pattern);
    sbAppend("  \"arp_latch\": %d,\n", arp.latch ? 1 : 0);
    sbAppend("  \"arp_tempo\": %d,\n", arp.tempoBpm);
    sbAppend("  \"arp_rate\": %d,\n", (int)arp.rate);
    sbAppend("  \"arp_swing\": %d,\n", arp.swing);
    sbAppend("  \"arp_gate\": %d,\n", arp.gate);
    sbAppend("  \"arp_octave\": %d,\n", arp.octave);
    sbAppend("  \"arp_midi_out\": %d,\n", arp.midiOut ? 1 : 0);
    sbAppend("  \"arp_clock_sync\": %d,\n", arp.clockSync ? 1 : 0);
    sbAppend("  \"arp_shift_mode\": %d,\n", (int)arp.shiftMode);
    sbAppend("  \"arp_latch_mode\": %d,\n", (int)arp.latchMode);
    writePatternFields();
    sbAppend("  \"midi_channel\": %d,\n", sys.midiChannel);
    sbAppend("  \"bend_range\": %d,\n", sys.bendRange);
    for (int i = 0; i < NUM_CC_SLOTS; i++) {
        sbAppend("  \"cc_slot%d_number\": %d,\n", i + 1, sys.ccSlot[i].number);
        sbAppend("  \"cc_slot%d_target\": %d,\n", i + 1, (int)sys.ccSlot[i].target);
        sbAppend("  \"cc_slot%d_depth\": %d,\n", i + 1, sys.ccSlot[i].depth);
    }
    for (int i = 0; i < NUM_SW_SLOTS; i++) {
        sbAppend("  \"sw_slot%d_number\": %d,\n", i + 1, sys.swSlot[i].number);
        sbAppend("  \"sw_slot%d_action\": %d,\n", i + 1, (int)sys.swSlot[i].action);
        sbAppend("  \"sw_slot%d_mode\": %d,\n", i + 1, (int)sys.swSlot[i].mode);
    }
    sbAppend("  \"pattern_base_cc\": %d,\n", sys.patternBaseCc);
    sbAppend("  \"midi_pc_mode\": %d,\n", (int)sys.pcMode);
    sbAppend("  \"imu_x_en\": %d,\n", sys.imuXEnabled ? 1 : 0);
    sbAppend("  \"imu_y_en\": %d,\n", sys.imuYEnabled ? 1 : 0);
    sbAppend("  \"imu_x_held\": %d,\n", sys.imuXHeld ? 1 : 0);
    sbAppend("  \"imu_y_held\": %d,\n", sys.imuYHeld ? 1 : 0);
    sbAppend("  \"imu_x_held_norm\": %.3f,\n", sys.imuXHeldNorm);
    sbAppend("  \"imu_y_held_norm\": %.3f,\n", sys.imuYHeldNorm);
    sbAppend("  \"ui_theme\": %d,\n", (int)sys.theme);
    sbAppend("  \"ui_brightness\": %d,\n", sys.brightness);
    sbAppend("  \"pattern_slot\": %d,\n", sys.patternSlot);
    sbAppend("  \"master_volume\": %d,\n", sys.masterVolume);
    sbAppend("  \"imu_calibrated\": %d,\n", sys.imuCalibrated ? 1 : 0);
    sbAppend("  \"imu_x_cal\": %.3f,\n", sys.imuXCalDeg);
    sbAppend("  \"imu_y_cal\": %.3f,\n", sys.imuYCalDeg);
    sbAppend("  \"imu_x_sens\": %d,\n", sys.imuXSens);
    sbAppend("  \"imu_y_sens\": %d,\n", sys.imuYSens);
    sbAppend("  \"imu_x_invert\": %d,\n", sys.imuXInvert ? 1 : 0);
    sbAppend("  \"imu_y_invert\": %d,\n", sys.imuYInvert ? 1 : 0);
    sbAppend("  \"imu_x_curve\": %d,\n", (int)sys.imuXCurve);
    sbAppend("  \"imu_y_curve\": %d,\n", (int)sys.imuYCurve);
    sbAppend("  \"imu_x_deadzone\": %d,\n", sys.imuXDeadzone);
    sbAppend("  \"imu_y_deadzone\": %d,\n", sys.imuYDeadzone);
    sbAppend("  \"octave\": %d,\n", sys.octave);
    sbAppend("  \"transpose\": %d,\n", sys.transpose);
    sbAppend("  \"morph_enabled\": %d,\n", sys.morphEnabled ? 1 : 0);
    sbAppend("  \"morph_paused\": %d,\n", morphChainPaused() ? 1 : 0);   // v0.82: live state, read back at boot
    sbAppend("  \"morph_pos_base\": %d,\n", sys.morphPosBase);
    sbAppend("  \"morph_time_ms\": %d,\n", sys.morphTimeMs);
    sbAppend("  \"scope_gmfb\": %d,\n", sys.scopeGmFb ? 1 : 0);
    sbAppend("  \"scope_morph\": %d,\n", sys.scopeMorph ? 1 : 0);
    sbAppend("  \"scope_fx\": %d,\n", sys.scopeFx ? 1 : 0);
    sbAppend("  \"scope_gl\": %d,\n", sys.scopeGl ? 1 : 0);
    sbAppend("  \"gl_on\": %d,\n", sys.glOn ? 1 : 0);
    sbAppend("  \"gl_rate\": %d,\n", (int)sys.glRate);
    sbAppend("  \"gl_prob\": %d,\n", sys.glProb);
    sbAppend("  \"gl_hold\": %d,\n", sys.glHold);
    sbAppend("  \"gl_buf\": %d,\n", (int)sys.glBuf);
    sbAppend("  \"gl_chaos\": %d,\n", (int)sys.glChaos);
    sbAppend("  \"gl_len\": %d,\n", (int)sys.glLen);
    sbAppend("  \"gl_amt\": %d,\n", sys.glAmt);
    sbAppend("  \"gl_arp\": %d,\n", (int)sys.glArp);   // v0.82
    // Theremin (v0.81). Short keys: this file is the largest one written.
    sbAppend("  \"th_on\": %d,\n", sys.thOn ? 1 : 0);
    sbAppend("  \"th_bus\": %d,\n", (int)sys.thBus);
    sbAppend("  \"th_mode\": %d,\n", (int)sys.thMode);
    sbAppend("  \"th_near\": %d,\n", sys.thNearMm);
    sbAppend("  \"th_far\": %d,\n", sys.thFarMm);
    sbAppend("  \"th_tgt\": %d,\n", (int)sys.thTarget);
    sbAppend("  \"th_dep\": %.0f,\n", sys.thDepth);
    sbAppend("  \"th_rel\": %d,\n", (int)sys.thRelease);
    sbAppend("  \"th_pitch\": %d,\n", (int)sys.thPitch);
    sbAppend("  \"th_scale\": %d,\n", sys.thScale);
    sbAppend("  \"th_top\": %d,\n", sys.thTopSemis);
    sbAppend("  \"th_span\": %d,\n", sys.thSpan);
    sbAppend("  \"th_vol\": %d,\n", (int)sys.thVolAxis);
    sbAppend("  \"factory_version\": %d,\n", sys.factoryVersion);   // v0.72
#ifdef MORPHASE_BLE
    sbAppend("  \"ble_midi_enabled\": %d,\n", sys.bleMidiEnabled ? 1 : 0);   // v0.77, BLE build only
#endif
}

// ---------------------------------------------------------------------------
// One way to write every file (v0.72): settings.json, current.json, patch
// files and morph.txt all go through here.
//  * Too big: nothing is written - the old file stays - and the player is
//    told. Checked after the closing brace, so a buffer that only overflows
//    on "}" can no longer leave a truncated file behind.
//  * The new text goes to <path>.tmp first; only when every byte is on the
//    card is the old file removed and the .tmp renamed. A failed write
//    leaves the old file alone. Power lost between the remove and the
//    rename leaves just the .tmp, which recoverTmp() puts back at boot.
// ---------------------------------------------------------------------------
enum SaveResult : uint8_t { SAVE_OK = 0, SAVE_TOO_BIG, SAVE_SD_ERROR };

static char          saveNoticeBuf[40];
static volatile bool saveNoticePending = false;
const char *patchTakeSaveNotice() {
    if (!saveNoticePending) return nullptr;
    saveNoticePending = false;
    return saveNoticeBuf;
}
static SaveResult notifySave(SaveResult r, const char *what) {
    if (r == SAVE_OK) return r;
    snprintf(saveNoticeBuf, sizeof(saveNoticeBuf), "%s NOT saved: %s", what, r == SAVE_TOO_BIG ? "too big" : "SD error");
    saveNoticePending = true;
    uiDirty = true;                     // shown on whatever screen is up
    Serial.printf("[SD] %s\n", saveNoticeBuf);
    return r;
}

// Closes the JSON object: the last line ends with ",\n".
static void sbCloseJson() {
    if (settingsBufLen >= 2 && settingsBuf[settingsBufLen - 2] == ',') { settingsBuf[settingsBufLen - 2] = '\n'; settingsBufLen--; }
    sbAppend("}\n");
}

static void tmpPathOf(char *out, size_t n, const char *path) { snprintf(out, n, "%s.tmp", path); }

static SaveResult commitBuffer(const char *path) {
    if (!sdMounted) return SAVE_SD_ERROR;
    if (settingsBufOverflow) return SAVE_TOO_BIG;
    char tmp[80]; tmpPathOf(tmp, sizeof(tmp), path);
    if (SD.exists(tmp)) SD.remove(tmp);
    File f = SD.open(tmp, FILE_WRITE);
    if (!f) return SAVE_SD_ERROR;
    size_t w = f.write((const uint8_t *)settingsBuf, settingsBufLen);
    f.close();
    if (w != (size_t)settingsBufLen) { SD.remove(tmp); return SAVE_SD_ERROR; }
    if (SD.exists(path) && !SD.remove(path)) { SD.remove(tmp); return SAVE_SD_ERROR; }
    if (!SD.rename(tmp, path)) return SAVE_SD_ERROR;    // the .tmp stays for recoverTmp()
    return SAVE_OK;
}

// At boot, before a file is read: no file but a .tmp means power went off
// between the remove and the rename - the .tmp is complete, use it. Both
// present means a write failed earlier - the old file is the good one.
static void recoverTmp(const char *path) {
    if (!sdMounted) return;
    char tmp[80]; tmpPathOf(tmp, sizeof(tmp), path);
    if (!SD.exists(tmp)) return;
    if (SD.exists(path)) SD.remove(tmp);
    else { SD.rename(tmp, path); Serial.printf("[SD] recovered %s\n", path); }
}

// The same path for writers outside this file (morph.txt).
void fileWriteBegin() { sbReset(); }
void fileWritef(const char *fmt, ...) {
    char line[96];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sbAppend("%s", line);
}
bool fileWriteCommit(const char *path, const char *what) { return notifySave(commitBuffer(path), what) == SAVE_OK; }
void fileRecoverTmp(const char *path) { recoverTmp(path); }

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
static inline bool enumOk(float v, int count) { int u = (int)v; return u >= 0 && u < count; }

// A modulation destination is valid if it is in range and is not the
// boundary marker sitting between the audio and control groups.
static inline bool modTargetOk(float v) {
    int u = (int)v;
    return u >= 0 && u < (int)ModTarget::MOD_TARGET_COUNT && u != (int)ModTarget::MOD_AUDIO_END;
}

// Set while a file is being parsed so a patch written before v0.3, which
// has no glide_enabled key, can fall back to "enabled if a time was set".
static bool sawGlideEnabled = false;

// Where patch keys land: `patch` normally, a chain slot while the morph
// chain loads (v0.5) - so loading a chain patch never touches the sound.
static Patch *pt = &patch;

static void parseLine(const String &line) {
    int q1 = line.indexOf('"'); if (q1 < 0) return;
    int q2 = line.indexOf('"', q1 + 1); if (q2 < 0) return;
    String key = line.substring(q1 + 1, q2);
    int col = line.indexOf(':', q2); if (col < 0) return;
    String vs = line.substring(col + 1); vs.trim();
    if (vs.endsWith(",")) vs.remove(vs.length() - 1);
    vs.trim();
    if (!vs.length()) return;

    // String values
    if (key == "name") {
        int s1 = vs.indexOf('"'), s2 = vs.lastIndexOf('"');
        if (s1 >= 0 && s2 > s1) {
            String nm = vs.substring(s1 + 1, s2);
            strncpy(pt->name, nm.c_str(), PATCH_NAME_LEN); pt->name[PATCH_NAME_LEN] = 0;
        }
        return;
    }
    float v = vs.toFloat();

    // Operator keys: opN_xxx
    if (key.startsWith("op") && key.length() > 4 && key[3] == '_') {
        int n = key[2] - '1';
        if (n < 0 || n >= NUM_OPS) return;
        String f = key.substring(4);
        OpParams &p = pt->op[n];
        if      (f == "morph")    p.morph    = clampf(v, 0.f, 100.f);
        else if (f == "ratio")    p.ratio    = clampf(v, 0.25f, 16.f);
        else if (f == "detune")   p.detune   = clampf(v, -50.f, 50.f);
        else if (f == "level")    p.level    = clampf(v, 0.f, 100.f);
        else if (f == "attack")   p.attack   = clampf(v, 0.f, 100.f);
        else if (f == "decay")    p.decay    = clampf(v, 0.f, 100.f);
        else if (f == "sustain")  p.sustain  = clampf(v, 0.f, 100.f);
        else if (f == "release")  p.release  = clampf(v, 0.f, 100.f);
        else if (f == "velsens")  p.velSens  = clampf(v, 0.f, 100.f);
        else if (f == "keyscale") p.keyScale = clampf(v, 0.f, 100.f);
        else if (f == "fixed")    p.fixed    = ((int)v) != 0;                                   // v0.71
        else if (f == "fixnote")  p.fixNote  = clampf((float)(int)lroundf(v), FIX_NOTE_MIN, FIX_NOTE_MAX);
        return;
    }
    // MIDI slot keys: cc_slotN_xxx / sw_slotN_xxx (session data)
    if (!loadingPatch && (key.startsWith("cc_slot") || key.startsWith("sw_slot"))) {
        bool isCc = key.startsWith("cc_");
        int us = key.indexOf('_', 7); if (us < 0) return;
        int n = key.substring(7, us).toInt() - 1;
        String f = key.substring(us + 1);
        if (isCc) {
            if (n < 0 || n >= NUM_CC_SLOTS) return;
            if      (f == "number") sys.ccSlot[n].number = clampi((int)v, CC_OFF, 127);
            else if (f == "target") { if (modTargetOk(v)) sys.ccSlot[n].target = (ModTarget)(int)v; }
            else if (f == "depth")  sys.ccSlot[n].depth = clampi((int)v, 0, 100);
        } else {
            if (n < 0 || n >= NUM_SW_SLOTS) return;
            if      (f == "number") sys.swSlot[n].number = clampi((int)v, CC_OFF, 127);
            else if (f == "action") { if (enumOk(v, (int)SwitchAction::SWITCH_ACTION_COUNT)) sys.swSlot[n].action = (SwitchAction)(int)v; }
            else if (f == "mode")   { if (enumOk(v, (int)SwitchMode::SWITCH_MODE_COUNT)) sys.swSlot[n].mode = (SwitchMode)(int)v; }
        }
        return;
    }
    // Step keys: stepN_xxx (pattern data, ignored while loading a patch)
    if (key.startsWith("step")) {
        if (loadingPatch) return;
        int us = key.indexOf('_'); if (us < 0) return;
        int i = key.substring(4, us).toInt();
        if (i < 0 || i >= ARP_STEPS) return;
        String f = key.substring(us + 1);
        ArpStep &s = arp.step[i];
        if      (f == "note") s.note = (uint8_t)clampi((int)v, 0, ARP_STEP_NOTES);
        else if (f == "oct")  s.oct  = (int8_t)clampi((int)v, -2, 2);
        else if (f == "vel")  s.vel  = (uint8_t)clampi((int)v, 1, 127);
        else if (f == "gate") s.gate = (uint8_t)clampi((int)v, 10, 200);
        return;
    }

    // Patch-level keys
    if      (key == "algorithm")    pt->algorithm   = clampi((int)v, 1, NUM_ALGORITHMS);
    else if (key == "feedback")     pt->feedback    = clampf(v, 0.f, 100.f);
    else if (key == "global_morph") pt->globalMorph = clampf(v, 0.f, 100.f);
    else if (key == "volume")       pt->volume      = clampf(v, 0.f, 100.f);
    else if (key == "voices")       pt->voices      = clampi((int)v, 1, PATCH_MAX_VOICES);   // kept as written; the engine clamps (v0.77)
    else if (key == "glide_ms")     pt->glideMs     = clampf(v, 0.f, 1000.f);
    else if (key == "glide_enabled"){ pt->glideEnabled = ((int)v) != 0; sawGlideEnabled = true; }
    else if (key == "glide_mode")   { if (enumOk(v, (int)GlideMode::GLIDE_MODE_COUNT)) pt->glideMode = (GlideMode)(int)v; }
    else if (key == "lfo_wave")     { if (enumOk(v, (int)LfoWave::LFO_WAVE_COUNT)) pt->lfoWave = (LfoWave)(int)v; }
    else if (key == "lfo_rate")     pt->lfoRateHz   = clampf(v, 0.1f, 20.f);
    else if (key == "lfo_depth")    pt->lfoDepth    = clampf(v, 0.f, 100.f);
    // Off (0) is a valid LFO target since v0.73; control targets stay refused
    else if (key == "lfo_target")   { if (modTargetOk(v) && ((int)v == 0 || modTargetIsAudio((ModTarget)(int)v))) pt->lfoTarget = (ModTarget)(int)v; }
    else if (key == "lfo_delay_ms") pt->lfoDelayMs  = clampf(v, 0.f, 2000.f);
    else if (key == "imu_x_target") { if (modTargetOk(v)) pt->imuXTarget = (ModTarget)(int)v; }
    else if (key == "imu_x_depth")  pt->imuXDepth   = clampf(v, 0.f, 100.f);
    else if (key == "imu_y_target") { if (modTargetOk(v)) pt->imuYTarget = (ModTarget)(int)v; }
    else if (key == "imu_y_depth")  pt->imuYDepth   = clampf(v, 0.f, 100.f);
    else if (key == "fx_chorus_rate")     pt->chorusRateHz  = clampf(v, 0.1f, 5.f);
    else if (key == "fx_chorus_depth")    pt->chorusDepthMs = clampf(v, 0.f, 20.f);
    else if (key == "fx_chorus_mix")      pt->chorusMix     = clampf(v, 0.f, 100.f);
    else if (key == "fx_delay_time")      pt->delayTimeMs   = clampf(v, 50.f, 600.f);
    else if (key == "fx_delay_feedback")  pt->delayFeedback = clampf(v, 0.f, 90.f);
    else if (key == "fx_delay_mix")       pt->delayMix      = clampf(v, 0.f, 100.f);
    else if (key == "fx_reverb_size")     pt->reverbSize    = clampf(v, 0.f, 100.f);
    else if (key == "fx_reverb_damp")     pt->reverbDamp    = clampf(v, 0.f, 100.f);
    else if (key == "fx_reverb_mix")      pt->reverbMix     = clampf(v, 0.f, 100.f);
    else if (key == "fx_limiter_drive")   pt->limiterDrive  = clampf(v, 1.f, 5.f);
    else if (key == "fx_limiter_mix")     pt->limiterMix    = clampf(v, 0.f, 100.f);
    else if (key == "fx_crush_bits")      pt->crushBits     = clampf(v, 1.f, 16.f);
    else if (key == "fx_crush_down")      pt->crushDown     = clampf(v, 1.f, 32.f);
    else if (key == "fx_crush_mix")       pt->crushMix      = clampf(v, 0.f, 100.f);
    else if (key == "penv_amt")           pt->penvAmt       = clampf(v, -PENV_AMT_MAX, PENV_AMT_MAX);   // v0.71
    else if (key == "penv_decay")         pt->penvDecay     = clampf(v, 0.f, 100.f);
    else if (key == "penv_vel")           pt->penvVel       = clampf(v, 0.f, 100.f);
    else if (key == "morphase_format")    { /* stamp - accepted, nothing to migrate yet */ }
    // Everything below is session / device data: ignored while a PATCH loads
    else if (loadingPatch) { return; }
    else if (key == "arp_on")        arp.on = ((int)v) != 0;
    else if (key == "arp_pattern")   { if (enumOk(v, (int)ArpPattern::ARP_PATTERN_COUNT)) arp.pattern = (ArpPattern)(int)v; }
    else if (key == "arp_latch")     arp.latch = ((int)v) != 0;
    else if (key == "arp_tempo")     arp.tempoBpm = clampi((int)v, 40, 300);
    else if (key == "arp_rate")      { if (enumOk(v, (int)ArpRate::ARP_RATE_COUNT)) arp.rate = (ArpRate)(int)v; }
    else if (key == "arp_swing")     arp.swing = clampi((int)v, 0, 100);
    else if (key == "arp_gate")      arp.gate = clampi((int)v, 10, 200);
    else if (key == "arp_octave")    arp.octave = clampi((int)v, 1, 4);
    else if (key == "arp_midi_out")  arp.midiOut = ((int)v) != 0;
    else if (key == "arp_clock_sync") arp.clockSync = ((int)v) != 0;
    else if (key == "arp_shift_mode") { if (enumOk(v, (int)ShiftMode::SHIFT_MODE_COUNT)) arp.shiftMode = (ShiftMode)(int)v; }
    else if (key == "arp_latch_mode") { if (enumOk(v, (int)LatchMode::LATCH_MODE_COUNT)) arp.latchMode = (LatchMode)(int)v; }
    else if (key == "arp_length")    arp.length = clampi((int)v, 1, ARP_STEPS);
    else if (key == "arp_chord_wrap"){ if (enumOk(v, (int)ChordWrap::CHORD_WRAP_COUNT)) arp.chordWrap = (ChordWrap)(int)v; }
    else if (key == "midi_channel")  sys.midiChannel = clampi((int)v, 0, 16);
    else if (key == "bend_range")    sys.bendRange = clampi((int)v, 1, 12);
    else if (key == "pattern_base_cc") sys.patternBaseCc = clampi((int)v, CC_OFF, 120);
    else if (key == "midi_pc_mode") { if (enumOk(v, (int)PcMode::PC_MODE_COUNT)) sys.pcMode = (PcMode)(int)v; }
    else if (key == "imu_x_en")      sys.imuXEnabled = ((int)v) != 0;
    else if (key == "imu_y_en")      sys.imuYEnabled = ((int)v) != 0;
    else if (key == "imu_x_held")    sys.imuXHeld = ((int)v) != 0;
    else if (key == "imu_y_held")    sys.imuYHeld = ((int)v) != 0;
    else if (key == "imu_x_held_norm") sys.imuXHeldNorm = clampf(v, -1.f, 1.f);
    else if (key == "imu_y_held_norm") sys.imuYHeldNorm = clampf(v, -1.f, 1.f);
    else if (key == "ui_theme")      { if (enumOk(v, (int)UiTheme::UI_THEME_COUNT)) sys.theme = (UiTheme)(int)v; }
    else if (key == "ui_brightness") sys.brightness = clampi((int)v, 10, 100);
    else if (key == "pattern_slot")  sys.patternSlot = clampi((int)v, 0, NUM_PATTERN_SLOTS - 1);
    else if (key == "master_volume") sys.masterVolume = clampi((int)v, 0, 100);
    else if (key == "imu_calibrated") sys.imuCalibrated = ((int)v) != 0;
    else if (key == "imu_x_cal")     sys.imuXCalDeg = clampf(v, -90.f, 90.f);
    else if (key == "imu_y_cal")     sys.imuYCalDeg = clampf(v, -90.f, 90.f);
    else if (key == "imu_x_sens")    sys.imuXSens = clampi((int)v, 50, 200);
    else if (key == "imu_y_sens")    sys.imuYSens = clampi((int)v, 50, 200);
    else if (key == "imu_x_invert")  sys.imuXInvert = ((int)v) != 0;
    else if (key == "imu_y_invert")  sys.imuYInvert = ((int)v) != 0;
    else if (key == "imu_x_curve")   { if (enumOk(v, (int)ImuCurve::IMU_CURVE_COUNT)) sys.imuXCurve = (ImuCurve)(int)v; }
    else if (key == "imu_y_curve")   { if (enumOk(v, (int)ImuCurve::IMU_CURVE_COUNT)) sys.imuYCurve = (ImuCurve)(int)v; }
    else if (key == "imu_x_deadzone") sys.imuXDeadzone = clampi((int)v, 0, 30);
    else if (key == "imu_y_deadzone") sys.imuYDeadzone = clampi((int)v, 0, 30);
    else if (key == "octave")        sys.octave = clampi((int)v, OCTAVE_MIN, OCTAVE_MAX);
    else if (key == "transpose")     sys.transpose = clampi((int)v, TRANSPOSE_MIN, TRANSPOSE_MAX);
    else if (key == "morph_enabled") sys.morphEnabled = ((int)v) != 0;
    else if (key == "morph_paused")  sys.morphPaused = ((int)v) != 0;
    else if (key == "morph_pos_base") sys.morphPosBase = clampi((int)v, 0, 100);
    else if (key == "morph_time_ms") sys.morphTimeMs = clampi((int)v, 0, MORPH_TIME_MAX_MS);
    else if (key == "scope_gmfb")    sys.scopeGmFb = ((int)v) != 0;
    else if (key == "scope_morph")   sys.scopeMorph = ((int)v) != 0;
    else if (key == "scope_fx")      sys.scopeFx = ((int)v) != 0;
    else if (key == "scope_gl")      sys.scopeGl = ((int)v) != 0;
    else if (key == "gl_on")         sys.glOn = ((int)v) != 0;
    else if (key == "gl_rate")       { if (enumOk(v, (int)GlRate::GL_RATE_COUNT)) sys.glRate = (GlRate)(int)v; }
    else if (key == "gl_prob")       sys.glProb = clampi((int)v, 0, 100);
    else if (key == "gl_hold")       sys.glHold = clampi((int)v, 0, 100);
    else if (key == "gl_buf")        { if (enumOk(v, (int)GlBuf::GL_BUF_COUNT)) sys.glBuf = (GlBuf)(int)v; }
    else if (key == "gl_chaos")      { if (enumOk(v, (int)GlChaos::GL_CHAOS_COUNT)) sys.glChaos = (GlChaos)(int)v; }
    else if (key == "gl_len")        { if (enumOk(v, (int)GlLen::GL_LEN_COUNT)) sys.glLen = (GlLen)(int)v; }
    else if (key == "gl_amt")        sys.glAmt = clampi((int)v, 0, 100);
    else if (key == "gl_arp")        { if (enumOk(v, (int)GlArp::GL_ARP_COUNT)) sys.glArp = (GlArp)(int)v; }
    else if (key == "th_on")         sys.thOn = ((int)v) != 0;
    else if (key == "th_bus")        { if (enumOk(v, (int)ThBus::TH_BUS_COUNT)) sys.thBus = (ThBus)(int)v; }
    else if (key == "th_mode")       { if (enumOk(v, (int)ThMode::TH_MODE_COUNT)) sys.thMode = (ThMode)(int)v; }
    else if (key == "th_near")       sys.thNearMm = clampi((int)v, TH_NEAR_MIN, TH_NEAR_MAX);
    else if (key == "th_far")        sys.thFarMm = clampi((int)v, TH_NEAR_MIN + TH_FAR_GAP, TH_FAR_MAX);
    else if (key == "th_tgt")        { if (enumOk(v, (int)ModTarget::MOD_TARGET_COUNT) && (int)v != (int)ModTarget::MOD_AUDIO_END) sys.thTarget = (ModTarget)(int)v; }
    else if (key == "th_dep")        sys.thDepth = clampf(v, 0.f, 100.f);
    else if (key == "th_rel")        { if (enumOk(v, (int)ThRelease::TH_RELEASE_COUNT)) sys.thRelease = (ThRelease)(int)v; }
    else if (key == "th_pitch")      { if (enumOk(v, (int)ThPitch::TH_PITCH_COUNT)) sys.thPitch = (ThPitch)(int)v; }
    else if (key == "th_scale")      sys.thScale = clampi((int)v, 0, 127);   // theremin.cpp clamps to its table
    else if (key == "th_top")        sys.thTopSemis = clampi((int)v, TH_TOP_MIN, TH_TOP_MAX);
    else if (key == "th_span")       sys.thSpan = clampi((int)v, 1, 4);
    else if (key == "th_vol")        { if (enumOk(v, (int)ThVolAxis::TH_VOLAXIS_COUNT)) sys.thVolAxis = (ThVolAxis)(int)v; }
#ifdef MORPHASE_BLE
    else if (key == "ble_midi_enabled") sys.bleMidiEnabled = ((int)v) != 0;
#endif
    else if (key == "factory_version") sys.factoryVersion = clampi((int)v, 0, 9999);   // v0.72
}

static bool parseFile(const char *path) {
    if (!sdMounted) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    while (f.available()) parseLine(f.readStringUntil('\n'));
    f.close();
    return true;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
// v0.72: the sound being edited lives in current.json, in the patch-file
// format, and settings.json keeps the device and session data. The two
// grow apart from now on (settings.json was at ~89% of the buffer with both
// in it). current.json is only rewritten when its text changed; a hash of
// the last text written is kept for that.
static uint32_t lastCurrentHash = 0;
// v0.91: settings.json the same way - a file whose text has not changed
// is not written. Both hashes are taken from what loadSettings() read, so
// the first save after boot writes only what really changed.
static uint32_t lastSettingsHash = 0;

static uint32_t hashBuffer() {
    uint32_t h = 2166136261u;                       // FNV-1a
    for (int i = 0; i < settingsBufLen; i++) { h ^= (uint8_t)settingsBuf[i]; h *= 16777619u; }
    return h;
}

volatile bool saveRequested = false;

bool saveSettings() {
    if (!sdMounted) return false;
    uint32_t t0 = millis();
    bool wroteS = false, wroteC = false;
    sbReset();
    sbAppend("{\n");
    writeSessionFields();
    sbCloseJson();
    bool ok = true;
    uint32_t hs = hashBuffer();
    if (settingsBufOverflow || hs != lastSettingsHash) {
        if (notifySave(commitBuffer(SETTINGS_PATH), "Settings") == SAVE_OK) lastSettingsHash = hs;
        else ok = false;
        wroteS = true;
    }

    sbReset();
    sbAppend("{\n");
    writePatchFields(patch);
    sbCloseJson();
    uint32_t h = hashBuffer();
    if (settingsBufOverflow || h != lastCurrentHash) {
        if (notifySave(commitBuffer(CURRENT_PATCH_PATH), "Current patch") == SAVE_OK) lastCurrentHash = h;
        else ok = false;
        wroteC = true;
    }
    Serial.printf("[SD] save: %s%s%s (%lu ms)\n", wroteS ? "settings.json " : "", wroteC ? "current.json" : "",
                  (wroteS || wroteC) ? "" : "nothing changed", (unsigned long)(millis() - t0));
    return ok;
}

bool loadSettings() {
    if (!sdMounted) return false;
    recoverTmp(SETTINGS_PATH);
    recoverTmp(CURRENT_PATCH_PATH);
    loadingPatch = false;
    sawGlideEnabled = false;
    // A settings.json from before v0.72 still holds the patch fields; they
    // land in `patch` here, so nothing needs migrating. current.json, when
    // it exists, is the sound and replaces them.
    bool ok = parseFile(SETTINGS_PATH);
    if (ok && !sawGlideEnabled) patch.glideEnabled = patch.glideMs > 0.5f;
    if (SD.exists(CURRENT_PATCH_PATH)) {
        Patch backup = patch;
        patchInitDefaults(patch);
        loadingPatch = true;
        sawGlideEnabled = false;
        bool pok = parseFile(CURRENT_PATCH_PATH);
        loadingPatch = false;
        if (!pok) patch = backup;
        else if (!sawGlideEnabled) patch.glideEnabled = patch.glideMs > 0.5f;
        ok = ok || pok;
    }
    // A saved ARP state must not leave notes hanging on boot
    if (ok) rebuildArpChord();
    // What is on the card now (v0.91): the next save writes a file only
    // when its text differs from this
    sbReset(); sbAppend("{\n"); writeSessionFields(); sbCloseJson(); lastSettingsHash = hashBuffer();
    sbReset(); sbAppend("{\n"); writePatchFields(patch); sbCloseJson(); lastCurrentHash = hashBuffer();
    return ok;
}

// ---------------------------------------------------------------------------
// Patch bank
// ---------------------------------------------------------------------------
static char patchNames[MAX_PATCHES][PATCH_NAME_LEN + 1];
static int  patchNameCount = 0;

bool patchNameValid(const char *name) {
    int len = strlen(name);
    if (len < 1 || len > PATCH_NAME_LEN) return false;
    for (int i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

static void patchPath(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s%s", PATCH_FOLDER, name, PATCH_EXT);
}

// Position in the factory table, or -1 for a player's own patch.
static int factoryRank(const char *name) {
    for (int i = 0; i < FACTORY_COUNT; i++) if (strcmp(FACTORY_NAMES[i], name) == 0) return i;
    return -1;
}

// List order (v0.72): the factory patches first, in table order, then every
// other patch alphabetically. Program Change and Pch+/- count in this order,
// so the factory numbers never move, whatever the player names their own -
// even "-x" or "05_mine", which would sort before them alphabetically.
static bool listBefore(const char *a, const char *b) {
    int ra = factoryRank(a), rb = factoryRank(b);
    if (ra >= 0 || rb >= 0) {
        if (ra < 0) return false;
        if (rb < 0) return true;
        return ra < rb;
    }
    return strcasecmp(a, b) < 0;
}

// "<name>.json.tmp" in the Patch folder: finish what a cut-off save began
// (see recoverTmp). Collected first, fixed after the listing is closed.
static void recoverPatchTmps() {
    char names[8][PATCH_NAME_LEN + 1]; int n = 0;
    File dir = SD.open(PATCH_FOLDER);
    if (!dir) return;
    const char *TAIL = PATCH_EXT ".tmp";
    while (n < 8) {
        File f = dir.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            const char *nm = f.name();
            const char *slash = strrchr(nm, '/'); if (slash) nm = slash + 1;
            int len = strlen(nm), tl = strlen(TAIL);
            if (len > tl && strcasecmp(nm + len - tl, TAIL) == 0 && len - tl <= PATCH_NAME_LEN) {
                strncpy(names[n], nm, len - tl); names[n][len - tl] = 0; n++;
            }
        }
        f.close();
        if (bootProgress) bootProgress(1);     // splash frame at boot (v0.89T2)
    }
    dir.close();
    for (int i = 0; i < n; i++) { char path[64]; patchPath(path, sizeof(path), names[i]); recoverTmp(path); }
}

uint32_t patchLoadSeq = 0;

int scanPatches() {
    patchNameCount = 0;
    if (!sdMounted) return 0;
    recoverPatchTmps();
    File dir = SD.open(PATCH_FOLDER);
    if (!dir) return 0;
    for (;;) {
        File f = dir.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            const char *nm = f.name();
            const char *slash = strrchr(nm, '/'); if (slash) nm = slash + 1;
            int len = strlen(nm), extLen = strlen(PATCH_EXT);
            if (len > extLen && strcasecmp(nm + len - extLen, PATCH_EXT) == 0 && len - extLen <= PATCH_NAME_LEN) {
                char name[PATCH_NAME_LEN + 1];
                strncpy(name, nm, len - extLen); name[len - extLen] = 0;
                int slot = -1;
                if (patchNameCount < MAX_PATCHES) slot = patchNameCount++;
                else if (factoryRank(name) >= 0) {
                    // Full: a factory patch still gets in, in place of a
                    // player's patch, so it can never drop off the list
                    for (int i = patchNameCount - 1; i >= 0; i--) if (factoryRank(patchNames[i]) < 0) { slot = i; break; }
                }
                if (slot >= 0) strcpy(patchNames[slot], name);
            }
        }
        f.close();
        if (bootProgress) bootProgress(1);     // splash frame at boot (v0.89T2)
    }
    dir.close();
    // Insertion sort, MAX_PATCHES entries at most
    for (int i = 1; i < patchNameCount; i++) {
        char key[PATCH_NAME_LEN + 1]; strcpy(key, patchNames[i]); int j = i - 1;
        while (j >= 0 && listBefore(key, patchNames[j])) { strcpy(patchNames[j + 1], patchNames[j]); j--; }
        strcpy(patchNames[j + 1], key);
    }
    return patchNameCount;
}

int patchCount() { return patchNameCount; }
const char *patchNameAt(int index) { return (index >= 0 && index < patchNameCount) ? patchNames[index] : ""; }
int patchIndexOfName(const char *name) {
    for (int i = 0; i < patchNameCount; i++) if (strcmp(patchNames[i], name) == 0) return i;
    return -1;
}

// Writes any patch to /MORPHASE/Patch/<name>.json, through the one writer.
static bool writePatchToFile(const Patch &P, const char *name) {
    char path[64]; patchPath(path, sizeof(path), name);
    sbReset();
    sbAppend("{\n");
    writePatchFields(P);
    sbCloseJson();
    return notifySave(commitBuffer(path), "Patch") == SAVE_OK;
}

bool savePatchFile(const char *name) {
    if (!sdMounted || !patchNameValid(name)) return false;
    strncpy(patch.name, name, PATCH_NAME_LEN); patch.name[PATCH_NAME_LEN] = 0;
    savingPatch = true;
    bool ok = writePatchToFile(patch, name);
    savingPatch = false;
    if (ok) morphOnPatchSaved(name);    // a chain patch was edited: the chain follows
    return ok;
}

bool loadPatchFile(const char *name) {
    if (!sdMounted) return false;
    char path[64]; patchPath(path, sizeof(path), name);
    if (!SD.exists(path)) return false;
    morphStopForPatchChange();          // a new sound replaces the one morph was playing
    fxBypassMask = 0;                   // bypass is a performance state: a load ends it (update spec 4, 3.3)
    fxForceMask = 0;                    // and so is a temporary On (v0.57)
    Patch backup = patch;
    patchInitDefaults(patch);           // missing keys fall back to Init values
    loadingPatch = true;
    sawGlideEnabled = false;
    bool ok = parseFile(path);
    loadingPatch = false;
    if (!ok) { patch = backup; return false; }
    // Patches written before v0.3 carry a glide time but no on/off flag.
    if (!sawGlideEnabled) patch.glideEnabled = patch.glideMs > 0.5f;
    strncpy(patch.name, name, PATCH_NAME_LEN); patch.name[PATCH_NAME_LEN] = 0;
    requestFxClear();                   // the previous sound's echoes must not ring into this one
    patchLoadSeq++;                     // saved 3 s after the last load (v0.90T2, main.cpp)
    return true;
}

bool deletePatchFile(const char *name) {
    if (!sdMounted) return false;
    char path[64]; patchPath(path, sizeof(path), name);
    bool ok = SD.remove(path);
    if (ok) morphOnPatchDeleted(name);
    return ok;
}

// A chain slot's patch, read without touching `patch` (v0.5)
bool loadPatchInto(const char *name, Patch &out) {
    if (!sdMounted || !name || !name[0]) return false;
    char path[64]; patchPath(path, sizeof(path), name);
    if (!SD.exists(path)) return false;
    if (bootProgress) bootProgress(1);         // splash frame while the chain loads at boot
    patchInitDefaults(out);
    pt = &out;
    loadingPatch = true;
    sawGlideEnabled = false;
    bool ok = parseFile(path);
    loadingPatch = false;
    pt = &patch;
    if (!sawGlideEnabled) out.glideEnabled = out.glideMs > 0.5f;
    strncpy(out.name, name, PATCH_NAME_LEN); out.name[PATCH_NAME_LEN] = 0;
    return ok;
}

bool renamePatchFile(const char *oldName, const char *newName) {
    if (!sdMounted || !patchNameValid(newName)) return false;
    char a[64], b[64]; patchPath(a, sizeof(a), oldName); patchPath(b, sizeof(b), newName);
    if (SD.exists(b)) return false;
    bool ok = SD.rename(a, b);
    if (ok) morphOnPatchRenamed(oldName, newName);
    return ok;
}

bool patchIsFactoryName(const char *name) { return name && factoryRank(name) >= 0; }

// Writes the factory patches; returns how many, or -1 when one could not be
// written. Built in a local copy: `patch` (what is sounding) is left alone.
// Since v0.75 a factory name cannot be saved over from the device (Save
// turns into SaveAs), so a factory file on the card is always the factory
// sound and rewriting it loses nothing: `overwrite` brings an improved
// definition to every card. Without it, only the missing ones are written.

int writeFactoryPatches(bool overwrite) {
    if (!sdMounted) return -1;
    int written = 0;
    for (int i = 0; i < FACTORY_COUNT; i++) {
        char path[64]; patchPath(path, sizeof(path), FACTORY_NAMES[i]);
        if (!overwrite && SD.exists(path)) continue;
        Patch fp;
        factoryPatch(i, fp);
        if (!writePatchToFile(fp, FACTORY_NAMES[i])) return -1;
        written++;
        if (bootProgress) bootProgress(1);
    }
    if (writeFactoryChains(overwrite) < 0) return -1;   // v0.83: the factory chains go with them
    scanPatches();
    morphOnFactoryRewritten();               // chain slots holding them read the new sound
    return written;
}

// Boot (factory patch plan 2, set 2 decision 4): an empty Patch folder gets
// the whole set; when the set's version goes up, every factory patch is
// rewritten - new ones added, changed ones updated. Not on every boot, so
// a factory patch the player deleted stays deleted until the set changes.
void ensureFactoryPatches() {
    if (!sdMounted) return;
    bool empty = scanPatches() == 0;
    if (!empty && sys.factoryVersion >= FACTORY_SET_VERSION) return;
    if (bootProgress) bootProgress(0);
    int n = writeFactoryPatches(true);
    if (bootProgress) bootProgress(2);
    // writeFactoryPatches() lists the folder when it succeeds; a write that
    // failed part-way leaves some files written, so list them here (v0.92T2:
    // setup() no longer lists the folder a second time)
    if (n < 0) { scanPatches(); return; }    // retried on the next boot
    // The demo chain, only onto a card that has no chain yet - morphInit()
    // reads it right after this. Any other time it is loaded from the menu.
    if (!SD.exists(MORPH_CHAIN_FILE)) {
        const char *const *demo; int n = factoryDemoChain(&demo);
        fileWriteBegin();
        for (int i = 0; i < MORPH_SLOTS; i++) fileWritef("%s\n", i < n ? demo[i] : "");
        fileWriteCommit(MORPH_CHAIN_FILE, "Chain");
    }
    sys.factoryVersion = FACTORY_SET_VERSION;
    saveSettings();
    Serial.printf("[SD] factory set v%d: %d patch(es) written\n", FACTORY_SET_VERSION, n);
}

// ---------------------------------------------------------------------------
// Chains (v0.83): /MORPHASE/Chain/<name>.txt, the morph.txt format - eight
// patch names, one per line, blank = Off - plus an optional "time=<ms>" line
// (the Shift+1-8 / PC glide time; older firmware reads only the 8 lines).
// The list is never kept in RAM: the screen asks for it when it opens.
// The two factory chains are the former Demo chains and follow the factory
// patches: rewritten when the set version rises, restored by Factory, and a
// factory name cannot be saved over from the device.
// ---------------------------------------------------------------------------
static const char *const FACTORY_CHAINS[DEMO_CHAIN_COUNT] = { "01_Pads", "02_Drums" };

static void chainPath(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s%s", CHAIN_FOLDER, name, CHAIN_EXT); }

bool chainIsFactoryName(const char *name) {
    for (int i = 0; i < DEMO_CHAIN_COUNT; i++) if (name && strcmp(FACTORY_CHAINS[i], name) == 0) return true;
    return false;
}

int chainList(char (*out)[PATCH_NAME_LEN + 1], int maxN) {
    int n = 0;
    if (!sdMounted) return 0;
    File dir = SD.open(CHAIN_FOLDER);
    if (!dir) return 0;
    int extLen = strlen(CHAIN_EXT);
    for (;;) {
        File f = dir.openNextFile();
        if (!f) break;
        if (!f.isDirectory() && n < maxN) {
            const char *nm = f.name();
            const char *slash = strrchr(nm, '/'); if (slash) nm = slash + 1;
            int len = strlen(nm);
            if (len > extLen && strcasecmp(nm + len - extLen, CHAIN_EXT) == 0 && len - extLen <= PATCH_NAME_LEN) {
                strncpy(out[n], nm, len - extLen); out[n][len - extLen] = 0; n++;
            }
        }
        f.close();
        if (bootProgress) bootProgress(1);     // splash frame at boot (v0.89T2)
    }
    dir.close();
    // Factory first (in their order), then the player's by name
    for (int i = 1; i < n; i++) {
        char key[PATCH_NAME_LEN + 1]; strcpy(key, out[i]); int j = i - 1;
        bool kf = chainIsFactoryName(key);
        while (j >= 0) {
            bool jf = chainIsFactoryName(out[j]);
            bool before = (kf != jf) ? kf : (strcasecmp(key, out[j]) < 0);
            if (!before) break;
            strcpy(out[j + 1], out[j]); j--;
        }
        strcpy(out[j + 1], key);
    }
    return n;
}

bool chainRead(const char *name, char (*slots)[PATCH_NAME_LEN + 1], int *timeMs) {
    for (int i = 0; i < MORPH_SLOTS; i++) slots[i][0] = 0;
    if (timeMs) *timeMs = -1;
    if (!sdMounted) return false;
    char path[64]; chainPath(path, sizeof(path), name);
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    for (int i = 0; f.available() && i < MORPH_SLOTS + 4; i++) {
        String s = f.readStringUntil('\n'); s.trim();
        if (s.startsWith("time=")) { if (timeMs) *timeMs = clampi(s.substring(5).toInt(), 0, MORPH_TIME_MAX_MS); continue; }
        if (i < MORPH_SLOTS) { strncpy(slots[i], s.c_str(), PATCH_NAME_LEN); slots[i][PATCH_NAME_LEN] = 0; }
    }
    f.close();
    return true;
}

bool chainWrite(const char *name, const char *const *slots, int timeMs) {
    if (!sdMounted) return false;
    if (!SD.exists(CHAIN_FOLDER)) SD.mkdir(CHAIN_FOLDER);
    char path[64]; chainPath(path, sizeof(path), name);
    fileWriteBegin();
    for (int i = 0; i < MORPH_SLOTS; i++) fileWritef("%s\n", slots[i] ? slots[i] : "");
    if (timeMs >= 0) fileWritef("time=%d\n", timeMs);
    return fileWriteCommit(path, "Chain");
}

bool chainDelete(const char *name) {
    if (!sdMounted) return false;
    char path[64]; chainPath(path, sizeof(path), name);
    return SD.remove(path);
}

// No time line: loading a factory chain keeps the player's glide Time
int writeFactoryChains(bool overwrite) {
    if (!sdMounted) return -1;
    int written = 0;
    for (int w = 0; w < DEMO_CHAIN_COUNT; w++) {
        char path[64]; chainPath(path, sizeof(path), FACTORY_CHAINS[w]);
        if (!overwrite && SD.exists(path)) continue;
        const char *const *names; int n = factoryDemoChain(&names, w);
        const char *slots[MORPH_SLOTS];
        for (int i = 0; i < MORPH_SLOTS; i++) slots[i] = (i < n) ? names[i] : "";
        if (!chainWrite(FACTORY_CHAINS[w], slots, -1)) return -1;
        written++;
    }
    return written;
}

// ---------------------------------------------------------------------------
// Pattern bank
// ---------------------------------------------------------------------------
static void patternPath(char *out, size_t n, int slot) {
    snprintf(out, n, "%s/%d.json", PATTERN_FOLDER, clampi(slot, 0, NUM_PATTERN_SLOTS - 1));
}

bool savePatternSlot(int slot) {
    if (!sdMounted) return false;
    char path[48]; patternPath(path, sizeof(path), slot);
    sbReset();
    sbAppend("{\n");
    sbAppend("  \"morphase_format\": %d,\n", MORPHASE_PATCH_FORMAT);
    writePatternFields();
    sbCloseJson();
    return notifySave(commitBuffer(path), "Pattern") == SAVE_OK;
}

bool loadPatternSlot(int slot) {
    if (!sdMounted) return false;
    char path[48]; patternPath(path, sizeof(path), slot);
    recoverTmp(path);
    if (!SD.exists(path)) return false;
    loadingPatch = false;
    return parseFile(path);
}

// ---------------------------------------------------------------------------
// Diagnostics ring log
// ---------------------------------------------------------------------------
constexpr int DIAG_LINES = 24;
constexpr int DIAG_LINE_LEN = 64;
static char diagRing[DIAG_LINES][DIAG_LINE_LEN];
static int  diagRingHead = 0, diagRingCount = 0;

void diagLog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(diagRing[diagRingHead], DIAG_LINE_LEN, fmt, ap);
    va_end(ap);
    diagRingHead = (diagRingHead + 1) % DIAG_LINES;
    if (diagRingCount < DIAG_LINES) diagRingCount++;
}

void diagDump() {
    Serial.println("[diag] ---- ring log ----");
    int start = (diagRingHead - diagRingCount + DIAG_LINES) % DIAG_LINES;
    for (int i = 0; i < diagRingCount; i++) Serial.println(diagRing[(start + i) % DIAG_LINES]);
    Serial.println("[diag] ---- end ----");
}
