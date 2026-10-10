// MORPHASE - 4-operator FM polyphonic synthesizer for M5Stack Cardputer ADV
// config.h - every constant, enum and shared struct in one place.
//
// Rules that shape this file (see the specification, section 11):
//  * C++11 build: no in-class default member initializers on structs that
//    are ever brace-initialized. Defaults live in explicit init functions.
//  * Enums that are persisted as numbers are APPEND-ONLY. Never insert a
//    value in the middle; add at the end, before the *_COUNT sentinel.
//  * Short generic identifiers collide with platform headers. `Display`
//    is a macro in M5Unified, `PS` is taken by the ESP32-S3 headers. Use
//    SCREEN / PAD_SIZE style names instead.
//  * Every large buffer is a static array. There is no PSRAM.

#ifndef MORPHASE_CONFIG_H
#define MORPHASE_CONFIG_H

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
#define MORPHASE_VERSION "v1.0"

// Two firmwares from one source (v0.77, BLE stage 1): env `morphase` is the
// standard build, env `morphase_ble` defines MORPHASE_BLE and adds BLE MIDI
// input. The BLE build pays for the BT stack with fewer voices and an 8-bit
// screen; the standard build carries none of it.
// The code audioTask runs once per buffer (v0.77, IRAM stage 3): in IRAM in
// the standard build, so the instruction cache cannot drop it. In flash it
// was evicted by the UI on core 1 and missed by luck of layout - 2KB of
// unused code elsewhere moved Peak by 250us (v0.77T1). About 11.8KB of IRAM,
// i.e. of heap; the BLE build cannot spare that (heap 42K, floor 40K) and
// has the headroom of 10 voices instead, so it keeps this code in flash.
#include <esp_attr.h>
#ifdef MORPHASE_BLE
#define AUDIO_IRAM
#else
#define AUDIO_IRAM IRAM_ATTR
#endif

#if defined(MORPHASE_BLE)
#define MORPHASE_EDITION " BLE"
#else
#define MORPHASE_EDITION ""
#endif

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
constexpr int   SAMPLE_RATE       = 44100;
constexpr int   AUDIO_BUFFER_SIZE = 1024;                 // samples per audioTask iteration
constexpr unsigned long AUDIO_BUDGET_US = 23220;          // 1024 / 44100 s
constexpr int   AUDIO_DMA_BUF_COUNT = 8;
constexpr int   AUDIO_DMA_BUF_LEN   = 512;
constexpr int   AUDIO_OUT_BUFS      = 4;                  // rotating int16 output buffers

// Core assignment. C.P.S. runs audioTask on PRO_CPU (core 0) and pins the
// Speaker's own I2S-feed task to APP_CPU (core 1) next to loop(). That
// split is proven on this hardware and is kept here: voices are NEVER
// spread across cores (spec 11.1).
#define AUDIO_TASK_CORE      PRO_CPU_NUM
#define SPEAKER_TASK_CORE    APP_CPU_NUM
constexpr int AUDIO_TASK_STACK = 6144;
constexpr int AUDIO_TASK_PRIO  = 5;
constexpr int AUDIO_YIELD_EVERY_BUFS = 8;                 // vTaskDelay(1) cadence

// ---------------------------------------------------------------------------
// Voices / operators
// ---------------------------------------------------------------------------
// 8 since v0.13, 12 since v0.62. The spec left this to be set by
// measurement (spec 2.3). v0.61 worst case on hardware (all FX, chain
// sweep, GLITCH flat out): 8 voices Peak 13,815us (59%), 12 voices Peak
// 18,233us (78%), about 1,105us per voice. It is still one constant: if a
// future change makes buffers run late, lower it here and nothing else
// needs touching (the PLAY voice row divides its width by it).
// The BLE build plays 10 (v0.77): the BT controller shares core 0 with
// audioTask, and the two voices give the time back.
#ifdef MORPHASE_BLE
constexpr int MAX_VOICES = 10;
#else
constexpr int MAX_VOICES = 12;
#endif
// What a patch may ask for, the same in both builds, so one patch file
// works in either: a Voices 12 patch keeps its 12 and plays 10 in the BLE
// build (polyLimit() in voice.cpp clamps to MAX_VOICES).
constexpr int PATCH_MAX_VOICES = 12;
static_assert(MAX_VOICES <= PATCH_MAX_VOICES, "the engine cannot have more voices than a patch can ask for");
// STEP pattern notes stay 1-8 (v0.62): they are entered with the digit
// keys, and '0' + 10.. would be ':' ';' '<' - the STEP cursor keys.
constexpr int ARP_STEP_NOTES = 8;
constexpr int NUM_OPS    = 4;
constexpr int NUM_ALGORITHMS = 14;   // 8 until v0.62; 9-14 move the feedback (v0.7)

// Envelopes are advanced once per ENV_BLOCK samples and linearly
// interpolated in between (spec 11.2). Pitch / glide / op increments are
// refreshed on the same cadence.
constexpr int ENV_BLOCK = 32;

// Voice steal: the stolen voice fades over this many samples before it is
// re-initialised with the new note (spec 3.2).
constexpr int VOICE_STEAL_FADE_SAMPLES = 88;              // ~2ms @44.1k

// Modulation depth constants. Operator output is in [-1, 1] * amp; a
// modulator offsets the carrier's phase by (output * FM_MOD_DEPTH) cycles.
// FM_MOD_DEPTH = 1.0 means a full-level modulator sweeps one whole cycle,
// i.e. a modulation index of 2*pi. Feedback is deliberately milder.
constexpr float FM_MOD_DEPTH = 1.0f;
constexpr float FM_FB_DEPTH  = 0.35f;

// Key scaling reference note (C4). keyScale=100% halves the level per
// octave above this (spec 3.7).
constexpr int KEY_SCALE_REF_NOTE = 60;

// ---------------------------------------------------------------------------
// Wave tables
// ---------------------------------------------------------------------------
constexpr int WAVE_TABLE_SIZE = 1024;
constexpr int WAVE_TABLE_MASK = WAVE_TABLE_SIZE - 1;
constexpr int NUM_WAVEFORMS   = 8;

// ---------------------------------------------------------------------------
// FX buffers (all static, sized here, see spec 6.4)
// ---------------------------------------------------------------------------
constexpr int CHORUS_BUFFER_SIZE = 2048;                  // ~46ms, float
constexpr int DELAY_BUFFER_SIZE  = 26460;                 // 600ms, int16
constexpr int NUM_REVERB_COMBS   = 8;
constexpr int NUM_REVERB_ALLPASS = 4;
constexpr int REVERB_COMB_LEN[NUM_REVERB_COMBS]   = {1116,1188,1277,1356,1422,1491,1557,1617};
constexpr int REVERB_AP_LEN[NUM_REVERB_ALLPASS]   = {556,441,341,225};

// FX tail (spec 6.5)
constexpr int FX_TAIL_SILENCE_PEAK = 32;
constexpr int FX_TAIL_QUIET_BUFS   = 2;
constexpr int FX_TAIL_MAX_BUFS     = 344;                 // ~8s
constexpr int FX_TAIL_FADE_BUFS    = 9;

// Buffer-rate exponential smoothing for modulation offsets (spec 4.6)
constexpr float SM_BUF = 0.30f;
constexpr float SNAP_THRESHOLD = 0.01f;                   // spec 11.4

// ---------------------------------------------------------------------------
// MIDI
// ---------------------------------------------------------------------------
constexpr int      MIDI_RX_PIN = 1;                       // Grove G1
constexpr int      MIDI_TX_PIN = 2;                       // Grove G2
constexpr uint32_t MIDI_BAUD   = 31250;
constexpr int      MIDI_CHANNEL_OMNI = 0;                 // midiChannel: 0 = Omni, 1-16

// ---------------------------------------------------------------------------
// SD card
// ---------------------------------------------------------------------------
constexpr int SD_SPI_SCK_PIN  = 40;
constexpr int SD_SPI_MISO_PIN = 39;
constexpr int SD_SPI_MOSI_PIN = 14;
constexpr int SD_SPI_CS_PIN   = 12;
constexpr int CAP_LORA_CS_PIN = 5;                        // held HIGH so a LoRa Cap never answers for the SD (spec 13.1)

#define MORPHASE_FOLDER   "/MORPHASE"
#define PATCH_FOLDER      "/MORPHASE/Patch"
#define PATTERN_FOLDER    "/MORPHASE/Pattern"
#define SETTINGS_PATH     "/MORPHASE/settings.json"
#define CURRENT_PATCH_PATH "/MORPHASE/current.json"   // v0.72: the sound being edited, patch-file format
#define CHAIN_FOLDER      "/MORPHASE/Chain"           // v0.83: saved chains, <name>.txt
#define CHAIN_EXT         ".txt"
#define MORPH_CHAIN_FILE  "/MORPHASE/morph.txt"       // morph chain: one patch name per line, blank = Off
#define NVS_NAMESPACE     "morphase"                  // v0.92T2: the splash's theme copy (NVS)
#define NVS_KEY_THEME     "theme"
#define PATCH_EXT         ".json"

constexpr int MAX_PATCHES        = 128;                   // v0.72 (was 64): +832B for the name table
// Factory set (v0.72). Raise FACTORY_SET_VERSION whenever patches are added
// to the table in patch.cpp: on the next boot, the missing ones are written
// once. A file that already exists is never overwritten.
constexpr int FACTORY_SET_VERSION = 10;    // 2: set 1 (01-08), v0.74 / 3: set 2 (09-16), v0.75 / 4: set 3 (17-24), v0.76 / 5: set 4 (25-32) + 18_Strings redone, v0.79T1 / 6: 27_Snare and 28_HiHat redone, v0.79T2 / 7: both again, v0.79T3 / 8: the factory chains 01_Pads / 02_Drums, v0.83 / 9: volumes after the run-through + 31_NoiseSwl, v0.86 / 10: IMU X GMorph 50% / Y Feedbk 50% by default, v0.87
constexpr int PATCH_NAME_LEN     = 12;                    // characters, excluding terminator
constexpr int NUM_PATTERN_SLOTS  = 8;
// "morphase_format" stamp (spec 9.3). 2 since v0.7: algorithm 1-14. Only
// written, never branched on - older files load through the defaults.
constexpr int MORPHASE_PATCH_FORMAT = 2;

// ---------------------------------------------------------------------------
// Arpeggiator
// ---------------------------------------------------------------------------
constexpr int ARP_STEPS        = 16;
constexpr int ARP_MAX_CHORD    = 12;                      // held notes tracked
constexpr int ARP_MAX_PENDING_OFF = 24;                   // notes waiting for their gate to end

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;
constexpr int TAB_W    = 34;
constexpr unsigned long MENU_REPEAT_DELAY_MS = 400;
constexpr unsigned long MENU_REPEAT_RATE_MS  = 70;
constexpr unsigned long MIN_REDRAW_MS        = 30;
// The PLAY and OP screens animate, so they refresh faster than the rest.
// Raised to 60ms (about 16fps) in v0.31 for the output scope and the
// operator preview. Every push is a 240x135 SPI transfer, and C.P.S.
// traced audio crackle to pushing too often - if the Info screen ever
// shows a buffer over budget, put this back up.
constexpr unsigned long PLAY_REDRAW_MS       = 60;

// Output scope (v0.31): one captured window, one pixel per sample, as wide
// as the plot area on the PLAY screen.
constexpr int SCOPE_POINTS = 152;
constexpr unsigned long KB_WATCHDOG_TIMEOUT_MS = 30000;   // spec 11.6

// ---------------------------------------------------------------------------
// Enums (persisted ones are append-only)
// ---------------------------------------------------------------------------
enum class GlideMode : uint8_t { ALWAYS = 0, LEGATO, GLIDE_MODE_COUNT };

// Modulation destinations for the IMU axes and the LFO.
// APPEND-ONLY: settings.json stores these as numbers, so a value inserted
// in the middle would silently change the meaning of every saved setting.
// The OP_* entries act on whichever operator the OP tab has selected
// (`selectedOp`), so the list stays short while reaching 4x more.
enum class ModTarget : uint8_t {
    OFF = 0, GMORPH, FEEDBACK, PITCH,
    VOLUME, OP_MORPH, OP_LEVEL, OP_RATIO, OP_DETUNE,   // added v0.2
    MOD_AUDIO_END,      // boundary only - never offered as a choice
    ARP_TEMPO, ARP_SWING, ARP_GATE, GLIDE_TIME,        // added v0.3
    CHORUS_DEPTH, CHORUS_MIX, DELAY_FDBK, DELAY_MIX,   // added v0.44: FX parameters
    REVERB_SIZE, REVERB_MIX, LIMITER_DRIVE,
    PATCH_MORPH,                                       // added v0.5: position along the morph chain
    CRUSH_BITS, CRUSH_DOWN, CRUSH_MIX,                 // added v0.55: Bitcrush
    GLITCH_PROB,                                       // added v0.6: adds to the GLITCH probability
    PENV_AMT,                                          // added v0.71: pitch envelope amount (semitones)
    DRIVE_MIX,                                         // added v0.72: Drive (ex-Limiter) mix
    MOD_TARGET_COUNT
};
// Destinations below MOD_AUDIO_END are audio parameters and take a bipolar
// offset around the patch value. ARP_TEMPO..GLIDE_TIME are control
// parameters: a knob's position IS the value, so a CC sets them absolutely.
// Tilt has a neutral, so the IMU still offsets them relatively - the two
// sources deliberately differ here. The FX destinations (v0.44) are audio
// parameters again; append-only forced them to sit after the control block,
// so the classes are decided by these functions, never by comparing with
// MOD_AUDIO_END directly. PATCH_MORPH (v0.5) is audio too - the LFO may sweep
// it - but was appended rather than inserted before MOD_AUDIO_END, because
// these numbers are stored in every patch file as well as in settings.json.
inline bool modTargetIsFx(ModTarget t) {
    return (t >= ModTarget::CHORUS_DEPTH && t <= ModTarget::LIMITER_DRIVE) ||
           (t >= ModTarget::CRUSH_BITS && t <= ModTarget::CRUSH_MIX) ||    // v0.55
           t == ModTarget::DRIVE_MIX;                                      // v0.72
}
inline bool modTargetIsAudio(ModTarget t) {
    return t < ModTarget::MOD_AUDIO_END || modTargetIsFx(t) ||
           t == ModTarget::PATCH_MORPH || t == ModTarget::GLITCH_PROB ||   // the LFO may drive both
           t == ModTarget::PENV_AMT;                                       // v0.71
}
inline bool modTargetIsControl(ModTarget t) { return t > ModTarget::MOD_AUDIO_END && t < ModTarget::CHORUS_DEPTH; }

// What a MIDI switch (a button sending CC) can do. Append-only.
enum class SwitchAction : uint8_t {
    OFF = 0, ARP_ON_OFF, ARP_LATCH, NOTE_HOLD, GLIDE_ON_OFF,
    PATCH_NEXT, PATCH_PREV, PATTERN_NEXT, PATTERN_PREV,
    TAP_TEMPO, RANDOMIZE_PATCH, RANDOMIZE_PATTERN, PANIC,
    BYP_CHORUS, BYP_DELAY, BYP_REVERB, BYP_CRUSH, BYP_LIMITER,   // added v0.56: FX bypass
    GLITCH_SW,                                                    // added v0.6: Toggle = Glitch On/Off, Momentary = hold
    THEREMIN_SW,                                                  // added v0.81: Theremin On/Off
    CHAIN_NEXT, CHAIN_PREV,                                       // added v0.84: load the next / previous saved chain
    SWITCH_ACTION_COUNT
};
// Controller buttons come in both flavours, so the mode is per slot.
enum class SwitchMode : uint8_t { TOGGLE = 0, MOMENTARY, SWITCH_MODE_COUNT };

// GLITCH settings (v0.6). Append-only like every stored enum.
enum class GlRate  : uint8_t { R8 = 0, R8T, R16, R16T, R32, R32T, GL_RATE_COUNT };
enum class GlBuf   : uint8_t { OFF = 0, STUTTER, REVERSE, GATE, RANDOM, GL_BUF_COUNT };
enum class GlChaos : uint8_t { OFF = 0, RATIO, TIMBRE, WILD, GL_CHAOS_COUNT };
enum class GlLen   : uint8_t { L32 = 0, L16, L8, GL_LEN_COUNT };
// ARP glitch (v0.82): decided on each ARP step, separately from the events above
enum class GlArp   : uint8_t { OFF = 0, RATCHET, OCTAVE, REVERSE, REST, RANDOM, GL_ARP_COUNT };

// FX pads in key order (q w e r t, v0.56). The processing order differs:
// Crush runs first. Bit n of the bypass mask is pad n.
enum FxPad : uint8_t { FXP_CHORUS = 0, FXP_DELAY, FXP_REVERB, FXP_CRUSH, FXP_LIMITER, FX_PAD_COUNT };
// Mix a pad gets when it is switched on: from the FX tab (written to the
// patch) or temporarily from a key (v0.57, never saved).
constexpr float FX_PAD_DEFAULT_MIX[FX_PAD_COUNT] = { 40.f, 30.f, 35.f, 100.f, 100.f };

constexpr int NUM_CC_SLOTS = 4;
constexpr int NUM_SW_SLOTS = 4;
constexpr int CC_OFF = -1;          // "no CC assigned"

struct CcSlot {                     // a knob
    int       number;               // 0-127, or CC_OFF
    ModTarget target;
    int       depth;                // 0-100 %
};
struct SwSlot {                     // a button
    int          number;            // 0-127, or CC_OFF
    SwitchAction action;
    SwitchMode   mode;
};

enum class LfoWave : uint8_t { SINE = 0, TRIANGLE, SAW, SQUARE, SAMPLE_HOLD, LFO_WAVE_COUNT };

enum class ArpPattern : uint8_t { UP = 0, DOWN, UPDOWN, RANDOM, AS_PLAYED, STEP, ARP_PATTERN_COUNT };

enum class ArpRate : uint8_t { R_1_4 = 0, R_1_8, R_1_8T, R_1_16, R_1_16T, R_1_32, ARP_RATE_COUNT };

enum class ChordWrap : uint8_t { REST = 0, WRAP, OCTAVE, CHORD_WRAP_COUNT };

// Latch behaviour (v0.12): ADD = every key pressed joins the chord, a
// latched key pressed again leaves it (Cardputer / C.P.S. style).
// CHORD = the usual keyboard latch: keys held together form the chord,
// and the first key after everything was released starts a new one.
enum class LatchMode : uint8_t { ADD = 0, CHORD, LATCH_MODE_COUNT };

// IMU response curve (v0.4): Exp squares the tilt, so small movements stay
// fine and the last part of the travel does most of the work.
enum class ImuCurve : uint8_t { LINEAR = 0, EXP, IMU_CURVE_COUNT };

enum class UiTheme : uint8_t { GREEN = 0, AMBER, CYAN, VIOLET, RED, UI_THEME_COUNT };

// Tabs. PLAY..SETTING is the Tab / Shift+Tab cycle; the rest are
// sub-screens reached from SETTING / FX and are never part of the cycle.
enum class AppMode : uint8_t {
    PLAY = 0, OP, ALGO, ARP, LFO, FX, SETTING,
    PATCH_BROWSER, NAME_ENTRY, CONFIRM,
    APP_MODE_COUNT
};
constexpr int NUM_TABS = 7;

// Envelope stages
enum class EnvStage : uint8_t { IDLE = 0, ATTACK, DECAY, SUSTAIN, RELEASE };

// ---------------------------------------------------------------------------
// Sound data: what a patch carries (spec 9.2)
// ---------------------------------------------------------------------------
struct OpParams {
    float morph;      // 0-100 %
    float ratio;      // 0.25 - 16.00
    float detune;     // -50 .. +50 cents
    float level;      // 0-100 %
    float attack;     // 0-100
    float decay;      // 0-100
    float sustain;    // 0-100
    float release;    // 0-100
    float velSens;    // 0-100 %
    float keyScale;   // 0-100 %
    // Fixed frequency (v0.71, update spec 5 section 3). Kept apart from
    // ratio so switching the mode back restores the old ratio. A fixed
    // operator ignores the key, glide, bend and the Pitch destination;
    // Detune and the pitch envelope still move it.
    bool  fixed;      // false = Ratio, true = Fixed
    float fixNote;    // FIX_NOTE_MIN..FIX_NOTE_MAX semitones on the MIDI scale (69 = 440Hz)
};
constexpr float FIX_NOTE_MIN = -36.f;      // ~1.0Hz
constexpr float FIX_NOTE_MAX = 120.f;      // ~8.4kHz
constexpr float FIX_NOTE_DEFAULT = 57.f;   // A3 = 220Hz

struct Patch {
    char      name[PATCH_NAME_LEN + 1];
    OpParams  op[NUM_OPS];
    int       algorithm;      // 1-NUM_ALGORITHMS (14 since v0.7)
    float     feedback;       // 0-100
    float     globalMorph;    // 0-100
    float     volume;         // 0-100
    int       voices;         // 1..PATCH_MAX_VOICES (the engine plays up to MAX_VOICES)
    float     glideMs;        // 0-1000
    bool      glideEnabled;   // v0.3: glide off without losing the time value
    GlideMode glideMode;
    // LFO
    LfoWave   lfoWave;
    float     lfoRateHz;      // 0.1 - 20
    float     lfoDepth;       // 0-100
    ModTarget lfoTarget;      // an audio destination, or OFF (selectable since v0.73)
    float     lfoDelayMs;     // 0-2000
    // IMU targets and depths (enable/hold state is NOT in the patch)
    ModTarget imuXTarget;
    float     imuXDepth;
    ModTarget imuYTarget;
    float     imuYDepth;
    // FX
    float chorusRateHz;   // 0.1-5
    float chorusDepthMs;  // 0-20
    float chorusMix;      // 0-100
    float delayTimeMs;    // 50-600
    float delayFeedback;  // 0-90
    float delayMix;       // 0-100
    float reverbSize;     // 0-100
    float reverbDamp;     // 0-100
    float reverbMix;      // 0-100
    float limiterDrive;   // 1.0-5.0
    float limiterMix;     // 0-100
    // Bitcrush (v0.55, update spec 4). First in the chain. Like every other
    // effect, Mix 0 means Off - there is no separate on/off flag.
    float crushBits;      // 1.0-16.0, continuous so morph / LFO sweep it without steps
    float crushDown;      // 1.0-32.0, sample-rate reduction factor (1 = none)
    float crushMix;       // 0-100
    // Pitch envelope (v0.71, update spec 5 section 4): one per voice, on
    // every operator. Starts Amt semitones off and returns to 0 over Decay;
    // no attack stage. Amt 0 = off, which is what older patches load as.
    float penvAmt;        // -48..+48 semitones
    float penvDecay;      // 0-100, the operator Decay curve
    float penvVel;        // 0-100 %, velocity depth on Amt (VelSns formula)
};
constexpr float PENV_AMT_MAX = 48.f;

// ---------------------------------------------------------------------------
// Arpeggiator data (session / pattern bank, never in a patch)
// ---------------------------------------------------------------------------
struct ArpStep {
    uint8_t note;   // 0 = Off, 1..ARP_STEP_NOTES = chord index (8)
    int8_t  oct;    // -2..+2
    uint8_t vel;    // 1-127
    uint8_t gate;   // 10-200 %
};

// Where octave / transpose act while the ARP is on (v0.43).
// KEYS_IN: on the keys going in - a latched chord keeps the shift it was
//        played with, and a change only affects notes played after it.
// ARP_OUT: on the notes the ARP fires - the chord is stored unshifted, so a
//        change moves the running (latched) arpeggio straight away.
enum class ShiftMode : uint8_t { KEYS_IN = 0, ARP_OUT, SHIFT_MODE_COUNT };   // not INPUT / OUTPUT: Arduino macros
// Program Change (v0.8): load a patch by its list position, or glide to a
// morph chain slot (PC 0-7 = slots 1-8, the same as Shift+1-8).
enum class PcMode : uint8_t { PATCH = 0, MORPH, PC_MODE_COUNT };
// Theremin (v0.81, SETTING > Theremin). Stored: append only.
enum class ThBus     : uint8_t { CAP = 0, GROVE, TH_BUS_COUNT };
enum class ThMode    : uint8_t { MOD = 0, PLAY, TH_MODE_COUNT };
enum class ThRelease : uint8_t { HOLD = 0, RETURN, TH_RELEASE_COUNT };
enum class ThPitch   : uint8_t { SMOOTH = 0, SEMITONE, TH_PITCH_COUNT };
enum class ThVolAxis : uint8_t { AXIS_OFF = 0, AXIS_X, AXIS_Y, TH_VOLAXIS_COUNT };

struct ArpSettings {
    bool       on;
    ArpPattern pattern;
    bool       latch;
    int        tempoBpm;    // 40-300
    ArpRate    rate;
    int        swing;       // 0-100
    int        gate;        // 10-200
    int        octave;      // 1-4
    ChordWrap  chordWrap;
    int        length;      // 1-16 (STEP)
    ArpStep    step[ARP_STEPS];
    bool       midiOut;     // send generated notes on MIDI OUT
    bool       clockSync;   // follow incoming MIDI clock instead of tempoBpm (v0.11, default Off)
    LatchMode  latchMode;   // v0.12
    ShiftMode  shiftMode;   // v0.43, default KEYS_IN (the v0.42 behaviour)
};

// ---------------------------------------------------------------------------
// Device / session settings (settings.json, never in a patch)
// ---------------------------------------------------------------------------
struct SystemSettings {
    int       midiChannel;      // 0 = Omni, 1-16
    int       bendRange;        // 1-12 semitones
    CcSlot    ccSlot[NUM_CC_SLOTS];
    SwSlot    swSlot[NUM_SW_SLOTS];
    int       patternBaseCc;    // base of 8 consecutive CCs selecting patterns, or CC_OFF
    PcMode    pcMode;           // what a Program Change does (v0.8)
    bool      imuXEnabled;
    bool      imuYEnabled;
    bool      imuXHeld;
    bool      imuYHeld;
    float     imuXHeldNorm;
    float     imuYHeldNorm;
    UiTheme   theme;
    int       brightness;       // 10-100
    int       patternSlot;      // last used pattern slot 0-7
    int       masterVolume;     // 0-100, K / L keys (v0.11) - device level, not in the patch
    // IMU calibration (v0.12): the tilt angles that count as "level".
    bool      imuCalibrated;
    float     imuXCalDeg;
    float     imuYCalDeg;
    // IMU response per axis (v0.4, from C.P.S.). Device settings, not patch.
    int       imuXSens;         // 50-200 %: 100 = full travel at 35 degrees
    int       imuYSens;
    bool      imuXInvert;
    bool      imuYInvert;
    ImuCurve  imuXCurve;
    ImuCurve  imuYCurve;
    int       imuXDeadzone;     // 0-30 % of the travel
    int       imuYDeadzone;
    // Octave / transpose (v0.42, AEOLYRA layout: PLAY ; . , / keys). Both
    // shift every incoming MIDI note - there is no on-device keyboard.
    int       octave;           // OCTAVE_MIN..OCTAVE_MAX
    int       transpose;        // TRANSPOSE_MIN..TRANSPOSE_MAX semitones
    // Patch morph (v0.5). The chain itself lives in /MORPHASE/morph.txt.
    bool      morphEnabled;     // the chain (position along slots 1..N)
    int       morphPosBase;     // 0-100 %: the position with no modulation
    int       morphTimeMs;      // 0-10000: Shift+1..8 glide time (v0.51)
    bool      morphPaused;      // v0.82: the chain was paused (a patch loaded over it) - restored at boot
    // Text drawn over the PLAY scope, each switchable (v0.56, Display)
    bool      scopeGmFb;        // GM / FB readouts
    bool      scopeMorph;       // morph position
    bool      scopeFx;          // FX state letters
    bool      scopeGl;          // GL indicator (v0.6)
    // GLITCH (v0.6, SETTING > Glitch). Device settings, never in a patch.
    bool      glOn;             // always-on random mode
    GlRate    glRate;           // decision grid
    int       glProb;           // 0-100 %, always-on probability
    int       glHold;           // 0-100 %, probability while held (g / switch)
    GlBuf     glBuf;
    GlChaos   glChaos;
    GlLen     glLen;            // event length
    int       glAmt;            // 0-100 %: slices / throw size; ARP glitch: ratchet count / reverse length
    GlArp     glArp;            // v0.82: ARP glitch kind
    bool      bleMidiEnabled;   // v0.77, BLE build: start the BT stack at boot (On by default there)
    // Theremin (v0.81). Device settings, never in a patch (like GLITCH).
    bool      thOn;
    ThBus     thBus;
    ThMode    thMode;
    int       thNearMm;         // TH_NEAR_MIN..TH_NEAR_MAX
    int       thFarMm;          // thNearMm + TH_FAR_GAP..TH_FAR_MAX
    ModTarget thTarget;         // Mod: the same list as the IMU
    float     thDepth;          // Mod: 0-100 %
    ThRelease thRelease;        // Mod: hand gone -> keep the value / glide back to 0
    ThPitch   thPitch;          // Play
    int       thScale;          // Play, Semitone: index into TH_SCALES
    int       thTopSemis;       // Play: top of the range, semitones from C4 (octave steps)
    int       thSpan;           // Play: 1-4 octaves
    ThVolAxis thVolAxis;        // Play: IMU axis for the volume
    int       factoryVersion;   // factory set last written to this card (v0.72)
};
constexpr int OCTAVE_MIN = -3, OCTAVE_MAX = 3;
constexpr int TRANSPOSE_MIN = -12, TRANSPOSE_MAX = 12;

// ---------------------------------------------------------------------------
// Events from loop() (MIDI / ARP / keys) to audioTask. Single producer,
// single consumer ring buffer - the only way note information crosses the
// core boundary (spec 11.1).
// ---------------------------------------------------------------------------
enum NoteEventType : uint8_t {
    EV_NOTE_ON = 0, EV_NOTE_OFF, EV_SUSTAIN_ON, EV_SUSTAIN_OFF,
    EV_ALL_NOTES_OFF, EV_ALL_SOUND_OFF
};
enum NoteEventFlag : uint8_t {
    EVF_NONE = 0,
    EVF_IGNORE_SUSTAIN = 1,    // ARP-generated notes: the pedal must not hold them
    EVF_THEREMIN = 2           // audioTask-internal: the theremin's own voice (v0.81), never queued
};
struct NoteEvent {
    uint8_t type;
    uint8_t note;
    uint8_t vel;
    uint8_t flags;
};
constexpr int NOTE_EVENT_QUEUE_SIZE = 64;   // power of two

// ---------------------------------------------------------------------------
// Live control values written by loop(), read by audioTask once per buffer.
// Plain 32-bit stores are atomic on this core; no struct-level consistency
// is needed because audioTask copies what it needs at buffer start.
// ---------------------------------------------------------------------------
struct LiveControl {
    volatile float pitchBendNorm;     // -1..1 (wheel and Z/X keys)
    volatile float ccMorphNorm;       // 0..1  (CC#1 / CC#74)
    volatile float ccFeedbackNorm;    // 0..1  (CC#2)
    volatile float ccSlotNorm[NUM_CC_SLOTS];   // 0..1 per knob slot, -1 until its CC first arrives
    volatile float ccVolumeNorm;      // 0..1  (CC#7)
    volatile float ccExpressionNorm;  // 0..1  (CC#11)
    volatile float imuXNorm;          // -1..1 (already hold/enable resolved; 0 when inactive)
    volatile float imuYNorm;
    // Theremin (v0.81). Mod: thModNorm 0..1 (near = 1), 0 when not in Mod.
    // Play: thReserve keeps the last voice for it while Play is on; thGate
    // = a hand in range; thHz the pitch; thVol 0..1 (VolAxis, 1 when Off).
    volatile float thModNorm;
    volatile bool  thReserve;
    volatile bool  thGate;
    volatile float thHz;
    volatile float thVol;
};

// Values audioTask publishes for the display.
struct AudioStatus {
    volatile int   activeVoices;
    volatile float effGlobalMorph;    // 0-100 after modulation
    volatile float effFeedback;       // 0-100 after modulation
    volatile float effPitchSemi;      // semitones of pitch modulation
    volatile float lfoValue;          // -1..1
    volatile float lfoPhase;          // 0..1, for the LFO tab's shape preview (v0.41)
    volatile uint32_t bufSeq;         // bumps after livePatch is built (morph chain handshake)
    volatile float morphPos;          // 0..1 overall position (v0.5)
    volatile int   morphSeg;          // segment start slot, -1 when not morphing, -2 while gliding
    volatile int   morphGlideTo;      // slot a Shift+number glide heads for, -1 none (v0.51)
    volatile float morphT;            // 0..1 within the segment
    volatile bool  lfoModulating;     // LFO depth > 0
    volatile bool  imuModulating;     // an IMU axis is live
    // v0.85: where the last key note is on its envelopes, for the dot on the
    // OP / ALGO graphs. The theremin's note is never the one shown.
    volatile uint8_t probeStage;      // EnvStage of the OP tab's operator, 0 (IDLE) = no note
    volatile float   probePos;        // 0..1 within that stage
    volatile float   probeEnv;        // its envelope level 0..1
    volatile float   probePenvPos;    // 0..1 along the pitch envelope, -1 when it has ended
    volatile float   probePenvVelK;   // that note's pitch envelope velocity factor
    volatile unsigned long lastDspUs;
    volatile unsigned long heartbeatUs;
};

// Diagnostics summary produced by audioTask, printed by loop() once per
// second (never printed from audioTask - spec 11.1).
struct DiagSummary {
    volatile bool          pending;
    volatile unsigned long count;
    volatile unsigned long sumUs;
    volatile unsigned long maxUs;
    volatile unsigned long overCount;
    volatile int           tailBufs;
};

// ---------------------------------------------------------------------------
// Shared globals (defined in the module named in the comment)
// ---------------------------------------------------------------------------
extern Patch          patch;          // patch.cpp  - the sound being edited
extern Patch          livePatch;      // morph.cpp  - patch or morph result, smoothed (audioTask writes)
extern Patch          enginePatch;    // voice.cpp  - livePatch + GLITCH Chaos: what the engine plays (v0.6)
extern ArpSettings    arp;            // arp.cpp
extern SystemSettings sys;            // patch.cpp
extern LiveControl    live;           // voice.cpp
extern AudioStatus    audioStatus;    // voice.cpp
extern DiagSummary    diag;           // voice.cpp

// Offsets for the control-domain destinations. Written by loop() from the
// IMU, read where each value is used - never written back into the setting
// itself, so an offset can never freeze a parameter at a stale value.
struct ControlMod {
    volatile float tempoOff;   // BPM
    volatile float swingOff;   // %
    volatile float gateOff;    // %
    volatile float glideOff;   // ms
};
extern ControlMod ctrlMod;     // voice.cpp

// The operator the OP tab has selected, 0-3. Owned by the UI, read by
// audioTask for the OP_* modulation targets, so a plain aligned int.
extern volatile int selectedOp;

// Raw tilt angles in degrees, before calibration (main.cpp, updated ~50Hz)
extern float imuAngleXDeg;
extern float imuAngleYDeg;

extern bool loadingPatch;             // patch.cpp - guards non-sound keys while a patch is read
extern bool savingPatch;              // patch.cpp - guards non-sound keys while a patch is written

// Name tables (defined in patch.cpp)
extern const char *WAVEFORM_NAMES[NUM_WAVEFORMS];
extern const char *ALGORITHM_NAMES[NUM_ALGORITHMS];
extern const char *MOD_TARGET_NAMES[(int)ModTarget::MOD_TARGET_COUNT];
extern const char *LFO_WAVE_NAMES[(int)LfoWave::LFO_WAVE_COUNT];
extern const char *ARP_PATTERN_NAMES[(int)ArpPattern::ARP_PATTERN_COUNT];
extern const char *ARP_RATE_NAMES[(int)ArpRate::ARP_RATE_COUNT];
extern const float ARP_RATE_MULT[(int)ArpRate::ARP_RATE_COUNT];
extern const char *CHORD_WRAP_NAMES[(int)ChordWrap::CHORD_WRAP_COUNT];
extern const char *GLIDE_MODE_NAMES[(int)GlideMode::GLIDE_MODE_COUNT];
extern const char *UI_THEME_NAMES[(int)UiTheme::UI_THEME_COUNT];
extern const char *IMU_CURVE_NAMES[(int)ImuCurve::IMU_CURVE_COUNT];
extern const char *SHIFT_MODE_NAMES[(int)ShiftMode::SHIFT_MODE_COUNT];
extern const char *PC_MODE_NAMES[(int)PcMode::PC_MODE_COUNT];
extern const char *TH_BUS_NAMES[(int)ThBus::TH_BUS_COUNT];
extern const char *TH_MODE_NAMES[(int)ThMode::TH_MODE_COUNT];
extern const char *TH_RELEASE_NAMES[(int)ThRelease::TH_RELEASE_COUNT];
extern const char *TH_PITCH_NAMES[(int)ThPitch::TH_PITCH_COUNT];
extern const char *TH_VOLAXIS_NAMES[(int)ThVolAxis::TH_VOLAXIS_COUNT];
extern const char *GL_RATE_NAMES[(int)GlRate::GL_RATE_COUNT];
extern const char *GL_BUF_NAMES[(int)GlBuf::GL_BUF_COUNT];
extern const char *GL_CHAOS_NAMES[(int)GlChaos::GL_CHAOS_COUNT];
extern const char *GL_LEN_NAMES[(int)GlLen::GL_LEN_COUNT];
extern const char *GL_ARP_NAMES[(int)GlArp::GL_ARP_COUNT];
extern const char *LATCH_MODE_NAMES[(int)LatchMode::LATCH_MODE_COUNT];
extern const char *SWITCH_ACTION_NAMES[(int)SwitchAction::SWITCH_ACTION_COUNT];
extern const char *SWITCH_MODE_NAMES[(int)SwitchMode::SWITCH_MODE_COUNT];

// Carrier mask per algorithm (bit0 = OP1 ... bit3 = OP4). fm.cpp
extern const uint8_t ALGO_CARRIER_MASK[NUM_ALGORITHMS];
// Operators that feed back on themselves, same bit order (v0.7). fm.cpp
extern const uint8_t ALGO_FB_MASK[NUM_ALGORITHMS];

// Small helpers used everywhere
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int   clampi(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }
inline bool  algoIsCarrier(int algorithm, int opIndex) {
    int a = clampi(algorithm, 1, NUM_ALGORITHMS) - 1;
    return (ALGO_CARRIER_MASK[a] >> opIndex) & 1;
}
inline bool  algoHasFb(int algorithm, int opIndex) {
    int a = clampi(algorithm, 1, NUM_ALGORITHMS) - 1;
    return (ALGO_FB_MASK[a] >> opIndex) & 1;
}

#endif // MORPHASE_CONFIG_H
