// MORPHASE - voice.cpp
// Voice manager and audioTask.
//
// Ownership (spec 11.1): everything in this file below the event queue is
// touched by audioTask ONLY. loop() communicates through:
//   * NoteEvent ring buffer (single producer / single consumer)
//   * `patch`, `sys` parameter structs (plain 32-bit fields, read per buffer)
//   * `live` control values (volatile floats, read per buffer)
// and reads back `audioStatus` / `diag`, which audioTask writes.

#include "voice.h"
#include "fm.h"
#include "fx.h"
#include "morph.h"
#include "glitch.h"
#include "arp.h"
#include <M5Cardputer.h>
#include <esp_system.h>   // esp_random()
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Shared globals owned by this module
// ---------------------------------------------------------------------------
LiveControl live;
ControlMod  ctrlMod = {0.f, 0.f, 0.f, 0.f};
AudioStatus audioStatus;
DiagSummary diag;

// ---------------------------------------------------------------------------
// Event queue (SPSC ring)
// ---------------------------------------------------------------------------
static NoteEvent    evQueue[NOTE_EVENT_QUEUE_SIZE];
static volatile int evHead = 0;   // written by producer (loop)
static volatile int evTail = 0;   // written by consumer (audioTask)

bool postNoteEvent(uint8_t type, uint8_t note, uint8_t vel, uint8_t flags) {
    int head = evHead;
    int next = (head + 1) & (NOTE_EVENT_QUEUE_SIZE - 1);
    if (next == evTail) return false;          // full: drop rather than block
    evQueue[head].type  = type;
    evQueue[head].note  = note;
    evQueue[head].vel   = vel;
    evQueue[head].flags = flags;
    evHead = next;
    return true;
}

static bool popNoteEvent(NoteEvent &out) {
    int tail = evTail;
    if (tail == evHead) return false;
    out = evQueue[tail];
    evTail = (tail + 1) & (NOTE_EVENT_QUEUE_SIZE - 1);
    return true;
}

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------
struct Voice {
    VoiceRender r;
    bool     active;
    uint8_t  note;
    uint8_t  vel;
    bool     keyHeld;         // key (or ARP gate) still down
    bool     sustained;       // note-off deferred by the pedal
    bool     ignoreSustain;   // ARP notes: pedal must not hold them
    bool     theremin;        // the theremin's note (v0.81): keys, pedal and panic never touch it
    uint32_t order;           // allocation sequence, for oldest-first stealing
    // steal handshake (spec 3.2: 2ms fade before re-init)
    bool     stealPending;
    int      stealFadeLeft;
    uint8_t  pendingNote, pendingVel, pendingFlags;
};

static Voice voices[MAX_VOICES];
static uint32_t allocCounter = 0;
static int lastKeyVoice = -1;           // the voice a key started last (v0.85, the graph's dot)
static bool  sustainDown = false;
static float lastNoteFreq = 0.f;        // for glide
static bool  anyKeyHeldBefore = false;  // for Legato glide

// Mono note stack (voices == 1): returning to a still-held key on release
static uint8_t monoStack[16];
static int     monoStackLen = 0;
static uint8_t monoStackVel[16];
static bool    monoMode = false;  // the voice limit the stack was built under

// The stack only means "keys held right now" if every key went in and came
// out under the mono rules. When the voice limit crosses between 1 and more
// (a patch change, a morph between a mono and a poly patch), keys pressed in
// one mode are released in the other and the stack goes stale - and a stale
// entry is a note the voice falls back to that will never get a note-off.
// So the stack starts empty whenever the mode changes (v0.52).
static void syncMonoMode(bool mono) {
    if (mono != monoMode) { monoMode = mono; monoStackLen = 0; }
}

// Every copy of `note` leaves the stack (v0.52: was only the first, so a
// note started twice - an ARP step retriggering itself with Gate > 100% -
// left one behind).
static AUDIO_IRAM void monoStackRemove(uint8_t note) {
    int w = 0;
    for (int i = 0; i < monoStackLen; i++) {
        if (monoStack[i] == note) continue;
        monoStack[w] = monoStack[i]; monoStackVel[w] = monoStackVel[i]; w++;
    }
    monoStackLen = w;
}

// Display mirror (written once per buffer)
static volatile int  voiceNoteMirror[MAX_VOICES];
static volatile bool voiceActiveMirror[MAX_VOICES];

int  voiceNoteAt(int i)   { if (i < 0 || i >= MAX_VOICES) return -1; return voiceActiveMirror[i] ? voiceNoteMirror[i] : -1; }
bool voiceActiveAt(int i) { if (i < 0 || i >= MAX_VOICES) return false; return voiceActiveMirror[i]; }

// LFO
static float lfoPhase = 0.f;
static float lfoShValue = 0.f;
static float lfoDelayElapsedMs = 0.f;
static float lfoValue = 0.f;

// Smoothed modulation offsets live in smOff[], indexed by ModTarget (see
// updateModulation). Indexing by the enum means a destination added later
// cannot be forgotten in the smoothing or the reset.
static float prevPitchMul = 1.f;

// Detune cache (4 powf only when a detune value changes)
static float detuneCache[NUM_OPS] = {9999.f, 9999.f, 9999.f, 9999.f};
static float detuneMul[NUM_OPS]   = {1.f, 1.f, 1.f, 1.f};

// FX tail state
static volatile bool fxClearRequest = false;
static bool fxTailPending = false;
static int  fxTailQuietBufs = 0;
static int  fxTailBufCount = 0;

// Cumulative DSP counters (spec A). Written by audioTask, read by the UI:
// plain uint32_t on purpose, nothing wider and nothing grouped, so a read
// from the other core can never catch a half-updated value.
volatile uint32_t dspOverBudgetCount = 0;
volatile uint32_t dspMaxUs = 0;
void dspResetCounters() { dspOverBudgetCount = 0; dspMaxUs = 0; }

// Diagnostics accumulators (audioTask side)
static unsigned long diagCount = 0, diagSumUs = 0, diagMaxUs = 0, diagOverCount = 0;
static unsigned long diagWindowStartMs = 0;
static int diagTailBufs = 0;

// Buffers
static int16_t outBufs[AUDIO_OUT_BUFS][AUDIO_BUFFER_SIZE];   // 8KB
static float   mixBuf[AUDIO_BUFFER_SIZE];                    // 4KB

void requestFxClear() { fxClearRequest = true; }

static bool susPedal = false, susKey = false, susCombined = false;
void sustainInput(int source, bool down) {
    if (source == SUS_PEDAL) susPedal = down; else susKey = down;
    bool now = susPedal || susKey;
    if (now == susCombined) return;
    susCombined = now;
    if (arp.on) arpSustain(now); else postSustain(now);
}
bool sustainKeyHeld() { return susKey; }

void voiceEngineInit() {
    memset(voices, 0, sizeof(voices));
    live.pitchBendNorm = 0.f;
    live.ccMorphNorm = 0.f;
    live.ccFeedbackNorm = 0.f;
    for (int i = 0; i < NUM_CC_SLOTS; i++) live.ccSlotNorm[i] = -1.f;   // not heard yet
    live.ccVolumeNorm = 1.f;
    live.ccExpressionNorm = 1.f;
    live.imuXNorm = 0.f;
    live.imuYNorm = 0.f;
    live.thModNorm = 0.f; live.thReserve = false; live.thGate = false;   // v0.81
    live.thHz = 261.6f; live.thVol = 1.f;
    audioStatus.activeVoices = 0;
    audioStatus.effGlobalMorph = 0.f;
    audioStatus.effFeedback = 0.f;
    audioStatus.effPitchSemi = 0.f;
    audioStatus.lfoValue = 0.f;
    audioStatus.lfoModulating = false;
    audioStatus.imuModulating = false;
    audioStatus.lastDspUs = 0;
    audioStatus.heartbeatUs = 0;
    diag.pending = false;
    for (int i = 0; i < MAX_VOICES; i++) { voiceNoteMirror[i] = 0; voiceActiveMirror[i] = false; }
}

// ---------------------------------------------------------------------------
// Note helpers
// ---------------------------------------------------------------------------
static inline float midiNoteToFreq(int note) {
    return 440.f * powf(2.f, (float)(note - 69) / 12.f);
}

// Theremin Play (v0.81, instruction (6) 1.5): while it is on, the last voice
// is the theremin's and the keys get min(Voices, MAX_VOICES - 1). The patch's
// Voices is never rewritten (as with the BLE build's 10). A mono patch keeps
// voices[0] and its stack for the keys; the theremin's voice is outside both.
constexpr int     TH_VOICE = MAX_VOICES - 1;
constexpr uint8_t TH_VEL   = 100;
static bool  thOn = false;            // the theremin holds a note (until its hand leaves)
static float thGainNow = 1.f, thGainNext = 1.f;   // VolAxis gain, ramped across each buffer

static int polyLimit() {
    int lim = clampi(enginePatch.voices, 1, MAX_VOICES);
    if (live.thReserve && lim > MAX_VOICES - 1) lim = MAX_VOICES - 1;
    return lim;
}

static bool anyVoiceActive() {
    for (int i = 0; i < MAX_VOICES; i++) if (voices[i].active) return true;
    return false;
}

// A key's note sounding (v0.82): the theremin's voice does not count, so a
// key played while the theremin sounds still starts the LFO Delay over -
// 14_SawLead's vibrato arriving after 0.6s is part of the sound.
static AUDIO_IRAM bool anyKeyVoiceActive() {
    for (int i = 0; i < MAX_VOICES; i++) {
        const Voice &v = voices[i];
        if (v.active && !v.theremin && !(v.stealPending && (v.pendingFlags & EVF_THEREMIN))) return true;
    }
    return false;
}

static bool anyKeyHeld() {
    for (int i = 0; i < MAX_VOICES; i++) if (voices[i].active && voices[i].keyHeld && !voices[i].theremin) return true;
    return false;
}

// Per-block pitch following for the theremin's note: ~25ms (Smooth) or ~4ms
// (Semitone, the steps stay steps) time constant, 1 - exp(-32 / 44100 / tau)
static inline float thGlideCap() { return sys.thPitch == ThPitch::SEMITONE ? 0.166f : 0.0286f; }

// Initialise a voice for a new note. Velocity and key scaling are frozen
// here (spec 4.2 / 3.7); level itself stays live so edits are heard.
static AUDIO_IRAM void startVoice(Voice &v, uint8_t note, uint8_t vel, uint8_t flags, bool retrigger) {
    // The theremin's note (v0.81) sounds at its own pitch, never glides in
    // from the keys and leaves the keys' glide state alone.
    bool th = (flags & EVF_THEREMIN) != 0;
    float target = th ? clampf(live.thHz, 8.f, 12000.f) : midiNoteToFreq(note);
    float glideMs = clampf(enginePatch.glideMs + ctrlMod.glideOff, 0.f, 1000.f);
    bool glideOn = !th && enginePatch.glideEnabled && glideMs > 0.5f && lastNoteFreq > 0.f;
    if (glideOn && enginePatch.glideMode == GlideMode::LEGATO && !anyKeyHeldBefore) glideOn = false;
    if (!retrigger || th) {
        v.r.freqCur = glideOn ? lastNoteFreq : target;
    }
    v.r.freqTarget = target;
    v.r.glideCap = th ? thGlideCap() : 1.f;
    v.theremin = th;
    v.r.gate = true;
    for (int o = 0; o < NUM_OPS; o++) {
        const OpParams &p = enginePatch.op[o];
        float velF = 1.f - (p.velSens / 100.f) * (1.f - (float)vel / 127.f);
        float ks = 1.f;
        if (!algoIsCarrier(enginePatch.algorithm, o) && note > KEY_SCALE_REF_NOTE && p.keyScale > 0.f) {
            float oct = (float)(note - KEY_SCALE_REF_NOTE) / 12.f;
            ks = powf(0.5f, (p.keyScale / 100.f) * oct);
        }
        v.r.op[o].velKs = clampf(velF, 0.f, 1.f) * ks;
        opNoteOn(v.r.op[o], retrigger);
    }
    // Pitch envelope (v0.71): started right where the operator envelopes
    // are, so it restarts exactly when they do - and a legato note, which
    // never comes through here, keeps gliding without a blip (spec 5, 4.4).
    // Velocity scales Amt like VelSns, frozen at note-on.
    penvNoteOn(v.r, 1.f - (clampf(enginePatch.penvVel, 0.f, 100.f) / 100.f) * (1.f - (float)vel / 127.f));
    v.active = true;
    v.note = note; v.vel = vel;
    v.keyHeld = true; v.sustained = false;
    v.ignoreSustain = (flags & EVF_IGNORE_SUSTAIN) != 0;
    v.order = ++allocCounter;
    v.stealPending = false; v.stealFadeLeft = 0;
    if (!th) { lastNoteFreq = target; lastKeyVoice = (int)(&v - voices); }   // v0.85: the graphs follow it
}

static AUDIO_IRAM void releaseVoice(Voice &v) {
    v.keyHeld = false; v.sustained = false;
    v.r.gate = false;
    for (int o = 0; o < NUM_OPS; o++) opNoteOff(v.r.op[o]);
}

// A theremin note that is killed (silence, All Sound Off) stays "on" in
// thOn: it comes back only when the hand leaves and returns, so a patch
// that decays to silence under a held hand does not retrigger by itself.
static void killVoice(Voice &v) {
    v.active = false; v.keyHeld = false; v.sustained = false;
    v.stealPending = false; v.theremin = false;
    for (int o = 0; o < NUM_OPS; o++) opKill(v.r.op[o]);
}

// Voice allocation (spec 3.2)
static void noteOn(uint8_t note, uint8_t vel, uint8_t flags) {
    int limit = polyLimit();
    bool wasActive = anyKeyVoiceActive();   // v0.82: was anyVoiceActive() (the theremin counted)
    anyKeyHeldBefore = anyKeyHeld();
    syncMonoMode(limit == 1);

    // Mono: keep a stack of held keys so releasing the top returns to the
    // previous one (classic lead behaviour, pairs with Legato glide).
    if (limit == 1) {
        monoStackRemove(note);         // a key is on the stack once, at its newest position
        if (monoStackLen < 16) { monoStack[monoStackLen] = note; monoStackVel[monoStackLen] = vel; monoStackLen++; }
        Voice &v = voices[0];
        if (v.active && v.keyHeld && enginePatch.glideMode == GlideMode::LEGATO) {
            // Legato: no envelope retrigger, just a new pitch target
            v.note = note; v.vel = vel;
            v.r.freqTarget = midiNoteToFreq(note);
            if (!enginePatch.glideEnabled || enginePatch.glideMs <= 0.5f) v.r.freqCur = v.r.freqTarget;
            lastNoteFreq = v.r.freqTarget;
            return;
        }
        if (v.active) {
            v.stealPending = true; v.stealFadeLeft = VOICE_STEAL_FADE_SAMPLES;
            v.pendingNote = note; v.pendingVel = vel; v.pendingFlags = flags;
        } else {
            startVoice(v, note, vel, flags, false);
        }
        if (!wasActive) lfoDelayElapsedMs = 0.f;
        return;
    }

    // Rule 2: same note already sounding -> retrigger that voice
    for (int i = 0; i < limit; i++) {
        Voice &v = voices[i];
        if (v.active && !v.stealPending && v.note == note) {
            startVoice(v, note, vel, flags, true);
            return;
        }
    }
    // Rule 1: a free voice
    for (int i = 0; i < limit; i++) {
        if (!voices[i].active) {
            startVoice(voices[i], note, vel, flags, false);
            if (!wasActive) lfoDelayElapsedMs = 0.f;
            return;
        }
    }
    // Rule 3: the most-released voice (lowest OP1 envelope in RELEASE)
    int pick = -1; float bestEnv = 2.f;
    for (int i = 0; i < limit; i++) {
        Voice &v = voices[i];
        if (v.stealPending) continue;
        if (v.r.op[0].stage == EnvStage::RELEASE && v.r.op[0].env < bestEnv) { bestEnv = v.r.op[0].env; pick = i; }
    }
    // Rule 4: the oldest voice
    if (pick < 0) {
        uint32_t oldest = 0xFFFFFFFF;
        for (int i = 0; i < limit; i++) {
            if (voices[i].stealPending) continue;
            if (voices[i].order < oldest) { oldest = voices[i].order; pick = i; }
        }
    }
    if (pick < 0) pick = 0;
    Voice &v = voices[pick];
    v.stealPending = true; v.stealFadeLeft = VOICE_STEAL_FADE_SAMPLES;
    v.pendingNote = note; v.pendingVel = vel; v.pendingFlags = flags;
}

static AUDIO_IRAM void noteOff(uint8_t note, uint8_t flags) {
    int limit = polyLimit();
    syncMonoMode(limit == 1);
    if (limit == 1) {
        // Remove from the mono stack; if the sounding note was released and
        // another key is still held, fall back to it.
        monoStackRemove(note);
        Voice &v = voices[0];
        if (v.active && v.stealPending && v.pendingNote == note) { v.stealPending = false; return; }
        if (v.active && v.keyHeld && v.note == note) {
            if (monoStackLen > 0) {
                uint8_t back = monoStack[monoStackLen - 1];
                if (enginePatch.glideMode == GlideMode::LEGATO) {
                    v.note = back; v.r.freqTarget = midiNoteToFreq(back);
                    if (!enginePatch.glideEnabled || enginePatch.glideMs <= 0.5f) v.r.freqCur = v.r.freqTarget;
                    lastNoteFreq = v.r.freqTarget;
                } else {
                    startVoice(v, back, monoStackVel[monoStackLen - 1], flags, true);
                }
                return;
            }
            if (sustainDown && !v.ignoreSustain && !(flags & EVF_IGNORE_SUSTAIN)) { v.keyHeld = false; v.sustained = true; }
            else releaseVoice(v);
        }
        return;
    }
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice &v = voices[i];
        if (!v.active) continue;
        if (v.theremin || (v.stealPending && (v.pendingFlags & EVF_THEREMIN))) continue;   // not a key's (v0.81)
        // A note released before its stolen voice even started: cancel the
        // steal, otherwise that note would start later with no note-off.
        if (v.stealPending && v.pendingNote == note) { v.stealPending = false; continue; }
        if (!v.keyHeld || v.note != note) continue;
        if (sustainDown && !v.ignoreSustain && !(flags & EVF_IGNORE_SUSTAIN)) { v.keyHeld = false; v.sustained = true; }
        else releaseVoice(v);
    }
}

static void sustainOff() {
    sustainDown = false;
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice &v = voices[i];
        if (v.active && v.sustained) releaseVoice(v);
    }
}

static void handleEvent(const NoteEvent &e) {
    switch (e.type) {
        case EV_NOTE_ON:
            if (e.vel == 0) noteOff(e.note, e.flags); else noteOn(e.note, e.vel, e.flags);
            break;
        case EV_NOTE_OFF:     noteOff(e.note, e.flags); break;
        case EV_SUSTAIN_ON:   sustainDown = true; break;
        case EV_SUSTAIN_OFF:  sustainOff(); break;
        case EV_ALL_NOTES_OFF:
            // The keys' notes only: the theremin follows its hand (v0.81)
            monoStackLen = 0;
            for (int i = 0; i < MAX_VOICES; i++) {
                Voice &v = voices[i];
                if (!v.active || v.theremin || (v.stealPending && (v.pendingFlags & EVF_THEREMIN))) continue;
                v.stealPending = false; releaseVoice(v);
            }
            break;
        case EV_ALL_SOUND_OFF:
            monoStackLen = 0;
            for (int i = 0; i < MAX_VOICES; i++) killVoice(voices[i]);
            break;
        default: break;
    }
}

// Theremin Play (v0.81): the gate is a level, not an event, so a lost or
// late message can never leave the note hanging. Once per buffer.
static uint8_t hzToNote(float hz) {
    int n = (int)lroundf(69.f + 12.f * log2f(hz / 440.f));
    return (uint8_t)clampi(n, 0, 127);
}

static AUDIO_IRAM void thereminBuffer() {
    Voice &v = voices[TH_VOICE];
    bool want = live.thReserve && live.thGate;
    if (want && !thOn) {
        thGainNow = live.thVol;           // a new note starts at the volume of the moment
        if (!anyKeyVoiceActive()) lfoDelayElapsedMs = 0.f;   // alone: the LFO Delay starts with it (v0.82)
        uint8_t note = hzToNote(clampf(live.thHz, 8.f, 12000.f));
        if (v.active && !v.theremin) {
            // A key's note is still ringing out on that voice: the usual 2ms
            // steal fade, then the theremin's note
            if (!v.stealPending) { v.stealPending = true; v.stealFadeLeft = VOICE_STEAL_FADE_SAMPLES; }
            v.pendingNote = note; v.pendingVel = TH_VEL; v.pendingFlags = EVF_IGNORE_SUSTAIN | EVF_THEREMIN;
        } else {
            v.stealPending = false;
            startVoice(v, note, TH_VEL, EVF_IGNORE_SUSTAIN | EVF_THEREMIN, v.active);
        }
        thOn = true;
    } else if (!want && thOn) {
        thOn = false;
        if (v.stealPending && (v.pendingFlags & EVF_THEREMIN)) v.stealPending = false;   // never started
        else if (v.active && v.theremin) releaseVoice(v);
    }
    if (thOn && v.active && v.theremin) {
        v.r.freqTarget = clampf(live.thHz, 8.f, 12000.f);   // followed per block (glideCap)
        v.r.glideCap = thGlideCap();
        v.note = hzToNote(v.r.freqTarget);                   // the PLAY screen's voice row
    }
    thGainNext = clampf(live.thVol, 0.f, 1.f);
}

// ---------------------------------------------------------------------------
// LFO (one for all voices, evaluated once per buffer - spec 4.3)
// Saw / Square use the naive formula directly, never the audio tables.
// ---------------------------------------------------------------------------
static float lfoTick(float bufSeconds) {
    float rate = clampf(enginePatch.lfoRateHz, 0.1f, 20.f);
    lfoPhase += rate * bufSeconds;
    bool wrapped = false;
    while (lfoPhase >= 1.f) { lfoPhase -= 1.f; wrapped = true; }
    float v = 0.f;
    switch (enginePatch.lfoWave) {
        case LfoWave::SINE:     v = sinf(6.283185307f * lfoPhase); break;
        case LfoWave::TRIANGLE: v = (lfoPhase < 0.5f) ? (lfoPhase * 4.f - 1.f) : (3.f - lfoPhase * 4.f); break;
        case LfoWave::SAW:      v = lfoPhase * 2.f - 1.f; break;
        case LfoWave::SQUARE:   v = (lfoPhase < 0.5f) ? 1.f : -1.f; break;
        case LfoWave::SAMPLE_HOLD:
            if (wrapped || lfoShValue == 0.f) lfoShValue = ((float)(esp_random() & 0xFFFF) / 32767.5f) - 1.f;
            v = lfoShValue; break;
        default: break;
    }
    // Delay: depth ramps up linearly from zero over lfoDelayMs (spec 4.3)
    float scale = 1.f;
    if (enginePatch.lfoDelayMs > 0.5f) {
        lfoDelayElapsedMs += bufSeconds * 1000.f;
        scale = clampf(lfoDelayElapsedMs / enginePatch.lfoDelayMs, 0.f, 1.f);
    }
    return v * scale;
}

// ---------------------------------------------------------------------------
// Modulation resolution (spec 4.6) - once per buffer
// ---------------------------------------------------------------------------
struct BufferMod {
    float effMorph[NUM_OPS];
    float effGMorph;      // 0-100
    float effFeedback;    // 0-1
    float pitchMul;       // multiplier
    float volume;         // 0-1
    int   selOp;          // operator the OP_* targets act on, 0-3
};

// Offset at full deflection with Depth = 100%, per destination (spec B.6).
// IMU and LFO differ only on PITCH: a tilt is a whole gesture and earns an
// octave, while an LFO on pitch is vibrato and has to stay small.
static AUDIO_IRAM float modRange(ModTarget t, bool fromLfo) {
    switch (t) {
        case ModTarget::GMORPH:    return 50.f;                  // %
        case ModTarget::FEEDBACK:  return 50.f;                  // %
        case ModTarget::PITCH:     return fromLfo ? 2.f : 12.f;  // semitones
        case ModTarget::VOLUME:    return 50.f;                  // %
        case ModTarget::OP_MORPH:  return 50.f;                  // %
        case ModTarget::OP_LEVEL:  return 50.f;                  // %
        case ModTarget::OP_RATIO:  return 4.f;                   // ratio units
        case ModTarget::OP_DETUNE: return 50.f;                  // cents
        // FX (v0.44) - half the parameter's own span, like the others
        case ModTarget::CHORUS_DEPTH:  return 10.f;              // ms of 0-20
        case ModTarget::CHORUS_MIX:    return 50.f;              // %
        case ModTarget::DELAY_FDBK:    return 45.f;              // % of 0-90
        case ModTarget::DELAY_MIX:     return 50.f;              // %
        case ModTarget::REVERB_SIZE:   return 50.f;              // %
        case ModTarget::REVERB_MIX:    return 50.f;              // %
        case ModTarget::LIMITER_DRIVE: return 2.f;               // x of 1-5 (Drive amount, "DrAmt")
        case ModTarget::DRIVE_MIX:     return 50.f;              // % (v0.72)
        case ModTarget::PATCH_MORPH:   return 0.5f;              // of the 0-1 chain (v0.5)
        case ModTarget::CRUSH_BITS:    return 7.5f;              // bits of 1-16 (v0.55)
        case ModTarget::CRUSH_DOWN:    return 15.5f;             // x of 1-32
        case ModTarget::CRUSH_MIX:     return 50.f;              // %
        case ModTarget::GLITCH_PROB:   return fromLfo ? 1.f : 0.5f;  // 0-1; the LFO's negative half does nothing
        case ModTarget::PENV_AMT:      return PENV_AMT_MAX;          // semitones: half of -48..+48 (v0.71)
        default:                   return 0.f;
    }
}

// The IMU rests at a neutral position, so an offset from tilt has to know
// whether its destination rests at the CENTRE of its range or at an END.
// Pitch rests in the middle and wants both directions. GMorph and Feedback
// normally sit at 0 in a patch, and a signed offset there leaves half the
// travel doing nothing at all - which is exactly how "tilting changes
// nothing" happens. Those take the MAGNITUDE of the tilt instead and sweep
// upward whichever way the device leans (v0.32).
//
// This applies to the IMU only. The LFO stays signed everywhere: rectifying
// an LFO would double its frequency instead of centring it.
static AUDIO_IRAM bool imuUnipolar(ModTarget t) {
    switch (t) {
        case ModTarget::GMORPH:
        case ModTarget::FEEDBACK:
        case ModTarget::OP_MORPH:
        case ModTarget::GLIDE_TIME:
        case ModTarget::PATCH_MORPH:     // rests at its base, usually 0 (v0.5)
        case ModTarget::GLITCH_PROB:     // 0 = no added probability (v0.6)
            return true;
        default:
            return false;
    }
}

// Ratio and detune move the phase increment, so they follow more slowly:
// a fast tilt must not walk the increment in audible steps (spec B.8).
static float modSmoothing(ModTarget t) {
    if (t == ModTarget::OP_RATIO || t == ModTarget::OP_DETUNE) return 0.15f;
    return SM_BUF;
}

constexpr int MOD_COUNT = (int)ModTarget::MOD_TARGET_COUNT;
static float smOff[MOD_COUNT] = {0.f};
static float smCcVolMul = 1.f;    // knob-slot Volume gain, smoothed like the offsets
static float ccMorphAbs = -1.f;   // knob-slot PatchMorph position 0..1, -1 when none has sent
static float ccGlitchAbs = -1.f;  // knob-slot GlitchProb 0..1, -1 when none has sent (v0.6)

// What the engine plays (v0.6): livePatch plus a running GLITCH Chaos event.
// Copied fresh every buffer, so Chaos never reaches livePatch - whose fields
// are also the morph smoothing state - and is gone the buffer after it ends.
Patch enginePatch;
static int   lastSelectedOp = 0;

static inline AUDIO_IRAM void addMod(float *raw, ModTarget t, float amount) {
    int i = (int)t;
    if (i > 0 && i < MOD_COUNT) raw[i] += amount;
}

static void updateModulation(BufferMod &m, float bufSeconds) {
    float raw[MOD_COUNT];
    for (int i = 0; i < MOD_COUNT; i++) raw[i] = 0.f;

    // The OP_* destinations follow whichever operator the OP tab has
    // selected. Nothing here ever writes back into `patch` - every offset
    // is rebuilt from zero each buffer and applied to a copy - so when the
    // selection moves, the operator left behind returns to its own stored
    // value by construction and can never freeze holding an offset
    // (spec B.7). The accumulators are zeroed so the offset fades onto the
    // NEW operator instead of landing on it in one step.
    int sel = clampi((int)selectedOp, 0, NUM_OPS - 1);
    if (sel != lastSelectedOp) {
        lastSelectedOp = sel;
        smOff[(int)ModTarget::OP_MORPH]  = 0.f;
        smOff[(int)ModTarget::OP_LEVEL]  = 0.f;
        smOff[(int)ModTarget::OP_RATIO]  = 0.f;
        smOff[(int)ModTarget::OP_DETUNE] = 0.f;
    }
    m.selOp = sel;

    // 1. LFO
    lfoValue = lfoTick(bufSeconds);
    float lfoDepth = clampf(enginePatch.lfoDepth, 0.f, 100.f);
    bool lfoLive = lfoDepth > 0.f && enginePatch.lfoTarget != ModTarget::OFF;
    if (lfoLive) addMod(raw, enginePatch.lfoTarget, lfoValue * (lfoDepth / 100.f) * modRange(enginePatch.lfoTarget, true));

    // 2. IMU (already enable/hold-resolved in loop(); 0 when inactive).
    // Every destination is bipolar: level means no offset, and the two tilt
    // directions add and subtract (spec B.5).
    float ix = live.imuXNorm, iy = live.imuYNorm;
    bool imuLive = false;
    if (enginePatch.imuXTarget != ModTarget::OFF && enginePatch.imuXDepth > 0.f) {
        imuLive = true;
        ModTarget t = enginePatch.imuXTarget;
        bool uni = imuUnipolar(t);
        // A one-directional destination gets twice the span, so full
        // deflection still covers its whole range from a base of 0.
        float n = uni ? fabsf(ix) : ix;
        addMod(raw, t, n * (enginePatch.imuXDepth / 100.f) * modRange(t, false) * (uni ? 2.f : 1.f));
    }
    if (enginePatch.imuYTarget != ModTarget::OFF && enginePatch.imuYDepth > 0.f) {
        imuLive = true;
        ModTarget t = enginePatch.imuYTarget;
        bool uni = imuUnipolar(t);
        float n = uni ? fabsf(iy) : iy;
        addMod(raw, t, n * (enginePatch.imuYDepth / 100.f) * modRange(t, false) * (uni ? 2.f : 1.f));
    }
    // 2b. Theremin Mod (v0.81): one more source on the IMU's rules - the
    // distance reads like a tilt to one side (near = full), a one-directional
    // destination gets the doubled span. 0 whenever Mod is not running.
    // Target / Depth are device settings, not in the patch.
    {
        float tn = live.thModNorm;
        ModTarget t = sys.thTarget;
        if (tn > 0.f && t != ModTarget::OFF && sys.thDepth > 0.f) {
            imuLive = true;
            bool uni = imuUnipolar(t);
            addMod(raw, t, tn * (sys.thDepth / 100.f) * modRange(t, false) * (uni ? 2.f : 1.f));
        }
    }

    // 3. MIDI CC. Wheels rest at zero rather than at a centre, so these
    // stay unipolar. The two free slots keep the original four
    // destinations (spec D).
    raw[(int)ModTarget::GMORPH]   += live.ccMorphNorm * 100.f;
    raw[(int)ModTarget::FEEDBACK] += live.ccFeedbackNorm * 100.f;
    // A knob on Volume is a fader, like CC#7: its position scales the patch
    // level (0 = silent at Depth 100, 127 = the patch level) instead of
    // adding to it. Added to a patch sitting at 80 it could only reach 100,
    // about +2dB, and could never turn anything down (v0.34). Until the
    // knob's CC first arrives the slot leaves the level alone.
    float ccVolMul = 1.f;
    float ccMorph = -1.f;
    float ccGlitch = -1.f;
    for (int i = 0; i < NUM_CC_SLOTS; i++) {
        ModTarget t = sys.ccSlot[i].target;
        if (!modTargetIsAudio(t) || t == ModTarget::OFF) continue;   // control ones are written directly, in midi.cpp
        float cn = live.ccSlotNorm[i];
        if (cn < 0.f) continue;                                       // not heard yet
        float d = (float)clampi(sys.ccSlot[i].depth, 0, 100) / 100.f;
        if (t == ModTarget::VOLUME) { ccVolMul *= 1.f - d + d * cn; continue; }
        // PatchMorph is a performance position, not a patch value: the knob
        // position IS the position, like the control targets (v0.5).
        if (t == ModTarget::PATCH_MORPH) { ccMorph = cn * d; continue; }
        if (t == ModTarget::GLITCH_PROB) { ccGlitch = cn * d; continue; }   // absolute, like PtchMrph (v0.6)
        float n = cn * d;
        // An FX knob sweeps the parameter's whole span in its own units
        if (modTargetIsFx(t)) { addMod(raw, t, n * modRange(t, false) * 2.f); continue; }
        // Pitch envelope amount is bipolar: centre = no change (v0.71)
        if (t == ModTarget::PENV_AMT) { addMod(raw, t, (n * 2.f - 1.f) * PENV_AMT_MAX); continue; }
        addMod(raw, t, (t == ModTarget::PITCH) ? (n * 2.f - 1.f) * 2.f : n * 100.f);
    }
    ccMorphAbs = ccMorph;
    ccGlitchAbs = ccGlitch;
    raw[(int)ModTarget::PITCH] += live.pitchBendNorm * (float)clampi(sys.bendRange, 1, 12);

    // 4. Smooth at buffer rate, snapping once close (a plain exponential
    // never quite arrives, which matters for anything read as an index).
    for (int i = 1; i < MOD_COUNT; i++) {
        smOff[i] += (raw[i] - smOff[i]) * modSmoothing((ModTarget)i);
        if (fabsf(raw[i] - smOff[i]) < 0.001f) smOff[i] = raw[i];
    }
    smCcVolMul += (ccVolMul - smCcVolMul) * modSmoothing(ModTarget::VOLUME);
    if (fabsf(ccVolMul - smCcVolMul) < 0.001f) smCcVolMul = ccVolMul;

    // 5. Resolve the effective values this buffer will use
    m.effGMorph = clampf(enginePatch.globalMorph + smOff[(int)ModTarget::GMORPH], 0.f, 100.f);
    for (int o = 0; o < NUM_OPS; o++) {
        float mo = enginePatch.op[o].morph + m.effGMorph;
        if (o == sel) mo += smOff[(int)ModTarget::OP_MORPH];
        m.effMorph[o] = clampf(mo, 0.f, 100.f);
    }
    m.effFeedback = clampf(enginePatch.feedback + smOff[(int)ModTarget::FEEDBACK], 0.f, 100.f) / 100.f;
    m.pitchMul = powf(2.f, smOff[(int)ModTarget::PITCH] / 12.f);
    m.volume = clampf(enginePatch.volume + smOff[(int)ModTarget::VOLUME], 0.f, 100.f) / 100.f
             * smCcVolMul * live.ccVolumeNorm * live.ccExpressionNorm
             * (float)clampi(sys.masterVolume, 0, 100) / 100.f;   // K / L keys (v0.11)

    // 6. Morphed tables for this buffer
    buildMorphTables(m.effMorph);

    audioStatus.effGlobalMorph = m.effGMorph;
    audioStatus.effFeedback = m.effFeedback * 100.f;
    audioStatus.effPitchSemi = smOff[(int)ModTarget::PITCH];
    audioStatus.lfoValue = lfoValue;
    audioStatus.lfoPhase = lfoPhase;
    audioStatus.lfoModulating = lfoLive;
    audioStatus.imuModulating = imuLive;
}

static void fillRenderContext(RenderContext &ctx, const BufferMod &m) {
    static float prevRatioMul[NUM_OPS] = {1.f, 1.f, 1.f, 1.f};
    static bool  ratioPrimed = false;
    static bool  prevFixed[NUM_OPS] = {false, false, false, false};
    static float fixNoteCache[NUM_OPS] = {9999.f, 9999.f, 9999.f, 9999.f};
    static float fixIncCache[NUM_OPS]  = {0.f, 0.f, 0.f, 0.f};
    ctx.algorithm = clampi(enginePatch.algorithm, 1, NUM_ALGORITHMS);
    ctx.feedback = m.effFeedback;
    const int blocks = AUDIO_BUFFER_SIZE / ENV_BLOCK;
    int carriers = 0;
    ctx.fixedMask = 0;
    // Pitch envelope (v0.71): Amt with its modulation, read again every
    // buffer so a tilt moves the fall of a note that is already sounding
    ctx.penvAmt  = clampf(enginePatch.penvAmt + smOff[(int)ModTarget::PENV_AMT], -PENV_AMT_MAX, PENV_AMT_MAX);
    ctx.penvRate = envTimeToRate(enginePatch.penvDecay, 8.f);   // the operator Decay curve and time
    for (int o = 0; o < NUM_OPS; o++) {
        const OpParams &p = enginePatch.op[o];
        bool sel = (o == m.selOp);
        ctx.table[o] = morphTable(o);
        ctx.opLevel[o] = clampf(p.level, 0.f, 100.f) / 100.f;
        ctx.opLevelAdd[o] = sel ? smOff[(int)ModTarget::OP_LEVEL] / 100.f : 0.f;
        // An operator silent in the patch still has to run while a positive
        // level offset is reaching it.
        ctx.opOn[o] = (ctx.opLevel[o] > 0.0005f) || (ctx.opLevelAdd[o] > 0.0005f);
        float detune = clampf(p.detune + (sel ? smOff[(int)ModTarget::OP_DETUNE] : 0.f), -50.f, 50.f);
        if (fabsf(detune - detuneCache[o]) > 0.001f) {
            detuneCache[o] = detune;
            detuneMul[o] = powf(2.f, detune / 1200.f);
        }
        float ratioOff = sel ? smOff[(int)ModTarget::OP_RATIO] : 0.f;
        float target;
        if (p.fixed) {
            // Fixed frequency (v0.71): the whole increment, from FixNote. One
            // powf only when FixNote moves (a morph), never per sample.
            // OP_RATIO modulation scales it as a factor around 1.
            if (fabsf(p.fixNote - fixNoteCache[o]) > 0.0001f) {
                fixNoteCache[o] = p.fixNote;
                fixIncCache[o] = 440.f * powf(2.f, (p.fixNote - 69.f) / 12.f) / (float)SAMPLE_RATE;
            }
            target = fixIncCache[o] * clampf(1.f + ratioOff, 0.25f, 16.f) * detuneMul[o];
            ctx.fixedMask |= (uint8_t)(1u << o);
        } else {
            float ratio = clampf(p.ratio + ratioOff, 0.25f, 16.f);
            target = ratio * detuneMul[o];
        }
        // Ratio and fixed increments are different units: a mode change
        // jumps instead of sweeping one into the other across the buffer.
        if (!ratioPrimed || p.fixed != prevFixed[o]) prevRatioMul[o] = target;
        prevFixed[o] = p.fixed;
        ctx.ratioMulStart[o] = prevRatioMul[o];
        ctx.ratioMulStep[o]  = (target - prevRatioMul[o]) / (float)blocks;
        prevRatioMul[o] = target;
        ctx.attackRate[o]  = envTimeToRate(p.attack, 3.f);
        ctx.decayRate[o]   = envTimeToRate(p.decay, 8.f);
        ctx.releaseRate[o] = envTimeToRate(p.release, 8.f);
        ctx.sustain[o]     = clampf(p.sustain, 0.f, 100.f) / 100.f;
        if (algoIsCarrier(ctx.algorithm, o)) carriers++;
    }
    ratioPrimed = true;
    ctx.carrierScale = (carriers <= 1) ? 1.f : (carriers == 2 ? 0.7071f : (carriers == 3 ? 0.5774f : 0.5f));
    // Pitch: interpolate from the previous buffer's multiplier to this one
    ctx.pitchMulStart = prevPitchMul;
    ctx.pitchMulStep = (m.pitchMul - prevPitchMul) / (float)blocks;
    prevPitchMul = m.pitchMul;
    // Glide: exponential; ~95% of the way in glideMs
    float glideMs = clampf(enginePatch.glideMs + ctrlMod.glideOff, 0.f, 1000.f);
    if (!enginePatch.glideEnabled || glideMs <= 0.5f) ctx.glideCoef = 1.f;
    else {
        float tau = glideMs / 3000.f;
        ctx.glideCoef = 1.f - expf(-((float)ENV_BLOCK / SAMPLE_RATE) / tau);
    }
}

// Starts a voice's interpolated values at the point in the buffer where
// its rendering begins (blockOffset > 0 only after a steal fade).
static inline void primeVoiceRamp(VoiceRender &v, const RenderContext &ctx, int blockOffset) {
    v.pitchMul = ctx.pitchMulStart + ctx.pitchMulStep * (float)blockOffset;
    for (int o = 0; o < NUM_OPS; o++) v.ratioNow[o] = ctx.ratioMulStart[o] + ctx.ratioMulStep[o] * (float)blockOffset;
}

// One output stage for BOTH the normal path and the FX-tail path.
//
// These used to be two separate loops, and the tail one was missing the
// MIX_HEADROOM factor: at the instant the last voice reached IDLE and the
// tail took over, everything jumped 6dB louder while the load fell to a
// third (the tail renders no voices). Sharing the stage makes that class
// of mismatch impossible (v0.2).
constexpr float MIX_HEADROOM = 0.5f;   // voices sum; the limiter takes the rest

static int16_t scopeBuf[SCOPE_POINTS];
static volatile bool scopeFull = false;

// One-slot mailbox: audioTask only writes while the flag is clear and the
// UI only reads while it is set, so neither can catch the other mid-copy.
static void scopeCapture(const int16_t *out, int n) {
    if (scopeFull) return;
    int limit = n - SCOPE_POINTS;
    if (limit < 1) limit = 1;
    int start = 0;
    for (int i = 0; i < limit; i++) {
        if (out[i] < 0 && out[i + 1] >= 0) { start = i; break; }   // rising zero crossing
    }
    for (int i = 0; i < SCOPE_POINTS; i++) scopeBuf[i] = out[start + i];
    scopeFull = true;
}

bool scopeTake(int16_t *dst) {
    if (!scopeFull) return false;
    memcpy(dst, scopeBuf, sizeof(scopeBuf));
    scopeFull = false;
    return true;
}

// FX destinations (v0.44). Applied to the per-buffer copy of the FX
// parameters, never to `patch`. An effect whose Mix is 0 in the patch stays
// OFF (as in C.P.S.): modulation never switches an effect on, so an unused
// effect keeps costing nothing, and a modulated Mix bottoms out at 1% rather
// than flicking the effect off and on.
static void applyFxMod(FxParams &f) {
    const float *o = smOff;
    if (f.chorusMix > 0.f) {
        f.chorusDepthMs = clampf(enginePatch.chorusDepthMs + o[(int)ModTarget::CHORUS_DEPTH], 0.f, 20.f);
        f.chorusMix     = clampf(enginePatch.chorusMix + o[(int)ModTarget::CHORUS_MIX], 1.f, 100.f) / 100.f;
    }
    if (f.delayMix > 0.f) {
        f.delayFeedback = clampf(enginePatch.delayFeedback + o[(int)ModTarget::DELAY_FDBK], 0.f, 90.f) / 100.f;
        f.delayMix      = clampf(enginePatch.delayMix + o[(int)ModTarget::DELAY_MIX], 1.f, 100.f) / 100.f;
    }
    if (f.reverbMix > 0.f) {
        f.reverbSize = clampf(enginePatch.reverbSize + o[(int)ModTarget::REVERB_SIZE], 0.f, 100.f) / 100.f;
        f.reverbMix  = clampf(enginePatch.reverbMix + o[(int)ModTarget::REVERB_MIX], 1.f, 100.f) / 100.f;
    }
    if (f.limiterMix > 0.f) {
        f.limiterDrive = clampf(enginePatch.limiterDrive + o[(int)ModTarget::LIMITER_DRIVE], 1.f, 5.f);
        f.limiterMix   = clampf(enginePatch.limiterMix + o[(int)ModTarget::DRIVE_MIX], 1.f, 100.f) / 100.f;   // v0.72
    }
    if (f.crushMix > 0.f) {
        // Bits / Down move the per-buffer constants; the exp2f stays here,
        // outside the sample loop.
        float bits = clampf(enginePatch.crushBits + o[(int)ModTarget::CRUSH_BITS], 1.f, 16.f);
        f.crushScale = exp2f(bits - 1.f);
        f.crushInvScale = 1.f / f.crushScale;
        f.crushStep = 1.f / clampf(enginePatch.crushDown + o[(int)ModTarget::CRUSH_DOWN], 1.f, 32.f);
        f.crushMix = clampf(enginePatch.crushMix + o[(int)ModTarget::CRUSH_MIX], 1.f, 100.f) / 100.f;
    }
}

// v0.72: volume -> output limiter (gain only, look-ahead) -> the int16 clamp,
// which only catches what the limiter could not. Since v0.77 the three run
// as one pass in fxOutputStage() (fx.cpp, IRAM). Both the normal and the
// tail path come through here (the v0.13 +6dB lesson). The scope capture is
// in the callers so the output path calls nothing in flash.
static inline int writeOutput(int16_t *out, float *mix, int n, float gain) {
    return fxOutputStage(out, mix, n, gain * MIX_HEADROOM * 32767.f);
}

// ---------------------------------------------------------------------------
// Diagnostics: accumulate here, print from loop() (spec 11.1)
// ---------------------------------------------------------------------------
static AUDIO_IRAM void diagRecordBuffer(unsigned long dspUs, bool tail) {
    // Cumulative since boot or since the last reset from the Info screen.
    // Measured before playRaw(), so this is DSP cost and not playback rate.
    if ((uint32_t)dspUs > dspMaxUs) dspMaxUs = (uint32_t)dspUs;
    if (dspUs > AUDIO_BUDGET_US) dspOverBudgetCount++;
    diagCount++; diagSumUs += dspUs;
    if (dspUs > diagMaxUs) diagMaxUs = dspUs;
    if (dspUs > AUDIO_BUDGET_US) diagOverCount++;
    if (tail) diagTailBufs++;
    audioStatus.lastDspUs = dspUs;
    unsigned long nowMs = millis();
    if (diagWindowStartMs == 0) diagWindowStartMs = nowMs;
    if (nowMs - diagWindowStartMs >= 1000 && !diag.pending) {
        diag.count = diagCount; diag.sumUs = diagSumUs; diag.maxUs = diagMaxUs;
        diag.overCount = diagOverCount; diag.tailBufs = diagTailBufs;
        diag.pending = true;
        diagWindowStartMs = nowMs;
        diagCount = 0; diagSumUs = 0; diagMaxUs = 0; diagOverCount = 0; diagTailBufs = 0;
    }
}

// ---------------------------------------------------------------------------
// The task
// ---------------------------------------------------------------------------
void AUDIO_IRAM audioTask(void *pvParameters) {
    glitchInit();
    (void)pvParameters;
    const int BUF = AUDIO_BUFFER_SIZE;
    const float bufSeconds = (float)BUF / SAMPLE_RATE;
    int bi = 0;
    uint8_t yieldCounter = 0;
    RenderContext ctx;
    BufferMod mod;
    FxParams fx;
    memset(&ctx, 0, sizeof(ctx));
    memset(&mod, 0, sizeof(mod));

    while (true) {
        unsigned long bufStartUs = micros();
        audioStatus.heartbeatUs = bufStartUs;
        int16_t *out = outBufs[bi]; bi = (bi + 1) % AUDIO_OUT_BUFS;

        // 0. The patch this buffer plays (v0.5): a copy of `patch`, or the
        // morph chain at the current position. The position uses the
        // previous buffer's offsets (the LFO that moves it reads this very
        // patch), which is one buffer - 23ms - of latency.
        {
            float pos = (ccMorphAbs >= 0.f) ? ccMorphAbs : (float)clampi(sys.morphPosBase, 0, 100) / 100.f;
            pos = clampf(pos + smOff[(int)ModTarget::PATCH_MORPH], 0.f, 1.f);
            audioStatus.morphPos = pos;
            morphBuildLivePatch(livePatch, pos);
            audioStatus.bufSeq = audioStatus.bufSeq + 1;
        }
        // 0b. GLITCH (v0.6): the engine's own copy, then a running Chaos event
        // written onto it. Before the note events, so a note started under
        // Chaos uses the thrown values like everything else this buffer.
        enginePatch = livePatch;
        {
            float gp = (ccGlitchAbs >= 0.f) ? ccGlitchAbs : 0.f;
            gp = clampf(gp + smOff[(int)ModTarget::GLITCH_PROB], 0.f, 1.f);
            glitchBeginBuffer(BUF, anyVoiceActive(), gp, enginePatch);
        }

        // 1. Events from loop()
        NoteEvent e;
        while (popNoteEvent(e)) handleEvent(e);
        if (fxClearRequest) {
            fxClearRequest = false;
            clearFxBuffers();
            fxTailPending = false; fxTailQuietBufs = 0; fxTailBufCount = 0;
        }

        thereminBuffer();                        // v0.81

        // Voice count reduced while notes sound: release the extras
        {
            int limit = polyLimit();
            for (int i = limit; i < MAX_VOICES; i++)
                if (voices[i].active && voices[i].keyHeld && !voices[i].theremin) releaseVoice(voices[i]);
        }

        // 2. Modulation, tables, FX parameters - once per buffer
        updateModulation(mod, bufSeconds);
        fillRenderContext(ctx, mod);
        fxParamsFromPatch(fx);
        applyFxMod(fx);
        fxApplyForce(fx, fxForceEffective());    // temporary On from a key (v0.57)
        fx.bypass = fxBypassEffective();         // after morph and modulation (v0.56)
        // A reverb coming back on must not replay what its frozen lines held
        // when it went off - a morph or a pad can raise Mix from 0 (v0.5).
        {
            static float prevReverbMix = 0.f;
            if (prevReverbMix <= 0.f && fx.reverbMix > 0.f) clearReverbBuffers();
            prevReverbMix = fx.reverbMix;
        }

        // 3. Idle / FX tail path (spec 6.5)
        bool active = anyVoiceActive();
        if (!active) {
            audioStatus.activeVoices = 0;
            audioStatus.probeStage = 0; audioStatus.probePenvPos = -1.f;   // no dot (v0.85)
            for (int i = 0; i < MAX_VOICES; i++) voiceActiveMirror[i] = false;
            if (fxTailPending) {
                if (fxHasTail(fx)) {
                    float fade = 1.f;
                    int remaining = FX_TAIL_MAX_BUFS - fxTailBufCount;
                    if (remaining < FX_TAIL_FADE_BUFS) fade = (remaining > 0) ? ((float)remaining / FX_TAIL_FADE_BUFS) : 0.f;
                    memset(mixBuf, 0, sizeof(mixBuf));
                    fxCrushBlock(mixBuf, BUF, fx);       // same chain as the normal path
                    fxProcessBlock(mixBuf, BUF, fx);
                    glitchRenderBuffer(mixBuf, BUF);     // an event already running finishes
                    fxLimiterBlock(mixBuf, BUF, fx);     // Drive
                    int peak = writeOutput(out, mixBuf, BUF, mod.volume * fade);
                    scopeCapture(out, BUF);
                    diagRecordBuffer(micros() - bufStartUs, true);
                    M5Cardputer.Speaker.playRaw(out, BUF, SAMPLE_RATE, false, 1, 0, false);
                    fxTailBufCount++;
                    // Delay: the line's read point is still inside the silent
                    // stretch right after a note stops, so require enough
                    // quiet buffers to cover a full delay time (C.P.S. v0.9964).
                    int quietNeeded = FX_TAIL_QUIET_BUFS;
                    if (fx.delayMix > 0.f) {
                        int delayBufs = (int)(fx.delayTimeMs / 1000.f * SAMPLE_RATE / BUF) + 2;
                        if (delayBufs > quietNeeded) quietNeeded = delayBufs;
                    }
                    if (peak < FX_TAIL_SILENCE_PEAK) fxTailQuietBufs++; else fxTailQuietBufs = 0;
                    if (fxTailQuietBufs >= quietNeeded || fxTailBufCount >= FX_TAIL_MAX_BUFS) {
                        fxTailPending = false; fxTailQuietBufs = 0; fxTailBufCount = 0;
                        clearFxBuffers();
                    }
                    if (++yieldCounter >= AUDIO_YIELD_EVERY_BUFS) { yieldCounter = 0; vTaskDelay(1); }
                    continue;
                }
                // No time-based effect on: nothing rings out, but the lines
                // still hold the last note. Clear now while nothing sounds.
                fxTailPending = false; fxTailQuietBufs = 0; fxTailBufCount = 0;
                clearFxBuffers();
            }
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        }
        fxTailPending = true; fxTailQuietBufs = 0; fxTailBufCount = 0;

        // 4. Render voices
        memset(mixBuf, 0, sizeof(mixBuf));
        int sounding = 0;
        for (int i = 0; i < MAX_VOICES; i++) {
            Voice &v = voices[i];
            if (!v.active) { voiceActiveMirror[i] = false; continue; }
            primeVoiceRamp(v.r, ctx, 0);
            int start = 0;
            if (v.stealPending) {
                // 2ms fade-out of the old note, then re-init in the same buffer
                int fadeN = v.stealFadeLeft; if (fadeN > BUF) fadeN = BUF;
                if (fadeN > 0) renderVoice(v.r, ctx, mixBuf, fadeN, 1.f, -1.f / (float)fadeN);
                start = fadeN;
                v.stealPending = false;
                startVoice(v, v.pendingNote, v.pendingVel, v.pendingFlags, false);
                primeVoiceRamp(v.r, ctx, start / ENV_BLOCK);
            }
            // The theremin's VolAxis gain, ramped across the buffer (v0.81)
            float g0 = 1.f, gs = 0.f;
            if (v.theremin && start < BUF) { g0 = thGainNow; gs = (thGainNext - thGainNow) / (float)(BUF - start); }
            if (start < BUF) renderVoice(v.r, ctx, mixBuf + start, BUF - start, g0, gs);
            if (voiceIsSilent(v.r, ctx.algorithm)) { killVoice(v); voiceActiveMirror[i] = false; }
            else { sounding++; voiceNoteMirror[i] = v.note; voiceActiveMirror[i] = true; }
        }
        audioStatus.activeVoices = sounding;
        thGainNow = thGainNext;
        // The dot on the envelope graphs (v0.85): a few stores per buffer
        {
            const Voice *pv = (lastKeyVoice >= 0) ? &voices[lastKeyVoice] : nullptr;
            if (pv && pv->active && !pv->theremin) {
                const OpState &o = pv->r.op[clampi((int)selectedOp, 0, NUM_OPS - 1)];
                audioStatus.probePos = o.stagePos;
                audioStatus.probeEnv = o.env;
                audioStatus.probePenvPos = pv->r.penvOn ? pv->r.penvPos : -1.f;
                audioStatus.probePenvVelK = pv->r.penvVelK;
                audioStatus.probeStage = (uint8_t)o.stage;
            } else {
                audioStatus.probeStage = 0; audioStatus.probePenvPos = -1.f;
            }
        }

        // 5. Mix bus: FX -> Drive -> Volume -> output limiter -> int16
        fxCrushBlock(mixBuf, BUF, fx);           // Crush first (v0.55)
        fxProcessBlock(mixBuf, BUF, fx);
        glitchRenderBuffer(mixBuf, BUF);         // after Reverb, before Drive (v0.6)
        fxLimiterBlock(mixBuf, BUF, fx);         // Drive (named Limiter before v0.72)
        writeOutput(out, mixBuf, BUF, mod.volume);
        scopeCapture(out, BUF);
        diagRecordBuffer(micros() - bufStartUs, false);
        M5Cardputer.Speaker.playRaw(out, BUF, SAMPLE_RATE, false, 1, 0, false);
        if (++yieldCounter >= AUDIO_YIELD_EVERY_BUFS) { yieldCounter = 0; vTaskDelay(1); }
    }
}
