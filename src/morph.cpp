// MORPHASE - morph.cpp
// Patch morph (v0.5, glide v0.51). See morph.h for the layer structure.
//
// Threading (update spec 8.2). The slot patches are written by loop() and
// read by audioTask, so they are never written while audioTask may read
// them: loop() clears `chainReady`, waits until audioTask has started a new
// buffer (it samples the flag once, at the top), rewrites the slots and sets
// the flag again. A glide is driven the same way - loop() only posts a
// request, a cancel or a release; audioTask alone owns the glide state. No
// lock, and no window in which both touch the same data.

#include <Arduino.h>
#include <SD.h>
#include "morph.h"
#include "patch.h"
#include "ui_common.h"

Patch livePatch;                     // what the engine plays - written by audioTask only

#define MORPH_CHAIN_PATH MORPH_CHAIN_FILE         // config.h: one patch name per line, blank = Off

static char   slotName[MORPH_SLOTS][PATCH_NAME_LEN + 1];
static Patch  chainPatches[MORPH_SLOTS];         // ~2.4KB, .bss (update spec 7.2)
static volatile bool     slotLoaded[MORPH_SLOTS];
static volatile int      chainLen = 0;
static volatile bool     chainReady = false;
static volatile uint32_t chainGen = 0;           // bumps on every reload: audioTask resets its state

// Glide (v0.51). loop() -> audioTask requests:
static volatile int  glideReq = -1;              // slot to glide to
static volatile bool glideCancel = false;
static volatile bool glideRelease = false;       // the arrival has been committed to `patch`
// audioTask -> loop() state:
enum GlidePhase : int { GP_IDLE = 0, GP_MOVING, GP_ARRIVED, GP_SETTLED };
static volatile int  glidePhase = GP_IDLE;
static volatile int  glideSlot = -1;

// Chain pause (v0.52), loop() side. audioTask only reads the flag.
static volatile bool chainPaused = false;
static int   pauseBase = 0;              // the position inputs when the pause began
static float pauseCc = -1.f, pauseImuX = 0.f, pauseImuY = 0.f, pauseTh = 0.f;

static char          noticeBuf[40];
static volatile bool noticePending = false;

static void notify(const char *msg) {
    strncpy(noticeBuf, msg, sizeof(noticeBuf) - 1); noticeBuf[sizeof(noticeBuf) - 1] = 0;
    noticePending = true;
    uiDirty = true;
}

// ---------------------------------------------------------------------------
// loop() side
// ---------------------------------------------------------------------------
bool morphChainRunning() { return sys.morphEnabled && !chainPaused && chainReady && chainLen >= 2; }
bool morphChainPaused()  { return sys.morphEnabled && chainPaused; }
bool morphGliding()      { return glideReq >= 0 || glidePhase != GP_IDLE; }
bool morphRunning()      { return morphChainRunning() || morphGliding(); }

// The manual position inputs, read the way the engine reads them
static float ccMorphInput() {
    float v = -1.f;
    for (int i = 0; i < NUM_CC_SLOTS; i++)
        if (sys.ccSlot[i].target == ModTarget::PATCH_MORPH && live.ccSlotNorm[i] >= 0.f) v = live.ccSlotNorm[i];
    return v;
}
static float imuMorphInput(bool x) {
    ModTarget t = x ? patch.imuXTarget : patch.imuYTarget;
    return (t == ModTarget::PATCH_MORPH) ? fabsf(x ? live.imuXNorm : live.imuYNorm) : 0.f;
}
static float thMorphInput() {             // the theremin in Mod (v0.81)
    return (sys.thTarget == ModTarget::PATCH_MORPH) ? live.thModNorm : 0.f;
}

static void pauseChain() {
    if (!sys.morphEnabled || chainPaused) return;
    pauseBase = sys.morphPosBase;
    pauseCc = ccMorphInput();
    pauseImuX = imuMorphInput(true); pauseImuY = imuMorphInput(false); pauseTh = thMorphInput();
    chainPaused = true;
}

// Resume when a hand moves the position; small jitter does not count.
static void checkChainResume() {
    if (!sys.morphEnabled || !chainPaused) return;
    float cc = ccMorphInput();
    bool moved = sys.morphPosBase != pauseBase ||
                 (cc >= 0.f && (pauseCc < 0.f || fabsf(cc - pauseCc) > 2.f / 127.f)) ||
                 fabsf(imuMorphInput(true) - pauseImuX) > 0.08f ||
                 fabsf(thMorphInput() - pauseTh) > 0.08f ||
                 fabsf(imuMorphInput(false) - pauseImuY) > 0.08f;
    if (moved) { chainPaused = false; notify("Chain resumed"); }
}

static void cancelGlide() {
    if (!morphGliding()) return;
    glideReq = -1;
    glideCancel = true;
}

static void suspendChain() {
    if (!chainReady) return;
    chainReady = false;
    // One buffer boundary is enough: a buffer that read the flag as set
    // finishes its build before the counter moves.
    uint32_t seq = audioStatus.bufSeq;
    unsigned long t0 = millis();
    while (audioStatus.bufSeq == seq && millis() - t0 < 200) vTaskDelay(1);
}

static void loadChain() {
    if (morphGliding()) morphCommitAndStop();    // a glide reads the slots: end it on what is heard
    suspendChain();
    int len = 0;
    bool run = true;
    for (int i = 0; i < MORPH_SLOTS; i++) {
        slotLoaded[i] = slotName[i][0] && loadPatchInto(slotName[i], chainPatches[i]);
        // The chain is the leading run of loaded slots (update spec 2.3)
        if (run && slotLoaded[i]) len++; else run = false;
    }
    chainLen = len;
    chainGen = chainGen + 1;
    chainReady = true;
    uiDirty = true;
}

// Through the common writer (v0.72): one write, temp file then rename, a
// notice on failure - never a half-written chain.
static void saveChainFile() {
    if (!sdReady()) return;
    fileWriteBegin();
    for (int i = 0; i < MORPH_SLOTS; i++) fileWritef("%s\n", slotName[i]);
    fileWriteCommit(MORPH_CHAIN_PATH, "Chain");
}

void morphInit() {
    for (int i = 0; i < MORPH_SLOTS; i++) { slotName[i][0] = 0; slotLoaded[i] = false; }
    if (sdReady()) {
        fileRecoverTmp(MORPH_CHAIN_PATH);
        File f = SD.open(MORPH_CHAIN_PATH, FILE_READ);
        if (f) {
            for (int i = 0; i < MORPH_SLOTS && f.available(); i++) {
                String s = f.readStringUntil('\n'); s.trim();
                strncpy(slotName[i], s.c_str(), PATCH_NAME_LEN); slotName[i][PATCH_NAME_LEN] = 0;
            }
            f.close();
        }
    }
    loadChain();
    // v0.82: a chain that was paused when the power went off stays paused,
    // so the patch loaded last (current.json) is what sounds - not the chain
    if (sys.morphPaused) pauseChain();
}

// A glide that has arrived and settled becomes the loaded patch, exactly as
// if it had been loaded from the card - C.P.S. behaviour. `patch` is written
// first; audioTask keeps holding the target until it sees the release.
void morphPoll() {
    checkChainResume();
    if (glidePhase != GP_SETTLED || glideRelease || glideCancel) return;
    int s = glideSlot;
    if (s < 0 || s >= MORPH_SLOTS) return;
    // No FX clear, unlike a load: the tails belong to the sound being glided
    // into and must ring on.
    patch = chainPatches[s];
    glideRelease = true;
    uiDirty = true;
}

const char *morphSlotName(int i) { return (i >= 0 && i < MORPH_SLOTS) ? slotName[i] : ""; }
int  morphChainLength() { return chainLen; }
int  morphSlotAlgorithm(int i) {
    if (i < 0 || i >= MORPH_SLOTS || !slotLoaded[i]) return 0;
    return chainPatches[i].algorithm;
}

void morphSetSlot(int i, const char *name) {
    if (i < 0 || i >= MORPH_SLOTS) return;
    strncpy(slotName[i], name ? name : "", PATCH_NAME_LEN); slotName[i][PATCH_NAME_LEN] = 0;
    saveChainFile();
    loadChain();
}

// Replaces the whole chain at once (v0.76: the demo chain) - one file write,
// one reload, instead of eight.
void morphSetChain(const char *const *names, int n) {
    for (int i = 0; i < MORPH_SLOTS; i++) {
        const char *nm = (i < n && names[i]) ? names[i] : "";
        strncpy(slotName[i], nm, PATCH_NAME_LEN); slotName[i][PATCH_NAME_LEN] = 0;
    }
    saveChainFile();
    loadChain();
}

void morphSetEnabled(bool on) { sys.morphEnabled = on; chainPaused = false; uiDirty = true; }

void morphGlideTo(int slot) {
    char b[40];
    if (slot < 0 || slot >= MORPH_SLOTS || !chainReady || !slotLoaded[slot]) {
        snprintf(b, sizeof(b), "Morph slot %d is empty", slot + 1); notify(b);
        return;
    }
    // The glide starts from whatever is heard, so it may take over from a
    // running chain or another glide. The request goes first: audioTask
    // gives a glide priority, so turning the chain off after it can never
    // let a buffer of the plain patch through.
    glideCancel = false;
    glideReq = slot;
    pauseChain();
    snprintf(b, sizeof(b), "Morph > %s", slotName[slot]); notify(b);
}

// PC Mode Morph (v0.8): Program Change 0-7 glides to slots 1-8 exactly like
// Shift+1-8 (chain on or off; an empty slot gets the same notice). Higher
// numbers are ignored, with a notice so a wrongly set pad shows up.
void morphProgramChange(int pc) {
    if (pc < MORPH_SLOTS) { morphGlideTo(pc); return; }
    char b[40];
    snprintf(b, sizeof(b), "PC %d ignored (Morph: 0-%d)", pc, MORPH_SLOTS - 1); notify(b);
}

void morphCommitAndStop() {
    if (!morphRunning()) return;
    Patch p = livePatch;             // a copy taken between two buffers is close enough
    strcpy(p.name, "Morph");
    // Routing is never morphed (it always came from `patch`), so it is kept
    p.lfoTarget = patch.lfoTarget;
    p.imuXTarget = patch.imuXTarget; p.imuXDepth = patch.imuXDepth;
    p.imuYTarget = patch.imuYTarget; p.imuYDepth = patch.imuYDepth;
    // `patch` first: while morph is still on audioTask reads only its routing,
    // which is unchanged, so the sound hands over without a buffer of the
    // old patch in between.
    patch = p;
    pauseChain();                    // audioTask copies `patch` from the next buffer on
    cancelGlide();
    notify(sys.morphEnabled ? "Chain paused" : "Morph off");
}

void morphStopForPatchChange() {
    if (!morphRunning()) return;
    pauseChain();
    cancelGlide();
    notify(sys.morphEnabled ? "Chain paused" : "Morph off");
}

const char *morphTakeNotice() {
    if (!noticePending) return nullptr;
    noticePending = false;
    return noticeBuf;
}

static bool chainHas(const char *name) {
    for (int i = 0; i < MORPH_SLOTS; i++) if (slotName[i][0] && strcmp(slotName[i], name) == 0) return true;
    return false;
}

void morphOnPatchSaved(const char *name) { if (chainHas(name)) loadChain(); }

// The factory patches were rewritten (v0.75): slots holding one re-read it.
// Harmless at boot, before morphInit() has filled any slot.
void morphOnFactoryRewritten() { loadChain(); }

void morphOnPatchDeleted(const char *name) {
    if (!chainHas(name)) return;
    for (int i = 0; i < MORPH_SLOTS; i++) if (strcmp(slotName[i], name) == 0) slotName[i][0] = 0;
    saveChainFile();
    loadChain();
}

void morphOnPatchRenamed(const char *oldName, const char *newName) {
    if (!chainHas(oldName)) return;
    // Names, not indices, are stored - so a rename is simply followed
    for (int i = 0; i < MORPH_SLOTS; i++)
        if (strcmp(slotName[i], oldName) == 0) { strncpy(slotName[i], newName, PATCH_NAME_LEN); slotName[i][PATCH_NAME_LEN] = 0; }
    saveChainFile();
    loadChain();
}

// ---------------------------------------------------------------------------
// audioTask side - once per buffer, never per sample
// ---------------------------------------------------------------------------
static int      hystSeg = -1;        // segment the discrete choice belongs to
static bool     hystB = false;       // discrete parameters currently taken from slot B
static uint32_t seenGen = 0;
static Patch    glideFrom;           // the sound heard when the glide began
static float    glideProg = 0.f;
static int      glideSettle = 0;
constexpr int   GLIDE_SETTLE_BUFS = 12;   // ~280ms: the smoothing has closed to ~1%

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
// Ratio and LFO rate are frequencies: the musical midpoint is geometric
static inline float lerpLog(float a, float b, float t) {
    if (a <= 0.f || b <= 0.f) return lerpf(a, b, t);
    return a * powf(b / a, t);
}
// Buffer-rate smoothing on every interpolated value (update spec 8.1),
// snapping once close as the modulation offsets do.
static inline void smooth(float &cur, float target) {
    cur += (target - cur) * SM_BUF;
    if (fabsf(target - cur) < 0.001f) cur = target;
}

// L moves towards A..B at t. Discrete values come from D (A or B, chosen by
// the caller). Routing always follows `patch` - a slot whose IMU or LFO
// drove something else could otherwise pull the morph's own source away
// mid-sweep.
static AUDIO_IRAM void blendInto(Patch &L, const Patch &A, const Patch &B, float t, const Patch &D) {
    const Patch &nameSrc = (t < 0.5f) ? A : B;
    if (strcmp(L.name, nameSrc.name) != 0) strcpy(L.name, nameSrc.name);

    for (int o = 0; o < NUM_OPS; o++) {
        OpParams &l = L.op[o]; const OpParams &x = A.op[o], &y = B.op[o];
        smooth(l.morph,    lerpf(x.morph, y.morph, t));
        smooth(l.ratio,    lerpLog(x.ratio, y.ratio, t));
        smooth(l.detune,   lerpf(x.detune, y.detune, t));
        smooth(l.level,    lerpf(x.level, y.level, t));
        smooth(l.attack,   lerpf(x.attack, y.attack, t));
        smooth(l.decay,    lerpf(x.decay, y.decay, t));
        smooth(l.sustain,  lerpf(x.sustain, y.sustain, t));
        smooth(l.release,  lerpf(x.release, y.release, t));
        smooth(l.velSens,  lerpf(x.velSens, y.velSens, t));
        smooth(l.keyScale, lerpf(x.keyScale, y.keyScale, t));
        // v0.71: the mode switches at the midpoint with the other discrete
        // values (the pitch jumps there, by design); FixNote is in semitones,
        // so a straight line is a log sweep in Hz
        l.fixed = D.op[o].fixed;
        smooth(l.fixNote,  lerpf(x.fixNote, y.fixNote, t));
    }
    smooth(L.penvAmt,   lerpf(A.penvAmt, B.penvAmt, t));       // v0.71
    smooth(L.penvDecay, lerpf(A.penvDecay, B.penvDecay, t));
    smooth(L.penvVel,   lerpf(A.penvVel, B.penvVel, t));
    L.algorithm = D.algorithm;
    smooth(L.feedback,    lerpf(A.feedback, B.feedback, t));
    smooth(L.globalMorph, lerpf(A.globalMorph, B.globalMorph, t));
    smooth(L.volume,      lerpf(A.volume, B.volume, t));
    // Voices: the larger of the two, so a chord is never cut mid-morph;
    // settles on the slot's own value at either end (update spec 4.4).
    if (t <= 0.f)      L.voices = A.voices;
    else if (t >= 1.f) L.voices = B.voices;
    else               L.voices = A.voices > B.voices ? A.voices : B.voices;
    smooth(L.glideMs, lerpf(A.glideMs, B.glideMs, t));
    L.glideEnabled = D.glideEnabled;
    L.glideMode    = D.glideMode;

    L.lfoWave = D.lfoWave;
    smooth(L.lfoRateHz,  lerpLog(A.lfoRateHz, B.lfoRateHz, t));
    smooth(L.lfoDepth,   lerpf(A.lfoDepth, B.lfoDepth, t));
    smooth(L.lfoDelayMs, lerpf(A.lfoDelayMs, B.lfoDelayMs, t));
    L.lfoTarget  = patch.lfoTarget;
    L.imuXTarget = patch.imuXTarget; L.imuXDepth = patch.imuXDepth;
    L.imuYTarget = patch.imuYTarget; L.imuYDepth = patch.imuYDepth;

    smooth(L.chorusRateHz,  lerpf(A.chorusRateHz, B.chorusRateHz, t));
    smooth(L.chorusDepthMs, lerpf(A.chorusDepthMs, B.chorusDepthMs, t));
    smooth(L.chorusMix,     lerpf(A.chorusMix, B.chorusMix, t));
    L.delayTimeMs = D.delayTimeMs;   // swept continuously it would pitch like tape
    smooth(L.delayFeedback, lerpf(A.delayFeedback, B.delayFeedback, t));
    smooth(L.delayMix,      lerpf(A.delayMix, B.delayMix, t));
    smooth(L.reverbSize,    lerpf(A.reverbSize, B.reverbSize, t));
    smooth(L.reverbDamp,    lerpf(A.reverbDamp, B.reverbDamp, t));
    smooth(L.reverbMix,     lerpf(A.reverbMix, B.reverbMix, t));
    smooth(L.limiterDrive,  lerpf(A.limiterDrive, B.limiterDrive, t));
    smooth(L.limiterMix,    lerpf(A.limiterMix, B.limiterMix, t));
    smooth(L.crushBits,     lerpf(A.crushBits, B.crushBits, t));
    smooth(L.crushDown,     lerpf(A.crushDown, B.crushDown, t));
    smooth(L.crushMix,      lerpf(A.crushMix, B.crushMix, t));   // Mix 0 = Off, so On/Off morphs too
}

// Returns true when a glide produced this buffer's patch.
static bool glideStep(Patch &L, bool ready) {
    if (glideCancel)  { glideCancel = false; glidePhase = GP_IDLE; glideSlot = -1; }
    if (glideRelease) { glideRelease = false; glidePhase = GP_IDLE; glideSlot = -1; }
    int req = glideReq;
    if (req >= 0 && ready) {
        glideReq = -1;
        if (slotLoaded[req]) {
            glideFrom = L;               // from the sound heard right now
            glideSlot = req;
            glideProg = 0.f;
            glideSettle = 0;
            glidePhase = GP_MOVING;
        }
    }
    if (glidePhase == GP_IDLE) return false;
    if (!ready) return true;             // slots being rewritten: hold what is playing

    const Patch &to = chainPatches[glideSlot];
    if (glidePhase == GP_MOVING) {
        int ms = clampi(sys.morphTimeMs, 0, MORPH_TIME_MAX_MS);
        float step = (ms > 0) ? (1000.f * (float)AUDIO_BUFFER_SIZE / (float)SAMPLE_RATE) / (float)ms : 1.f;
        glideProg += step;
        if (glideProg >= 1.f) { glideProg = 1.f; glidePhase = GP_ARRIVED; }
    } else if (glidePhase == GP_ARRIVED && ++glideSettle >= GLIDE_SETTLE_BUFS) {
        glidePhase = GP_SETTLED;         // loop() now makes it the loaded patch
    }
    // A glide only ever moves forward, so the midpoint needs no hysteresis
    blendInto(L, glideFrom, to, glideProg, glideProg >= 0.5f ? to : glideFrom);
    audioStatus.morphSeg = -2;
    audioStatus.morphGlideTo = glideSlot;
    audioStatus.morphT = glideProg;
    return true;
}

void AUDIO_IRAM morphBuildLivePatch(Patch &L, float pos) {
    bool ready = chainReady;
    if (glideStep(L, ready)) { hystSeg = -1; return; }
    audioStatus.morphGlideTo = -1;
    if (!(sys.morphEnabled && !chainPaused && ready && chainLen >= 2)) {
        L = patch;                   // every buffer, unconditionally (update spec 2.2)
        hystSeg = -1;
        audioStatus.morphSeg = -1;
        return;
    }
    if (seenGen != chainGen) { seenGen = chainGen; hystSeg = -1; }
    int n = chainLen;
    float f = clampf(pos, 0.f, 1.f) * (float)(n - 1);
    int a = (int)f; if (a > n - 2) a = n - 2;
    float t = clampf(f - (float)a, 0.f, 1.f);
    const Patch &A = chainPatches[a], &B = chainPatches[a + 1];

    // Discrete parameters switch at the midpoint, with hysteresis so a knob
    // resting near it cannot flip the algorithm every buffer (update spec
    // 4.3). Entering a new segment picks the side the position is on.
    if (a != hystSeg)              { hystSeg = a; hystB = (t >= 0.5f); }
    else if (!hystB && t >= 0.55f) hystB = true;
    else if (hystB && t <= 0.45f)  hystB = false;
    blendInto(L, A, B, t, hystB ? B : A);

    audioStatus.morphSeg = a;
    audioStatus.morphT = t;
}
