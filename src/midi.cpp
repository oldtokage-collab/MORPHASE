// MORPHASE - midi.cpp
// Serial MIDI transport and parser. Runs entirely in loop(); it never
// touches voice state directly - notes are posted as NoteEvents (or handed
// to the arpeggiator when it is on), control changes land in `live`.
//
// Hardware notes inherited from C.P.S. (proven wiring):
//  * Unit MIDI (U187) on the Grove port, DIP switch = BYPASS. Only in
//    Bypass does the unit's TX pin carry what arrives at its INPUT socket.
//  * Grove G1 is RX, G2 is TX. Baud is 31250 (M5's docs say 31520 - typo).
//  * The Grove 5V direction switch must be 5VOUT (as printed on the case) or the unit is dead.

#include "midi.h"
#include "voice.h"
#include "arp.h"
#include "glitch.h"
#include "patch.h"
#include "ui_screens.h"
#include "ble_midi.h"
#include <M5Cardputer.h>

static HardwareSerial midiSerial(1);   // UART1; UART0 is the USB console
static bool midiReady = false;

// Parser state, one per input (v0.77): DIN and BLE bytes must never share a
// running status or a half-received message.
struct MidiParser {
    uint8_t status;
    uint8_t data[2];
    int     dataIdx;
    int     dataNeeded;
};
static MidiParser dinParser = {};
#ifdef MORPHASE_BLE
static MidiParser bleParser = {};
#endif

static uint32_t rxBytes = 0;
static unsigned long lastRxMs = 0;

// Clock
static int   clockTicks = 0;
static unsigned long clockLastTickMs = 0;
static unsigned long clockLastBeatMs = 0;
static float clockBpm = 120.f;

static int programRequest = -1;

// ---- MIDI Learn -----------------------------------------------------------
// v0.33: a CC is learned only when it MOVES - the same CC number arriving
// again with a value at least LEARN_MIN_DELTA away from the first one seen.
// Controllers send CC on their own (a jittery knob, a dump of knob positions),
// so taking the first message that arrives grabbed whatever was chattering.
// Nothing that arrives in the first LEARN_SETTLE_MS after Enter counts.
static bool          learnActive = false;
static int           learnResult = -1;
static unsigned long learnStartMs = 0;
static int16_t       learnFirstVal[128];
constexpr unsigned long MIDI_LEARN_TIMEOUT_MS = 10000;
constexpr unsigned long LEARN_SETTLE_MS = 200;
constexpr int           LEARN_MIN_DELTA = 8;           // of 127

void midiLearnBegin()  {
    learnActive = true; learnResult = -1; learnStartMs = millis();
    for (int i = 0; i < 128; i++) learnFirstVal[i] = -1;
}

static bool learnOffer(uint8_t cc, uint8_t val) {
    if (millis() - learnStartMs < LEARN_SETTLE_MS) return false;
    int16_t &first = learnFirstVal[cc & 0x7F];
    if (first < 0) { first = val; return false; }
    int d = (int)val - (int)first;
    if (d < 0) d = -d;
    return d >= LEARN_MIN_DELTA;
}
void midiLearnCancel() { learnActive = false; learnResult = -1; }
bool midiLearnActive() { return learnActive; }
int  midiLearnTakeResult() { int r = learnResult; learnResult = -1; return r; }

// ---- switch / pattern-select edge state -----------------------------------
// Buttons are acted on when the value crosses 64 upward. Without this, a
// controller that keeps sending while held would re-fire an action (and
// re-read the SD card for a pattern) dozens of times a second.
static bool swHigh[NUM_SW_SLOTS] = {false, false, false, false};
static bool patSelHigh[NUM_PATTERN_SLOTS] = {false};

constexpr uint8_t MIDI_NOTE_OFF   = 0x80;
constexpr uint8_t MIDI_NOTE_ON    = 0x90;
constexpr uint8_t MIDI_CC         = 0xB0;
constexpr uint8_t MIDI_PROGRAM    = 0xC0;
constexpr uint8_t MIDI_PITCH_BEND = 0xE0;

void midiBegin() {
    midiSerial.end();
    midiSerial.begin(MIDI_BAUD, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);
    midiReady = true;
    Serial.printf("[MIDI] listening on RX=%d TX=%d @%u baud (Unit MIDI DIP = BYPASS)\n",
                  MIDI_RX_PIN, MIDI_TX_PIN, (unsigned)MIDI_BAUD);
}

// The theremin on the main Grove port (v0.81) needs GPIO1/2, the Unit MIDI
// pins: DIN MIDI stops while it is on there and comes back after. Only
// theremin.cpp calls these, and only for Bus = Grove.
void midiDinSuspend() {
    if (!midiReady) return;
    midiReady = false;
    midiSerial.end();
    Serial.println("[MIDI] DIN stopped: Theremin on Grove");
}
void midiDinResume() {
    if (midiReady) return;
    midiBegin();
}
bool midiDinActive() { return midiReady; }

static uint8_t outChannelByte() {
    int ch = (sys.midiChannel >= 1 && sys.midiChannel <= 16) ? sys.midiChannel : 1;
    return (uint8_t)(ch - 1);
}

void midiSendNoteOn(uint8_t note, uint8_t vel) {
    if (!midiReady) return;
    uint8_t b[3] = { (uint8_t)(MIDI_NOTE_ON | outChannelByte()), (uint8_t)(note & 0x7F), (uint8_t)(vel & 0x7F) };
    midiSerial.write(b, 3);
}

void midiSendNoteOff(uint8_t note) {
    if (!midiReady) return;
    uint8_t b[3] = { (uint8_t)(MIDI_NOTE_OFF | outChannelByte()), (uint8_t)(note & 0x7F), 0 };
    midiSerial.write(b, 3);
}

bool  midiClockLocked() { return clockLastTickMs != 0 && (millis() - clockLastTickMs) < 500; }
float midiClockBpm()    { return clockBpm; }
bool  midiRxActive()    { return lastRxMs != 0 && (millis() - lastRxMs) < 300; }
uint32_t midiRxByteCount() { return rxBytes; }
int   midiTakeProgramRequest() { int r = programRequest; programRequest = -1; return r; }

// ---------------------------------------------------------------------------
static bool channelAccepted(uint8_t status) {
    if (sys.midiChannel == MIDI_CHANNEL_OMNI) return true;
    return (int)(status & 0x0F) + 1 == sys.midiChannel;
}

// Octave / transpose (v0.42) are applied here, before the ARP, so the ARP
// and its MIDI OUT see the shifted notes too. sentNote remembers what each
// incoming key became, so changing either while a key is down never leaves
// a note hanging: the note-off goes to the note that was actually started.
static int8_t sentNote[128];
static bool   sentNoteInit = false;

static int shiftedNote(uint8_t note) {
    // ARP with OctTrn = Output: the ARP applies it to the notes it fires, so
    // the chord it holds stays unshifted (v0.43).
    if (arp.on && arp.shiftMode == ShiftMode::ARP_OUT) return note;
    int n = (int)note + clampi(sys.octave, OCTAVE_MIN, OCTAVE_MAX) * 12
                      + clampi(sys.transpose, TRANSPOSE_MIN, TRANSPOSE_MAX);
    return (n < 0 || n > 127) ? -1 : n;   // off the MIDI range: dropped, not folded
}

static void onNoteOff(uint8_t note) {
    note &= 0x7F;
    int out = sentNote[note];
    sentNote[note] = -1;
    if (out < 0) return;
    if (arp.on) arpNoteOff((uint8_t)out);
    else        postNoteOff((uint8_t)out);
}

static void onNoteOn(uint8_t note, uint8_t vel) {
    note &= 0x7F;
    if (vel == 0) { onNoteOff(note); return; }
    if (sentNote[note] >= 0) onNoteOff(note);   // retrigger without a note-off in between
    int out = shiftedNote(note);
    if (out < 0) return;
    sentNote[note] = (int8_t)out;
    if (arp.on) arpNoteOn((uint8_t)out, vel);
    else        postNoteOn((uint8_t)out, vel);
}

// A knob pointed at a control parameter sets it outright: the knob's
// position IS the value, which is what makes tempo and swing feel right
// under a hand. Audio destinations stay relative and go through `live`.
static void applyControlCc(ModTarget t, float n, int depth) {
    float d = (float)clampi(depth, 0, 100) / 100.f;
    float f = n * d;
    switch (t) {
        case ModTarget::ARP_TEMPO:  arp.tempoBpm = clampi((int)(40.f + f * 260.f + 0.5f), 40, 300); break;
        case ModTarget::ARP_SWING:  arp.swing = clampi((int)(f * 100.f + 0.5f), 0, 100); break;
        case ModTarget::ARP_GATE:   arp.gate = clampi((int)(10.f + f * 190.f + 0.5f), 10, 200); break;
        case ModTarget::GLIDE_TIME: patch.glideMs = clampf(f * 1000.f, 0.f, 1000.f); break;
        default: return;
    }
    uiDirty = true;
}

static void onControlChange(uint8_t cc, uint8_t val) {
    float n = (float)val / 127.f;
    // Learn swallows the message whole: it must not also reach whatever the
    // CC happens to be assigned to already (spec B.2).
    if (learnActive) {
        if (learnOffer(cc, val)) { learnResult = cc; learnActive = false; uiDirty = true; }
        return;
    }
    switch (cc) {
        case 1:  live.ccMorphNorm = n; break;              // Modulation -> globalMorph
        case 2:  live.ccFeedbackNorm = n; break;           // Breath -> feedback
        case 7:  live.ccVolumeNorm = n; break;
        case 11: live.ccExpressionNorm = n; break;
        case 64:                                            // Sustain
            sustainInput(SUS_PEDAL, val >= 64);
            break;
        case 74: live.ccMorphNorm = n; break;              // Brightness -> globalMorph
        case 120: postAllSoundOff(); if (arp.on) arpPanic(); break;
        case 123: postAllNotesOff(); if (arp.on) arpPanic(); break;
        default: break;
    }
    // Knob slots (checked after the fixed map so a slot may double a CC)
    for (int i = 0; i < NUM_CC_SLOTS; i++) {
        const CcSlot &s = sys.ccSlot[i];
        if (s.target == ModTarget::OFF || s.number == CC_OFF || (int)cc != s.number) continue;
        if (modTargetIsAudio(s.target)) live.ccSlotNorm[i] = n;
        else                            applyControlCc(s.target, n, s.depth);
    }
    // Switch slots
    for (int i = 0; i < NUM_SW_SLOTS; i++) {
        const SwSlot &s = sys.swSlot[i];
        if (s.action == SwitchAction::OFF || s.number == CC_OFF || (int)cc != s.number) continue;
        bool high = val >= 64;
        bool rising = high && !swHigh[i];
        swHigh[i] = high;
        applySwitchAction(s.action, high, rising, s.mode == SwitchMode::MOMENTARY);
    }
    // Pattern select: one base CC covers eight consecutive buttons, so a
    // pad bank costs one setting instead of eight switch slots (spec B.5).
    if (sys.patternBaseCc != CC_OFF) {
        int idx = (int)cc - sys.patternBaseCc;
        if (idx >= 0 && idx < NUM_PATTERN_SLOTS) {
            bool high = val >= 64;
            if (high && !patSelHigh[idx]) selectPatternSlot(idx);   // rising edge only: this reads the card
            patSelHigh[idx] = high;
        }
    }
}

static void onPitchBend(int value14) {
    live.pitchBendNorm = clampf((float)(value14 - 8192) / 8192.f, -1.f, 1.f);
}

static void onClockTick() {
    unsigned long now = millis();
    clockLastTickMs = now;
    clockTicks++;
    if (clockTicks >= 24) {
        clockTicks = 0;
        if (clockLastBeatMs != 0) {
            unsigned long beatMs = now - clockLastBeatMs;
            if (beatMs > 0) {
                float bpm = 60000.f / (float)beatMs;
                if (bpm >= 20.f && bpm <= 400.f) clockBpm += (bpm - clockBpm) * 0.6f;   // EMA 0.6 (spec 5.5)
            }
        }
        clockLastBeatMs = now;
    }
}

static void processByte(MidiParser &p, uint8_t b) {
    // Realtime bytes can sit between the data bytes of another message;
    // they must not disturb the running status.
    if (b >= 0xF8) {
        switch (b) {
            case 0xF8: onClockTick(); break;
            case 0xFA: clockTicks = 0; clockLastBeatMs = 0; arpResync(); glitchResync(); break;   // Start
            case 0xFB: arpResync(); break;                                          // Continue
            case 0xFC: clockLastTickMs = 0; break;                                  // Stop -> unlock
            default: break;                                                         // 0xFE active sensing etc.
        }
        return;
    }
    if (b & 0x80) {
        uint8_t hi = b & 0xF0;
        if (hi == MIDI_NOTE_ON || hi == MIDI_NOTE_OFF || hi == MIDI_PITCH_BEND || hi == MIDI_CC) {
            p.status = b; p.dataIdx = 0; p.dataNeeded = 2;
        } else if (hi == MIDI_PROGRAM) {
            p.status = b; p.dataIdx = 0; p.dataNeeded = 1;   // one data byte
        } else {
            p.status = 0;   // SysEx and friends: ignored
        }
        return;
    }
    if (!p.status) return;
    p.data[p.dataIdx++] = b;
    if (p.dataIdx < p.dataNeeded) return;
    p.dataIdx = 0;   // running status keeps the status byte
    if (!channelAccepted(p.status)) return;
    switch (p.status & 0xF0) {
        case MIDI_NOTE_ON:    onNoteOn(p.data[0], p.data[1]); break;
        case MIDI_NOTE_OFF:   onNoteOff(p.data[0]); break;
        case MIDI_PITCH_BEND: onPitchBend(((int)p.data[1] << 7) | p.data[0]); break;
        case MIDI_CC:         onControlChange(p.data[0], p.data[1]); break;
        case MIDI_PROGRAM:    programRequest = p.data[0]; break;
        default: break;
    }
}

#ifdef MORPHASE_BLE
// BLE bytes are taken here, in the same task and at the same rate as DIN
// (spec 3.5). A dropped link must not leave notes or the pedal down: every
// note goes off and the sustain comes up - DIN's too, since the voices do
// not know which input started them (stage 1 decision 4).
static void blePoll() {
    if (bleMidiTakeDisconnect()) {
        postAllNotesOff();
        sustainInput(SUS_PEDAL, false);
        if (arp.on) arpPanic();
        for (int i = 0; i < 128; i++) sentNote[i] = -1;
        bleParser = MidiParser();
    }
    int guard = 256;
    uint8_t b;
    while (guard-- > 0 && bleMidiRead(&b)) {
        if (b != 0xFE) lastRxMs = millis();
        processByte(bleParser, b);
    }
}
#endif

void midiPoll() {
    if (!sentNoteInit) { for (int i = 0; i < 128; i++) sentNote[i] = -1; sentNoteInit = true; }
    if (learnActive && millis() - learnStartMs > MIDI_LEARN_TIMEOUT_MS) {
        learnActive = false; uiDirty = true;
    }
#ifdef MORPHASE_BLE
    blePoll();
#endif
    if (!midiReady) return;
    int guard = 256;   // a wheel sweep sends a lot; the UI must not stall behind it
    while (midiSerial.available() && guard-- > 0) {
        uint8_t b = (uint8_t)midiSerial.read();
        rxBytes++;
        if (b != 0xFE) lastRxMs = millis();   // active sensing does not count as activity
        processByte(dinParser, b);
    }
}
