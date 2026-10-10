// MORPHASE - 4-operator FM polyphonic synthesizer for M5Stack Cardputer ADV
// main.cpp - setup(), loop(), task creation, global keys, IMU, watchdog.
//
// 100% vibe-coded with Claude. See README.md.
//
// Thread model (spec 11.1): loop() runs on APP_CPU and owns the UI, MIDI
// parsing, the arpeggiator and the IMU. audioTask runs on PRO_CPU and owns
// every voice. The two meet only through NoteEvents, parameter structs and
// the `live` control block.

#include <M5Cardputer.h>
#include "config.h"
#include "fm.h"
#include "voice.h"
#include "fx.h"
#include "midi.h"
#include "ble_midi.h"
#include "arp.h"
#include "patch.h"
#include "morph.h"
#include "glitch.h"
#include "theremin.h"
#include "ui_common.h"
#include "ui_screens.h"
#include "splash.h"
#include <math.h>
#include <string.h>

static bool isCardputerAdv = false;
static bool imuOk = false;

// ---------------------------------------------------------------------------
// IMU (spec 4.4). Offsets are relative to the base value and are resolved
// in audioTask; loop() only publishes the normalised tilt per axis, already
// reduced by enable / hold.
// ---------------------------------------------------------------------------
constexpr float TILT_MAX_DEGREES = 35.f;   // full travel at Sens 100%
static float imuXRaw = 0.f, imuYRaw = 0.f;
float imuAngleXDeg = 0.f, imuAngleYDeg = 0.f;   // raw, shared with the UI (calibrate / pad)

// angle -> -1..1 with the calibration zero point removed (v0.12), then the
// per-axis response from SETTING > IMU (v0.4): sensitivity narrows or widens
// the angle that counts as full travel, the deadzone is taken out and the
// rest rescaled so full travel still reaches 1, Exp squares the magnitude,
// and Invert flips the sign last. Hold captures this processed value.
static float tiltNorm(float angleDeg, float calDeg, int sens, int deadzone,
                      ImuCurve curve, bool invert) {
    float maxDeg = TILT_MAX_DEGREES * 100.f / (float)clampi(sens, 50, 200);
    float n = clampf((angleDeg - calDeg) / maxDeg, -1.f, 1.f);
    float dz = (float)clampi(deadzone, 0, 30) / 100.f;
    float a = fabsf(n);
    if (a <= dz) return 0.f;
    a = (a - dz) / (1.f - dz);
    if (curve == ImuCurve::EXP) a *= a;
    float sgn = n < 0.f ? -1.f : 1.f;
    if (invert) sgn = -sgn;
    return sgn * a;
}

// Tilt has a neutral position, so a control destination driven by the IMU
// is an offset around the stored setting rather than an absolute value the
// way a knob is (see the note on ModTarget in config.h). The offsets are
// smoothed because tempo reacts badly to accelerometer jitter.
static void addImuControl(ModTarget t, float norm, float depth,
                          float &tempo, float &swing, float &gate, float &glide) {
    float d = clampf(depth, 0.f, 100.f) / 100.f;
    switch (t) {
        case ModTarget::ARP_TEMPO:  tempo += norm * d * 60.f;  break;   // +-60 BPM
        case ModTarget::ARP_SWING:  swing += norm * d * 50.f;  break;   // +-50 %
        case ModTarget::ARP_GATE:   gate  += norm * d * 50.f;  break;   // +-50 %
        // Glide rests at 0, so it takes the magnitude and sweeps up
        // whichever way the device leans (matches imuUnipolar in voice.cpp).
        case ModTarget::GLIDE_TIME: glide += fabsf(norm) * d * 1000.f; break;   // 0..1000 ms
        default: break;
    }
}

static void updateImu() {
    if (!imuOk) return;
    static unsigned long lastMs = 0;
    unsigned long now = millis();
    if (now - lastMs < 20) return;          // 50Hz is plenty for a tilt control
    lastMs = now;
    if (!M5.Imu.update()) return;
    auto d = M5.Imu.getImuData();
    imuAngleXDeg = asinf(clampf(d.accel.x, -1.f, 1.f)) * 180.f / 3.14159265f;
    imuAngleYDeg = asinf(clampf(d.accel.y, -1.f, 1.f)) * 180.f / 3.14159265f;
    imuXRaw = tiltNorm(imuAngleXDeg, sys.imuCalibrated ? sys.imuXCalDeg : 0.f,
                       sys.imuXSens, sys.imuXDeadzone, sys.imuXCurve, sys.imuXInvert);
    imuYRaw = tiltNorm(imuAngleYDeg, sys.imuCalibrated ? sys.imuYCalDeg : 0.f,
                       sys.imuYSens, sys.imuYDeadzone, sys.imuYCurve, sys.imuYInvert);
    float x = 0.f, y = 0.f;
    if (sys.imuXEnabled) x = sys.imuXHeld ? sys.imuXHeldNorm : imuXRaw;
    if (sys.imuYEnabled) y = sys.imuYHeld ? sys.imuYHeldNorm : imuYRaw;
    // Theremin Play (v0.81): the VolAxis tilt is the theremin's volume - level
    // = half, one way up to full, the other down to silence (hold freezes it,
    // the axis's On/Off does not apply). That axis's own destinations rest
    // meanwhile, so one tilt never moves both (instruction (6) 1.5).
    int va = thereminVolAxis();
    if (va) {
        float n = (va == 1) ? (sys.imuXHeld ? sys.imuXHeldNorm : imuXRaw)
                            : (sys.imuYHeld ? sys.imuYHeldNorm : imuYRaw);
        live.thVol = clampf((n + 1.f) * 0.5f, 0.f, 1.f);
        if (va == 1) x = 0.f; else y = 0.f;
    }
    live.imuXNorm = x;
    live.imuYNorm = y;

    float tempo = 0.f, swing = 0.f, gate = 0.f, glide = 0.f;
    if (patch.imuXTarget != ModTarget::OFF && patch.imuXDepth > 0.f)
        addImuControl(patch.imuXTarget, x, patch.imuXDepth, tempo, swing, gate, glide);
    if (patch.imuYTarget != ModTarget::OFF && patch.imuYDepth > 0.f)
        addImuControl(patch.imuYTarget, y, patch.imuYDepth, tempo, swing, gate, glide);
    // Theremin Mod on a control destination (v0.81): the same offsets
    if (sys.thTarget != ModTarget::OFF && sys.thDepth > 0.f && live.thModNorm > 0.f)
        addImuControl(sys.thTarget, live.thModNorm, sys.thDepth, tempo, swing, gate, glide);
    ctrlMod.tempoOff += (tempo - ctrlMod.tempoOff) * 0.25f;
    ctrlMod.swingOff += (swing - ctrlMod.swingOff) * 0.25f;
    ctrlMod.gateOff  += (gate  - ctrlMod.gateOff)  * 0.25f;
    ctrlMod.glideOff += (glide - ctrlMod.glideOff) * 0.25f;
}

// ---------------------------------------------------------------------------
// Keyboard watchdog (spec 11.6): a non-empty, unchanging key set for 30s
// means the TCA8418 is stuck; the only recovery is a restart.
// ---------------------------------------------------------------------------
static char lastKbWord[24] = {0};
static unsigned long lastKbChangeMs = 0;

static void updateKeyboardWatchdog(const KeyFrame &k) {
    static unsigned long lastCheckMs = 0;
    unsigned long now = millis();
    if (now - lastCheckMs < 1000) return;
    lastCheckMs = now;
    char w[24] = {0}; int n = 0;
    for (int i = 0; i < k.wordLen && n < 20; i++) w[n++] = k.word[i];
    // Sort: the library does not guarantee character order between reads
    for (int i = 1; i < n; i++) { char key = w[i]; int j = i - 1; while (j >= 0 && w[j] > key) { w[j + 1] = w[j]; j--; } w[j + 1] = key; }
    if (strcmp(w, lastKbWord) != 0) { strcpy(lastKbWord, w); lastKbChangeMs = now; return; }
    if (w[0] == 0) return;                   // nothing held: normal
    if (now - lastKbChangeMs >= KB_WATCHDOG_TIMEOUT_MS) {
        Serial.printf("[watchdog] keyboard word \"%s\" unchanged for %lums - restarting\n", w, now - lastKbChangeMs);
        delay(50);
        ESP.restart();
    }
}

// ---------------------------------------------------------------------------
// Diagnostics printing (loop side)
// ---------------------------------------------------------------------------
static void printDiagIfPending() {
    if (!diag.pending) return;
    unsigned long count = diag.count, sum = diag.sumUs, mx = diag.maxUs, over = diag.overCount;
    int tail = diag.tailBufs;
    diag.pending = false;
    if (count == 0) return;
    // v0.87 clean-up: only a second with a late buffer is printed. Build with
    // -DMORPHASE_VERBOSE to get every second back (for looking into a report).
#ifndef MORPHASE_VERBOSE
    if (over == 0) return;
#endif
    Serial.printf("[audioTask] %lu buffers/s  avg %lu us  max %lu us  over budget %lu  (tail %d)  voices %d  heap %u\n",
                  count, sum / count, mx, over, tail, audioStatus.activeVoices, (unsigned)ESP.getFreeHeap());
}

// ---------------------------------------------------------------------------
// Global keys (spec 8.2) - handled before the screen-specific ones
// ---------------------------------------------------------------------------
static bool zHeld = false, xHeld = false;
static RepeatKey volUp = {false, 0, 0}, volDown = {false, 0, 0};

static void handleGlobalKeys(const KeyFrame &k) {
    // Sub-screens own Tab / Enter entirely
    bool subScreen = uiScreenConsumesTab();

    if (k.tabPressed && !subScreen) {
        if (appMode == AppMode::SETTING) saveRequested = true;   // v0.90T3: 3 s later, not now
        uiSetMode(uiNextTab(appMode, k.shift));
    }
    // Help (Shift+H -> 'H')
    if (keyPressed(k, 'H')) { helpVisible = !helpVisible; uiDirty = true; }

    // Name entry needs the letters: skip the letter shortcuts there
    // g: GLITCH while held (v0.6). Read every frame from the keys down now,
    // so a missed release cannot leave it stuck; typing a name never holds it.
    glitchSetKeyHeld(appMode != AppMode::NAME_ENTRY && (keyDown(k, 'g') || keyDown(k, 'G')));
    if (appMode == AppMode::NAME_ENTRY) return;
#ifdef MORPHASE_SCREENSHOT
    if (keyPressed(k, 'P')) uiScreenshotArm();   // Shift+P: the screen 5s later, for the manuals
#endif

    // IMU hold / enable
    if (keyPressed(k, 'a')) { sys.imuXHeld = !sys.imuXHeld; if (sys.imuXHeld) sys.imuXHeldNorm = imuXRaw; uiDirty = true; }
    if (keyPressed(k, 's')) { sys.imuYHeld = !sys.imuYHeld; if (sys.imuYHeld) sys.imuYHeldNorm = imuYRaw; uiDirty = true; }
    if (keyPressed(k, 'A')) { sys.imuXEnabled = !sys.imuXEnabled; uiDirty = true; }
    if (keyPressed(k, 'S')) { sys.imuYEnabled = !sys.imuYEnabled; uiDirty = true; }
    // Pitch bend keys Z / X (held)
    bool z = keyDown(k, 'z') || keyDown(k, 'Z');
    bool x = keyDown(k, 'x') || keyDown(k, 'X');
    if (z != zHeld || x != xHeld) {
        zHeld = z; xHeld = x;
        live.pitchBendNorm = z ? -1.f : (x ? 1.f : 0.f);
    }
    // D: note hold TOGGLE (v0.11 - was momentary, which read as "not
    // working" because a tap released it again). Same engine path as the
    // sustain pedal; either source holds, both off releases.
    if (keyPressed(k, 'd') || keyPressed(k, 'D')) {
        sustainInput(SUS_KEY, !sustainKeyHeld());
        uiDirty = true;
    }
    // K / L: master volume, 1% steps, hold to repeat (v0.11)
    if (repeatFire(volDown, keyDown(k, 'k') || keyDown(k, 'K'))) { sys.masterVolume = clampi(sys.masterVolume - 1, 0, 100); uiDirty = true; }
    if (repeatFire(volUp,   keyDown(k, 'l') || keyDown(k, 'L'))) { sys.masterVolume = clampi(sys.masterVolume + 1, 0, 100); uiDirty = true; }
    // ARP on/off (Shift+V -> 'V'), Latch (V -> 'v')
    if (keyPressed(k, 'V')) { arpSetOn(!arp.on); uiDirty = true; }
    if (keyPressed(k, 'v')) { arpToggleLatch(); uiDirty = true; }
    // Tap tempo
    if (k.enterPressed && k.shift) { arpTapTempo(); uiDirty = true; }
    // q w e r t: bypass Chorus / Delay / Reverb / Crush / Limiter (v0.56).
    // Edge only, so a held key does not flip it again.
    static const char BYP_KEYS[FX_PAD_COUNT] = { 'q', 'w', 'e', 'r', 't' };
    for (int i = 0; i < FX_PAD_COUNT; i++)
        if (keyPressed(k, BYP_KEYS[i])) { uiFxBypassKey(i); uiDirty = true; }
    // Shift+1..8: glide to that morph slot's patch over Morph Time (v0.51).
    // The keyboard reports the shifted characters, not the digits.
    static const char SHIFTED_DIGITS[MORPH_SLOTS] = { '!', '@', '#', '$', '%', '^', '&', '*' };
    for (int i = 0; i < MORPH_SLOTS; i++)
        if (keyPressed(k, SHIFTED_DIGITS[i])) { morphGlideTo(i); uiDirty = true; }
}

// Volume, octave and transpose change from single key presses, often in
// runs. Writing the card on each press would wear it and stall loop(), so
// they are saved once nothing has changed for SAVE_DEFER_MS (v0.42, the
// CONNECTOME rule) - and still survive a power-off.
constexpr unsigned long SAVE_DEFER_MS = 3000;
// v0.91: and only while nothing is being played - a save stalls loop()
// (MIDI in, the ARP, the screen; an external clock reads its beat wrong)
// for ~130-260ms. Not a voice sounding (held, sustained, Note Hold, a
// release tail, the theremin's Play voice - audioTask counts them all),
// the ARP not stepping, no Shift+number glide. FX tails do not count: they
// keep ringing on the other core. A running chain does not count either:
// with Chain on it never stops, and with no voice there is nothing to hear.
static bool performanceIdle() {
    return audioStatus.activeVoices == 0 && !live.thGate && !arpIsRunning() && !morphGliding();
}
static void deferredPerformSave() {
    // v0.82: the chain pause too (a patch loaded over a running chain), so
    // a power-off right after still boots into that patch
    // v0.90T3: and leaving SET (Tab) or an edit tab (G0) - saveRequested.
    // v0.90T2: and a patch loaded (SET > Load, Program Change, Pch+/-) -
    // written once the loading stops, not on each load (a save stalls
    // loop() ~260ms: MIDI, ARP and the screen wait for the card)
    static int lastVol = -999, lastOct = -999, lastTrn = -999;
    static bool lastPaused = false;
    static uint32_t lastLoad = 0;
    static unsigned long changedMs = 0;
    static bool pending = false;
    // v0.92T2: a theme that NVS does not hold yet (changed in SET, or the
    // first boot of this version) is written there with the next save
    if (lastVol == -999) { lastVol = sys.masterVolume; lastOct = sys.octave; lastTrn = sys.transpose; lastPaused = morphChainPaused(); lastLoad = patchLoadSeq;
                           changedMs = millis(); pending = nvsThemeStale(); return; }
    unsigned long now = millis();
    bool paused = morphChainPaused();
    if (sys.masterVolume != lastVol || sys.octave != lastOct || sys.transpose != lastTrn || paused != lastPaused || patchLoadSeq != lastLoad
        || saveRequested) {
        lastVol = sys.masterVolume; lastOct = sys.octave; lastTrn = sys.transpose; lastPaused = paused; lastLoad = patchLoadSeq;
        saveRequested = false;
        changedMs = now; pending = true;
    }
    if (pending && now - changedMs >= SAVE_DEFER_MS && performanceIdle()) { pending = false; saveSettings(); nvsWriteTheme(); }
}

static void handleG0() {
    // G0 short press: PLAY <-> EDIT (the last edit tab)
    if (M5Cardputer.BtnA.wasReleased()) {
        if (appMode == AppMode::PLAY) uiSetMode(lastEditMode);
        else { saveRequested = true; uiSetMode(AppMode::PLAY); }   // v0.90T3: edits saved 3 s later
    }
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(921600);
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    isCardputerAdv = (M5.getBoard() == m5::board_t::board_M5CardputerADV);
    Serial.printf("[Board] %s\n", isCardputerAdv ? "CardputerADV" : "not an ADV - this firmware targets the ADV only");
    M5Cardputer.Display.setRotation(1);

    M5Cardputer.Display.fillScreen(BLACK);

    // Defaults before anything reads them
    patchInitDefaults(patch);
    sysInitDefaults();
    arpInitDefaults();
    voiceEngineInit();
    buildWaveTables();
    clearFxBuffers();

    // Splash (v0.89) at power-on (v0.92T2), before the card: its colour is
    // the theme number NVS keeps a copy of (Green when there is none). It
    // runs before the canvas, the audio task and BLE, and uses no buffer.
    // All the SD work - looking for the card, the settings, the factory
    // install (first boot, or a new factory set), the patch list, the
    // chain - happens inside it and steps the animation (bootProgress).
    // The settings' theme is applied only after it, so the colour never
    // changes half-way (it matches from the next boot).
    int nvsTheme = nvsReadTheme();
    if (nvsTheme >= 0) sys.theme = (UiTheme)nvsTheme;
    applyUiTheme();
    splashBegin();
    bootProgress = splashFactoryProgress;

    Serial.printf("[Mem] free heap %u, largest block %u (before SD)\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    bool sdOk = sdInit();
    splashStartWave();                   // v0.92T3: a flat line until here (SD.begin() blocks)
    Serial.printf("[Mem] free heap %u (after SD)\n", (unsigned)ESP.getFreeHeap());
    if (sdOk) {
        ensureFolders();
        loadSettings();
        ensureFactoryPatches();          // lists the patch folder on every path (v0.92T2: no second scanPatches())
        morphInit();
        Serial.println("[SD] OK");
    } else {
        Serial.println("[SD] not found - running from defaults, nothing will be saved");
    }
    bootProgress = nullptr;
    splashFinish();
    applyUiTheme();                      // the settings' theme from here on
    applyUiBrightness();
    Serial.printf("[Mem] free heap %u, largest block %u (after splash)\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    // UI (canvas after the SD mount so the mount gets first claim on DRAM)
    uiInitCanvas();
    uiScreensInit();

    midiBegin();
    imuOk = isCardputerAdv && M5.Imu.begin();
    Serial.println(imuOk ? "[IMU] OK" : "[IMU] not found");
    thereminInit();          // v0.81: after MIDI (Grove takes its pins) and the IMU (Cap shares its bus)

    // Speaker: 8 x 512 DMA buffers, feed task on APP_CPU next to loop()
    auto sc = M5Cardputer.Speaker.config();
    sc.sample_rate = SAMPLE_RATE;
    sc.dma_buf_count = AUDIO_DMA_BUF_COUNT;
    sc.dma_buf_len = AUDIO_DMA_BUF_LEN;
    sc.task_pinned_core = SPEAKER_TASK_CORE;
    M5Cardputer.Speaker.config(sc);
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(255);

    xTaskCreatePinnedToCore(audioTask, "audioTask", AUDIO_TASK_STACK, nullptr, AUDIO_TASK_PRIO, nullptr, AUDIO_TASK_CORE);

#ifdef MORPHASE_BLE
    // BLE MIDI (v0.77): with the setting Off the BT stack never starts. A
    // change of the setting applies at the next boot.
    if (sys.bleMidiEnabled) bleMidiBegin();
    else Serial.println("[BLE] off (SETTING > MIDI > BLE)");
#endif

    Serial.printf("[MORPHASE] %s" MORPHASE_EDITION " ready. voices=%d buffer=%d budget=%luus\n", MORPHASE_VERSION, MAX_VOICES, AUDIO_BUFFER_SIZE, AUDIO_BUDGET_US);
    uiDirty = true;
}

// ---------------------------------------------------------------------------
void loop() {
    M5Cardputer.update();

    midiPoll();
    arpUpdate();
    thereminUpdate();      // v0.81, before the IMU: it decides the VolAxis
    updateImu();
    deferredPerformSave();
    morphPoll();
    glitchSetBpm(arpEffectiveBpm());       // GLITCH grid: ARP tempo / MIDI clock, ARP on or off

    // Program Change -> patch by index (spec 7.2), or a morph glide (PC Mode, v0.8)
    int pc = midiTakeProgramRequest();
    if (pc >= 0) {
        if (sys.pcMode == PcMode::MORPH) morphProgramChange(pc);
        else uiLoadPatchByIndex(pc);
    }

    KeyFrame k;
    uiPollKeys(k);
    AppMode before = appMode;
    handleG0();
    handleGlobalKeys(k);   // every pass: the volume keys auto-repeat while held
    uiScreenHandleKeys(k);
    if (appMode != before) uiOnModeEntered(appMode);
    updateKeyboardWatchdog(k);

    // Redraw: on demand (throttled) plus a slow periodic refresh for live
    // values. uiColor & co are only ever changed by the input handlers
    // above, so by the time we draw they are already right for this frame.
    static unsigned long lastDrawMs = 0;
    static AppMode lastDrawnMode = AppMode::APP_MODE_COUNT;
    unsigned long now = millis();
    bool modeChanged = (appMode != lastDrawnMode);
    bool periodic = uiScreenWantsPeriodicRedraw() ? (now - lastDrawMs >= PLAY_REDRAW_MS) : (now - lastDrawMs >= 250);
    if (modeChanged || (uiDirty && now - lastDrawMs >= MIN_REDRAW_MS) || periodic) {
        uiDrawScreen();
        lastDrawMs = now; lastDrawnMode = appMode; uiDirty = false;
#ifdef MORPHASE_SCREENSHOT
        uiScreenshotPoll();      // right after a fresh frame
#endif
    }

    printDiagIfPending();
    // No delay() here on purpose (spec 11.1); loop() pacing comes from
    // M5Cardputer.update() and the display push.
}
