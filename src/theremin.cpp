// MORPHASE - theremin.cpp
// Theremin (v0.81): VL53L1X reading, connection watch, Mod value, Play pitch.
// See theremin.h. Taken from C.P.S. v0.999-v0.9993, lessons included:
//  * Bus Cap shares the board's own I2C bus (Wire1, keyboard + IMU). It is
//    never begun or ended here - reconfiguring it is what broke the IMU in
//    C.P.S. v0.99904. Bus Grove owns Wire on GPIO1/2 and stops DIN MIDI.
//  * Short mode, 20ms timing budget, 33ms period (budget == period gave
//    stale readings, C.P.S. v0.99905), read(false) so nothing blocks.
//  * A fresh measurement is the sign of life. Out of range = no hand, which
//    is normal and never "lost" (C.P.S. v0.99919).
//  * 2.5s of grace after starting; the first 6 readings are thrown away.
//  * A reading changes the hand state only after 3 in a row agree, so a
//    hand at the edge of the window cannot stutter notes (C.P.S. v0.99908).
//  * Smoothing runs on the raw pitch, then the scale snap (C.P.S. v0.99912).

#include "theremin.h"
#include "midi.h"
#include <Arduino.h>
#include <Wire.h>
#include <VL53L1X.h>
#include <math.h>
#include <stdio.h>

extern bool uiDirty;   // ui_common.cpp

// ---------------------------------------------------------------------------
// Scales: C.P.S. SCALES[] (main.cpp), same order. Append only - the index is
// saved in settings.json.
// ---------------------------------------------------------------------------
const ThScale TH_SCALES[] = {
    {"Chromatic",        "Chromatic",  "Chromatic",   12, {0,1,2,3,4,5,6,7,8,9,10,11}},
    {"Major",            "Major",      "Classical",   7,  {0,2,4,5,7,9,11}},
    {"Natural Minor",    "Minor",      "Classical",   7,  {0,2,3,5,7,8,10}},
    {"Dorian",           "Dorian",     "Classical",   7,  {0,2,3,5,7,9,10}},
    {"Phrygian",         "Phrygian",   "Classical",   7,  {0,1,3,5,7,8,10}},
    {"Lydian",           "Lydian",     "Classical",   7,  {0,2,4,6,7,9,11}},
    {"Mixolydian",       "Mixolydian", "Classical",   7,  {0,2,4,5,7,9,10}},
    {"Locrian",          "Locrian",    "Classical",   7,  {0,1,3,5,6,8,10}},
    {"Harmonic Minor",   "HarmMinor",  "Classical",   7,  {0,2,3,5,7,8,11}},
    {"Melodic Minor",    "MelMinor",   "Classical",   7,  {0,2,3,5,7,9,11}},
    {"Whole Tone",       "WholeTone",  "Symmetrical", 6,  {0,2,4,6,8,10}},
    {"Diminished (W-H)", "Dim W-H",    "Symmetrical", 8,  {0,2,3,5,6,8,9,11}},
    {"Diminished (H-W)", "Dim H-W",    "Symmetrical", 8,  {0,1,3,4,6,7,9,10}},
    {"Augmented",        "Augmented",  "Symmetrical", 6,  {0,3,4,7,8,11}},
    {"Major Pentatonic", "MajPenta",   "Pentatonic",  5,  {0,2,4,7,9}},
    {"Minor Pentatonic", "MinPenta",   "Pentatonic",  5,  {0,3,5,7,10}},
    {"Blues",            "Blues",      "Pentatonic",  6,  {0,3,5,6,7,10}},
    {"Egyptian (Sus)",   "Egyptian",   "Pentatonic",  5,  {0,2,5,7,10}},
    {"Hirajoshi",        "Hirajoshi",  "Japan",       5,  {0,2,3,7,8}},
    {"In Sen",           "In Sen",     "Japan",       5,  {0,1,5,7,10}},
    {"Iwato",            "Iwato",      "Japan",       5,  {0,1,5,6,10}},
    {"Kumoi",            "Kumoi",      "Japan",       5,  {0,2,3,7,9}},
    {"Yo Scale",         "Yo",         "Japan",       5,  {0,2,5,7,9}},
    {"Ryukyu (Okinawa)", "Ryukyu",     "Japan",       5,  {0,4,5,7,11}},
    {"Shang Mode",       "Shang",      "China",       5,  {0,2,5,7,10}},
    {"Jue Mode",         "Jue",        "China",       5,  {0,3,5,8,10}},
    {"Zhi Mode",         "Zhi",        "China",       5,  {0,2,5,7,9}},
    {"Bhairav",          "Bhairav",    "India",       7,  {0,1,4,5,7,8,11}},
    {"Todi",             "Todi",       "India",       7,  {0,1,3,6,7,8,11}},
    {"Hijaz",            "Hijaz",      "Middle East", 7,  {0,1,4,5,7,8,10}},
    {"Double Harmonic",  "DblHarm",    "Middle East", 7,  {0,1,4,5,7,8,11}},
    {"Hungarian Minor",  "HungMinor",  "Europe",      7,  {0,2,3,6,7,8,11}},
    {"Neapolitan Minor", "NeapMinor",  "Europe",      7,  {0,1,3,5,7,8,11}},
    {"Harmonic Major",   "HarmMajor",  "Classical",   7,  {0,2,4,5,7,8,11}},
    {"Neapolitan Major", "NeapMajor",  "Classical",   7,  {0,1,3,5,7,9,11}},
    {"Lydian Augmented", "LydAug",     "Classical",   7,  {0,2,4,6,8,9,11}},
    {"Lydian Dominant",  "LydDom",     "Classical",   7,  {0,2,4,6,7,9,10}},
    {"Messiaen Mode 3",  "Messiaen3",  "Symmetrical", 9,  {0,2,3,4,6,7,8,10,11}},
    {"Messiaen Mode 6",  "Messiaen6",  "Symmetrical", 8,  {0,2,4,5,6,8,10,11}},
    {"Major b6 Pent.",   "MajB6Pent",  "Pentatonic",  5,  {0,2,4,7,8}},
    {"Pentatonic b5",    "PentaB5",    "Pentatonic",  5,  {0,3,5,6,10}},
    {"Ahir Bhairav",     "AhirBhrv",   "India",       7,  {0,1,4,5,7,9,10}},
    {"Marva",            "Marva",      "India",       7,  {0,1,4,6,7,9,11}},
    {"Purvi",            "Purvi",      "India",       7,  {0,1,4,6,7,8,11}},
    {"Charukeshi",       "Charukeshi", "India",       7,  {0,2,4,5,7,8,10}},
    {"Nikriz",           "Nikriz",     "Middle East", 7,  {0,2,3,6,7,9,10}},
    {"Persian",          "Persian",    "Middle East", 7,  {0,1,4,5,6,8,11}},
    {"Romanian Minor",   "RomMinor",   "Europe",      7,  {0,2,3,6,7,9,10}},
    {"Hungarian Major",  "HungMajor",  "Europe",      7,  {0,3,4,6,7,9,10}},
};
const int TH_SCALE_COUNT = sizeof(TH_SCALES) / sizeof(TH_SCALES[0]);

// ---------------------------------------------------------------------------
static VL53L1X tof;
constexpr uint8_t       TOF_ADDR  = 0x29;
constexpr int           GROVE_SDA = 2, GROVE_SCL = 1;   // the Unit MIDI pins
constexpr unsigned long POLL_MS   = 15;     // faster than the 33ms period
constexpr unsigned long GRACE_MS  = 2500;   // after (re)starting: no watch yet
constexpr unsigned long LOST_MS   = 1500;   // no fresh measurement this long = lost
constexpr unsigned long RETRY_MS  = 500;    // Grove re-probe has blocking delays: no faster
constexpr int           CONFIRM   = 3;
constexpr float         C4_HZ     = 261.6256f;

static bool  running = false;        // the sensor side is on (sys.thOn as applied)
static ThBus runBus = ThBus::CAP;
static bool  present = false;        // initialised and measuring
static bool  lostFlag = false;       // was measuring, then stopped answering
static unsigned long startedMs = 0, lastPollMs = 0, lastAliveMs = 0, lastRetryMs = 0, lastTickMs = 0;
static int   goodCount = 0, badCount = 0;
static int   lastMm = 0;             // last valid reading, any distance (display)
static bool  hand = false;           // a hand confirmed inside Near..Far
static float handT = 0.f;            // 0 at Near .. 1 at Far
static int   scanCount = -1;         // Rescan result, -1 = not scanned
static float rawHz = 0.f, outHz = 0.f;
// Mod: the value walks between readings instead of stepping every 33ms
static float modFrom = 0.f, modTo = 0.f, modOut = 0.f;
static unsigned long modReadMs = 0, modRampMs = 33;

static TwoWire *bus() { return runBus == ThBus::CAP ? &Wire1 : &Wire; }

static bool probe(TwoWire *b) { b->beginTransmission(TOF_ADDR); return b->endTransmission() == 0; }

static int scanAll(TwoWire *b) {
    int n = 0;
    for (uint8_t a = 1; a < 127; a++) { b->beginTransmission(a); if (b->endTransmission() == 0) n++; }
    return n;
}

static void handGone() {
    hand = false;
    rawHz = 0.f;                     // the next note starts at its own pitch
}

// Find and start the sensor. fullScan (Rescan only) also counts every device
// on the bus - 126 transactions, too slow for every retry (C.P.S. v0.99912).
static bool sensorStart(bool fullScan) {
    TwoWire *b = bus();
    bool found = false;
    if (runBus == ThBus::GROVE) {
        // Which Grove pin is SDA: both orders, as C.P.S. does
        static const int PAIRS[2][2] = { { GROVE_SDA, GROVE_SCL }, { GROVE_SCL, GROVE_SDA } };
        for (int i = 0; i < 2 && !found; i++) {
            b->end(); delay(5);
            b->begin(PAIRS[i][0], PAIRS[i][1], 400000); delay(10);
            if (fullScan) scanCount = scanAll(b);
            found = probe(b);
            if (!found) b->end();
        }
    } else {
        if (fullScan) scanCount = scanAll(b);
        found = probe(b);
    }
    if (!found) { present = false; return false; }
    tof.setBus(b);
    tof.setTimeout(50);              // each failed transaction stalls loop() at most this long
    if (!tof.init()) {
        if (runBus == ThBus::GROVE) b->end();
        present = false;
        return false;
    }
    tof.setDistanceMode(VL53L1X::Short);
    tof.setMeasurementTimingBudget(20000);
    tof.startContinuous(33);
    goodCount = -6; badCount = 0;    // the first readings after a start are unreliable
    startedMs = lastAliveMs = millis();
    present = true; lostFlag = false;
    Serial.printf("[ToF] ready on %s\n", runBus == ThBus::CAP ? "Cap (G8/G9)" : "Grove (G1/G2)");
    return true;
}

static void sensorStop() {
    if (present) tof.stopContinuous();
    if (runBus == ThBus::GROVE) { Wire.end(); midiDinResume(); }
    present = false; lostFlag = false;
    handGone();
}

// Follow sys.thOn / sys.thBus. Cheap enough for every loop() pass.
static void applyRun() {
    bool want = sys.thOn;
    if (running && (!want || runBus != sys.thBus)) { sensorStop(); running = false; }
    if (want && !running) {
        running = true;
        runBus = sys.thBus;
        scanCount = -1;
        if (runBus == ThBus::GROVE) midiDinSuspend();   // only Grove touches the MIDI pins
        lastRetryMs = millis();
        sensorStart(false);
    }
}

static float quantize(float hz, float topHz) {
    const ThScale &s = TH_SCALES[sys.thScale < 0 ? 0 : (sys.thScale >= TH_SCALE_COUNT ? TH_SCALE_COUNT - 1 : sys.thScale)];
    float target = 12.f * log2f(hz / topHz);          // semitones from the top, negative
    float best = 0.f, bestDist = 1e9f;
    for (int oct = -(sys.thSpan + 1); oct <= 1; oct++)
        for (int d = 0; d < s.len; d++) {
            float semis = (float)s.iv[d] + oct * 12.f;
            float dist = fabsf(semis - target);
            if (dist < bestDist) { bestDist = dist; best = semis; }
        }
    return topHz * powf(2.f, best / 12.f);
}

// A confirmed reading inside the window
static void newReading(unsigned long now) {
    // Mod: near = 1. After a gap the value comes back over 150ms, not 33.
    modRampMs = hand ? 33 : 150;
    modFrom = modOut; modTo = 1.f - handT; modReadMs = now;
    // Play: Top is independent of the octave keys; Transpose moves it (C.P.S. v0.99921)
    float topHz = C4_HZ * powf(2.f, (float)(sys.thTopSemis + sys.transpose) / 12.f);
    float hz = topHz * powf(2.f, -handT * (float)sys.thSpan);
    if (rawHz <= 0.f) rawHz = hz;
    else rawHz += (hz - rawHz) * 0.25f;
    outHz = (sys.thPitch == ThPitch::SEMITONE) ? quantize(rawHz, topHz) : rawHz;
    hand = true;
}

static void poll(unsigned long now) {
    if (!present) {
        if (now - lastRetryMs >= RETRY_MS) { lastRetryMs = now; sensorStart(false); }
        return;
    }
    bool ready = tof.dataReady() && tof.last_status == 0;
    if (!ready) {
        if (now - startedMs >= GRACE_MS && now - lastAliveMs >= LOST_MS) {
            Serial.println("[ToF] not responding - retrying");
            present = false; lostFlag = true;
            if (runBus == ThBus::GROVE) Wire.end();
            handGone();
            lastRetryMs = now;
        }
        return;
    }
    lastAliveMs = now;
    int mm = (int)tof.read(false);
    bool valid = tof.ranging_data.range_status == VL53L1X::RangeValid;
    lastMm = valid ? mm : 0;
    bool usable = valid && mm >= sys.thNearMm && mm <= sys.thFarMm;
    if (!usable) {
        if (goodCount > 0) goodCount = 0;
        if (++badCount >= CONFIRM) { badCount = CONFIRM; if (hand) handGone(); }
        return;
    }
    badCount = 0;
    if (goodCount < 0) { goodCount++; return; }          // still settling after the start
    if (!hand && ++goodCount < CONFIRM) return;          // a hand arrives after 3 in a row
    goodCount = CONFIRM;
    int span = sys.thFarMm - sys.thNearMm; if (span < 1) span = 1;
    handT = (float)(mm - sys.thNearMm) / (float)span;
    if (handT < 0.f) handT = 0.f; if (handT > 1.f) handT = 1.f;
    newReading(now);
}

// ---------------------------------------------------------------------------
void thereminInit() {
    live.thModNorm = 0.f; live.thReserve = false; live.thGate = false;
    live.thHz = C4_HZ; live.thVol = 1.f;
    lastTickMs = millis();
    applyRun();
}

void thereminUpdate() {
    applyRun();
    unsigned long now = millis();
    float dt = (float)(now - lastTickMs) / 1000.f; if (dt > 0.1f) dt = 0.1f;
    lastTickMs = now;
    if (running && now - lastPollMs >= POLL_MS) { lastPollMs = now; poll(now); }

    bool modMode  = running && sys.thMode == ThMode::MOD;
    bool playMode = running && sys.thMode == ThMode::PLAY;

    // Mod value: walked between readings; with no hand, Hold keeps it and
    // Return glides it to 0 in 250ms. Leaving Mod always returns it.
    if (modMode && hand) {
        float k = (float)(now - modReadMs) / (float)modRampMs; if (k > 1.f) k = 1.f;
        modOut = modFrom + (modTo - modFrom) * k;
    } else if (!modMode || sys.thRelease == ThRelease::RETURN) {
        modOut -= dt * 4.f; if (modOut < 0.f) modOut = 0.f;
    }
    live.thModNorm = modOut;

    // Play: the pitch first, then the gate, so a new note never starts on
    // the previous note's pitch.
    live.thHz = (outHz > 0.f) ? outHz : C4_HZ;
    live.thReserve = playMode;
    live.thGate = playMode && hand;
    if (thereminVolAxis() == 0) live.thVol = 1.f;          // else main.cpp writes it from the tilt
}

void thereminSetEnabled(bool on) { sys.thOn = on; applyRun(); uiDirty = true; }
void thereminSettingsChanged() { applyRun(); }

void thereminRescan() {
    if (!running) { scanCount = -1; return; }
    if (present) tof.stopContinuous();
    present = false;
    lastRetryMs = millis();
    sensorStart(true);
}

int thereminVolAxis() {
    if (!sys.thOn || sys.thMode != ThMode::PLAY) return 0;
    if (sys.thVolAxis == ThVolAxis::AXIS_X) return 1;
    if (sys.thVolAxis == ThVolAxis::AXIS_Y) return 2;
    return 0;
}

ThState thereminState() {
    if (!running) return TH_OFF;
    if (present) return TH_OK;
    return lostFlag ? TH_LOST : TH_SEARCHING;
}
const char *thereminStateName() {
    static const char *N[4] = { "Off", "Searching", "OK", "Lost" };
    return N[thereminState()];
}
int   thereminDistanceMm() { return (running && present) ? lastMm : 0; }
float thereminBarValue()   { return (running && present && hand) ? 1.f - handT : -1.f; }

const char *thereminScanLabel() {
    static char b[16];
    if (scanCount < 0) return "--";
    // On Cap the count includes the keyboard, the IMU and the rest of the
    // board's bus (C.P.S. v0.99919)
    snprintf(b, sizeof(b), runBus == ThBus::CAP ? "%d shared" : "%d found", scanCount);
    return b;
}

const char *thereminNoteName() {
    static char b[8];
    if (!(running && present && hand) || sys.thMode != ThMode::PLAY || outHz <= 0.f) return "";
    static const char *NAMES[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int n = (int)lroundf(69.f + 12.f * log2f(outHz / 440.f));
    if (n < 0) n = 0; if (n > 127) n = 127;
    snprintf(b, sizeof(b), "%s%d", NAMES[n % 12], n / 12 - 1);
    return b;
}
