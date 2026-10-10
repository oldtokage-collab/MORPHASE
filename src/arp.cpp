// MORPHASE - arp.cpp
// Arpeggiator with a STEP pattern living inside the Pattern list, so
// tempo / swing / latch / clock sync / MIDI out are shared by every
// pattern (spec 5.1). One chord-building entry point: rebuildArpChord().

#include "arp.h"
#include "voice.h"
#include "midi.h"
#include "glitch.h"
#include <M5Cardputer.h>
#include <esp_system.h>

ArpSettings arp;

// Held keys, in the order they were pressed
struct HeldNote { uint8_t note; uint8_t vel; bool sustained; };
static HeldNote held[ARP_MAX_CHORD];
static int      heldCount = 0;
static HeldNote latched[ARP_MAX_CHORD];
static int      latchedCount = 0;
static bool     pedalDown = false;

// The chord the patterns index into (sorted ascending, or as played)
static uint8_t chordNote[ARP_MAX_CHORD];
static uint8_t chordVel[ARP_MAX_CHORD];
static int     chordLen = 0;

// Pending note-offs (gate ends)
struct PendingOff { uint8_t note; bool used; unsigned long offMs; };
static PendingOff pendingOff[ARP_MAX_PENDING_OFF];

// Sequencer state
static int  seqIndex = 0;          // position within the generated sequence
static int  stepIndex = 0;         // STEP pattern position
// Steps played since the ARP started (v0.90): Swing's long / short follows
// this, so an odd-length pattern or a Reverse glitch can't break the swing
// at the loop point (it used to follow seqIndex / stepIndex).
static uint32_t swingCount = 0;
static unsigned long nextStepMs = 0;
static bool running = false;
static int  lastNote = -1;
static int  lastChordIdx = -1;    // which chord member fired last (v0.41)
static int  playingStep = 0;       // step shown by the playhead

// ARP glitch (v0.82, instruction (6) section 2). Decided once per ARP step,
// apart from the buffer / Chaos events (a different grid); loop() only, so
// no DSP cost. Nothing here is saved: the pattern stays as it is.
static int  ratchetLeft = 0;       // re-hits still to fire in this step
static uint8_t ratchetNote = 0, ratchetVel = 0;
static float ratchetSubMs = 0.f;
static unsigned long ratchetGateMs = 0;
static int  reverseLeft = 0;       // steps still to walk backwards
constexpr float RATCHET_MIN_MS = 15.f;

static void glitchArpReset() { ratchetLeft = 0; reverseLeft = 0; }

// Tap tempo
static unsigned long tapTimes[4];
static int tapCount = 0;

void arpPatternDefaults() {
    for (int i = 0; i < ARP_STEPS; i++) {
        arp.step[i].note = 1; arp.step[i].oct = 0; arp.step[i].vel = 100; arp.step[i].gate = 80;
    }
    arp.length = 16;
    arp.chordWrap = ChordWrap::OCTAVE;
}

void arpInitDefaults() {
    arp.shiftMode = ShiftMode::KEYS_IN;
    arp.on = false;
    arp.pattern = ArpPattern::UP;
    arp.latch = false;
    arp.tempoBpm = 120;
    arp.rate = ArpRate::R_1_16;
    arp.swing = 50;
    arp.gate = 80;
    arp.octave = 1;
    arp.midiOut = false;
    arp.clockSync = false;
    arp.latchMode = LatchMode::ADD;
    arpPatternDefaults();
    for (int i = 0; i < ARP_MAX_PENDING_OFF; i++) pendingOff[i].used = false;
}

// A random pattern that is actually playable (spec A.3). Straight random
// values give a pattern with no shape, so: the beats keep a high velocity
// to give the bar a skeleton, one gate value is shared by every step so
// the outline stays crisp, and rests are common enough to breathe.
// Chord Wrap is left alone - it carries the player's intent.
void arpRandomizePattern() {
    arp.length = (esp_random() & 1) ? 8 : 16;
    int gate = 50 + (int)(esp_random() % 71);          // 50-120 %, shared
    for (int i = 0; i < ARP_STEPS; i++) {
        ArpStep &s = arp.step[i];
        uint32_t r = esp_random();
        s.note = ((r % 100) < 75) ? (uint8_t)(1 + (r >> 8) % 4) : 0;   // 75% a note, 25% rest
        uint32_t ro = (esp_random() % 100);
        s.oct = (ro < 70) ? 0 : (ro < 90 ? 1 : -1);
        bool beat = (i % 4) == 0;
        s.vel = beat ? (uint8_t)(100 + esp_random() % 28)              // 100-127 on the beats
                     : (uint8_t)(60 + esp_random() % 68);              // 60-127 elsewhere
        s.gate = (uint8_t)gate;
    }
    rebuildArpChord();
}

// ---------------------------------------------------------------------------
// Note-off bookkeeping
// ---------------------------------------------------------------------------
static void fireNoteOff(uint8_t note) {
    postNoteOff(note, EVF_IGNORE_SUSTAIN);
    if (arp.midiOut) midiSendNoteOff(note);
}

static void cancelPendingOff(uint8_t note) {
    for (int i = 0; i < ARP_MAX_PENDING_OFF; i++)
        if (pendingOff[i].used && pendingOff[i].note == note) pendingOff[i].used = false;
}

static void schedulePendingOff(uint8_t note, unsigned long offMs) {
    for (int i = 0; i < ARP_MAX_PENDING_OFF; i++) {
        if (!pendingOff[i].used) { pendingOff[i].used = true; pendingOff[i].note = note; pendingOff[i].offMs = offMs; return; }
    }
    // Table full: end the oldest one now
    int oldest = 0;
    for (int i = 1; i < ARP_MAX_PENDING_OFF; i++) if (pendingOff[i].offMs < pendingOff[oldest].offMs) oldest = i;
    fireNoteOff(pendingOff[oldest].note);
    pendingOff[oldest].note = note; pendingOff[oldest].offMs = offMs;
}

static void flushPendingOffs() {
    for (int i = 0; i < ARP_MAX_PENDING_OFF; i++) {
        if (pendingOff[i].used) { fireNoteOff(pendingOff[i].note); pendingOff[i].used = false; }
    }
}

// Octave + transpose in semitones when they act on the ARP's output (v0.43),
// else 0 - in KEYS_IN mode midi.cpp has already applied them.
static int outputShift() {
    if (arp.shiftMode != ShiftMode::ARP_OUT) return 0;
    return clampi(sys.octave, OCTAVE_MIN, OCTAVE_MAX) * 12 + clampi(sys.transpose, TRANSPOSE_MIN, TRANSPOSE_MAX);
}

static void fireNoteOn(uint8_t rawNote, uint8_t vel, unsigned long gateMs) {
    // The shift is taken at fire time, so a change moves the very next step.
    // The pending note-off stores the shifted note, so a change during the
    // gate still ends the note that is actually sounding.
    int n = (int)rawNote + outputShift();
    if (n < 0 || n > 127) return;      // shifted off the MIDI range: a rest
    uint8_t note = (uint8_t)n;
    cancelPendingOff(note);            // retrigger of a still-sounding note: reschedule its end
    postNoteOn(note, vel, EVF_IGNORE_SUSTAIN);
    if (arp.midiOut) midiSendNoteOn(note, vel);
    schedulePendingOff(note, millis() + gateMs);
    lastNote = note;
}

// ---------------------------------------------------------------------------
// Chord construction - the ONLY entry point (spec 5.6)
// ---------------------------------------------------------------------------
void rebuildArpChord() {
    // Latch (v0.11): the latched list is edited directly by arpNoteOn /
    // arpToggleLatch and is NOT rebuilt from the held keys here, so notes
    // pressed one after another accumulate (a Cardputer-style latch, like
    // C.P.S.) instead of the last key replacing the chord.
    const HeldNote *src = arp.latch ? latched : held;
    int n = arp.latch ? latchedCount : heldCount;
    chordLen = 0;
    lastChordIdx = -1;                 // indices shift when the chord changes
    if (!arp.on) { running = false; return; }
    for (int i = 0; i < n && chordLen < ARP_MAX_CHORD; i++) {
        chordNote[chordLen] = src[i].note; chordVel[chordLen] = src[i].vel; chordLen++;
    }
    if (arp.pattern != ArpPattern::AS_PLAYED) {
        // insertion sort ascending
        for (int i = 1; i < chordLen; i++) {
            uint8_t kn = chordNote[i], kv = chordVel[i]; int j = i - 1;
            while (j >= 0 && chordNote[j] > kn) { chordNote[j + 1] = chordNote[j]; chordVel[j + 1] = chordVel[j]; j--; }
            chordNote[j + 1] = kn; chordVel[j + 1] = kv;
        }
    }
    if (chordLen == 0) {
        running = false;
        glitchArpReset();
    } else if (!running) {
        running = true;
        seqIndex = 0; stepIndex = 0; swingCount = 0;
        glitchArpReset();
        nextStepMs = millis();          // first step fires immediately
    }
}

// ---------------------------------------------------------------------------
// Key input
// ---------------------------------------------------------------------------
static void removeHeld(int idx) {
    for (int j = idx; j < heldCount - 1; j++) held[j] = held[j + 1];
    heldCount--;
}

void arpNoteOn(uint8_t note, uint8_t vel) {
    if (arp.latch) {
        if (arp.latchMode == LatchMode::CHORD) {
            // Standard latch: the first key after everything was released
            // starts a new chord; keys held together join it.
            if (heldCount == 0) latchedCount = 0;
            bool present = false;
            for (int i = 0; i < latchedCount; i++) if (latched[i].note == note) { latched[i].vel = vel; present = true; }
            if (!present && latchedCount < ARP_MAX_CHORD) { latched[latchedCount].note = note; latched[latchedCount].vel = vel; latched[latchedCount].sustained = false; latchedCount++; }
        } else {
            // ADD: every new key joins the latched chord; a latched key
            // pressed again leaves it. Turning Latch off clears it.
            for (int i = 0; i < latchedCount; i++) {
                if (latched[i].note == note) {
                    for (int j = i; j < latchedCount - 1; j++) latched[j] = latched[j + 1];
                    latchedCount--;
                    rebuildArpChord();
                    return;
                }
            }
            if (latchedCount < ARP_MAX_CHORD) { latched[latchedCount].note = note; latched[latchedCount].vel = vel; latched[latchedCount].sustained = false; latchedCount++; }
        }
    }
    for (int i = 0; i < heldCount; i++) if (held[i].note == note) { held[i].vel = vel; held[i].sustained = false; rebuildArpChord(); return; }
    if (heldCount < ARP_MAX_CHORD) { held[heldCount].note = note; held[heldCount].vel = vel; held[heldCount].sustained = false; heldCount++; }
    rebuildArpChord();
}

void arpNoteOff(uint8_t note) {
    for (int i = 0; i < heldCount; i++) {
        if (held[i].note != note) continue;
        if (pedalDown) { held[i].sustained = true; return; }   // pedal keeps it in the chord
        removeHeld(i);
        break;
    }
    rebuildArpChord();
}

void arpSustain(bool down) {
    pedalDown = down;
    if (!down) {
        for (int i = heldCount - 1; i >= 0; i--) if (held[i].sustained) removeHeld(i);
        rebuildArpChord();
    }
}

void arpPanic() {
    flushPendingOffs();
    heldCount = 0; latchedCount = 0; pedalDown = false;
    rebuildArpChord();
}

void arpSetOn(bool on) {
    if (arp.on == on) return;
    arp.on = on;
    flushPendingOffs();
    heldCount = 0; latchedCount = 0; pedalDown = false;
    // Whatever was sounding directly must stop when the ARP takes over,
    // and ARP notes must stop when it lets go.
    postAllNotesOff();
    rebuildArpChord();
}

void arpToggleLatch() {
    arp.latch = !arp.latch;
    if (arp.latch) {
        // Start from whatever is held right now
        for (int i = 0; i < heldCount; i++) latched[i] = held[i];
        latchedCount = heldCount;
    } else {
        latchedCount = 0;
    }
    rebuildArpChord();
}

void arpTapTempo() {
    unsigned long now = millis();
    if (tapCount > 0 && now - tapTimes[tapCount - 1] > 2000) tapCount = 0;
    if (tapCount == 4) { for (int i = 0; i < 3; i++) tapTimes[i] = tapTimes[i + 1]; tapCount = 3; }
    tapTimes[tapCount++] = now;
    if (tapCount >= 2) {
        unsigned long span = tapTimes[tapCount - 1] - tapTimes[0];
        float avg = (float)span / (float)(tapCount - 1);
        if (avg > 0.f) arp.tempoBpm = clampi((int)(60000.f / avg + 0.5f), 40, 300);
    }
}

void arpResync() {
    seqIndex = 0; stepIndex = 0; swingCount = 0;
    glitchArpReset();
    if (running) nextStepMs = millis();
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
float arpEffectiveBpm() {
    // External clock only when the player asked for it (v0.11): many
    // keyboards send clock all the time, which silently overrode Tempo.
    if (arp.clockSync && midiClockLocked()) return midiClockBpm();
    return clampf((float)arp.tempoBpm + ctrlMod.tempoOff, 40.f, 300.f);
}

// Swing and gate with the IMU offset folded in (v0.3). The stored setting
// is never touched, so letting go of the tilt restores it exactly.
static int effectiveSwing() { return clampi(arp.swing + (int)ctrlMod.swingOff, 0, 100); }
static int effectiveGate()  { return clampi(arp.gate + (int)ctrlMod.gateOff, 10, 200); }

static float baseStepMs() {
    return 60000.f / arpEffectiveBpm() * ARP_RATE_MULT[(int)arp.rate];
}

// Step k's duration with swing: even steps stretch, odd steps shrink (k is
// swingCount, the steps played so far).
// swing 50 = straight, 100 = 2:1 (triplet feel), 0 = the reverse.
static float stepDurationMs(int k, float base) {
    float off = base * ((float)(effectiveSwing() - 50) / 50.f) * (1.f / 3.f);
    return (k % 2 == 0) ? base + off : base - off;
}

// n-th chord member for the STEP pattern (0-based), applying chordWrap
// (spec 5.3). Returns -1 for a rest.
static int chordMember(int idx) {
    if (chordLen <= 0) return -1;
    if (idx < chordLen) return chordNote[idx];
    switch (arp.chordWrap) {
        case ChordWrap::REST:   return -1;
        case ChordWrap::WRAP:   return chordNote[idx % chordLen];
        case ChordWrap::OCTAVE:
        default: {
            int n = chordNote[idx % chordLen] + 12 * (idx / chordLen);
            return n > 127 ? -1 : n;
        }
    }
}

// Sequence length / member for the ordinary patterns
static int seqLength() {
    int span = chordLen * clampi(arp.octave, 1, 4);
    if (arp.pattern == ArpPattern::UPDOWN && span > 2) return span * 2 - 2;
    return span;
}

static bool seqMember(int i, int &note, int &vel, int &chordIdx) {
    int span = chordLen * clampi(arp.octave, 1, 4);
    if (span <= 0) return false;
    int pos = 0;
    switch (arp.pattern) {
        case ArpPattern::UP:
        case ArpPattern::AS_PLAYED: pos = i % span; break;
        case ArpPattern::DOWN:      pos = span - 1 - (i % span); break;
        case ArpPattern::UPDOWN: {
            int len = seqLength(); int p = i % len;
            pos = (p < span) ? p : (2 * span - 2 - p);
            break;
        }
        case ArpPattern::RANDOM:    pos = (int)(esp_random() % (uint32_t)span); break;
        default: pos = i % span; break;
    }
    int oct = pos / chordLen, idx = pos % chordLen;
    note = chordNote[idx] + 12 * oct;
    vel  = chordVel[idx];
    chordIdx = idx;
    return note <= 127;
}

// The ARP glitch for this step, or OFF. Random draws one of the four for
// each event, as the buffer events' Random does.
static GlArp rollArpGlitch() {
    if (sys.glArp == GlArp::OFF) return GlArp::OFF;
    float p = glitchEffectiveProb();
    if (p <= 0.f || (float)(esp_random() & 0xFFFF) / 65536.f >= p) return GlArp::OFF;
    if (sys.glArp != GlArp::RANDOM) return sys.glArp;
    return (GlArp)(1 + esp_random() % 4);
}

static unsigned long ratchetStartMs = 0;
static int ratchetIdx = 0;

// One step's note, with this step's glitch. Rest plays nothing (the
// position still moves); Octave jumps up or down, the other way when that
// would leave the MIDI range; Ratchet re-hits the note 2-4 times inside the
// step (Amount), never shorter than 15ms each, each with the step's gate
// share. Returns false when nothing sounded.
static bool playStep(int note, int vel, float dur, float gateShare, unsigned long stepStartMs, GlArp g) {
    if (g == GlArp::REST) return false;
    if (g == GlArp::OCTAVE) {
        int heard = note + outputShift();
        int d = (esp_random() & 1) ? 12 : -12;
        if (heard + d < 0 || heard + d > 127) d = -d;
        if (heard + d >= 0 && heard + d <= 127) note += d;
    }
    if (note < 0 || note > 127) return false;
    unsigned long gateMs = (unsigned long)(dur * gateShare);
    if (g == GlArp::RATCHET) {
        int count = 2 + (int)((float)clampi(sys.glAmt, 0, 100) / 50.f + 0.5f);   // 2..4
        while (count > 1 && dur / (float)count < RATCHET_MIN_MS) count--;
        if (count >= 2) {
            float sub = dur / (float)count;
            ratchetGateMs = (unsigned long)(sub * gateShare); if (ratchetGateMs < 1) ratchetGateMs = 1;
            fireNoteOn((uint8_t)note, (uint8_t)vel, ratchetGateMs);
            ratchetNote = (uint8_t)note; ratchetVel = (uint8_t)vel;
            ratchetLeft = count - 1; ratchetSubMs = sub; ratchetStartMs = stepStartMs; ratchetIdx = 1;
            return true;
        }
    }
    fireNoteOn((uint8_t)note, (uint8_t)vel, gateMs);
    return true;
}

void arpUpdate() {
    unsigned long now = millis();
    // Gate ends
    for (int i = 0; i < ARP_MAX_PENDING_OFF; i++) {
        if (pendingOff[i].used && (long)(now - pendingOff[i].offMs) >= 0) {
            pendingOff[i].used = false; fireNoteOff(pendingOff[i].note);
        }
    }
    if (!arp.on || !running || chordLen == 0) { glitchArpReset(); return; }
    // Ratchet re-hits inside the current step (v0.82)
    if (ratchetLeft > 0 && (long)(now - (ratchetStartMs + (unsigned long)(ratchetSubMs * (float)ratchetIdx))) >= 0) {
        fireNoteOn(ratchetNote, ratchetVel, ratchetGateMs);
        ratchetLeft--; ratchetIdx++;
    }
    if ((long)(now - nextStepMs) < 0) return;

    ratchetLeft = 0;                   // a new step ends any re-hits left over
    GlArp g = rollArpGlitch();
    unsigned long stepStart = nextStepMs;
    float base = baseStepMs();
    if (arp.pattern == ArpPattern::STEP) {
        int len = clampi(arp.length, 1, ARP_STEPS);
        if (stepIndex >= len) stepIndex = 0;
        const ArpStep &s = arp.step[stepIndex];
        float dur = stepDurationMs((int)(swingCount++ & 1), base);
        if (s.note > 0) {
            int n = chordMember((int)s.note - 1);
            if (n >= 0) {
                n += 12 * (int)s.oct;
                if (n >= 0 && n <= 127 &&
                    playStep(n, clampi(s.vel, 1, 127), dur, (float)clampi(s.gate, 10, 200) / 100.f, stepStart, g)) {
                    // By member index, not by pitch: a STEP note is the chord
                    // member moved by its octave, so a pitch match would miss.
                    lastChordIdx = ((int)s.note - 1) % chordLen;
                }
            }
        }
        playingStep = stepIndex;
        nextStepMs += (unsigned long)dur;
        // Reverse reads the steps backwards for a while, then carries on
        // forwards from wherever it got to
        if (g == GlArp::REVERSE) reverseLeft = 2 + (int)((float)clampi(sys.glAmt, 0, 100) * 6.f / 100.f + 0.5f);
        int dir = reverseLeft > 0 ? -1 : 1;
        if (reverseLeft > 0) reverseLeft--;
        stepIndex = (stepIndex + dir + len) % len;
    } else {
        int len = seqLength();
        if (len <= 0) return;
        if (seqIndex >= len) seqIndex = 0;
        int note, vel, ci = -1;
        float dur = stepDurationMs((int)(swingCount++ & 1), base);
        if (seqMember(seqIndex, note, vel, ci) &&
            playStep(note, vel, dur, (float)effectiveGate() / 100.f, stepStart, g)) {
            lastChordIdx = ci;
        }
        playingStep = seqIndex;
        nextStepMs += (unsigned long)dur;
        if (g == GlArp::REVERSE) reverseLeft = 2 + (int)((float)clampi(sys.glAmt, 0, 100) * 6.f / 100.f + 0.5f);
        int dir = reverseLeft > 0 ? -1 : 1;
        if (reverseLeft > 0) reverseLeft--;
        seqIndex = (seqIndex + dir + len) % len;
    }
    // If we fell far behind (e.g. a long SD write), resynchronise rather
    // than firing a burst of catch-up steps.
    if ((long)(now - nextStepMs) > (long)(base * 2.f)) nextStepMs = now;
}

int   arpChordSize()   { return chordLen; }
int   arpChordNote(int i) {        // as heard: ARP_OUT mode adds the shift
    if (i < 0 || i >= chordLen) return -1;
    return clampi((int)chordNote[i] + outputShift(), 0, 127);
}
int   arpLastChordIndex() { return (lastChordIdx < chordLen) ? lastChordIdx : -1; }
int   arpCurrentStep() { return playingStep; }
bool  arpIsRunning()   { return running && arp.on; }
int   arpLastNote()    { return lastNote; }
