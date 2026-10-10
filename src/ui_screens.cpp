// MORPHASE - ui_screens.cpp
// PLAY / OP / ALGO / ARP / LFO / FX / SETTING screens and the patch
// browser, name entry and confirm dialogs.
//
// Layout constants: 6x8 font, tab bar y=0..10, content y=12..124, nav
// hint at y=126. UI labels are kept short on purpose (spec 8.6).

#include "ui_screens.h"
#include "ble_midi.h"
#include "fm.h"
#include "voice.h"
#include "arp.h"
#include "morph.h"
#include "glitch.h"
#include "theremin.h"
#include "midi.h"
#include "patch.h"
#include "fx.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Shared small helpers
// ---------------------------------------------------------------------------
static const char *noteName(int midi) {
    static const char *N[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static char buf[6];
    if (midi < 0) return "--";
    snprintf(buf, sizeof(buf), "%s%d", N[midi % 12], midi / 12 - 1);
    return buf;
}

// ===========================================================================
// OP tab
// ===========================================================================
// v0.77: one item table shared by every list screen (it was a table per
// screen and per SETTING category: 168 items x 64 bytes = 10.7KB). A screen
// fills it when it is drawn or handles a key and the table holds some other
// list - only one list is ever on screen. The binds only point items at
// fields that live for the whole run, so rebuilding is always safe; it
// happens between frames, never while a key handler is using the items.
constexpr int ITEM_POOL_SIZE = 13;                 // the longest list (ARP, SETTING > IMU)
static MenuItem itemPool[ITEM_POOL_SIZE];
enum ItemOwner : int { IO_NONE = -1, IO_OP = 0, IO_ALGO, IO_ARP, IO_LFO,
                       IO_FX0 = 10, IO_SET0 = 20, IO_MIDI_CC = 30, IO_MIDI_SW };
static int itemPoolOwner = IO_NONE;

static ListNav  opNav;

// True for the four destinations that act on the selected operator.
static bool targetIsOp(ModTarget t) {
    return t >= ModTarget::OP_MORPH && t <= ModTarget::OP_DETUNE;
}
// Marker for "some OP destination is live", shown next to the selected
// operator so it is obvious which one the tilt or the LFO is reaching.
static const char *opModMarker() {
    bool imu = (sys.imuXEnabled && targetIsOp(patch.imuXTarget) && patch.imuXDepth > 0.f) ||
               (sys.imuYEnabled && targetIsOp(patch.imuYTarget) && patch.imuYDepth > 0.f);
    bool lfo = targetIsOp(patch.lfoTarget) && patch.lfoDepth > 0.f;
    return imu ? (lfo ? "*~" : "*") : (lfo ? "~" : "");
}

static void setInfo(const char *msg);   // SETTING section, below

// Fixed-frequency row text (v0.71): "220Hz A3", "8.4k C9", "1.3Hz" under
// 20Hz where a note name means nothing. At most 8 characters, so the left
// column stays clear of the divider.
static const char *fixNoteLabel() {
    static char buf[12];
    float n = patch.op[selectedOp].fixNote;
    float hz = 440.f * powf(2.f, (n - 69.f) / 12.f);
    int note = (int)lroundf(n);
    if (hz < 20.f)        snprintf(buf, sizeof(buf), "%.1fHz", hz);
    else if (hz < 1000.f) snprintf(buf, sizeof(buf), "%dHz %s", (int)lroundf(hz), noteName(note));
    else                  snprintf(buf, sizeof(buf), "%.1fk %s", hz / 1000.f, noteName(note));
    return buf;
}

static bool opRowIsFixed = false;   // which form row 1 was bound in

static void bindOpItems() {
    MenuItem *opItems = itemPool; itemPoolOwner = IO_OP;
    OpParams &p = patch.op[selectedOp];
    // Each row carries the destination that modulates it, so drawItemList
    // marks it automatically when the IMU or the LFO is pointing there.
    opItems[0] = itemFloat("Morph",  &p.morph,   0.f, 100.f, 1.f,  "%.0f%%", ModTarget::OP_MORPH);
    // Row 1 is Ratio or, on a fixed operator, its pitch in semitone steps
    // (hold for x10 like any value). Both values are kept; `f` swaps (v0.71).
    if (p.fixed) {
        opItems[1] = itemFloat("Fixed", &p.fixNote, FIX_NOTE_MIN, FIX_NOTE_MAX, 1.f, nullptr, ModTarget::OP_RATIO);
        opItems[1].label = fixNoteLabel;
    } else {
        opItems[1] = itemFloat("Ratio",  &p.ratio,   0.25f, 16.f, 0.01f, "%.2f", ModTarget::OP_RATIO);
    }
    opRowIsFixed = p.fixed;
    opItems[2] = itemFloat("Detune", &p.detune,  -50.f, 50.f, 1.f, "%+.0fc", ModTarget::OP_DETUNE);
    opItems[3] = itemFloat("Level",  &p.level,   0.f, 100.f, 1.f,  "%.0f%%", ModTarget::OP_LEVEL);
    opItems[4] = itemFloat("A",      &p.attack,  0.f, 100.f, 1.f,  "%.0f");
    opItems[5] = itemFloat("D",      &p.decay,   0.f, 100.f, 1.f,  "%.0f");
    opItems[6] = itemFloat("S",      &p.sustain, 0.f, 100.f, 1.f,  "%.0f");
    opItems[7] = itemFloat("R",      &p.release, 0.f, 100.f, 1.f,  "%.0f");
    opItems[8] = itemFloat("VelSns", &p.velSens, 0.f, 100.f, 1.f,  "%.0f%%");
    opItems[9] = itemFloat("KScl",   &p.keyScale,0.f, 100.f, 1.f,  "%.0f%%");
}
static MenuItem *opList() {
    if (itemPoolOwner != IO_OP || patch.op[selectedOp].fixed != opRowIsFixed) bindOpItems();   // a load / morph changed the mode
    return itemPool;
}

static void drawOpPreview(int y, int h) {
    // The operator's own morphed table, as the audio engine sees it
    // (includes the global morph offset).
    float m = clampf(patch.op[selectedOp].morph + audioStatus.effGlobalMorph, 0.f, 100.f);
    int cy = y + h / 2;
    canvas.fillRect(0, y, SCREEN_W, h, BLACK);
    canvas.drawFastHLine(0, cy, SCREEN_W, uiDim);
    int py = cy;
    for (int px = 0; px < SCREEN_W; px++) {
        int idx = (int)((float)px / SCREEN_W * WAVE_TABLE_SIZE * 2) & WAVE_TABLE_MASK;
        float s = morphPreviewSample(m, idx);
        int yy = clampi(cy - (int)(s * (h / 2 - 1)), y, y + h - 1);
        if (px > 0) canvas.drawLine(px - 1, py, px, yy, uiColor);
        py = yy;
    }
}

// --- What a modulation destination does (v0.85) -----------------------------
// One line per ModTarget, shown in the nav row while a Target row is
// selected - the IMU's, the LFO's, the knob slots' and the theremin's all
// read this one table. The short names are easy to mix up (GMorph is not the
// chain - that is PtchMrph).
static const char *const MOD_TARGET_HELP[(int)ModTarget::MOD_TARGET_COUNT] = {
    "Not routed",                         // Off
    "All 4 OP waveforms (one patch)",     // GMorph
    "Feedback amount (grit, saw-like)",   // Feedbk
    "Pitch, in semitones",                // Pitch
    "Patch volume",                       // Volume
    "Selected OP waveform",               // OpMorph
    "Selected OP level (brightness)",     // OpLevel
    "Selected OP ratio (tone, pitch)",    // OpRatio
    "Selected OP detune (beating)",       // OpDetun
    "",                                   // MOD_AUDIO_END, never shown
    "ARP tempo",                          // ArpTmpo
    "ARP swing",                          // ArpSwng
    "ARP gate (note length)",             // ArpGate
    "Pitch glide time (portamento)",      // Glide
    "Chorus depth",                       // ChDepth
    "Chorus mix",                         // ChMix
    "Delay feedback (echo repeats)",      // DlFdbk
    "Delay mix",                          // DlMix
    "Reverb size",                        // RvSize
    "Reverb mix",                         // RvMix
    "Drive amount (distortion)",          // DrAmt
    "Chain position (patch to patch)",    // PtchMrph
    "Crush bit depth",                    // CrBits
    "Crush sample-rate reduction",        // CrDown
    "Crush mix",                          // CrMix
    "Glitch probability",                 // GlProb
    "Pitch envelope depth",               // PEnvAmt
    "Drive mix",                          // DrMix
};
static const char *modTargetHelp(ModTarget t) {
    int i = (int)t;
    return (i >= 0 && i < (int)ModTarget::MOD_TARGET_COUNT) ? MOD_TARGET_HELP[i] : "";
}

// --- Envelope graphs (v0.85) -------------------------------------------------
// Drawn from the engine's own numbers: the time comes back out of
// envTimeToRate() (fm.cpp), the shapes are envAdvanceBlock()'s and
// penvAdvanceBlock()'s, so the picture cannot drift from the sound.
static float envSeconds(float v, float maxSeconds) {
    return (float)ENV_BLOCK / (envTimeToRate(v, maxSeconds) * (float)SAMPLE_RATE);
}
static void fmtSeconds(char *b, size_t n, float s) {
    if (s < 10.f) snprintf(b, n, "%.2fs", s); else snprintf(b, n, "%.1fs", s);
}
static bool opRowIsEnvelope() { return opNav.sel >= 4 && opNav.sel <= 7; }   // A D S R

// A D (S held) R across the graph. A, D and R are as wide as their times
// (with 6px at least, so a 2ms attack stays visible - the numbers above give
// the real times); the held sustain takes a fixed quarter.
static void drawOpEnvelope(int y, int h) {
    const OpParams &p = patch.op[selectedOp];
    float t[3] = { envSeconds(p.attack, 3.f), envSeconds(p.decay, 8.f), envSeconds(p.release, 8.f) };
    float S = clampf(p.sustain, 0.f, 100.f) / 100.f;
    const int W = SCREEN_W - 2, HOLD = W / 4, MINW = 6;
    int avail = W - HOLD;
    float w[3]; bool fixedW[3] = { false, false, false };
    float sum = t[0] + t[1] + t[2];
    for (int i = 0; i < 3; i++) w[i] = avail * t[i] / sum;
    float rem = (float)avail, freeT = 0.f;
    for (int i = 0; i < 3; i++) { if (w[i] < MINW) { w[i] = MINW; fixedW[i] = true; rem -= MINW; } else freeT += t[i]; }
    for (int i = 0; i < 3; i++) if (!fixedW[i]) w[i] = (freeT > 0.f) ? rem * t[i] / freeT : MINW;
    int xA = 1, xD = xA + (int)(w[0] + 0.5f), xS = xD + (int)(w[1] + 0.5f), xR = xS + HOLD, xE = 1 + W;
    int top = y + 1, bot = y + h - 2, amp = bot - top;
    canvas.fillRect(0, y, SCREEN_W, h, BLACK);
    canvas.drawFastHLine(0, bot, SCREEN_W, uiDim);
    for (int x : { xD, xS, xR }) canvas.drawFastVLine(x, bot - 2, 3, uiDim);   // segment ticks
    int py = bot;
    for (int x = xA; x <= xE; x++) {
        float e;
        if (x < xD)      { float q = 1.f - (float)(x - xA) / (float)(xD - xA); e = 1.f - q * q; }          // A: 1-(1-pos)^2
        else if (x < xS) { float q = 1.f - (float)(x - xD) / (float)(xS - xD); e = S + (1.f - S) * q * q; } // D
        else if (x < xR) e = S;                                                                            // held
        else             { float q = 1.f - (float)(x - xR) / (float)(xE - xR); e = S * q * q; }             // R
        int yy = bot - (int)(e * amp + 0.5f);
        if (x > xA) canvas.drawLine(x - 1, py, x, yy, uiColor);
        py = yy;
    }
    // The last key note, on this operator (the theremin's never)
    int st = audioStatus.probeStage, dx = -1;
    float pos = clampf(audioStatus.probePos, 0.f, 1.f);
    if (st == (int)EnvStage::ATTACK)       dx = xA + (int)(pos * (xD - xA));
    else if (st == (int)EnvStage::DECAY)   dx = xD + (int)(pos * (xS - xD));
    else if (st == (int)EnvStage::SUSTAIN) dx = xS + HOLD / 2;
    else if (st == (int)EnvStage::RELEASE) dx = xR + (int)(pos * (xE - xR));
    if (dx >= 0) canvas.fillCircle(dx, bot - (int)(clampf(audioStatus.probeEnv, 0.f, 1.f) * amp + 0.5f), 2, WHITE);
}

static void drawOpEnvTimes(int x, int y) {
    const OpParams &p = patch.op[selectedOp];
    char a[10], d[10], r[10];
    fmtSeconds(a, sizeof(a), envSeconds(p.attack, 3.f));
    fmtSeconds(d, sizeof(d), envSeconds(p.decay, 8.f));
    fmtSeconds(r, sizeof(r), envSeconds(p.release, 8.f));
    canvas.setCursor(x, y);
    canvas.printf("A %s  D %s  S %.0f  R %s", a, d, p.sustain, r);
}

static void drawOpScreen() {
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, 14);
    canvas.print("OP ");
    for (int i = 0; i < NUM_OPS; i++) {
        if (i == selectedOp) {
            canvas.setTextColor(BLACK, uiColor); canvas.printf("[%d]", i + 1); canvas.setTextColor(uiColor, BLACK);
            canvas.print(opModMarker());
        }
        else {
            // Fixed operators stand out in the row of numbers (v0.71)
            if (patch.op[i].fixed) canvas.setTextColor(uiHighlight, BLACK);
            canvas.printf(" %d ", i + 1);
            canvas.setTextColor(uiColor, BLACK);
        }
    }
    bool carrier = algoIsCarrier(patch.algorithm, selectedOp);
    canvas.setCursor(110, 14);
    // "+FB" when this operator feeds back in the current algorithm (v0.7).
    // 12 columns + " Algo 14" still ends at x=230.
    char role[16];
    snprintf(role, sizeof(role), "%s%s", carrier ? "CARRIER" : "MODULATOR",
             algoHasFb(patch.algorithm, selectedOp) ? "+FB" : "");
    canvas.printf("%-12s Algo %d", role, patch.algorithm);
    // Waveform name of the blend
    float m = clampf(patch.op[selectedOp].morph + audioStatus.effGlobalMorph, 0.f, 100.f);
    int wi = clampi((int)(m * (NUM_WAVEFORMS - 1) / 100.f + 0.5f), 0, NUM_WAVEFORMS - 1);
    // The number is the value the engine is actually using this buffer, so
    // it tracks the mod wheel, the IMU and the LFO as they move.
    canvas.setCursor(4, 24);
    if (opRowIsEnvelope()) drawOpEnvTimes(4, 24);     // v0.85: the times, while the graph shows the envelope
    else canvas.printf("Wave ~%-9s %3.0f%%%s", WAVEFORM_NAMES[wi], m,
                       modMarker(ModTarget::GMORPH)[0] ? modMarker(ModTarget::GMORPH) : modMarker(ModTarget::OP_MORPH));
    if (patch.op[selectedOp].fixed) {
        canvas.fillRect(220, 23, 19, 10, uiHighlight);
        canvas.setTextColor(BLACK, uiHighlight);
        canvas.setCursor(221, 24); canvas.print("FIX");
        canvas.setTextColor(uiColor, BLACK);
    }
    if (opRowIsEnvelope()) drawOpEnvelope(33, 24); else drawOpPreview(33, 24);
    drawItemList(opList(), 10, opNav.sel, 60, 5, 13);
    drawNavHint("1-4:OP ;/.:row ,//:value f:fix");
}

static void opHandleKeys(const KeyFrame &k) {
    for (int i = 0; i < NUM_OPS; i++) {
        if (keyPressed(k, (char)('1' + i)) && !k.shift) {
            // Switching operators moves the OP_* modulation with it. The
            // engine rebuilds every offset from zero each buffer, so the
            // operator left behind returns to its stored value on its own.
            selectedOp = i; bindOpItems(); uiDirty = true;
        }
    }
    // f: Ratio <-> Fixed for the selected operator (v0.71, update spec 5,
    // 3.4). Name entry is a different screen, so typing never reaches here.
    if (keyPressed(k, 'f')) {
        OpParams &p = patch.op[selectedOp];
        p.fixed = !p.fixed;
        bindOpItems();
        char msg[20];
        snprintf(msg, sizeof(msg), "OP%d: %s", selectedOp + 1, p.fixed ? "Fixed" : "Ratio");
        setInfo(msg);
        uiDirty = true;
    }
    if (listHandleKeys(opNav, opList(), 10, k)) uiDirty = true;
}

// ===========================================================================
// ALGO tab
// ===========================================================================
// v0.71: ten rows in a 7-row window that scrolls (update spec 5, 4.6). Tone
// first, then the voicing settings that are rarely touched.
constexpr int ALGO_ITEMS = 10;
constexpr int ALGO_ROWS  = 7;
static ListNav  algoNav;
static int      algoTop = 0;        // first row shown

// Voices (v0.77): the patch may hold more than this build plays (12 in a
// patch, 10 in the BLE build). The row shows both, "12 (10)", and the value
// in the file stays as it is until the row is actually changed - then it
// moves within what this build plays.
static const char *voicesLabel() {
    static char b[12];
    if (patch.voices > MAX_VOICES) snprintf(b, sizeof(b), "%d (%d)", patch.voices, MAX_VOICES);
    else snprintf(b, sizeof(b), "%d", patch.voices);
    return b;
}
static void voicesStep(int d) { patch.voices = clampi(clampi(patch.voices, 1, MAX_VOICES) + d, 1, MAX_VOICES); }
static void voicesInc() { voicesStep(1); }
static void voicesDec() { voicesStep(-1); }

// Swing (v0.90): stored 0-100 with 50 = straight (settings.json, CC and the
// tilt all keep that), shown -50..+50 - minus pulls the off-beat early,
// plus pushes it late.
static const char *swingLabel() {
    static char b[8];
    int s = arp.swing - 50;
    if (s == 0) snprintf(b, sizeof(b), "0"); else snprintf(b, sizeof(b), "%+d", s);
    return b;
}

static void bindAlgoItems() {
    MenuItem *algoItems = itemPool; itemPoolOwner = IO_ALGO;
    algoItems[0] = itemInt("Algo", &patch.algorithm, 1, NUM_ALGORITHMS);
    algoItems[1] = itemFloat("Feedbk", &patch.feedback, 0.f, 100.f, 1.f, "%.0f", ModTarget::FEEDBACK);
    algoItems[2] = itemFloat("PEnvAmt", &patch.penvAmt, -PENV_AMT_MAX, PENV_AMT_MAX, 1.f, "%+.0fst", ModTarget::PENV_AMT);
    algoItems[3] = itemFloat("PEnvDcy", &patch.penvDecay, 0.f, 100.f, 1.f, "%.0f");
    algoItems[4] = itemFloat("PEnvVel", &patch.penvVel, 0.f, 100.f, 1.f, "%.0f%%");
    algoItems[5] = itemFloat("GMorph", &patch.globalMorph, 0.f, 100.f, 1.f, "%.0f%%", ModTarget::GMORPH);
    algoItems[6] = itemCustom("Voices", voicesLabel, voicesInc, voicesDec);
    algoItems[7] = itemFloat("Glide", &patch.glideMs, 0.f, 1000.f, 5.f, "%.0fms");
    algoItems[8] = itemEnum("GlMode", (uint8_t *)&patch.glideMode, GLIDE_MODE_NAMES, (int)GlideMode::GLIDE_MODE_COUNT);
    algoItems[9] = itemBool("GlideOn", &patch.glideEnabled);
    algoItems[7].modOf = ModTarget::GLIDE_TIME;   // v0.87: the * ~ ^ marks
}
static MenuItem *algoList() { if (itemPoolOwner != IO_ALGO) bindAlgoItems(); return itemPool; }

// Box positions in half-columns / rows, edges (from,to) in OP indices 0..3
struct AlgoLayout { int8_t col[4]; int8_t row[4]; int8_t edges[6][2]; int8_t edgeCount; };
static const AlgoLayout ALGO_LAYOUT[NUM_ALGORITHMS] = {
    // OP1 OP2 OP3 OP4 cols / rows
    { {0,0,0,0}, {3,2,1,0}, {{3,2},{2,1},{1,0}}, 3 },                 // 1: 4>3>2>1
    { {1,1,2,0}, {2,1,0,0}, {{3,1},{2,1},{1,0}}, 3 },                 // 2: (4,3)>2>1
    { {1,0,2,0}, {2,1,1,0}, {{3,1},{1,0},{2,0}}, 3 },                 // 3: 4>2>1 + 3>1
    { {2,4,2,0}, {1,0,0,0}, {{3,0},{2,0},{1,0}}, 3 },                 // 4: (4,3,2)>1
    { {2,2,0,0}, {1,0,1,0}, {{3,2},{1,0}}, 2 },                       // 5: 4>3 + 2>1
    { {2,0,0,0}, {2,2,1,0}, {{3,2},{2,1}}, 2 },                       // 6: 4>3>2 + 1
    { {4,2,0,2}, {1,1,1,0}, {{3,2},{3,1},{3,0}}, 3 },                 // 7: 4>(3,2,1)
    { {0,2,4,6}, {0,0,0,0}, {{0,0}}, 0 },                             // 8: 1 2 3 4
    // v0.7. Feedback is marked from ALGO_FB_MASK, not from here.
    { {0,0,0,0}, {3,2,1,0}, {{3,2},{2,1},{1,0}}, 3 },                 // 9:  4>3>2>1, FB OP1
    { {0,0,0,0}, {3,2,1,0}, {{3,2},{2,1},{1,0}}, 3 },                 // 10: 4>3>2>1, FB OP2
    { {0,0,4,4}, {1,0,1,0}, {{3,2},{1,0}}, 2 },                       // 11: 4>3 + 2>1, FB OP4+OP2
    { {0,0,0,4}, {2,1,0,2}, {{2,1},{1,0}}, 2 },                       // 12: 3>2>1 + 4
    { {0,0,3,5}, {1,0,1,1}, {{1,0}}, 1 },                             // 13: 2>1 + 3 + 4
    { {2,4,0,2}, {2,1,1,0}, {{3,2},{3,1},{2,0},{1,0}}, 4 },           // 14: diamond
};

static void drawAlgoDiagram() {
    const int BW = 22, BH = 11;
    const AlgoLayout &L = ALGO_LAYOUT[clampi(patch.algorithm, 1, NUM_ALGORITHMS) - 1];
    int bx[4], by[4];
    for (int o = 0; o < 4; o++) { bx[o] = 6 + L.col[o] * 14; by[o] = 15 + L.row[o] * 25; }
    // edges first so boxes cover the line ends
    for (int e = 0; e < L.edgeCount; e++) {
        int a = L.edges[e][0], b = L.edges[e][1];
        int x0 = bx[a] + BW / 2, y0 = by[a] + BH, x1 = bx[b] + BW / 2, y1 = by[b];
        canvas.drawLine(x0, y0, x1, y1, uiColor);
        canvas.fillTriangle(x1, y1, x1 - 2, y1 - 4, x1 + 2, y1 - 4, uiColor);
    }
    // output bar
    const int OUT_Y = 108;
    for (int o = 0; o < 4; o++) {
        if (!algoIsCarrier(patch.algorithm, o)) continue;
        canvas.drawLine(bx[o] + BW / 2, by[o] + BH, bx[o] + BW / 2, OUT_Y, uiColor);
    }
    canvas.drawFastHLine(4, OUT_Y, 110, uiColor);
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, OUT_Y + 3); canvas.print("OUT");
    // boxes
    for (int o = 0; o < 4; o++) {
        bool carrier = algoIsCarrier(patch.algorithm, o);
        bool on = patch.op[o].level > 0.f;
        if (carrier) { canvas.fillRect(bx[o], by[o], BW, BH, on ? uiColor : uiMuted); canvas.setTextColor(BLACK, on ? uiColor : uiMuted); }
        else         { canvas.fillRect(bx[o], by[o], BW, BH, BLACK); canvas.drawRect(bx[o], by[o], BW, BH, on ? uiColor : uiMuted); canvas.setTextColor(on ? uiColor : uiMuted, BLACK); }
        canvas.setCursor(bx[o] + 2, by[o] + 2); canvas.printf("OP%d", o + 1);
    }
    // Feedback loops: on every operator in ALGO_FB_MASK (v0.7 - was OP4
    // only; algorithm 11 has two). The "FB" label is dropped where it would
    // cross the column divider at x=119 (OP4 of algorithm 8).
    if (patch.feedback > 0.f) {
        for (int o = 0; o < 4; o++) {
            if (!algoHasFb(patch.algorithm, o)) continue;
            int x = bx[o] + BW, y = by[o];
            canvas.drawLine(x, y + 2, x + 5, y + 2, uiColor);
            canvas.drawLine(x + 5, y + 2, x + 5, y + BH - 2, uiColor);
            canvas.drawLine(x + 5, y + BH - 2, x, y + BH - 2, uiColor);
            if (x + 8 + 12 <= 118) {
                canvas.setTextColor(uiColor, BLACK);
                canvas.setCursor(x + 8, y + 2); canvas.print("FB");
            }
        }
    }
    canvas.setTextColor(uiColor, BLACK);
}

// ALGO: the pitch envelope in the diagram's place while a PEnv row is
// selected (v0.85). Amt x velK x (1-pos)^2 over the Decay time; bright =
// velocity 127, dim = velocity 40 (the factory patches were measured at
// 40 / 100 / 127), the dot = the last key note.
static bool algoRowIsPenv() { return algoNav.sel >= 2 && algoNav.sel <= 4; }

static void drawAlgoPitchEnv() {
    const int X0 = 6, X1 = 112, Y0 = 30, Y1 = 98, W = X1 - X0;
    float amt = clampf(patch.penvAmt, -PENV_AMT_MAX, PENV_AMT_MAX);
    float tD = envSeconds(patch.penvDecay, 8.f);
    float k40 = 1.f - (clampf(patch.penvVel, 0.f, 100.f) / 100.f) * (1.f - 40.f / 127.f);   // penvNoteOn()'s velK
    char ts[10]; fmtSeconds(ts, sizeof(ts), tD);
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, 14); canvas.printf("PEnv %+.0fst %s", amt, ts);
    // 0 at the bottom for a rise from below, at the top for a fall from above
    int yZero = (amt >= 0.f) ? Y1 : Y0;
    if (fabsf(amt) < 0.5f) yZero = (Y0 + Y1) / 2;
    float scale = (float)(Y1 - Y0) / fmaxf(fabsf(amt), 1.f);
    canvas.drawFastHLine(X0, yZero, W + 1, uiDim);
    canvas.drawFastVLine(X0, Y0, Y1 - Y0 + 1, uiDim);
    canvas.setTextColor(uiMuted, BLACK);
    canvas.setCursor(X1 - 23, (amt >= 0.f) ? Y0 : Y1 - 7); canvas.printf("%+.0f", amt);
    for (int pass = 0; pass < 2; pass++) {             // dim (v40) under bright (v127)
        float k = pass == 0 ? k40 : 1.f;
        uint16_t col = pass == 0 ? uiMuted : uiColor;
        int py = yZero;
        for (int px = 0; px <= W; px++) {
            float q = 1.f - (float)px / (float)W;
            int yy = yZero - (int)(amt * k * q * q * scale + (amt >= 0.f ? 0.5f : -0.5f));
            if (px > 0) canvas.drawLine(X0 + px - 1, py, X0 + px, yy, col);
            py = yy;
        }
    }
    float pp = audioStatus.probePenvPos;
    if (pp >= 0.f && pp <= 1.f && fabsf(amt) >= 0.5f) {
        float q = 1.f - pp;
        canvas.fillCircle(X0 + (int)(pp * W), yZero - (int)(amt * audioStatus.probePenvVelK * q * q * scale), 2, WHITE);
    }
    // Legend
    canvas.drawFastHLine(6, 110, 10, uiColor);  canvas.setTextColor(uiColor, BLACK); canvas.setCursor(19, 107); canvas.print("v127");
    canvas.drawFastHLine(52, 110, 10, uiMuted);   canvas.setTextColor(uiMuted, BLACK);   canvas.setCursor(65, 107); canvas.print("v40");
    canvas.setTextColor(uiColor, BLACK);
    if (fabsf(amt) < 0.5f) { canvas.setCursor(X0 + 4, Y0 + 4); canvas.print("Amt 0 = off"); }
}

static void drawAlgoScreen() {
    canvas.drawFastVLine(119, 12, 112, uiDim);
    if (algoRowIsPenv()) drawAlgoPitchEnv(); else drawAlgoDiagram();
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(123, 14); canvas.printf("%d: %s", patch.algorithm, ALGORITHM_NAMES[clampi(patch.algorithm, 1, NUM_ALGORITHMS) - 1]);
    // The list lives in the right column only: a 7-row window that follows
    // the cursor one row at a time, with marks where rows are hidden.
    if (algoNav.sel < algoTop) algoTop = algoNav.sel;
    if (algoNav.sel >= algoTop + ALGO_ROWS) algoTop = algoNav.sel - ALGO_ROWS + 1;
    algoTop = clampi(algoTop, 0, ALGO_ITEMS - ALGO_ROWS);
    const MenuItem *algoItems = algoList();
    for (int r = 0; r < ALGO_ROWS; r++) {
        int i = algoTop + r;
        int y = 28 + r * 13;
        canvas.setCursor(123, y);
        canvas.printf("%c%-7s%s%s", (i == algoNav.sel) ? '>' : ' ', algoItems[i].name, itemLabel(algoItems[i]), modMarker(algoItems[i].modOf));
    }
    // Up / down marks at the column's right edge (the font has no arrows)
    if (algoTop > 0)
        canvas.fillTriangle(235, 28, 231, 34, 239, 34, uiColor);
    if (algoTop + ALGO_ROWS < ALGO_ITEMS)
        canvas.fillTriangle(231, 110, 239, 110, 235, 116, uiColor);
    drawNavHint(";/. row  ,// value  Tab:next");
}

static void algoHandleKeys(const KeyFrame &k) {
    if (listHandleKeys(algoNav, algoList(), ALGO_ITEMS, k)) uiDirty = true;
}

// ===========================================================================
// ARP tab
// ===========================================================================
constexpr int ARP_ITEMS = 13;
static ListNav  arpNav;
static bool     arpStepFocus = false;
static int      stepCursor = 0;
static int      stepField = 0;     // 0 Note, 1 Oct, 2 Vel, 3 Gate
static RepeatKey stepLeft, stepRight, stepUp, stepDown, stepDec, stepInc;
static ArpPattern lastPattern = ArpPattern::UP;

static const char *arpOnLabel() { return arp.on ? "On" : "Off"; }
static void arpOnInc() { arpSetOn(!arp.on); }
static const char *arpLatchLabel() { return arp.latch ? "On" : "Off"; }
static void arpLatchInc() { arpToggleLatch(); }

static void bindArpItems() {
    MenuItem *arpItems = itemPool; itemPoolOwner = IO_ARP;
    arpItems[0]  = itemCustom("Arp", arpOnLabel, arpOnInc, arpOnInc);
    arpItems[1]  = itemEnum("Pattern", (uint8_t *)&arp.pattern, ARP_PATTERN_NAMES, (int)ArpPattern::ARP_PATTERN_COUNT);
    arpItems[2]  = itemCustom("Latch", arpLatchLabel, arpLatchInc, arpLatchInc);
    arpItems[3]  = itemInt("Tempo", &arp.tempoBpm, 40, 300);
    arpItems[4]  = itemEnum("Rate", (uint8_t *)&arp.rate, ARP_RATE_NAMES, (int)ArpRate::ARP_RATE_COUNT);
    arpItems[5]  = itemInt("Swing", &arp.swing, 0, 100);
    arpItems[5].label = swingLabel;
    arpItems[6]  = itemInt("Gate", &arp.gate, 10, 200);
    arpItems[7]  = itemInt("Octave", &arp.octave, 1, 4);
    arpItems[8]  = itemEnum("Wrap", (uint8_t *)&arp.chordWrap, CHORD_WRAP_NAMES, (int)ChordWrap::CHORD_WRAP_COUNT);
    arpItems[9]  = itemInt("Length", &arp.length, 1, ARP_STEPS);
    arpItems[10] = itemBool("ClkSync", &arp.clockSync);
    arpItems[11] = itemEnum("LatchMd", (uint8_t *)&arp.latchMode, LATCH_MODE_NAMES, (int)LatchMode::LATCH_MODE_COUNT);
    // Where PLAY's octave / transpose act while the ARP runs (v0.43)
    arpItems[12] = itemEnum("OctTrn", (uint8_t *)&arp.shiftMode, SHIFT_MODE_NAMES, (int)ShiftMode::SHIFT_MODE_COUNT);
    // v0.87: the rows the IMU / theremin can move carry the * ~ ^ marks too
    arpItems[3].modOf = ModTarget::ARP_TEMPO;
    arpItems[5].modOf = ModTarget::ARP_SWING;
    arpItems[6].modOf = ModTarget::ARP_GATE;
}
static MenuItem *arpList() { if (itemPoolOwner != IO_ARP) bindArpItems(); return itemPool; }

static void drawStepEditor() {
    const int GX = 8, GY = 93, BW = 14, BH = 19;
    int len = clampi(arp.length, 1, ARP_STEPS);
    int play = arpIsRunning() ? arpCurrentStep() : -1;
    // beat colours for the frames (4 per bar)
    uint16_t beat[4] = { canvas.color565(220, 60, 60), canvas.color565(230, 140, 40), canvas.color565(90, 210, 90), canvas.color565(60, 190, 190) };
    for (int i = 0; i < ARP_STEPS; i++) {
        int x = GX + i * BW;
        bool inLen = i < len;
        uint16_t frame = inLen ? beat[(i / 4) % 4] : uiDim;
        canvas.drawRect(x, GY, BW - 1, BH, frame);
        const ArpStep &s = arp.step[i];
        if (s.note > 0 && inLen) {
            int bh = clampi((int)((BH - 4) * (float)s.vel / 127.f), 1, BH - 4);
            uint16_t fill = (i == play) ? uiHighlight : WHITE;
            canvas.fillRect(x + 2, GY + BH - 2 - bh, BW - 5, bh, fill);
            canvas.setTextColor(BLACK, fill);
            if (bh >= 9) { canvas.setCursor(x + 4, GY + BH - 9); canvas.printf("%d", s.note); }
        } else if (i == play) {
            canvas.drawFastHLine(x + 2, GY + BH - 3, BW - 5, uiHighlight);
        }
        if (arpStepFocus && i == stepCursor) canvas.drawRect(x - 1, GY - 1, BW + 1, BH + 2, WHITE);
    }
    // selected step's values
    const ArpStep &s = arp.step[stepCursor];
    static const char *fields[4] = { "Note", "Oct", "Vel", "Gate" };
    char vals[4][8];
    if (s.note == 0) strcpy(vals[0], "Off"); else snprintf(vals[0], 8, "%d", s.note);
    snprintf(vals[1], 8, "%+d", (int)s.oct);
    snprintf(vals[2], 8, "%d", (int)s.vel);
    snprintf(vals[3], 8, "%d%%", (int)s.gate);
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, 113);
    canvas.printf("S%02d ", stepCursor + 1);
    for (int f = 0; f < 4; f++) {
        bool sel = arpStepFocus && f == stepField;
        if (sel) canvas.setTextColor(BLACK, uiColor); else canvas.setTextColor(uiColor, BLACK);
        canvas.printf("%s:%s", fields[f], vals[f]);
        canvas.setTextColor(uiColor, BLACK);
        canvas.print(" ");
    }
}

static void drawArpScreen() {
    drawItemList(arpList(), ARP_ITEMS, arpStepFocus ? -1 : arpNav.sel, 13, 6, 10);
    // status line
    canvas.setTextColor(uiColor, BLACK);
    if (arp.pattern == ArpPattern::STEP) {
        drawStepEditor();
        drawNavHint(arpStepFocus ? ",//:step ;/.:field </>:val Ent:list" : "Enter:edit steps  Sh+V:Arp V:Latch");
    } else {
        canvas.setCursor(4, 96);
        canvas.printf("Chord:%d  %s  %.1f BPM", arpChordSize(), (arp.clockSync && midiClockLocked()) ? "CLOCK" : (arp.clockSync ? "no clk" : "local"), arpEffectiveBpm());
        canvas.setCursor(4, 108);
        canvas.printf("Playing: %s", arpIsRunning() ? noteName(arpLastNote()) : "--");
        drawNavHint("Sh+V:Arp  V:Latch  Sh+Enter:Tap");
    }
}

static void stepAdjust(int dir) {
    ArpStep &s = arp.step[stepCursor];
    switch (stepField) {
        case 0: s.note = (uint8_t)clampi((int)s.note + dir, 0, ARP_STEP_NOTES); break;
        case 1: s.oct  = (int8_t)clampi((int)s.oct + dir, -2, 2); break;
        case 2: s.vel  = (uint8_t)clampi((int)s.vel + dir, 1, 127); break;
        case 3: s.gate = (uint8_t)clampi((int)s.gate + dir, 10, 200); break;
    }
}

static void arpHandleKeys(const KeyFrame &k) {
    bool stepMode = (arp.pattern == ArpPattern::STEP);
    if (!stepMode) arpStepFocus = false;
    if (stepMode && k.enterPressed && !k.shift) { arpStepFocus = !arpStepFocus; uiDirty = true; return; }
    if (arpStepFocus) {
        bool left  = keyDown(k, ',');
        bool right = keyDown(k, '/');
        bool up    = keyDown(k, ';') || keyDown(k, ':');
        bool down  = keyDown(k, '.') || keyDown(k, '>');
        bool dec   = keyDown(k, '<');
        bool inc   = keyDown(k, '?');
        if (repeatFire(stepLeft, left))   { stepCursor = (stepCursor + ARP_STEPS - 1) % ARP_STEPS; uiDirty = true; }
        if (repeatFire(stepRight, right)) { stepCursor = (stepCursor + 1) % ARP_STEPS; uiDirty = true; }
        if (repeatFire(stepUp, up))       { stepField = (stepField + 3) % 4; uiDirty = true; }
        if (repeatFire(stepDown, down))   { stepField = (stepField + 1) % 4; uiDirty = true; }
        if (repeatFire(stepDec, dec))     { stepAdjust(-1); uiDirty = true; }
        if (repeatFire(stepInc, inc))     { stepAdjust(+1); uiDirty = true; }
        for (int d = 0; d <= ARP_STEP_NOTES; d++) {     // digit keys only - see ARP_STEP_NOTES
            if (keyPressed(k, (char)('0' + d))) { arp.step[stepCursor].note = (uint8_t)d; uiDirty = true; }
        }
        return;
    }
    if (listHandleKeys(arpNav, arpList(), ARP_ITEMS, k)) {
        uiDirty = true;
        if (arp.pattern != lastPattern) { lastPattern = arp.pattern; rebuildArpChord(); }
    }
}

// Confirm dialog state lives with the SETTING section below; the LFO tab's
// Calib row needs it too, so it is declared here.
enum ConfirmKind  : uint8_t { CK_RESET = 0, CK_DELETE, CK_OVERWRITE, CK_CALIBRATE, CK_CHAIN_LOAD, CK_REBOOT,
                              CK_CHAIN_OVERWRITE, CK_CHAIN_DELETE };   // v0.83: CK_DEMOCHAIN became CK_CHAIN_LOAD
static ConfirmKind confirmKind = CK_RESET;
static AppMode confirmReturn = AppMode::SETTING;   // where Enter / Tab go back to

// ===========================================================================
// LFO tab. The IMU rows moved to SETTING > IMU in v0.41, and their space
// now shows the LFO's shape with a live phase marker (as in C.P.S.).
// ===========================================================================
constexpr int LFO_ITEMS = 5;

// IMU calibrate (v0.12, same flow as C.P.S.): OFF -> confirm dialog, the
// current tilt becomes the zero point; ON -> turns off immediately.
// Lives on SETTING > IMU since v0.41.
static const char *calibLabel() { return sys.imuCalibrated ? "ON" : "OFF"; }
static void calibToggle() {
    if (sys.imuCalibrated) { sys.imuCalibrated = false; sys.imuXCalDeg = 0.f; sys.imuYCalDeg = 0.f; saveSettings(); return; }
    confirmKind = CK_CALIBRATE; confirmReturn = AppMode::SETTING;
    uiSetMode(AppMode::CONFIRM);
}
static ListNav  lfoNav;

static void bindLfoItems() {
    MenuItem *lfoItems = itemPool; itemPoolOwner = IO_LFO;
    lfoItems[0] = itemEnum("Wave", (uint8_t *)&patch.lfoWave, LFO_WAVE_NAMES, (int)LfoWave::LFO_WAVE_COUNT);
    lfoItems[1] = itemFloat("Rate", &patch.lfoRateHz, 0.1f, 20.f, 0.1f, "%.1fHz");
    lfoItems[2] = itemFloat("Depth", &patch.lfoDepth, 0.f, 100.f, 1.f, "%.0f%%");
    // The LFO stops at the audio destinations: an LFO on the ARP tempo
    // would wobble the clock that times the LFO itself (spec B.3). Off is a
    // choice since v0.73 (the author's request): the LFO is unrouted while
    // Depth and Rate keep their values for when it is routed again.
    lfoItems[3] = itemEnum("Target", (uint8_t *)&patch.lfoTarget, MOD_TARGET_NAMES, (int)ModTarget::MOD_TARGET_COUNT, 0,
                           (int)ModTarget::MOD_AUDIO_END, (int)ModTarget::GLIDE_TIME);
    lfoItems[4] = itemFloat("Delay", &patch.lfoDelayMs, 0.f, 2000.f, 10.f, "%.0fms");
}
static MenuItem *lfoList() { if (itemPoolOwner != IO_LFO) bindLfoItems(); return itemPool; }

// The LFO's waveform at phase p (0..1), matching lfoTick() in voice.cpp.
// S&H is random by nature, so the preview draws a fixed staircase.
static float lfoShapeAt(LfoWave w, float p) {
    switch (w) {
        case LfoWave::SINE:     return sinf(6.283185307f * p);
        case LfoWave::TRIANGLE: return (p < 0.5f) ? (p * 4.f - 1.f) : (3.f - p * 4.f);
        case LfoWave::SAW:      return p * 2.f - 1.f;
        case LfoWave::SQUARE:   return (p < 0.5f) ? 1.f : -1.f;
        case LfoWave::SAMPLE_HOLD: {
            static const float SH[4] = { 0.6f, -0.3f, 0.9f, -0.7f };
            return SH[clampi((int)(p * 4.f), 0, 3)];
        }
        default: return 0.f;
    }
}

// One cycle of the shape, and a marker at the phase audioTask is at. The
// dot rides on the actual output, so the Delay fade-in shows as the dot
// growing out of the centre line.
static void drawLfoShape(int x, int y, int w, int h) {
    canvas.drawRect(x, y, w, h, uiDim);
    int cy = y + h / 2, amp = h / 2 - 3;
    for (int i = x + 2; i < x + w - 2; i += 4) canvas.drawPixel(i, cy, uiDim);
    int px = -1, py = 0;
    for (int i = 0; i < w - 4; i++) {
        float p = (float)i / (float)(w - 5);
        int sx = x + 2 + i, sy = cy - (int)(lfoShapeAt(patch.lfoWave, p) * amp);
        if (px >= 0) canvas.drawLine(px, py, sx, sy, uiColor);
        px = sx; py = sy;
    }
    float ph = clampf(audioStatus.lfoPhase, 0.f, 1.f);
    int mx = x + 2 + (int)(ph * (float)(w - 5));
    int my = cy - (int)(clampf(audioStatus.lfoValue, -1.f, 1.f) * amp);
    canvas.drawFastVLine(mx, y + 1, h - 2, uiDim);
    // The dot is sampled at the screen's ~16 redraws a second, so a fast LFO
    // can look slow or even backwards (a wagon wheel on film) - the sound is
    // right. A trail was tried in v0.87T2 and made it stand out more.
    canvas.fillCircle(mx, my, 3, WHITE);
}

static void targetLabel(char *buf, size_t n, ModTarget t);   // PLAY section

static void drawLfoScreen() {
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, 14); canvas.print("LFO (all voices)");
    drawItemList(lfoList(), LFO_ITEMS, lfoNav.sel, 26, -1, 13);
    canvas.drawFastVLine(119, 24, 66, uiDim);
    drawLfoShape(123, 24, 113, 66);
    // live output and where it goes
    float lv = audioStatus.lfoValue;
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, 98); canvas.print("Out");
    drawBar(28, 98, 88, 8, (lv + 1.f) * 0.5f, uiColor);
    char lt[12]; targetLabel(lt, sizeof(lt), patch.lfoTarget);
    canvas.setCursor(123, 98);
    const char *dest = (patch.lfoTarget == ModTarget::OFF) ? "(Off)" : (patch.lfoDepth > 0.f ? lt : "(Depth 0)");
    canvas.printf("%+.2f > %s", lv, dest);
    // On the Target row: what the destination does (v0.85)
    drawNavHint(lfoNav.sel == 3 ? modTargetHelp(patch.lfoTarget) : ";/. row  ,// value  Tab:next");
}

static void lfoHandleKeys(const KeyFrame &k) {
    if (listHandleKeys(lfoNav, lfoList(), LFO_ITEMS, k)) uiDirty = true;
}

// ===========================================================================
// FX tab - pad selector + parameter pages (spec 6.2)
// ===========================================================================
enum FxView : uint8_t { FX_PADS = 0, FX_PARAMS };
static FxView   fxView = FX_PADS;
static int      fxCursor = 0;
// Pads in key order (q w e r t from v0.56): Chorus, Delay, Reverb, Crush,
// Limiter. The processing order differs - Crush runs first - and the title
// line shows that one.
constexpr int NUM_FX_PADS = 5;
constexpr int FX_PAD_CRUSH = 3, FX_PAD_LIMITER = 4;
static int      fxItemCount[NUM_FX_PADS] = { 3, 3, 3, 3, 2 };
static ListNav  fxNav;
static RepeatKey fxLeft, fxRight;
// v0.72: the fifth pad is Drive (the x/(1+|x|) curve, a sound). Protecting
// the level is the output limiter's job now, which is always on.
static const char *FX_NAMES[NUM_FX_PADS] = { "Chorus", "Delay", "Reverb", "Crush", "Drive" };
static const char *FX_PAD_LABELS[NUM_FX_PADS] = { "Chorus", "Delay", "Reverb", "Crush", "Drive" };   // fit a 44px pad

// The parameter page of one pad (the pad selector itself has no list).
static void bindFxItems(int pad) {
    MenuItem *it = itemPool; itemPoolOwner = IO_FX0 + pad;
    switch (pad) {
        case 0:
            it[0] = itemFloat("Rate",  &patch.chorusRateHz, 0.1f, 5.f, 0.1f, "%.1fHz");
            it[1] = itemFloat("Depth", &patch.chorusDepthMs, 0.f, 20.f, 0.5f, "%.1fms", ModTarget::CHORUS_DEPTH);
            it[2] = itemFloat("Mix",   &patch.chorusMix, 0.f, 100.f, 1.f, "%.0f%%", ModTarget::CHORUS_MIX);
            break;
        case 1:
            it[0] = itemFloat("Time",  &patch.delayTimeMs, 50.f, 600.f, 10.f, "%.0fms");
            it[1] = itemFloat("Fdbk",  &patch.delayFeedback, 0.f, 90.f, 1.f, "%.0f%%", ModTarget::DELAY_FDBK);
            it[2] = itemFloat("Mix",   &patch.delayMix, 0.f, 100.f, 1.f, "%.0f%%", ModTarget::DELAY_MIX);
            break;
        case 2:
            it[0] = itemFloat("Size",  &patch.reverbSize, 0.f, 100.f, 1.f, "%.0f%%", ModTarget::REVERB_SIZE);
            it[1] = itemFloat("Damp",  &patch.reverbDamp, 0.f, 100.f, 1.f, "%.0f%%");
            it[2] = itemFloat("Mix",   &patch.reverbMix, 0.f, 100.f, 1.f, "%.0f%%", ModTarget::REVERB_MIX);
            break;
        case 3:
            it[0] = itemFloat("Bits",  &patch.crushBits, 1.f, 16.f, 0.1f, "%.1f", ModTarget::CRUSH_BITS);
            it[1] = itemFloat("Down",  &patch.crushDown, 1.f, 32.f, 0.1f, "%.1fx", ModTarget::CRUSH_DOWN);
            it[2] = itemFloat("Mix",   &patch.crushMix, 0.f, 100.f, 1.f, "%.0f%%", ModTarget::CRUSH_MIX);
            break;
        default:
            it[0] = itemFloat("Amount", &patch.limiterDrive, 1.f, 5.f, 0.1f, "%.1fx", ModTarget::LIMITER_DRIVE);
            it[1] = itemFloat("Mix",   &patch.limiterMix, 0.f, 100.f, 1.f, "%.0f%%", ModTarget::DRIVE_MIX);
            break;
    }
}
static MenuItem *fxList(int pad) { if (itemPoolOwner != IO_FX0 + pad) bindFxItems(pad); return itemPool; }

static float *fxMixPtr(int i) {
    switch (i) {
        case 0: return &patch.chorusMix;
        case 1: return &patch.delayMix;
        case 2: return &patch.reverbMix;
        case FX_PAD_CRUSH: return &patch.crushMix;
        default: return &patch.limiterMix;
    }
}
static bool fxIsOn(int i) { return *fxMixPtr(i) > 0.5f; }
// The pad state of the sound being heard (livePatch while morphing)
static bool fxPadOnHeard(int i) {
    const Patch &p = morphRunning() ? livePatch : patch;
    float m;
    switch (i) {
        case FXP_CHORUS: m = p.chorusMix; break;
        case FXP_DELAY:  m = p.delayMix; break;
        case FXP_REVERB: m = p.reverbMix; break;
        case FXP_CRUSH:  m = p.crushMix; break;
        default:         m = p.limiterMix; break;
    }
    return m > 0.5f;
}
static void fxToggle(int i) {
    float *m = fxMixPtr(i);
    *m = fxIsOn(i) ? 0.f : FX_PAD_DEFAULT_MIX[i];
    // The pad itself changed: a leftover bypass / temporary On of it would
    // only confuse what the pad now says (v0.57)
    fxBypassMask = (uint8_t)(fxBypassMask & ~(1u << i));
    fxForceMask  = (uint8_t)(fxForceMask & ~(1u << i));     // on restores the DEFAULT, not the last value (spec 6.3)
}

static void setInfo(const char *msg);   // SETTING section, below

// Is this effect sounding right now: pad On and not bypassed, or pad Off
// and temporarily forced On.
static bool fxPadActive(int i) {
    uint8_t bit = (uint8_t)(1u << i);
    return fxPadOnHeard(i) ? !(fxBypassEffective() & bit) : (fxForceEffective() & bit) != 0;
}

// q w e r t (v0.56; v0.57: works on every pad). The key flips the effect:
// a pad that is On in the patch is bypassed / restored, a pad that is Off is
// temporarily turned On at its default Mix / back Off. Neither is saved.
void uiFxBypassKey(int pad) {
    if (pad < 0 || pad >= FX_PAD_COUNT) return;
    char b[40];
    uint8_t bit = (uint8_t)(1u << pad);
    if (fxPadOnHeard(pad)) {
        fxBypassMask = fxBypassMask ^ bit;
        snprintf(b, sizeof(b), "%s: %s", FX_NAMES[pad], (fxBypassMask & bit) ? "Bypass" : "On");
    } else {
        fxForceMask = fxForceMask ^ bit;
        snprintf(b, sizeof(b), "%s: %s", FX_NAMES[pad], (fxForceMask & bit) ? "On (temp)" : "Off");
    }
    setInfo(b);
}

static void drawFxScreen() {
    canvas.setTextColor(uiColor, BLACK);
    if (fxView == FX_PADS) {
        canvas.setCursor(4, 14); canvas.print("FX: Crush>Chorus>Delay>Reverb>Drive");
        const int PAD_W = 44, PAD_H = 34, PAD_Y = 40;
        for (int i = 0; i < NUM_FX_PADS; i++) {
            int x = 5 + i * 46;
            bool on = fxIsOn(i);
            if (on) { canvas.fillRect(x, PAD_Y, PAD_W, PAD_H, uiColor); canvas.setTextColor(BLACK, uiColor); }
            else    { canvas.drawRect(x, PAD_Y, PAD_W, PAD_H, uiColor); canvas.setTextColor(uiColor, BLACK); }
            canvas.setCursor(x + (PAD_W - (int)strlen(FX_PAD_LABELS[i]) * 6) / 2, PAD_Y + 4); canvas.print(FX_PAD_LABELS[i]);
            canvas.setCursor(x + (PAD_W - 3 * 6) / 2, PAD_Y + 14); canvas.print(on ? "ON" : "off");
            // Bypass (v0.56) / temporary On (v0.57): their own inverted tag on
            // the third line, so they never read as the pad's saved On/Off
            const char *tag = nullptr;
            if (on && (fxBypassEffective() & (1u << i)))   tag = "BYP";
            if (!on && (fxForceEffective() & (1u << i)))   tag = "TMP";
            if (tag) {
                uint16_t bg = on ? BLACK : uiColor, fg = on ? uiColor : BLACK;
                canvas.fillRect(x + 8, PAD_Y + 23, PAD_W - 16, 9, bg);
                canvas.setTextColor(fg, bg);
                canvas.setCursor(x + (PAD_W - 3 * 6) / 2, PAD_Y + 24); canvas.print(tag);
            }
            if (i == fxCursor) canvas.drawRect(x - 2, PAD_Y - 2, PAD_W + 4, PAD_H + 4, WHITE);
        }
        canvas.setTextColor(uiColor, BLACK);
        canvas.setCursor(4, 90);
        canvas.printf("%s  Mix %.0f%%", FX_NAMES[fxCursor], *fxMixPtr(fxCursor));
        drawNavHint(",//:pad Ent:on/off .:prm qwert:BYP");
    } else {
        canvas.setCursor(4, 14); canvas.printf("FX > %s", FX_NAMES[fxCursor]);
        drawItemList(fxList(fxCursor), fxItemCount[fxCursor], fxNav.sel, 30, -1, 13);
        drawNavHint(";/. row  ,// value  Tab:pads");
    }
}

// Returns true when it consumed Tab (param page -> back to pads)
static bool fxHandleKeys(const KeyFrame &k) {
    if (fxView == FX_PADS) {
        if (repeatFire(fxLeft, keyDown(k, ',') || keyDown(k, '<')))  { fxCursor = (fxCursor + NUM_FX_PADS - 1) % NUM_FX_PADS; uiDirty = true; }
        if (repeatFire(fxRight, keyDown(k, '/') || keyDown(k, '?'))) { fxCursor = (fxCursor + 1) % NUM_FX_PADS; uiDirty = true; }
        if (k.enterPressed && !k.shift) { fxToggle(fxCursor); uiDirty = true; }
        if (keyPressed(k, '.') || keyPressed(k, '>')) { fxView = FX_PARAMS; fxNav.sel = 0; memset(&fxNav.up, 0, sizeof(RepeatKey) * 4); uiDirty = true; }
        return false;
    }
    if (k.tabPressed) { fxView = FX_PADS; uiDirty = true; return true; }
    if (listHandleKeys(fxNav, fxList(fxCursor), fxItemCount[fxCursor], k)) uiDirty = true;
    return false;
}

// ===========================================================================
// SETTING tab, patch browser, name entry, confirm
// ===========================================================================
enum BrowserAction : uint8_t { BA_LOAD = 0, BA_RENAME, BA_DUP, BA_DELETE, BA_MORPH_PICK };
enum NamePurpose  : uint8_t { NP_SAVE_AS = 0, NP_RENAME, NP_DUP, NP_CHAIN_SAVE };   // v0.83: a chain
// SETTING is two levels (v0.11, like C.P.S.): a category list, then the
// category's own item list. Tab inside a category returns to the list.
enum SetCategory : uint8_t { SC_PATCH = 0, SC_DISPLAY, SC_MIDI, SC_IMU, SC_THEREMIN, SC_GLITCH, SC_PATTERN, SC_INFO, SC_COUNT };
static const char *SET_CATEGORY_NAMES[SC_COUNT] = { "Patch", "Display", "MIDI", "IMU", "Theremin", "Glitch", "ARP Pattern", "Info" };
#ifdef MORPHASE_BLE
constexpr int   SET_MIDI_ITEMS = 9;    // + BLE On/Off (v0.77)
#else
constexpr int   SET_MIDI_ITEMS = 8;    // PC Mode added in v0.8
#endif
constexpr int   SET_MIDI_SPLIT = 5;    // two columns (v0.8): settings left, sub-pages (and BLE) right
// SETTING > Theremin (v0.81): the left column is common, the right one is
// Mod's or Play's, so the count follows the Mode (thereminItemCount()).
constexpr int   TH_SPLIT = 6;
constexpr int   TH_ROW_SCALE = TH_SPLIT + 1;    // Play: Pitch, Scale, Top, Span, VolAxis
static int      setItemCount[SC_COUNT] = { 10, 6, SET_MIDI_ITEMS, 13, TH_SPLIT + 3, 9, 4, 0 };
static int thereminItemCount() { return TH_SPLIT + (sys.thMode == ThMode::PLAY ? 5 : 3); }
constexpr int   IMU_CALIB_ROW = 12;
static int      setCategory = 0;
static bool     setInCategory = false;
static ListNav  setNav;          // item list inside a category
static RepeatKey catUp, catDown;

static BrowserAction browserAction = BA_LOAD;
static int  browserCursor = 0;
static RepeatKey brUp, brDown;
static NamePurpose namePurpose = NP_SAVE_AS;
static char nameBuf[PATCH_NAME_LEN + 1];
static char nameSource[PATCH_NAME_LEN + 1];     // rename/dup source
static char overwriteName[PATCH_NAME_LEN + 1];  // pending save target
static char infoLine[40] = "";
static unsigned long infoUntilMs = 0;

static void setInfo(const char *msg) { strncpy(infoLine, msg, sizeof(infoLine) - 1); infoLine[sizeof(infoLine) - 1] = 0; infoUntilMs = millis() + 2500; uiDirty = true; }

// The list is rebuilt at boot and after every Save / SaveAs / Delete /
// Rename / Dup / Factory, so opening it reads nothing (v0.90T2). Walking
// the Patch folder twice (tmp recovery + names) stalled loop() - MIDI,
// ARP, screen - every time Load was pressed. Only an empty list (a card
// put in after boot) is read here.
static void openBrowser(BrowserAction a) {
    if (patchCount() == 0) scanPatches();
    browserAction = a;
    int cur = patchIndexOfName(patch.name);
    browserCursor = cur >= 0 ? cur : 0;
    uiSetMode(AppMode::PATCH_BROWSER);
}
static void openNameEntry(NamePurpose p, const char *initial) {
    namePurpose = p;
    strncpy(nameBuf, initial, PATCH_NAME_LEN); nameBuf[PATCH_NAME_LEN] = 0;
    uiSetMode(AppMode::NAME_ENTRY);
}

// Save under `name`; asks first when a file of that name already exists (v0.11)
static void saveWithConfirm(const char *name) {
    // A factory name is never saved over (v0.75): the file stays the factory
    // sound, so a new version can update it without losing anyone's work.
    if (patchIsFactoryName(name)) { setInfo("Factory name - pick another"); return; }
    if (patchIndexOfName(name) < 0) scanPatches();
    if (patchIndexOfName(name) >= 0) {
        strncpy(overwriteName, name, PATCH_NAME_LEN); overwriteName[PATCH_NAME_LEN] = 0;
        confirmKind = CK_OVERWRITE;
        uiSetMode(AppMode::CONFIRM);
        return;
    }
    setInfo(savePatchFile(name) ? "Saved" : "Save FAILED");
    saveSettings();
    scanPatches();
    uiSetMode(AppMode::SETTING);
}

static void actLoad()   { openBrowser(BA_LOAD); }
// While morphing, Save keeps the in-between sound as a NEW patch: the name
// shown is a chain patch's, and saving over it would replace one end of the
// morph with its middle (update spec 5.2).
// A name to offer when a factory patch is saved: "01_EPiano" -> "EPiano2".
static void suggestOwnName(char *out, size_t n, const char *from) {
    const char *base = from;
    if (from[0] >= '0' && from[0] <= '9' && from[1] >= '0' && from[1] <= '9' && from[2] == '_') base = from + 3;
    snprintf(out, n, "%.*s2", PATCH_NAME_LEN - 1, base);
}

static void actSave()   {
    if (morphRunning()) { morphCommitAndStop(); openNameEntry(NP_SAVE_AS, patch.name); return; }
    // Save on a factory patch becomes SaveAs (v0.75, set 2 decision 4)
    if (patchIsFactoryName(patch.name)) {
        char sug[PATCH_NAME_LEN + 2]; suggestOwnName(sug, sizeof(sug), patch.name);
        openNameEntry(NP_SAVE_AS, sug);
        setInfo("Factory patch: save as new");
        return;
    }
    if (patchNameValid(patch.name)) saveWithConfirm(patch.name);
    else openNameEntry(NP_SAVE_AS, patch.name);
}
static void actSaveAs() {
    morphCommitAndStop();
    if (patchIsFactoryName(patch.name)) {
        char sug[PATCH_NAME_LEN + 2]; suggestOwnName(sug, sizeof(sug), patch.name);
        openNameEntry(NP_SAVE_AS, sug);
    } else openNameEntry(NP_SAVE_AS, patch.name);
}
static bool morphPage = false;       // SETTING > Patch > Morph is open
static int  morphSel = 0;            // 0 Chain, 1 Pos, 2 Time, 3 Demo, 4-11 slots
constexpr int MORPH_ROW_CHAINS = 3;  // v0.83: the saved chains (was Demo, v0.76-v0.82)
constexpr int MORPH_ROW_SLOT0 = 4;
static int  morphPickSlot = 0;
static void actMorph()  { morphPage = true; morphSel = 0; uiDirty = true; }
static void actRename() { openBrowser(BA_RENAME); }
static void actDup()    { openBrowser(BA_DUP); }
static void actDelete() { openBrowser(BA_DELETE); }
static void actReset()  { confirmKind = CK_RESET; uiSetMode(AppMode::CONFIRM); }
static void actRandom() { patchRandomize(); setInfo("Randomized"); }
// Factory (v0.75): restores every factory patch to its original sound - the
// deleted ones come back, the rest are rewritten. Nothing of the player's is
// lost: a factory name cannot be saved over on the device.
static void actFactory() {
    int n = writeFactoryPatches(true);
    char b[40];
    if (n < 0) snprintf(b, sizeof(b), "Factory: write FAILED");
    else       snprintf(b, sizeof(b), "Factory: %d restored", n);
    setInfo(b);
}
static void actPtnLoad(){ setInfo(loadPatternSlot(sys.patternSlot) ? "Pattern loaded" : "No pattern"); }
static void actPtnSave(){ setInfo(savePatternSlot(sys.patternSlot) ? "Pattern saved" : "Save FAILED"); }
static void actPtnRandom(){ arpRandomizePattern(); setInfo("Pattern randomized"); }

// Load a pattern slot and make it the current one. Called from the menu and
// from the MIDI pattern-select CCs; both go through here so the slot number
// and the loaded data can never disagree.
void selectPatternSlot(int slot) {
    slot = clampi(slot, 0, NUM_PATTERN_SLOTS - 1);
    sys.patternSlot = slot;
    loadPatternSlot(slot);
    uiDirty = true;
}

static void chainStep(int dir);   // v0.84, with the chain page below

// Every MIDI switch action runs the same function a key or a menu would.
// `rising` marks the 0->1 edge; one-shot actions use only that, so a
// controller that keeps sending while held cannot fire them repeatedly.
void applySwitchAction(SwitchAction a, bool high, bool rising, bool momentary) {
    switch (a) {
        // --- stateful: Toggle flips on each press, Momentary follows the button
        case SwitchAction::ARP_ON_OFF:
            if (momentary) { if (arp.on != high) arpSetOn(high); }
            else if (rising) arpSetOn(!arp.on);
            break;
        case SwitchAction::ARP_LATCH:
            // Through arpToggleLatch() so the chord is rebuilt by its one
            // entry point rather than by poking arp.latch directly.
            if (momentary) { if (arp.latch != high) arpToggleLatch(); }
            else if (rising) arpToggleLatch();
            break;
        case SwitchAction::NOTE_HOLD:
            // The same path as the D key. Not the sustain pedal: this holds
            // what is sounding until it is switched off again (spec B.4).
            if (momentary) sustainInput(SUS_KEY, high);
            else if (rising) sustainInput(SUS_KEY, !sustainKeyHeld());
            break;
        case SwitchAction::GLIDE_ON_OFF:
            if (momentary) patch.glideEnabled = high;
            else if (rising) patch.glideEnabled = !patch.glideEnabled;
            break;
        // FX bypass (v0.56). Toggle = the q w e r t key. Momentary flips the
        // current state only while the button is held.
        case SwitchAction::BYP_CHORUS: case SwitchAction::BYP_DELAY: case SwitchAction::BYP_REVERB:
        case SwitchAction::BYP_CRUSH:  case SwitchAction::BYP_LIMITER: {
            int pad = (int)a - (int)SwitchAction::BYP_CHORUS;
            uint8_t bit = (uint8_t)(1u << pad);
            if (momentary) {
                // Held: flip the effect - bypass a pad that is On, force On a
                // pad that is Off. Released: both back to normal.
                if (high) {
                    if (fxPadOnHeard(pad)) fxBypassMomentary = (uint8_t)(fxBypassMomentary | bit);
                    else                   fxForceMomentary  = (uint8_t)(fxForceMomentary | bit);
                } else {
                    fxBypassMomentary = (uint8_t)(fxBypassMomentary & ~bit);
                    fxForceMomentary  = (uint8_t)(fxForceMomentary & ~bit);
                }
            } else if (rising) uiFxBypassKey(pad);
            break;
        }
        // GLITCH (v0.6): Toggle switches the always-on mode (saved with the
        // other settings, not on every press); Momentary = the g key
        case SwitchAction::GLITCH_SW:
            if (momentary) glitchSetSwitchHeld(high);
            else if (rising) { sys.glOn = !sys.glOn; setInfo(sys.glOn ? "Glitch: On" : "Glitch: Off"); }
            break;
        // Chains (v0.84): one-shot, the rising edge only
        case SwitchAction::CHAIN_NEXT: if (rising) chainStep(+1); break;
        case SwitchAction::CHAIN_PREV: if (rising) chainStep(-1); break;
        // Theremin (v0.81): Toggle flips it, Momentary = on while held
        case SwitchAction::THEREMIN_SW:
            if (momentary) { if (sys.thOn != high) thereminSetEnabled(high); }
            else if (rising) { thereminSetEnabled(!sys.thOn); setInfo(sys.thOn ? "Theremin: On" : "Theremin: Off"); }
            break;
        // --- one-shot: the rising edge only, whatever the mode says
        case SwitchAction::PATCH_NEXT:
        case SwitchAction::PATCH_PREV: {
            if (!rising) return;
            if (patchCount() == 0) scanPatches();
            if (patchCount() == 0) return;
            int cur = patchIndexOfName(patch.name);
            if (cur < 0) cur = 0;
            int next = (a == SwitchAction::PATCH_NEXT) ? cur + 1 : cur - 1;
            if (next < 0) next = patchCount() - 1;
            if (next >= patchCount()) next = 0;
            uiLoadPatchByIndex(next);
            break;
        }
        case SwitchAction::PATTERN_NEXT:
        case SwitchAction::PATTERN_PREV: {
            if (!rising) return;
            int next = sys.patternSlot + ((a == SwitchAction::PATTERN_NEXT) ? 1 : -1);
            if (next < 0) next = NUM_PATTERN_SLOTS - 1;
            if (next >= NUM_PATTERN_SLOTS) next = 0;
            selectPatternSlot(next);
            break;
        }
        case SwitchAction::TAP_TEMPO:          if (rising) arpTapTempo(); break;
        case SwitchAction::RANDOMIZE_PATCH:    if (rising) patchRandomize(); break;
        case SwitchAction::RANDOMIZE_PATTERN:  if (rising) arpRandomizePattern(); break;
        case SwitchAction::PANIC:
            if (rising) { postAllNotesOff(); if (arp.on) arpPanic(); }
            break;
        default: return;
    }
    uiDirty = true;   // PLAY follows a MIDI change as fast as a key press
}

// --- SETTING > Theremin rows (v0.81). Steps as C.P.S.: Far 50mm, Top an octave.
static const char *thNearLabel() { static char b[10]; snprintf(b, sizeof(b), "%dmm", sys.thNearMm); return b; }
static void thNearInc() { sys.thNearMm = clampi(sys.thNearMm + 10, TH_NEAR_MIN, TH_NEAR_MAX);
                          if (sys.thFarMm < sys.thNearMm + TH_FAR_GAP) sys.thFarMm = sys.thNearMm + TH_FAR_GAP; }
static void thNearDec() { sys.thNearMm = clampi(sys.thNearMm - 10, TH_NEAR_MIN, TH_NEAR_MAX); }
static const char *thFarLabel() { static char b[10]; snprintf(b, sizeof(b), "%dmm", sys.thFarMm); return b; }
static void thFarInc() { sys.thFarMm = clampi(sys.thFarMm + 50, sys.thNearMm + TH_FAR_GAP, TH_FAR_MAX); }
static void thFarDec() { sys.thFarMm = clampi(sys.thFarMm - 50, sys.thNearMm + TH_FAR_GAP, TH_FAR_MAX); }
static const char *thScaleLabel() { return TH_SCALES[clampi(sys.thScale, 0, TH_SCALE_COUNT - 1)].shortName; }
static void thScaleInc() { sys.thScale = (clampi(sys.thScale, 0, TH_SCALE_COUNT - 1) + 1) % TH_SCALE_COUNT; }
static void thScaleDec() { sys.thScale = (clampi(sys.thScale, 0, TH_SCALE_COUNT - 1) + TH_SCALE_COUNT - 1) % TH_SCALE_COUNT; }
static const char *thTopLabel() {
    static char b[8];
    static const char *NAMES[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int n = 60 + sys.thTopSemis;      // C4 = MIDI 60
    snprintf(b, sizeof(b), "%s%d", NAMES[((n % 12) + 12) % 12], n / 12 - 1);
    return b;
}
static void thTopInc() { sys.thTopSemis = clampi(sys.thTopSemis + 12, TH_TOP_MIN, TH_TOP_MAX); }
static void thTopDec() { sys.thTopSemis = clampi(sys.thTopSemis - 12, TH_TOP_MIN, TH_TOP_MAX); }
static const char *thSpanLabel() { static char b[8]; snprintf(b, sizeof(b), "%d oct", sys.thSpan); return b; }
static void thSpanInc() { sys.thSpan = clampi(sys.thSpan + 1, 1, 4); }
static void thSpanDec() { sys.thSpan = clampi(sys.thSpan - 1, 1, 4); }

static const char *midiChLabel() { static char b[8]; if (sys.midiChannel == 0) return "Omni"; snprintf(b, 8, "%d", sys.midiChannel); return b; }
static void midiChInc() { sys.midiChannel = (sys.midiChannel + 1) % 17; }
static void midiChDec() { sys.midiChannel = (sys.midiChannel + 16) % 17; }

// ---------------------------------------------------------------------------
// SETTING > MIDI is its own two-level screen (spec B.7): the main page, a
// page per slot family, and a read-only overview of every assignment.
// ---------------------------------------------------------------------------
enum MidiPage : uint8_t { MP_MAIN = 0, MP_CC, MP_SW, MP_LIST };
static MidiPage midiPage = MP_MAIN;
static ListNav  midiNav;
static int      midiListScroll = 0;
static RepeatKey listUp, listDown;
static int     *learnField = nullptr;    // the CC field MIDI Learn will fill

static void actMidiCc()   { midiPage = MP_CC;   midiNav.sel = 0; memset(&midiNav.up, 0, sizeof(RepeatKey) * 4); }
static void actMidiSw()   { midiPage = MP_SW;   midiNav.sel = 0; memset(&midiNav.up, 0, sizeof(RepeatKey) * 4); }
static void actMidiList() { midiPage = MP_LIST; midiListScroll = 0; }

// How many places claim this CC number - used to warn about a collision
// rather than to forbid one (spec B.2).
static int ccUseCount(int cc) {
    if (cc < 0) return 0;
    int n = 0;
    static const int fixed[] = { 1, 2, 7, 11, 64, 74 };
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) if (fixed[i] == cc) n++;
    for (int i = 0; i < NUM_CC_SLOTS; i++) if (sys.ccSlot[i].number == cc && sys.ccSlot[i].target != ModTarget::OFF) n++;
    for (int i = 0; i < NUM_SW_SLOTS; i++) if (sys.swSlot[i].number == cc && sys.swSlot[i].action != SwitchAction::OFF) n++;
    if (sys.patternBaseCc != CC_OFF && cc >= sys.patternBaseCc && cc < sys.patternBaseCc + NUM_PATTERN_SLOTS) n++;
    return n;
}

// The CC Slots page (sw = false) or the Switches page (sw = true).
static void bindMidiSlotItems(bool sw) {
    static_assert(NUM_CC_SLOTS * 3 <= ITEM_POOL_SIZE && NUM_SW_SLOTS * 3 <= ITEM_POOL_SIZE, "item pool too small");
    MenuItem *midiCcItems = itemPool, *midiSwItems = itemPool;
    itemPoolOwner = sw ? IO_MIDI_SW : IO_MIDI_CC;
    static const char *ccNames[NUM_CC_SLOTS * 3] = {
        "1 CC", "1 Tgt", "1 Dep", "2 CC", "2 Tgt", "2 Dep",
        "3 CC", "3 Tgt", "3 Dep", "4 CC", "4 Tgt", "4 Dep"
    };
    static const char *swNames[NUM_SW_SLOTS * 3] = {
        "1 CC", "1 Act", "1 Mod", "2 CC", "2 Act", "2 Mod",
        "3 CC", "3 Act", "3 Mod", "4 CC", "4 Act", "4 Mod"
    };
    for (int i = 0; i < NUM_CC_SLOTS && !sw; i++) {
        midiCcItems[i * 3 + 0] = itemCc(ccNames[i * 3 + 0], &sys.ccSlot[i].number);
        midiCcItems[i * 3 + 1] = itemEnum(ccNames[i * 3 + 1], (uint8_t *)&sys.ccSlot[i].target, MOD_TARGET_NAMES,
                                          (int)ModTarget::MOD_TARGET_COUNT, 0, (int)ModTarget::MOD_AUDIO_END);
        midiCcItems[i * 3 + 2] = itemInt(ccNames[i * 3 + 2], &sys.ccSlot[i].depth, 0, 100);
    }
    for (int i = 0; i < NUM_SW_SLOTS && sw; i++) {
        midiSwItems[i * 3 + 0] = itemCc(swNames[i * 3 + 0], &sys.swSlot[i].number);
        midiSwItems[i * 3 + 1] = itemEnum(swNames[i * 3 + 1], (uint8_t *)&sys.swSlot[i].action, SWITCH_ACTION_NAMES,
                                          (int)SwitchAction::SWITCH_ACTION_COUNT);
        midiSwItems[i * 3 + 2] = itemEnum(swNames[i * 3 + 2], (uint8_t *)&sys.swSlot[i].mode, SWITCH_MODE_NAMES,
                                          (int)SwitchMode::SWITCH_MODE_COUNT);
    }
}
static MenuItem *midiCcList() { if (itemPoolOwner != IO_MIDI_CC) bindMidiSlotItems(false); return itemPool; }
static MenuItem *midiSwList() { if (itemPoolOwner != IO_MIDI_SW) bindMidiSlotItems(true); return itemPool; }

// One SETTING category's list.
static void bindSetItems(int c) {
    static_assert(13 <= ITEM_POOL_SIZE, "item pool too small");
    itemPoolOwner = IO_SET0 + c;
    if (c == SC_PATCH) {
    MenuItem *p = itemPool;
    p[0] = itemAction("Load", actLoad);
    p[1] = itemAction("Save", actSave);
    p[2] = itemAction("SaveAs", actSaveAs);
    p[3] = itemAction("Rename", actRename);
    p[4] = itemAction("Dup", actDup);
    p[5] = itemAction("Delete", actDelete);
    p[6] = itemAction("Reset", actReset);
    p[7] = itemAction("Random", actRandom);
    p[8] = itemAction("Morph >", actMorph);
    p[9] = itemAction("Factory", actFactory);   // v0.72: write the missing factory patches
    } else if (c == SC_DISPLAY) {
    MenuItem *d = itemPool;
    d[0] = itemEnum("Theme", (uint8_t *)&sys.theme, UI_THEME_NAMES, (int)UiTheme::UI_THEME_COUNT);
    d[1] = itemInt("Bright", &sys.brightness, 10, 100);
    // Text over the PLAY scope, each on its own switch (v0.56)
    d[2] = itemBool("GM/FB", &sys.scopeGmFb);
    d[3] = itemBool("MorphPos", &sys.scopeMorph);
    d[4] = itemBool("FX stat", &sys.scopeFx);
    d[5] = itemBool("GL", &sys.scopeGl);
    } else if (c == SC_GLITCH) {
    // GLITCH (v0.6): device settings
    MenuItem *g = itemPool;
    g[0] = itemBool("Glitch", &sys.glOn);
    g[1] = itemEnum("Rate", (uint8_t *)&sys.glRate, GL_RATE_NAMES, (int)GlRate::GL_RATE_COUNT);
    g[2] = itemInt("Prob", &sys.glProb, 0, 100);
    g[2].modOf = ModTarget::GLITCH_PROB;          // v0.87: the * ~ ^ marks
    g[3] = itemInt("Hold%", &sys.glHold, 0, 100);
    g[4] = itemEnum("Buffer", (uint8_t *)&sys.glBuf, GL_BUF_NAMES, (int)GlBuf::GL_BUF_COUNT);
    g[5] = itemEnum("Chaos", (uint8_t *)&sys.glChaos, GL_CHAOS_NAMES, (int)GlChaos::GL_CHAOS_COUNT);
    g[6] = itemEnum("Length", (uint8_t *)&sys.glLen, GL_LEN_NAMES, (int)GlLen::GL_LEN_COUNT);
    g[7] = itemInt("Amount", &sys.glAmt, 0, 100);
    g[8] = itemEnum("Arp", (uint8_t *)&sys.glArp, GL_ARP_NAMES, (int)GlArp::GL_ARP_COUNT);   // v0.82: ARP glitch
    } else if (c == SC_MIDI) {
    MenuItem *m = itemPool;
    m[0] = itemCustom("Channel", midiChLabel, midiChInc, midiChDec);
    m[1] = itemInt("BendRng", &sys.bendRange, 1, 12);
    m[2] = itemBool("ArpSend", &arp.midiOut);
    m[3] = itemCc("PtnBase", &sys.patternBaseCc);
    m[4] = itemEnum("PC Mode", (uint8_t *)&sys.pcMode, PC_MODE_NAMES, (int)PcMode::PC_MODE_COUNT);   // v0.8
    // Right column (two columns since v0.8, the same order in both builds)
    m[5] = itemAction("CC Slots >", actMidiCc);
    m[6] = itemAction("Switches >", actMidiSw);
    m[7] = itemAction("Assigned >", actMidiList);
#ifdef MORPHASE_BLE
    m[8] = itemBool("BLE", &sys.bleMidiEnabled);   // applies at the next boot (v0.77)
#endif
    } else if (c == SC_IMU) {
    // Everything IMU in one place (v0.41): X in the left column, Y in the
    // right, Calib under Y. Target / Depth belong to the patch; the rest are
    // device settings. Tilt is a hand movement, so the targets may reach the
    // control destinations too.
    MenuItem *u = itemPool;
    u[0]  = itemEnum("XTarget", (uint8_t *)&patch.imuXTarget, MOD_TARGET_NAMES,
                     (int)ModTarget::MOD_TARGET_COUNT, 0, (int)ModTarget::MOD_AUDIO_END);
    u[1]  = itemFloat("XDepth", &patch.imuXDepth, 0.f, 100.f, 1.f, "%.0f%%");
    u[2]  = itemInt("XSens", &sys.imuXSens, 50, 200);
    u[3]  = itemBool("XInvert", &sys.imuXInvert);
    u[4]  = itemEnum("XCurve", (uint8_t *)&sys.imuXCurve, IMU_CURVE_NAMES, (int)ImuCurve::IMU_CURVE_COUNT);
    u[5]  = itemInt("XDead", &sys.imuXDeadzone, 0, 30);
    u[6]  = itemEnum("YTarget", (uint8_t *)&patch.imuYTarget, MOD_TARGET_NAMES,
                     (int)ModTarget::MOD_TARGET_COUNT, 0, (int)ModTarget::MOD_AUDIO_END);
    u[7]  = itemFloat("YDepth", &patch.imuYDepth, 0.f, 100.f, 1.f, "%.0f%%");
    u[8]  = itemInt("YSens", &sys.imuYSens, 50, 200);
    u[9]  = itemBool("YInvert", &sys.imuYInvert);
    u[10] = itemEnum("YCurve", (uint8_t *)&sys.imuYCurve, IMU_CURVE_NAMES, (int)ImuCurve::IMU_CURVE_COUNT);
    u[11] = itemInt("YDead", &sys.imuYDeadzone, 0, 30);
    u[IMU_CALIB_ROW] = itemCustom("Calib", calibLabel, calibToggle, calibToggle);
    } else if (c == SC_THEREMIN) {
    // v0.81. Left: common. Right: what the Mode uses (the other side hidden)
    MenuItem *h = itemPool;
    h[0] = itemBool("Theremin", &sys.thOn);
    h[1] = itemEnum("Mode", (uint8_t *)&sys.thMode, TH_MODE_NAMES, (int)ThMode::TH_MODE_COUNT);
    h[2] = itemEnum("Bus", (uint8_t *)&sys.thBus, TH_BUS_NAMES, (int)ThBus::TH_BUS_COUNT);
    h[3] = itemCustom("Near", thNearLabel, thNearInc, thNearDec);
    h[4] = itemCustom("Far", thFarLabel, thFarInc, thFarDec);
    h[5] = itemCustom("Rescan", thereminScanLabel, thereminRescan, thereminRescan);
    if (sys.thMode == ThMode::PLAY) {
        h[6]  = itemEnum("Pitch", (uint8_t *)&sys.thPitch, TH_PITCH_NAMES, (int)ThPitch::TH_PITCH_COUNT);
        h[TH_ROW_SCALE] = itemCustom("Scale", thScaleLabel, thScaleInc, thScaleDec);
        h[8]  = itemCustom("Top", thTopLabel, thTopInc, thTopDec);
        h[9]  = itemCustom("Span", thSpanLabel, thSpanInc, thSpanDec);
        h[10] = itemEnum("VolAxis", (uint8_t *)&sys.thVolAxis, TH_VOLAXIS_NAMES, (int)ThVolAxis::TH_VOLAXIS_COUNT);
    } else {
        h[6] = itemEnum("Target", (uint8_t *)&sys.thTarget, MOD_TARGET_NAMES,
                        (int)ModTarget::MOD_TARGET_COUNT, 0, (int)ModTarget::MOD_AUDIO_END);   // the IMU's list
        h[7] = itemFloat("Depth", &sys.thDepth, 0.f, 100.f, 1.f, "%.0f%%");
        h[8] = itemEnum("Release", (uint8_t *)&sys.thRelease, TH_RELEASE_NAMES, (int)ThRelease::TH_RELEASE_COUNT);
    }
    setItemCount[SC_THEREMIN] = thereminItemCount();
    } else if (c == SC_PATTERN) {
    MenuItem *t = itemPool;
    t[0] = itemInt("Slot", &sys.patternSlot, 0, NUM_PATTERN_SLOTS - 1);
    t[1] = itemAction("Load", actPtnLoad);
    t[2] = itemAction("Save", actPtnSave);
    t[3] = itemAction("Random", actPtnRandom);
    }
}
static MenuItem *setList(int c) { if (itemPoolOwner != IO_SET0 + c) bindSetItems(c); return itemPool; }

// One line per assignment, sorted by CC number, so a growing set of
// mappings stays readable (spec B.7).
struct CcAssign { int cc; char text[26]; };
static int buildAssignList(CcAssign *out, int maxN) {
    int n = 0;
    struct Fixed { int cc; const char *what; };
    static const Fixed FIXED[] = {
        { 1, "GMorph (fixed)" }, { 2, "Feedbk (fixed)" }, { 7, "Volume (fixed)" },
        { 11, "Expr (fixed)" }, { 64, "Sustain (fixed)" }, { 74, "GMorph (fixed)" }
    };
    for (unsigned i = 0; i < sizeof(FIXED) / sizeof(FIXED[0]) && n < maxN; i++) {
        out[n].cc = FIXED[i].cc; snprintf(out[n].text, sizeof(out[n].text), "%s", FIXED[i].what); n++;
    }
    for (int i = 0; i < NUM_CC_SLOTS && n < maxN; i++) {
        if (sys.ccSlot[i].number == CC_OFF || sys.ccSlot[i].target == ModTarget::OFF) continue;
        out[n].cc = sys.ccSlot[i].number;
        snprintf(out[n].text, sizeof(out[n].text), "Knob%d > %s %d%%", i + 1,
                 MOD_TARGET_NAMES[(int)sys.ccSlot[i].target], sys.ccSlot[i].depth);
        n++;
    }
    for (int i = 0; i < NUM_SW_SLOTS && n < maxN; i++) {
        if (sys.swSlot[i].number == CC_OFF || sys.swSlot[i].action == SwitchAction::OFF) continue;
        out[n].cc = sys.swSlot[i].number;
        snprintf(out[n].text, sizeof(out[n].text), "Sw%d > %s %s", i + 1,
                 SWITCH_ACTION_NAMES[(int)sys.swSlot[i].action], SWITCH_MODE_NAMES[(int)sys.swSlot[i].mode]);
        n++;
    }
    if (sys.patternBaseCc != CC_OFF && n < maxN) {
        out[n].cc = sys.patternBaseCc;
        snprintf(out[n].text, sizeof(out[n].text), "..%d Pattern 1-8", sys.patternBaseCc + NUM_PATTERN_SLOTS - 1);
        n++;
    }
    for (int i = 1; i < n; i++) {           // insertion sort by CC number
        CcAssign k = out[i]; int j = i - 1;
        while (j >= 0 && out[j].cc > k.cc) { out[j + 1] = out[j]; j--; }
        out[j + 1] = k;
    }
    return n;
}

static void drawMidiScreen() {
    canvas.setTextColor(uiColor, BLACK);
    switch (midiPage) {
        case MP_CC:
            canvas.setCursor(4, 14); canvas.print("MIDI > Knob slots (CC)");
            drawItemList(midiCcList(), NUM_CC_SLOTS * 3, midiNav.sel, 28, 6, 12);
            break;
        case MP_SW:
            canvas.setCursor(4, 14); canvas.print("MIDI > Switch slots (buttons)");
            drawItemList(midiSwList(), NUM_SW_SLOTS * 3, midiNav.sel, 28, 6, 12);
            break;
        case MP_LIST: {
            canvas.setCursor(4, 14); canvas.print("MIDI > Assigned CCs");
            CcAssign list[20];
            int n = buildAssignList(list, 20);
            const int ROWS = 8;
            if (midiListScroll > n - ROWS) midiListScroll = n - ROWS;
            if (midiListScroll < 0) midiListScroll = 0;
            for (int r = 0; r < ROWS; r++) {
                int i = midiListScroll + r; if (i >= n) break;
                canvas.setCursor(8, 28 + r * 11);
                canvas.printf("CC%3d  %s", list[i].cc, list[i].text);
            }
            if (n == 0) { canvas.setCursor(8, 28); canvas.print("(nothing assigned)"); }
            break;
        }
        case MP_MAIN:
        default:
            canvas.setCursor(4, 14); canvas.print("SET > MIDI");
            drawItemList(setList(SC_MIDI), setItemCount[SC_MIDI], setNav.sel, 28, SET_MIDI_SPLIT, 12);
            break;
    }
    // Learn banner - drawn last so it sits over the list it belongs to
    if (learnField && midiLearnActive()) {
        canvas.fillRect(14, 48, 212, 30, BLACK);
        canvas.drawRect(14, 48, 212, 30, uiColor);
        drawCenteredText(55, "Learn... turn a knob", uiColor);
        drawCenteredText(66, "Enter: cancel   (10s timeout)", uiColor);
    }
}

// --- SETTING > Patch > Morph > Chains (v0.83) ---------------------------------
// The list lives only while this page is open (BLE build: about 1K of heap to
// spare, so nothing is kept for good) - allocated on open, freed on Tab.
constexpr int CHAIN_LIST_MAX = 64;
typedef char ChainName[PATCH_NAME_LEN + 1];
static bool       chainPage = false;
static ChainName *chainNames = nullptr;
static int        chainCount = 0, chainSel = 0, chainAct = 0;   // 0 Load, 1 SaveAs, 2 Delete
static char       chainPreview[40] = "";
static RepeatKey  chUp, chDown, chDec, chInc;
static const char *CHAIN_ACTS[3] = { "Load", "SaveAs", "Delete" };

static const char *chainSelName() { return (chainNames && chainSel < chainCount) ? chainNames[chainSel] : ""; }
// The chain loaded (or saved) last - where Chain+ / Chain- step from (v0.84).
// Not saved: after a boot they start from the top of the list.
static char lastChain[PATCH_NAME_LEN + 1] = "";

// The selected chain's patches, "NN_" taken off, as far as the line goes
static void chainMakePreview() {
    chainPreview[0] = 0;
    if (!chainNames || chainSel >= chainCount) return;
    ChainName slots[MORPH_SLOTS]; int t;
    if (!chainRead(chainNames[chainSel], slots, &t)) return;
    int len = 0;
    for (int i = 0; i < MORPH_SLOTS; i++) {
        const char *nm = slots[i];
        if (!nm[0]) continue;
        if (nm[0] >= '0' && nm[0] <= '9' && nm[1] >= '0' && nm[1] <= '9' && nm[2] == '_') nm += 3;
        int n = snprintf(chainPreview + len, sizeof(chainPreview) - len, "%s%s", len ? " " : "", nm);
        if (n < 0 || len + n >= (int)sizeof(chainPreview) - 1) break;
        len += n;
    }
}

static void chainRefresh() {
    chainCount = chainNames ? chainList(chainNames, CHAIN_LIST_MAX) : 0;
    chainSel = clampi(chainSel, 0, chainCount > 0 ? chainCount - 1 : 0);
    chainMakePreview();
    uiDirty = true;
}

static void chainPageOpen() {
    if (!chainNames) chainNames = (ChainName *)malloc(sizeof(ChainName) * CHAIN_LIST_MAX);
    if (!chainNames) { setInfo("No memory for the list"); return; }
    chainPage = true; chainSel = 0; chainAct = 0;
    chainRefresh();
}

static void chainPageClose() {
    chainPage = false;
    free(chainNames); chainNames = nullptr; chainCount = 0;
    uiDirty = true;
}

static bool chainExists(const char *name) {
    for (int i = 0; i < chainCount; i++) if (strcmp(chainNames[i], name) == 0) return true;
    return false;
}

// The editing chain (slots 1-8 + Time) into a file
static bool chainSaveCurrent(const char *name) {
    const char *slots[MORPH_SLOTS];
    for (int i = 0; i < MORPH_SLOTS; i++) slots[i] = morphSlotName(i);
    bool ok = chainWrite(name, slots, clampi(sys.morphTimeMs, 0, MORPH_TIME_MAX_MS));
    setInfo(ok ? "Chain saved" : "Save FAILED");
    if (ok) { strncpy(lastChain, name, PATCH_NAME_LEN); lastChain[PATCH_NAME_LEN] = 0; }   // Chain+ / - go on from it
    chainRefresh();
    return ok;
}

// A file into the editing chain. A patch that is not on the card leaves its
// slot empty, and the player is told which.
static void chainLoadNamed(const char *name) {
    ChainName slots[MORPH_SLOTS]; int t;
    if (!chainRead(name, slots, &t)) { setInfo("Load FAILED"); return; }
    if (patchCount() == 0) scanPatches();
    int missing = 0, firstMissing = -1;
    char missName[PATCH_NAME_LEN + 1] = "";
    const char *ptrs[MORPH_SLOTS];
    for (int i = 0; i < MORPH_SLOTS; i++) {
        if (slots[i][0] && patchIndexOfName(slots[i]) < 0) {
            if (firstMissing < 0) { firstMissing = i; strcpy(missName, slots[i]); }
            missing++; slots[i][0] = 0;
        }
        ptrs[i] = slots[i];
    }
    morphSetChain(ptrs, MORPH_SLOTS);
    if (t >= 0) sys.morphTimeMs = t;
    saveSettings();
    char b[40];
    if (missing == 1) snprintf(b, sizeof(b), "Slot %d: %s missing", firstMissing + 1, missName);
    else if (missing > 1) snprintf(b, sizeof(b), "%d slots missing (left empty)", missing);
    else snprintf(b, sizeof(b), "Chain loaded: %s", name);
    setInfo(b);
    strncpy(lastChain, name, PATCH_NAME_LEN); lastChain[PATCH_NAME_LEN] = 0;
}

// MIDI switch Chain+ / Chain- (v0.84): the chain after / before the one
// loaded last, wrapping at the ends; with none loaded yet, the first of the
// list. Reading 8 patches stalls loop() for 0.1-0.2s (the sound goes on):
// for the change between songs or scenes. The list is read into a block that
// is freed again right away (the chain page's own one when it is open).
static void chainStep(int dir) {
    bool own = (chainNames == nullptr);
    ChainName *list = own ? (ChainName *)malloc(sizeof(ChainName) * CHAIN_LIST_MAX) : chainNames;
    if (!list) { setInfo("No memory for the list"); return; }
    int n = chainList(list, CHAIN_LIST_MAX);
    if (n == 0) { setInfo("No saved chains"); if (own) free(list); return; }
    int cur = -1;
    for (int i = 0; i < n; i++) if (lastChain[0] && strcmp(list[i], lastChain) == 0) { cur = i; break; }
    int next = (cur < 0) ? 0 : (cur + dir + n) % n;
    char name[PATCH_NAME_LEN + 1]; strcpy(name, list[next]);
    if (own) free(list);
    else { chainCount = n; chainSel = next; }
    chainLoadNamed(name);
    if (!own) chainMakePreview();
}

static void drawChainPage() {
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, 14); canvas.printf("MORPH > Chains (%d)", chainCount);
    // The action, chosen with , / - laid out by label width with a 7px gap
    // (fixed 37px steps left SaveAs and Delete touching, v0.88).
    for (int a = 0, x = 124; a < 3; a++) {
        int w = (int)strlen(CHAIN_ACTS[a]) * 6;
        if (a == chainAct) { canvas.fillRect(x - 2, 12, w + 3, 10, uiColor); canvas.setTextColor(BLACK, uiColor); }
        else canvas.setTextColor(uiColor, BLACK);
        canvas.setCursor(x, 14); canvas.print(CHAIN_ACTS[a]);
        x += w + 7;
    }
    canvas.setTextColor(uiColor, BLACK);
    const int ROWS = 7, Y0 = 28, RH = 11;
    int top = clampi(chainSel - ROWS / 2, 0, chainCount > ROWS ? chainCount - ROWS : 0);
    for (int r = 0; r < ROWS; r++) {
        int i = top + r; if (i >= chainCount) break;
        canvas.setCursor(5, Y0 + r * RH);
        canvas.printf("%c%-12s %s", i == chainSel ? '>' : ' ', chainNames[i], chainIsFactoryName(chainNames[i]) ? "F" : "");
    }
    if (chainCount == 0) { canvas.setCursor(8, Y0); canvas.print("(no chains yet - SaveAs)"); }
    canvas.drawFastHLine(0, 104, SCREEN_W, uiDim);
    canvas.setCursor(4, 108);
    if (chainAct == 1) canvas.print("Slots 1-8 + Time -> a new file");
    else canvas.print(chainPreview);
    if (millis() < infoUntilMs) drawNavHint(infoLine);
    else drawNavHint(";/.:pick ,//:action Ent:run Tab:back");
}

static void chainHandleKeys(const KeyFrame &k) {
    if (k.tabPressed) { chainPageClose(); return; }
    if (repeatFire(chUp,   keyDown(k, ';') || keyDown(k, ':')) && chainCount > 0) { chainSel = (chainSel + chainCount - 1) % chainCount; chainMakePreview(); uiDirty = true; }
    if (repeatFire(chDown, keyDown(k, '.') || keyDown(k, '>')) && chainCount > 0) { chainSel = (chainSel + 1) % chainCount; chainMakePreview(); uiDirty = true; }
    if (repeatFire(chDec,  keyDown(k, ',') || keyDown(k, '<'))) { chainAct = (chainAct + 2) % 3; uiDirty = true; }
    if (repeatFire(chInc,  keyDown(k, '/') || keyDown(k, '?'))) { chainAct = (chainAct + 1) % 3; uiDirty = true; }
    if (!k.enterPressed || k.shift) return;
    if (chainAct == 1) { openNameEntry(NP_CHAIN_SAVE, "MyChain"); return; }
    if (chainCount == 0) { setInfo("No chain selected"); return; }
    strncpy(nameSource, chainSelName(), PATCH_NAME_LEN); nameSource[PATCH_NAME_LEN] = 0;
    confirmKind = (chainAct == 0) ? CK_CHAIN_LOAD : CK_CHAIN_DELETE;
    confirmReturn = AppMode::SETTING;
    uiSetMode(AppMode::CONFIRM);
}

// --- SETTING > Patch > Morph (v0.5, update spec 6.1) -------------------------
static RepeatKey mUp, mDown, mDec, mInc;

// A slot whose algorithm differs from the one before it: the morph steps
// there instead of flowing. Marked, never forbidden.
static bool morphSlotSteps(int i) {
    if (i <= 0) return false;
    int a = morphSlotAlgorithm(i - 1), b = morphSlotAlgorithm(i);
    return a && b && a != b;
}

static void drawMorphRow(int idx, int x, int y, const char *text) {
    bool sel = (idx == morphSel);
    if (sel) { canvas.fillRect(x - 2, y - 1, 116, 10, uiColor); canvas.setTextColor(BLACK, uiColor); }
    else canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(x, y); canvas.print(text);
    canvas.setTextColor(uiColor, BLACK);
}

static void drawMorphScreen() {
    char b[40];
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, 14); canvas.print("MORPH > Slots");   // v0.85: MORPH > ... like the chain page
    // Chain: one position along slots 1..N. Time: Shift+1..8 glides.
    snprintf(b, sizeof(b), "Chain   %s", sys.morphEnabled ? "On" : "Off");
    drawMorphRow(0, 5, 26, b);
    snprintf(b, sizeof(b), "Pos     %d%%", clampi(sys.morphPosBase, 0, 100));
    drawMorphRow(1, 123, 26, b);
    snprintf(b, sizeof(b), "Time    %.1fs", (float)clampi(sys.morphTimeMs, 0, MORPH_TIME_MAX_MS) / 1000.f);
    drawMorphRow(2, 5, 37, b);
    drawMorphRow(MORPH_ROW_CHAINS, 123, 37, "Chains >");
    canvas.drawFastHLine(0, 48, SCREEN_W, uiDim);
    for (int i = 0; i < MORPH_SLOTS; i++) {
        const char *nm = morphSlotName(i);
        snprintf(b, sizeof(b), "%d %-10.10s%s", i + 1, nm[0] ? nm : "--", morphSlotSteps(i) ? " !" : "");
        drawMorphRow(MORPH_ROW_SLOT0 + i, i < 4 ? 5 : 123, 52 + (i % 4) * 11, b);
    }
    canvas.drawFastVLine(119, 50, 44, uiDim);
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, 99);
    int len = morphChainLength();
    if (morphGliding() && audioStatus.morphGlideTo >= 0)
        canvas.printf("Glide > %d  %d%%", audioStatus.morphGlideTo + 1, (int)(audioStatus.morphT * 100.f + 0.5f));
    else if (!sys.morphEnabled)     canvas.printf("Chain off  (%d in a row)", len);
    else if (morphChainPaused())    canvas.print("Paused - move Pos/knob/tilt");
    else if (!morphChainRunning())  canvas.print("Chain needs slots 1+2 filled");
    else if (audioStatus.morphSeg >= 0)
        canvas.printf("Now %d>%d %d%%  pos %d%%%s", audioStatus.morphSeg + 1, audioStatus.morphSeg + 2,
                      (int)(audioStatus.morphT * 100.f + 0.5f), (int)(audioStatus.morphPos * 100.f + 0.5f),
                      modMarker(ModTarget::PATCH_MORPH));
    canvas.setCursor(4, 111);
    if (morphSel >= MORPH_ROW_SLOT0 && morphSlotSteps(morphSel - MORPH_ROW_SLOT0)) canvas.print("Algo differs - chain will step");
    else if (morphSel == MORPH_ROW_CHAINS) canvas.print("Save / load chains (Pads, Drums)");
    else canvas.print("CC/IMU/LFO > PtchMrph  Sh+1-8");
    if (millis() < infoUntilMs) drawNavHint(infoLine);
    else drawNavHint(";/.:row ,//:value Ent:pick Del:off");
}

static void morphHandleKeys(const KeyFrame &k) {
    if (k.tabPressed) { morphPage = false; saveRequested = true; uiDirty = true; return; }
    const int N = MORPH_ROW_SLOT0 + MORPH_SLOTS;
    if (repeatFire(mUp,   keyDown(k, ';') || keyDown(k, ':'))) { morphSel = (morphSel + N - 1) % N; uiDirty = true; }
    if (repeatFire(mDown, keyDown(k, '.') || keyDown(k, '>'))) { morphSel = (morphSel + 1) % N; uiDirty = true; }
    bool dec = repeatFire(mDec, keyDown(k, ',') || keyDown(k, '<'));
    bool inc = repeatFire(mInc, keyDown(k, '/') || keyDown(k, '?'));
    bool ent = k.enterPressed && !k.shift;
    if (morphSel == 0) {
        if (dec || inc || ent) {
            morphSetEnabled(!sys.morphEnabled);
            if (sys.morphEnabled && morphChainLength() < 2) setInfo("Chain needs slots 1+2 filled");
        }
    } else if (morphSel == 1) {
        if (dec) { sys.morphPosBase = clampi(sys.morphPosBase - 1, 0, 100); uiDirty = true; }
        if (inc) { sys.morphPosBase = clampi(sys.morphPosBase + 1, 0, 100); uiDirty = true; }
    } else if (morphSel == 2) {
        if (dec) { sys.morphTimeMs = clampi(sys.morphTimeMs - 100, 0, MORPH_TIME_MAX_MS); uiDirty = true; }
        if (inc) { sys.morphTimeMs = clampi(sys.morphTimeMs + 100, 0, MORPH_TIME_MAX_MS); uiDirty = true; }
    } else if (morphSel == MORPH_ROW_CHAINS) {
        if (ent || inc) { chainPageOpen(); return; }
    } else {
        int slot = morphSel - MORPH_ROW_SLOT0;
        if (ent || inc) { morphPickSlot = slot; openBrowser(BA_MORPH_PICK); return; }
        if ((dec || k.delPressed) && morphSlotName(slot)[0]) {
            morphSetSlot(slot, "");
            char b[40]; snprintf(b, sizeof(b), "Slot %d: off", slot + 1); setInfo(b);
        }
    }
}

static void drawSetScreen() {
    canvas.setTextColor(uiColor, BLACK);
    if (!setInCategory) {
        canvas.setCursor(4, 14); canvas.print("SETTINGS");
        for (int c = 0; c < SC_COUNT; c++) {
            int y = 26 + c * 12;               // 12px since v0.81 (8 categories above the nav row)
            bool sel = (c == setCategory);
            if (sel) { canvas.fillRect(2, y - 2, 236, 11, uiColor); canvas.setTextColor(BLACK, uiColor); }
            else canvas.setTextColor(uiColor, BLACK);
            canvas.setCursor(6, y); canvas.print(SET_CATEGORY_NAMES[c]);
        }
        canvas.setTextColor(uiColor, BLACK);
        if (millis() < infoUntilMs) drawNavHint(infoLine);
        else drawNavHint(";/. select  Enter: open  Tab: next");
        return;
    }
    if (setCategory == SC_PATCH && morphPage) {
        if (chainPage) drawChainPage(); else drawMorphScreen();
        return;
    }
    if (setCategory == SC_MIDI) {
        drawMidiScreen();
        if (millis() < infoUntilMs) drawNavHint(infoLine);
        else if (midiPage == MP_MAIN) drawNavHint(";/. row  Enter:open/learn  Tab:back");
        else if (midiPage == MP_LIST) drawNavHint(";/. scroll   Tab: back");
        else if (midiPage == MP_CC && midiNav.sel % 3 == 1)
            drawNavHint(modTargetHelp(sys.ccSlot[clampi(midiNav.sel / 3, 0, NUM_CC_SLOTS - 1)].target));   // v0.85
        else drawNavHint("Enter on CC: MIDI Learn   Tab:back");
        return;
    }
    canvas.setCursor(4, 14); canvas.printf("SET > %s", SET_CATEGORY_NAMES[setCategory]);
    if (setCategory == SC_INFO) {
        uint32_t peak = dspMaxUs, over = dspOverBudgetCount;
        canvas.setCursor(4, 28);  canvas.printf("MORPHASE %s%s", MORPHASE_VERSION, MORPHASE_EDITION);   // " BLE" on the BLE build (v0.77)
#ifdef MORPHASE_BLE
        canvas.printf(" (%s)", bleMidiStateName());   // "... BLE (Conn)"
#endif
        canvas.setCursor(4, 40);  canvas.printf("DSP load   %lu us / %lu us (%lu%%)",
                                               audioStatus.lastDspUs, AUDIO_BUDGET_US,
                                               audioStatus.lastDspUs * 100 / AUDIO_BUDGET_US);
        canvas.setCursor(4, 52);  canvas.printf("Peak       %lu us (%lu%%)",
                                               (unsigned long)peak, (unsigned long)(peak * 100 / AUDIO_BUDGET_US));
        // A single late buffer is what breaks up the sound, and an average
        // never shows it - so this one is called out in red.
        canvas.setCursor(4, 64);
        // Red-on-red would hide the warning, so it turns amber under the Red
        // theme (v0.32).
        if (over > 0) canvas.setTextColor(sys.theme == UiTheme::RED ? canvas.color565(255, 190, 40)
                                                                   : canvas.color565(255, 60, 60), BLACK);
        canvas.printf("Over budget %lu", (unsigned long)over);
        canvas.setTextColor(uiColor, BLACK);
        // Heap (v0.62): free now, the lowest since boot (SD reads and writes
        // dip it ~5KB), and the largest free block - fragmentation bites
        // before the total does. Floors agreed for v0.7: heap 40K, min 32K.
        canvas.setCursor(4, 76);  canvas.printf("Voices %d/%d  heap %uK min %uK", audioStatus.activeVoices,
                                               clampi(patch.voices, 1, MAX_VOICES), (unsigned)(ESP.getFreeHeap() / 1024),
                                               (unsigned)(ESP.getMinFreeHeap() / 1024));
#ifdef MORPHASE_BLE
        // DIN and BLE bytes, and BLE bytes dropped because its queue was full
        canvas.setCursor(4, 88);  canvas.printf("rx %lu  BLE %lu ov%lu  SD:%s", (unsigned long)midiRxByteCount(),
                                               (unsigned long)bleMidiRxBytes(), (unsigned long)bleMidiOverflows(), sdReady() ? "ok" : "none");
#else
        canvas.setCursor(4, 88);  canvas.printf("MIDI rx    %lu bytes  SD:%s", (unsigned long)midiRxByteCount(), sdReady() ? "ok" : "none");
#endif
#ifdef MORPHASE_BLE
        // + the BLE host task's deepest stack use so far / its size (v0.78)
        canvas.setCursor(4, 100); canvas.printf("Patches %d  blk %uK  stk %lu/%lu", patchCount(), (unsigned)(ESP.getMaxAllocHeap() / 1024),
                                               (unsigned long)bleMidiHostStackUsed(), (unsigned long)bleMidiHostStackSize());
#else
        canvas.setCursor(4, 100); canvas.printf("Patches %d  largest blk %uK", patchCount(), (unsigned)(ESP.getMaxAllocHeap() / 1024));
#endif
        // Output limiter (v0.72), last second: the peak going in (dB under
        // int16 full scale) and how far it pulled the level down. The
        // yardstick for matching patch levels.
        float pk = outLimPeak1s, mg = outLimMinGain1s;
        float pkDb = (pk > 1.f) ? 20.f * log10f(pk / 32767.f) : -90.f;
        float grDb = (mg < 0.9999f) ? 20.f * log10f(mg) : 0.f;
        canvas.setCursor(4, 112);
        canvas.printf("OutLim  in %+.1fdB  GR %.1fdB", pkDb, grDb);
    } else if (setCategory == SC_GLITCH) {
        drawItemList(setList(SC_GLITCH), setItemCount[SC_GLITCH], setNav.sel, 26, -1, 11);   // 9 rows since v0.82
        canvas.drawFastVLine(140, 26, 96, uiDim);
        canvas.setCursor(146, 28);  canvas.print("hold g:");
        canvas.setCursor(146, 40);  canvas.print(" Hold%");
        canvas.setCursor(146, 58);  canvas.print("tilt/knob:");
        canvas.setCursor(146, 70);  canvas.print(" GlProb");
        canvas.setCursor(146, 94);
        if (glitchEventActive()) { canvas.fillRect(144, 92, 30, 11, uiColor); canvas.setTextColor(BLACK, uiColor); }
        canvas.print(" GL ");
        canvas.setTextColor(uiColor, BLACK);
        // v0.85: the ARP glitch needs the ARP running
        if (setNav.sel == 8 && !arp.on) { canvas.setCursor(146, 106); canvas.print("ARP is off"); }
    } else if (setCategory == SC_THEREMIN) {
        MenuItem *thList = setList(SC_THEREMIN);   // binds first: the count follows the Mode
        drawItemList(thList, setItemCount[SC_THEREMIN], setNav.sel, 28, TH_SPLIT, 12);
        // Scale only acts in Semitone: dimmed while Pitch is Smooth
        if (sys.thMode == ThMode::PLAY && sys.thPitch == ThPitch::SMOOTH) {
            canvas.setTextColor(uiMuted, BLACK);
            canvas.setCursor(123, 28 + (TH_ROW_SCALE - TH_SPLIT) * 12);
            canvas.printf("%c%-8s%s", setNav.sel == TH_ROW_SCALE ? '>' : ' ', "Scale", thScaleLabel());
            canvas.setTextColor(uiColor, BLACK);
        }
        canvas.drawFastHLine(0, 100, SCREEN_W, uiDim);
        // Live: state, distance, where the hand is in the window (near = full)
        // and what it does - so Near / Far can be set with the hand in place
        canvas.setCursor(4, 104);
        int mm = thereminDistanceMm();
        if (mm > 0) canvas.printf("%-9s %4dmm", thereminStateName(), mm);
        else        canvas.printf("%-9s   --mm", thereminStateName());
        float bv = thereminBarValue();
        drawBar(104, 104, 90, 8, bv < 0.f ? 0.f : bv, uiColor);
        canvas.setCursor(200, 104);
        if (sys.thMode == ThMode::PLAY) canvas.print(thereminNoteName());
        else canvas.printf("%d%%", (int)(live.thModNorm * 100.f + 0.5f));
        canvas.setCursor(4, 114);
        if (sys.thMode == ThMode::PLAY && setNav.sel == TH_ROW_SCALE) {
            const ThScale &s = TH_SCALES[clampi(sys.thScale, 0, TH_SCALE_COUNT - 1)];
            canvas.printf("%s: %s", s.group, s.name);
        } else if (sys.thBus == ThBus::GROVE) {
#ifdef MORPHASE_BLE
            canvas.print("Grove: DIN MIDI off (BLE ok)");
#else
            canvas.print("Grove: DIN MIDI off");
#endif
        } else if (sys.thMode == ThMode::PLAY) {
            canvas.print("Keys get 1 voice less");
        }
    } else if (setCategory == SC_IMU) {
        drawItemList(setList(SC_IMU), setItemCount[SC_IMU], setNav.sel, 25, 6, 11);
        // Live result, so a change can be judged by tilting right here
        canvas.setCursor(4, 102);  canvas.printf("X%+.2f", live.imuXNorm);
        drawBar(46, 102, 68, 8, (live.imuXNorm + 1.f) * 0.5f, uiColor);
        canvas.setCursor(123, 102); canvas.printf("Y%+.2f", live.imuYNorm);
        drawBar(165, 102, 70, 8, (live.imuYNorm + 1.f) * 0.5f, uiColor);
        canvas.setCursor(4, 114);
        canvas.printf("X:%s Y:%s  A/S:hold Sh+A/S:on",
                      sys.imuXEnabled ? (sys.imuXHeld ? "Hold" : "On") : "Off",
                      sys.imuYEnabled ? (sys.imuYHeld ? "Hold" : "On") : "Off");
    } else {
        if (setCategory == SC_PATCH) {
            drawItemList(setList(SC_PATCH), setItemCount[SC_PATCH], setNav.sel, 28, 5, 12);
            canvas.setCursor(4, 94); canvas.printf("Current: %s", patch.name);
            if (morphRunning()) { canvas.setCursor(4, 106); canvas.printf("Morphing: %s", livePatch.name); }
        } else {
            drawItemList(setList(setCategory), setItemCount[setCategory], setNav.sel, 28, -1, 12);
        }
    }
    if (millis() < infoUntilMs) drawNavHint(infoLine);
    else if (setCategory == SC_INFO) drawNavHint("Enter: Reset peak/over   Tab: back");
    // Target rows (v0.85): what the destination does
    else if (setCategory == SC_IMU && (setNav.sel == 0 || setNav.sel == 6))
        drawNavHint(modTargetHelp(setNav.sel == 0 ? patch.imuXTarget : patch.imuYTarget));
    else if (setCategory == SC_THEREMIN && sys.thMode == ThMode::MOD && setNav.sel == TH_SPLIT)
        drawNavHint(modTargetHelp(sys.thTarget));
    else drawNavHint(";/. row  ,// value  Enter:run  Tab:back");
}

// Starts or cancels MIDI Learn for the CC field under the cursor.
static void toggleLearn(int *field) {
    if (learnField) { midiLearnCancel(); learnField = nullptr; setInfo("Learn cancelled"); return; }
    learnField = field;
    midiLearnBegin();
    uiDirty = true;
}

// Collects the result once the MIDI layer has captured a CC (or timed out).
static void pollMidiLearn() {
    if (!learnField || midiLearnActive()) return;
    int cc = midiLearnTakeResult();
    char b[40];
    if (cc >= 0) {
        *learnField = cc;
        // A duplicate is allowed, but the player is told about it.
        if (ccUseCount(cc) > 1) snprintf(b, sizeof(b), "CC %d learned (already used!)", cc);
        else                    snprintf(b, sizeof(b), "CC %d learned", cc);
        saveSettings();
    } else {
        snprintf(b, sizeof(b), "Learn timed out");
    }
    setInfo(b);
    learnField = nullptr;
    uiDirty = true;
}

static void midiHandleKeys(const KeyFrame &k) {
    int count = (midiPage == MP_CC) ? NUM_CC_SLOTS * 3 : (midiPage == MP_SW ? NUM_SW_SLOTS * 3 : 0);
    if (k.tabPressed) {
        if (learnField) { midiLearnCancel(); learnField = nullptr; }
        if (midiPage == MP_MAIN) { setInCategory = false; saveRequested = true; }
        else midiPage = MP_MAIN;
        uiDirty = true;
        return;
    }
    if (midiPage == MP_LIST) {
        if (repeatFire(listUp, keyDown(k, ';') || keyDown(k, ':')))   { midiListScroll--; uiDirty = true; }
        if (repeatFire(listDown, keyDown(k, '.') || keyDown(k, '>'))) { midiListScroll++; uiDirty = true; }
        return;
    }
    if (midiPage == MP_MAIN) {
        // Enter on the CC field starts Learn instead of the generic action.
        if (k.enterPressed && !k.shift && setList(SC_MIDI)[setNav.sel].type == IT_CC) {
            toggleLearn((int *)setList(SC_MIDI)[setNav.sel].ptr);
            return;
        }
#ifdef MORPHASE_BLE
        bool bleWas = sys.bleMidiEnabled;
#endif
        if (listHandleKeys(setNav, setList(SC_MIDI), setItemCount[SC_MIDI], k)) uiDirty = true;
#ifdef MORPHASE_BLE
        // The BT stack starts (or stays off) at boot only, so a change asks
        // whether to restart now. Saved first: Tab (later) keeps it too.
        if (sys.bleMidiEnabled != bleWas) {
            saveSettings();
            if (sys.bleMidiEnabled != (bleMidiState() != BLE_STATE_OFF)) {
                confirmKind = CK_REBOOT; confirmReturn = AppMode::SETTING; uiSetMode(AppMode::CONFIRM);
            }
        }
#endif
        return;
    }
    MenuItem *items = (midiPage == MP_CC) ? midiCcList() : midiSwList();
    if (k.enterPressed && !k.shift && items[midiNav.sel].type == IT_CC) {
        toggleLearn((int *)items[midiNav.sel].ptr);
        return;
    }
    if (listHandleKeys(midiNav, items, count, k)) uiDirty = true;
}

static void setHandleKeys(const KeyFrame &k) {
    if (!setInCategory) {
        if (repeatFire(catUp,   keyDown(k, ';') || keyDown(k, ':'))) { setCategory = (setCategory + SC_COUNT - 1) % SC_COUNT; uiDirty = true; }
        if (repeatFire(catDown, keyDown(k, '.') || keyDown(k, '>'))) { setCategory = (setCategory + 1) % SC_COUNT; uiDirty = true; }
        if (k.enterPressed && !k.shift) {
            setInCategory = true; setNav.sel = 0; midiPage = MP_MAIN; morphPage = false;
            if (chainPage) chainPageClose();
            memset(&setNav.up, 0, sizeof(RepeatKey) * 4);
            uiDirty = true;
        }
        return;
    }
    if (setCategory == SC_MIDI) { midiHandleKeys(k); return; }
    if (setCategory == SC_PATCH && morphPage) { if (chainPage) chainHandleKeys(k); else morphHandleKeys(k); return; }
    if (k.tabPressed) { setInCategory = false; saveRequested = true; uiDirty = true; return; }   // v0.90T3: 3 s later
    if (setCategory == SC_INFO) {
        // Enter clears the cumulative counters so a new measurement does
        // not need a reboot (spec A.4).
        if (k.enterPressed && !k.shift) { dspResetCounters(); setInfo("Counters reset"); }
        return;
    }
    // Enter on the Calib row toggles it (a custom item, not an action)
    if (setCategory == SC_IMU && k.enterPressed && !k.shift && setNav.sel == IMU_CALIB_ROW) {
        calibToggle(); uiDirty = true; return;
    }
    if (setCategory == SC_THEREMIN) {
        // Enter on Rescan runs it (a custom row, so its label can show the count)
        if (k.enterPressed && !k.shift && setNav.sel == 5) { thereminRescan(); uiDirty = true; return; }
        ThMode before = sys.thMode;
        MenuItem *thList = setList(SC_THEREMIN);
        if (listHandleKeys(setNav, thList, setItemCount[SC_THEREMIN], k)) uiDirty = true;
        if (sys.thMode != before) {
            // The right column changes: rebuild the list, keep the cursor on Mode
            itemPoolOwner = IO_NONE;
            setList(SC_THEREMIN);
            setNav.sel = clampi(setNav.sel, 0, setItemCount[SC_THEREMIN] - 1);
        }
        return;
    }
    UiTheme t = sys.theme; int br = sys.brightness;
    if (listHandleKeys(setNav, setList(setCategory), setItemCount[setCategory], k)) {
        uiDirty = true;
        if (sys.theme != t) applyUiTheme();
        if (sys.brightness != br) applyUiBrightness();
    }
}

// --- patch browser ----------------------------------------------------------
static void drawBrowserScreen() {
    static const char *titles[5] = { "LOAD PATCH", "RENAME PATCH", "DUPLICATE PATCH", "DELETE PATCH", "MORPH SLOT" };
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, 14);
    if (browserAction == BA_MORPH_PICK) canvas.printf("%s %d  (%d/%d)", titles[browserAction], morphPickSlot + 1, patchCount() ? browserCursor + 1 : 0, patchCount());
    else canvas.printf("%s  (%d/%d)", titles[browserAction], patchCount() ? browserCursor + 1 : 0, patchCount());
    const int ROWS = 9, ROW_H = 11, Y0 = 26;
    int count = patchCount();
    int first = browserCursor - ROWS / 2; if (first > count - ROWS) first = count - ROWS; if (first < 0) first = 0;
    for (int r = 0; r < ROWS; r++) {
        int i = first + r; if (i >= count) break;
        bool sel = (i == browserCursor);
        bool cur = strcmp(patchNameAt(i), patch.name) == 0;
        canvas.setCursor(8, Y0 + r * ROW_H);
        canvas.printf("%c%2d %-12s%s", sel ? '>' : ' ', i + 1, patchNameAt(i), cur ? " *" : "");
    }
    if (count == 0) { canvas.setCursor(8, Y0); canvas.print("(no patches on card)"); }
    drawNavHint("; . :move  Enter:ok  Tab:back");
}

static void browserHandleKeys(const KeyFrame &k) {
    int count = patchCount();
    if (repeatFire(brUp, keyDown(k, ';') || keyDown(k, ':') || keyDown(k, ',')) && count) { browserCursor = (browserCursor + count - 1) % count; uiDirty = true; }
    if (repeatFire(brDown, keyDown(k, '.') || keyDown(k, '>') || keyDown(k, '/')) && count) { browserCursor = (browserCursor + 1) % count; uiDirty = true; }
    if (k.tabPressed) { uiSetMode(AppMode::SETTING); return; }
    if (!k.enterPressed || count == 0) return;
    const char *nm = patchNameAt(browserCursor);
    switch (browserAction) {
        case BA_LOAD: {
            setInfo(loadPatchFile(nm) ? "Loaded" : "Load FAILED");
            // No save here (v0.90T2): like a Program Change, the new sound is
            // written 3 s after the last load (deferredPerformSave, main.cpp).
            uiSetMode(AppMode::SETTING);
            break;
        }
        case BA_RENAME:
            strncpy(nameSource, nm, PATCH_NAME_LEN); nameSource[PATCH_NAME_LEN] = 0;
            openNameEntry(NP_RENAME, nm);
            break;
        case BA_DUP: {
            strncpy(nameSource, nm, PATCH_NAME_LEN); nameSource[PATCH_NAME_LEN] = 0;
            char sug[PATCH_NAME_LEN + 1]; snprintf(sug, sizeof(sug), "%.10s2", nm);
            openNameEntry(NP_DUP, sug);
            break;
        }
        case BA_DELETE:
            strncpy(nameSource, nm, PATCH_NAME_LEN); nameSource[PATCH_NAME_LEN] = 0;
            confirmKind = CK_DELETE; uiSetMode(AppMode::CONFIRM);
            break;
        case BA_MORPH_PICK: {
            morphSetSlot(morphPickSlot, nm);          // reads the patch off the card, once
            int i = morphPickSlot;
            int al = morphSlotAlgorithm(i);
            bool differs = al && ((i > 0 && morphSlotAlgorithm(i - 1) && morphSlotAlgorithm(i - 1) != al) ||
                                  (i < MORPH_SLOTS - 1 && morphSlotAlgorithm(i + 1) && morphSlotAlgorithm(i + 1) != al));
            // Allowed on purpose, but said out loud (update spec 6.1)
            if (differs) setInfo("Algo differs - morph will step");
            else { char b[40]; snprintf(b, sizeof(b), "Slot %d: %s", i + 1, nm); setInfo(b); }
            uiSetMode(AppMode::SETTING);
            break;
        }
    }
}

// --- name entry -------------------------------------------------------------
static void drawNameScreen() {
    static const char *titles[4] = { "SAVE AS", "RENAME TO", "DUPLICATE AS", "SAVE CHAIN AS" };
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, 14); canvas.print(titles[namePurpose]);
    canvas.drawRect(20, 40, 200, 24, uiColor);
    canvas.setTextSize(2);
    canvas.setCursor(26, 45); canvas.print(nameBuf); canvas.print("_");
    canvas.setTextSize(1);
    canvas.setCursor(4, 80); canvas.printf("%d/%d chars  A-Z a-z 0-9 - _ .", (int)strlen(nameBuf), PATCH_NAME_LEN);
    if (namePurpose == NP_CHAIN_SAVE) {     // v0.83: the chain list, not the patches
        canvas.setCursor(4, 92);
        if (!patchNameValid(nameBuf)) canvas.print("(name not valid yet)");
        else if (chainIsFactoryName(nameBuf)) canvas.print("(factory name - not allowed)");
        else if (chainExists(nameBuf)) canvas.print("(exists - will ask to overwrite)");
    }
    else if (!patchNameValid(nameBuf)) { canvas.setCursor(4, 92); canvas.print("(name not valid yet)"); }
    else if (patchIsFactoryName(nameBuf)) { canvas.setCursor(4, 92); canvas.print("(factory name - not allowed)"); }
    else if (patchIndexOfName(nameBuf) >= 0 && namePurpose != NP_SAVE_AS) { canvas.setCursor(4, 92); canvas.print("(name already exists)"); }
    else if (patchIndexOfName(nameBuf) >= 0) { canvas.setCursor(4, 92); canvas.print("(exists - will ask to overwrite)"); }
    drawNavHint("type  Bksp:del  Enter:ok  Tab:back");
}

static void nameHandleKeys(const KeyFrame &k) {
    if (k.tabPressed) { uiSetMode(AppMode::SETTING); return; }
    int len = strlen(nameBuf);
    if (k.delPressed && len > 0) { nameBuf[len - 1] = 0; uiDirty = true; }
    for (int i = 0; i < k.pressedLen; i++) {
        char c = k.pressed[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (ok && len < PATCH_NAME_LEN) { nameBuf[len++] = c; nameBuf[len] = 0; uiDirty = true; }
    }
    if (!k.enterPressed) return;
    if (!patchNameValid(nameBuf)) { setInfo("Invalid name"); return; }
    if (namePurpose == NP_CHAIN_SAVE) {     // v0.83
        if (chainIsFactoryName(nameBuf)) { setInfo("Factory name - pick another"); return; }
        if (chainExists(nameBuf)) {
            strncpy(overwriteName, nameBuf, PATCH_NAME_LEN); overwriteName[PATCH_NAME_LEN] = 0;
            confirmKind = CK_CHAIN_OVERWRITE; confirmReturn = AppMode::SETTING;
            uiSetMode(AppMode::CONFIRM);
            return;
        }
        chainSaveCurrent(nameBuf);
        uiSetMode(AppMode::SETTING);
        return;
    }
    if (patchIsFactoryName(nameBuf)) { setInfo("Factory name - pick another"); return; }   // v0.75
    bool ok = false;
    switch (namePurpose) {
        case NP_SAVE_AS: saveWithConfirm(nameBuf); return;   // confirms if the name exists
        case NP_RENAME:
            ok = renamePatchFile(nameSource, nameBuf);
            if (ok && strcmp(patch.name, nameSource) == 0) { strncpy(patch.name, nameBuf, PATCH_NAME_LEN); patch.name[PATCH_NAME_LEN] = 0; }
            break;
        case NP_DUP: {
            // write the CURRENT sound under the new name only if the source
            // is the current patch; otherwise load, save, restore
            if (strcmp(patch.name, nameSource) == 0) ok = savePatchFile(nameBuf);
            else {
                Patch keep = patch;
                if (loadPatchFile(nameSource)) ok = savePatchFile(nameBuf);
                patch = keep;
            }
            break;
        }
    }
    setInfo(ok ? "Done" : "FAILED");
    if (ok) saveSettings();
    scanPatches();
    uiSetMode(AppMode::SETTING);
}

// --- confirm ----------------------------------------------------------------
static void drawConfirmScreen() {
    canvas.setTextColor(uiColor, BLACK);
    canvas.drawRect(20, 30, 200, 60, uiColor);
    if (confirmKind == CK_RESET) {
        drawCenteredText(40, "Reset patch to Init?", uiColor);
        drawCenteredText(52, "(all FX off, sound cleared)", uiColor);
    } else if (confirmKind == CK_OVERWRITE) {
        char b[40]; snprintf(b, sizeof(b), "Overwrite \"%s\"?", overwriteName);
        drawCenteredText(40, b, uiColor);
        drawCenteredText(52, "The saved sound will be replaced", uiColor);
    } else if (confirmKind == CK_CALIBRATE) {
        drawCenteredText(40, "Set current tilt as zero point?", uiColor);
        drawCenteredText(52, "Hold the device the way you play", uiColor);
    } else if (confirmKind == CK_CHAIN_LOAD) {
        char b[40]; snprintf(b, sizeof(b), "Load chain \"%s\"?", nameSource);
        drawCenteredText(40, b, uiColor);
        drawCenteredText(52, "Slots 1-8 will be replaced", uiColor);
    } else if (confirmKind == CK_CHAIN_OVERWRITE) {
        char b[40]; snprintf(b, sizeof(b), "Overwrite chain \"%s\"?", overwriteName);
        drawCenteredText(40, b, uiColor);
        drawCenteredText(52, "The saved chain will be replaced", uiColor);
    } else if (confirmKind == CK_CHAIN_DELETE) {
        char b[40]; snprintf(b, sizeof(b), "Delete chain \"%s\"?", nameSource);
        drawCenteredText(40, b, uiColor);
        drawCenteredText(52, chainIsFactoryName(nameSource) ? "Factory restores it" : "This cannot be undone", uiColor);
    } else if (confirmKind == CK_REBOOT) {
        drawCenteredText(40, sys.bleMidiEnabled ? "BLE On: restart now?" : "BLE Off: restart now?", uiColor);
        drawCenteredText(52, "Reboot to apply (Tab: next boot)", uiColor);
    } else {
        char b[40]; snprintf(b, sizeof(b), "Delete \"%s\"?", nameSource);
        drawCenteredText(40, b, uiColor);
        drawCenteredText(52, "This cannot be undone", uiColor);
    }
    drawCenteredText(72, "Enter: YES    Tab: no", uiColor);
}

static void confirmHandleKeys(const KeyFrame &k) {
    if (k.tabPressed) { AppMode r = confirmReturn; confirmReturn = AppMode::SETTING; uiSetMode(r); return; }
    if (!k.enterPressed) return;
    if (confirmKind == CK_CALIBRATE) {
        sys.imuXCalDeg = imuAngleXDeg; sys.imuYCalDeg = imuAngleYDeg; sys.imuCalibrated = true;
        saveSettings();
        confirmReturn = AppMode::SETTING;
        uiSetMode(AppMode::SETTING);
        return;
    }
#ifdef MORPHASE_BLE
    if (confirmKind == CK_REBOOT) {   // the setting is already saved
        canvas.fillScreen(BLACK); drawCenteredText(60, "Restarting...", uiColor); canvas.pushSprite(0, 0);
        delay(300);
        ESP.restart();
        return;
    }
#endif
    if (confirmKind == CK_CHAIN_LOAD || confirmKind == CK_CHAIN_OVERWRITE || confirmKind == CK_CHAIN_DELETE) {
        // v0.83: back to the chain page in every case
        if (confirmKind == CK_CHAIN_LOAD) chainLoadNamed(nameSource);
        else if (confirmKind == CK_CHAIN_OVERWRITE) chainSaveCurrent(overwriteName);
        else { setInfo(chainDelete(nameSource) ? "Chain deleted" : "Delete FAILED"); chainRefresh(); }
        confirmReturn = AppMode::SETTING;
        uiSetMode(AppMode::SETTING);
        return;
    }
    if (confirmKind == CK_RESET) { patchResetTone(); setInfo("Reset"); }
    else if (confirmKind == CK_OVERWRITE) { setInfo(savePatchFile(overwriteName) ? "Saved" : "Save FAILED"); scanPatches(); }
    else { setInfo(deletePatchFile(nameSource) ? "Deleted" : "Delete FAILED"); scanPatches(); }
    saveSettings();
    uiSetMode(AppMode::SETTING);
}

// ===========================================================================
// PLAY tab
// ===========================================================================
// IMU pad (v0.12, after C.P.S.): a 44px square with a dot at the current
// tilt, hollow once Calibrate has moved the zero point. The axis letters
// live with the state text beside the pad rather than on its edges, so
// nothing sits outside the square (v0.13).
static void drawImuPad(int px, int py) {
    const int PAD_SIZE = 44;
    static float dispX = 0.f, dispY = 0.f;
    float cx = sys.imuCalibrated ? sys.imuXCalDeg : 0.f, cy = sys.imuCalibrated ? sys.imuYCalDeg : 0.f;
    dispX += ((imuAngleXDeg - cx) - dispX) * 0.3f;
    dispY += ((imuAngleYDeg - cy) - dispY) * 0.3f;
    int mx = px + PAD_SIZE / 2, my = py + PAD_SIZE / 2;
    canvas.fillRect(px, py, PAD_SIZE, PAD_SIZE, BLACK);
    canvas.drawRect(px, py, PAD_SIZE, PAD_SIZE, uiColor);
    canvas.drawFastHLine(px, my, PAD_SIZE, uiDim);
    canvas.drawFastVLine(mx, py, PAD_SIZE, uiDim);
    float nX = clampf(dispX / 35.f, -1.f, 1.f), nY = clampf(dispY / 35.f, -1.f, 1.f);
    int dX = clampi(mx + (int)(nX * (PAD_SIZE / 2 - 3)), px + 2, px + PAD_SIZE - 3);
    int dY = clampi(my + (int)(nY * (PAD_SIZE / 2 - 3)), py + 2, py + PAD_SIZE - 3);
    if (sys.imuCalibrated) canvas.drawCircle(dX, dY, 3, uiColor);
    else                   canvas.fillCircle(dX, dY, 3, uiColor);
}

// Output scope. Shows the signal that actually left the mix bus - FM
// character, the FX and the limiter included - rather than a table shape.
// The previous frame is kept on screen when no new window is ready, so the
// trace never blanks between captures.
static int16_t uiScope[SCOPE_POINTS];
static bool    uiScopeHas = false;
static float   uiScopePeak = 0.f;            // smoothed window peak, for auto-gain
static unsigned long uiScopeStamp = 0;       // when the last fresh window arrived

// A full-scale scaling looks broken in practice: the signal that leaves the
// mix bus is master * patch volume * MIX_HEADROOM * the voice peak, which is
// well under a third of full scale in normal playing, so a +-12px box gets
// about 3px of movement - a straight line. The scope therefore tracks the
// window peak and scales to it: fast attack so a note-on fills the box at
// once, slow release so a decaying note shrinks smoothly instead of being
// held up at constant height. SCOPE_FLOOR stops the gain from magnifying
// dither and idle noise into a fake waveform, and SCOPE_MAX_GAIN caps how
// far a very quiet signal may be lifted (v0.32).
constexpr float SCOPE_FLOOR    = 350.f;   // below this the window counts as silence
constexpr float SCOPE_MAX_GAIN = 12.f;    // relative to full-scale scaling
constexpr unsigned long SCOPE_STALE_MS = 300;   // no fresh window for this long = silent

static void drawScope(int x, int y, int w, int h) {
    unsigned long now = millis();
    if (scopeTake(uiScope)) {
        uiScopeHas   = true;
        uiScopeStamp = now;
        int pk = 0;
        for (int i = 0; i < SCOPE_POINTS; i++) {
            int a = uiScope[i] < 0 ? -uiScope[i] : uiScope[i];
            if (a > pk) pk = a;
        }
        float p = (float)pk;
        // Attack instantly, release gently.
        if (p > uiScopePeak) uiScopePeak = p;
        else                 uiScopePeak += (p - uiScopePeak) * 0.25f;
    }
    // Once every voice and the FX tail are done, audioTask stops writing
    // output entirely, so no new window ever arrives. Without this the last
    // frame would sit frozen on screen looking like a sounding note.
    if (uiScopeHas && (now - uiScopeStamp) > SCOPE_STALE_MS) {
        uiScopeHas  = false;
        uiScopePeak = 0.f;
    }

    canvas.drawRect(x, y, w, h, uiDim);
    int cy = y + h / 2;
    canvas.drawFastHLine(x + 1, cy, w - 2, uiDim);
    if (!uiScopeHas || uiScopePeak < SCOPE_FLOOR) {
        canvas.setTextColor(uiMuted, BLACK);
        canvas.setCursor(x + 6, cy - 3); canvas.print("- silent -");
        canvas.setTextColor(uiColor, BLACK);
        return;
    }
    const float half = (float)(h / 2 - 2);
    float scale = half / uiScopePeak;
    const float maxScale = (half / 32767.f) * SCOPE_MAX_GAIN;
    if (scale > maxScale) scale = maxScale;
    int top = y + 1, bot = y + h - 2;
    int py = clampi(cy - (int)(uiScope[0] * scale), top, bot);
    int n = (w - 2 < SCOPE_POINTS) ? w - 2 : SCOPE_POINTS;
    for (int i = 1; i < n; i++) {
        int yy = clampi(cy - (int)(uiScope[i] * scale), top, bot);
        canvas.drawLine(x + i, py, x + 1 + i, yy, uiColor);
        py = yy;
    }
}

// A labelled bar row on the PLAY screen. bipolar draws from the centre.
static void drawPlayBar(int y, const char *label, float frac, bool bipolar, const char *value) {
    const int BX = 44, BW = 64, BH = 8;
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor(4, y); canvas.print(label);
    canvas.drawRect(BX, y - 1, BW, BH, uiColor);
    if (bipolar) {
        int mid = BX + BW / 2;
        int w = (int)(clampf(frac, -1.f, 1.f) * (BW / 2 - 1));
        if (w > 0) canvas.fillRect(mid, y, w, BH - 2, uiColor);
        else if (w < 0) canvas.fillRect(mid + w, y, -w, BH - 2, uiColor);
        canvas.drawFastVLine(mid, y, BH - 2, WHITE);
    } else {
        int w = (int)((BW - 2) * clampf(frac, 0.f, 1.f));
        if (w > 0) canvas.fillRect(BX + 1, y, w, BH - 2, uiColor);
    }
    canvas.setCursor(BX + BW + 3, y); canvas.print(value);
}

// Small status pill: filled when on, outlined when off.
static void drawPill(int x, int y, int w, const char *label, bool on) {
    if (on) { canvas.fillRect(x, y, w, 10, uiColor); canvas.setTextColor(BLACK, uiColor); }
    else    { canvas.drawRect(x, y, w, 10, uiDim);   canvas.setTextColor(uiMuted, BLACK); }
    canvas.setCursor(x + (w - (int)strlen(label) * 6) / 2, y + 1);
    canvas.print(label);
    canvas.setTextColor(uiColor, BLACK);
}

// "OpRatio" alone does not say which operator; the selected one is part
// of the destination, so the number belongs in the label (spec B.4).
static void targetLabel(char *buf, size_t n, ModTarget t) {
    if (targetIsOp(t)) snprintf(buf, n, "%.7s%d", MOD_TARGET_NAMES[(int)t], clampi((int)selectedOp, 0, NUM_OPS - 1) + 1);
    else               snprintf(buf, n, "%.8s", MOD_TARGET_NAMES[(int)t]);
}

// Note row (v0.41, the AEOLYRA / CONNECTOME way). ARP off: one box per
// voice, 8 across, with slots above the patch's Voices setting left blank so
// the polyphony in use is visible. ARP on: the notes the ARP works from
// instead - the latched chord with Latch on, the held keys otherwise - since
// only one or two voices sound at a time and they say nothing about what is
// latched. The member fired last is lit; it is tracked by index, so STEP
// notes moved by an octave still light the right box. The two looks differ
// at a glance, which is also what tells ARP on from off.
static void drawNoteRow(int limit) {
    const int Y = 35, H = 12;
    if (arp.on) {
        int n = arpChordSize();
        if (n == 0) {
            canvas.setTextColor(uiMuted, BLACK);
            canvas.setCursor(4, Y + 2);
            canvas.print(arp.latch ? "ARP LATCH empty - play notes to add" : "ARP on - hold notes to play");
            canvas.setTextColor(uiColor, BLACK);
            return;
        }
        int bw = 232 / (n < MAX_VOICES ? MAX_VOICES : n);   // 29 px up to 8 notes, 19 px at 12
        int lit = arpIsRunning() ? arpLastChordIndex() : -1;
        for (int i = 0; i < n; i++) {
            int x = 4 + i * bw;
            const char *nm = noteName(arpChordNote(i));
            if (i == lit) { canvas.fillRect(x, Y, bw - 1, H, uiColor); canvas.setTextColor(BLACK, uiColor); }
            else          { canvas.fillRect(x, Y, bw - 1, H, uiDim);   canvas.setTextColor(WHITE, uiDim); }
            canvas.setCursor(x + (bw - 1 - (int)strlen(nm) * 6) / 2, Y + 2);
            canvas.print(nm);
        }
        canvas.setTextColor(uiColor, BLACK);
        return;
    }
    // One box per voice across the same 232px: 29px at 8 voices, 19px at 12
    // (v0.62) - still wide enough for a 3-character name such as "C#4".
    const int VB = 232 / MAX_VOICES;
    for (int i = 0; i < MAX_VOICES; i++) {
        int x = 4 + i * VB;
        if (i >= limit) continue;
        if (voiceActiveAt(i)) {
            canvas.fillRect(x, Y, VB - 1, H, uiColor);
            canvas.setTextColor(BLACK, uiColor);
            const char *nm = noteName(voiceNoteAt(i));
            canvas.setCursor(x + (VB - 1 - (int)strlen(nm) * 6) / 2, Y + 2);
            canvas.print(nm);
            canvas.setTextColor(uiColor, BLACK);
        } else {
            canvas.drawRect(x, Y, VB - 1, H, uiDim);
        }
    }
}

// Octave / transpose box (v0.42, from AEOLYRA): always shown, 0 included, so
// the state is never in doubt; filled in the theme colour when not zero,
// because playing off by an octave or a semitone without noticing is what
// this display is there to prevent.
static void drawShiftBox(int x, int y, int w, const char *label, int value) {
    char b[12];
    snprintf(b, sizeof(b), "%s%+d", label, value);
    if (value != 0) { canvas.fillRect(x, y, w, 10, uiColor); canvas.setTextColor(BLACK, uiColor); }
    else            { canvas.drawRect(x, y, w, 10, uiColor); canvas.setTextColor(uiColor, BLACK); }
    canvas.setCursor(x + (w - (int)strlen(b) * 6) / 2, y + 1);
    canvas.print(b);
    canvas.setTextColor(uiColor, BLACK);
}

static void drawPlayScreen() {
    char b[40];
    canvas.setTextColor(uiColor, BLACK);
    // --- header: patch name, bank position, algorithm, polyphony -----------
    // While morphing, the header shows what is heard (update spec 4.4)
    const Patch &shown = morphRunning() ? livePatch : patch;
    int idx = patchIndexOfName(shown.name);
    int limit = clampi(shown.voices, 1, MAX_VOICES);
    canvas.setTextSize(2);
    canvas.setCursor(4, 13); canvas.print(shown.name);
    canvas.setTextSize(1);
    if (idx >= 0) snprintf(b, sizeof(b), "%d/%d", idx + 1, patchCount()); else snprintf(b, sizeof(b), "edit");
    canvas.setCursor(238 - (int)strlen(b) * 6, 13); canvas.print(b);
#ifdef MORPHASE_BLE
    // BLE (v0.77): dim b = waiting for a connection, lit B = connected.
    // Left of the bank position; a 12-letter name ends at x=148.
    if (bleMidiState() == BLE_STATE_ADV || bleMidiState() == BLE_STATE_CONN) {
        bool conn = bleMidiState() == BLE_STATE_CONN;
        canvas.setTextColor(conn ? uiColor : uiMuted, BLACK);
        canvas.setCursor(238 - (int)strlen(b) * 6 - 12, 13); canvas.print(conn ? 'B' : 'b');
        canvas.setTextColor(uiColor, BLACK);
    }
#endif
    if (limit == 1) snprintf(b, sizeof(b), "Algo %d  mono", shown.algorithm);
    else            snprintf(b, sizeof(b), "Algo %d  %d/%d", shown.algorithm, audioStatus.activeVoices, limit);
    canvas.setCursor(238 - (int)strlen(b) * 6, 22); canvas.print(b);
    canvas.drawFastHLine(0, 32, SCREEN_W, uiDim);
    // --- note row -----------------------------------------------------------
    drawNoteRow(limit);
    canvas.drawFastHLine(0, 49, SCREEN_W, uiDim);
    // --- left column -------------------------------------------------------
    // The scope took the place of the GMorph and Feedbk bars in v0.31: the
    // tilt pad on the right already shows where the modulation is, so the
    // space is better spent on the sound itself. Their values stay, as
    // readouts in the corners of the scope.
    drawScope(4, 51, 154, 28);
    // Each piece of text over the scope can be hidden (SETTING > Display,
    // v0.56): the trace is worth seeing whole.
    canvas.setTextColor(uiMuted, BLACK);
    if (sys.scopeGmFb) {
        canvas.setCursor(7, 53);
        canvas.printf("GM%3.0f%s", audioStatus.effGlobalMorph, modMarker(ModTarget::GMORPH));
        snprintf(b, sizeof(b), "FB%3.0f%s", audioStatus.effFeedback, modMarker(ModTarget::FEEDBACK));
        canvas.setCursor(155 - (int)strlen(b) * 6, 53); canvas.print(b);
    }
    if (sys.scopeFx) {
        // C D R X L in key order: lit = the effect is sounding
        static const char FXL[FX_PAD_COUNT] = { 'C', 'D', 'R', 'X', 'V' };   // V: Drive (D is Delay; L before v0.72)
        for (int i = 0; i < FX_PAD_COUNT; i++) {
            bool lit = fxPadActive(i);
            canvas.setTextColor(lit ? uiColor : uiMuted, BLACK);
            canvas.setCursor(101 + i * 12, 71); canvas.print(FXL[i]);
        }
        canvas.setTextColor(uiMuted, BLACK);
    }
    if (sys.scopeGl) {
        // GL (v0.6): dim = Off, lit = On (or held), inverted = an event right now
        bool on = sys.glOn || glitchHeld();
        if (glitchEventActive()) { canvas.fillRect(82, 70, 14, 9, uiColor); canvas.setTextColor(BLACK, uiColor); }
        else canvas.setTextColor(on ? uiColor : uiMuted, BLACK);
        canvas.setCursor(83, 71); canvas.print("GL");
        canvas.setTextColor(uiMuted, BLACK);
    }
    // Morph position, compact in the scope's lower corner and only while
    // morphing, so the scope keeps its space (update spec 6.2)
    if (!sys.scopeMorph) {
        // morph readout hidden
    } else if (morphRunning() && audioStatus.morphSeg >= 0) {
        // v0.85: C = the chain (M stays for a Shift+1-8 / PC glide below)
        int sg = audioStatus.morphSeg;
        snprintf(b, sizeof(b), "C%d>%d %d%%%s", sg + 1, sg + 2, (int)(audioStatus.morphT * 100.f + 0.5f),
                 modMarker(ModTarget::PATCH_MORPH));
        canvas.setCursor(7, 71); canvas.print(b);
    } else if (morphGliding() && audioStatus.morphGlideTo >= 0) {
        snprintf(b, sizeof(b), "M>%d %d%%", audioStatus.morphGlideTo + 1, (int)(audioStatus.morphT * 100.f + 0.5f));
        canvas.setCursor(7, 71); canvas.print(b);
    } else if (morphChainPaused()) {
        canvas.setCursor(7, 71); canvas.print("C paused");
    } else if (!sys.morphEnabled && morphChainLength() >= 2) {
        canvas.setCursor(7, 71); canvas.print("C off");   // dim: a chain is there but not on (v0.85)
    }
    canvas.setTextColor(uiColor, BLACK);
    snprintf(b, sizeof(b), "%+.1f%s", audioStatus.effPitchSemi, modMarker(ModTarget::PITCH));
    drawPlayBar(82, "Pitch", audioStatus.effPitchSemi / (float)clampi(sys.bendRange, 1, 12), true, b);
    // ARP / LFO
    canvas.setCursor(4, 93);
    if (arp.on) {
        // STEP plays one of the eight stored patterns, and which one it is
        // matters while playing - so the slot number rides next to the name.
        // "STEP8" is exactly six characters, so the field width is unchanged
        // and nothing after it shifts (v0.32).
        char pat[10];
        if (arp.pattern == ArpPattern::STEP) snprintf(pat, sizeof(pat), "STEP%d", sys.patternSlot + 1);
        else                                snprintf(pat, sizeof(pat), "%.6s", ARP_PATTERN_NAMES[(int)arp.pattern]);
        canvas.printf("ARP %-6.6s %3.0f %-5s%s", pat, arpEffectiveBpm(),
                      ARP_RATE_NAMES[(int)arp.rate], arp.latch ? "L" : "");
        if (arp.clockSync && midiClockLocked()) canvas.print("CLK");
    } else canvas.print("ARP off");
    {
        char lt[12]; targetLabel(lt, sizeof(lt), patch.lfoTarget);
        canvas.setCursor(4, 103);
        canvas.printf("LFO %-3.3s %4.1fHz>%s%s", LFO_WAVE_NAMES[(int)patch.lfoWave], patch.lfoRateHz,
                      lt, (patch.lfoDepth > 0.f && patch.lfoTarget != ModTarget::OFF) ? " ~" : "");
    }
    // Status pills, then octave / transpose (v0.42). SD state gave up its
    // place (SETTING > Info still shows it) and the volume moved to the
    // bottom of the right column, where "CAL" was - the pad's hollow dot
    // already says calibration is on.
    // v0.85: narrower, to make room for the theremin. -DIN = DIN MIDI is
    // off because the theremin has the Grove pins.
    bool dinOff = sys.thOn && sys.thBus == ThBus::GROVE;
    drawPill(4, 114, 26, dinOff ? "-DIN" : "MIDI", !dinOff && midiRxActive());
    drawPill(32, 114, 26, "HOLD", sustainKeyHeld());
    drawShiftBox(60, 114, 36, "OCT", clampi(sys.octave, OCTAVE_MIN, OCTAVE_MAX));
    drawShiftBox(98, 114, 40, "TRN", clampi(sys.transpose, TRANSPOSE_MIN, TRANSPOSE_MAX));
    drawPill(140, 114, 20, sys.thMode == ThMode::PLAY ? "THP" : "THR", sys.thOn);   // theremin: Mod / Play
    canvas.setTextColor(uiColor, BLACK);
    // --- right column: IMU --------------------------------------------------
    canvas.drawFastVLine(160, 50, 74, uiDim);
    drawImuPad(166, 52);
    canvas.setTextColor(uiColor, BLACK);
    const int TX = 214;
    canvas.setCursor(TX, 54); canvas.print("X");
    canvas.setCursor(TX, 63); canvas.print(sys.imuXEnabled ? (sys.imuXHeld ? "Hold" : "On") : "Off");
    canvas.setCursor(TX, 78); canvas.print("Y");
    canvas.setCursor(TX, 87); canvas.print(sys.imuYEnabled ? (sys.imuYHeld ? "Hold" : "On") : "Off");
    canvas.setCursor(TX, 102); canvas.print("VOL");
    // v0.87: the patch Volume is being moved (IMU / LFO / theremin). Room
    // for one character before the screen edge: the first of the marks.
    { const char *mk = modMarker(ModTarget::VOLUME); if (mk[0]) canvas.print(mk[1]); }
    canvas.setCursor(TX, 112); canvas.printf("%3d%%", clampi(sys.masterVolume, 0, 100));
    { char t[12];
      // The VolAxis axis is the theremin's volume in Play, its target resting (v0.85)
      int va = thereminVolAxis();
      targetLabel(t, sizeof(t), patch.imuXTarget); canvas.setCursor(164, 102); canvas.print(va == 1 ? "vol" : t);
      targetLabel(t, sizeof(t), patch.imuYTarget); canvas.setCursor(164, 112); canvas.print(va == 2 ? "vol" : t); }
    drawNavHint(";/.:oct  ,//:trn  G0:Edit  Sh+H");
}

// PLAY keys (v0.42, AEOLYRA layout): ; . = octave up / down, , / =
// transpose down / up. Hold to repeat.
static RepeatKey playOctDn, playOctUp, playTrnDn, playTrnUp;
static void playHandleKeys(const KeyFrame &k) {
    if (repeatFire(playOctUp, keyDown(k, ';') || keyDown(k, ':'))) { sys.octave = clampi(sys.octave + 1, OCTAVE_MIN, OCTAVE_MAX); uiDirty = true; }
    if (repeatFire(playOctDn, keyDown(k, '.') || keyDown(k, '>'))) { sys.octave = clampi(sys.octave - 1, OCTAVE_MIN, OCTAVE_MAX); uiDirty = true; }
    if (repeatFire(playTrnDn, keyDown(k, ',') || keyDown(k, '<'))) { sys.transpose = clampi(sys.transpose - 1, TRANSPOSE_MIN, TRANSPOSE_MAX); uiDirty = true; }
    if (repeatFire(playTrnUp, keyDown(k, '/') || keyDown(k, '?'))) { sys.transpose = clampi(sys.transpose + 1, TRANSPOSE_MIN, TRANSPOSE_MAX); uiDirty = true; }
}

// ===========================================================================
// Dispatch
// ===========================================================================
void uiScreensInit() {
    memset(&opNav, 0, sizeof(opNav)); memset(&algoNav, 0, sizeof(algoNav)); memset(&arpNav, 0, sizeof(arpNav));
    memset(&lfoNav, 0, sizeof(lfoNav)); memset(&fxNav, 0, sizeof(fxNav)); memset(&setNav, 0, sizeof(setNav));
    memset(&stepLeft, 0, sizeof(RepeatKey)); memset(&stepRight, 0, sizeof(RepeatKey));
    memset(&stepUp, 0, sizeof(RepeatKey)); memset(&stepDown, 0, sizeof(RepeatKey));
    memset(&stepDec, 0, sizeof(RepeatKey)); memset(&stepInc, 0, sizeof(RepeatKey));
    memset(&fxLeft, 0, sizeof(RepeatKey)); memset(&fxRight, 0, sizeof(RepeatKey));
    memset(&brUp, 0, sizeof(RepeatKey)); memset(&brDown, 0, sizeof(RepeatKey));
    memset(&catUp, 0, sizeof(RepeatKey)); memset(&catDown, 0, sizeof(RepeatKey));
    memset(&midiNav, 0, sizeof(midiNav));
    memset(&listUp, 0, sizeof(RepeatKey)); memset(&listDown, 0, sizeof(RepeatKey));
    memset(&playOctDn, 0, sizeof(RepeatKey)); memset(&playOctUp, 0, sizeof(RepeatKey));
    memset(&playTrnDn, 0, sizeof(RepeatKey)); memset(&playTrnUp, 0, sizeof(RepeatKey));
    overwriteName[0] = 0;
    itemPoolOwner = IO_NONE;   // each list screen fills the shared item table when it opens
    lastPattern = arp.pattern;
    nameBuf[0] = 0; nameSource[0] = 0;
}

void uiOnModeEntered(AppMode m) {
    // Re-entering FX always starts on the pad selector (spec 6.2)
    if (m == AppMode::FX) fxView = FX_PADS;
    if (m == AppMode::OP) bindOpItems();
    if (m == AppMode::ARP) lastPattern = arp.pattern;
    // Holds that began on another screen must not auto-repeat here
    ListNav *navs[6] = { &opNav, &algoNav, &arpNav, &lfoNav, &fxNav, &setNav };
    for (int i = 0; i < 6; i++) { navs[i]->up.heldMs = 0; navs[i]->down.heldMs = 0; navs[i]->inc.heldMs = 0; navs[i]->dec.heldMs = 0; }
}

bool uiScreenConsumesTab() {
    if (appMode == AppMode::FX && fxView == FX_PARAMS) return true;
    if (appMode == AppMode::SETTING && setInCategory) return true;
    return appMode == AppMode::PATCH_BROWSER || appMode == AppMode::NAME_ENTRY || appMode == AppMode::CONFIRM;
}

bool uiScreenWantsPeriodicRedraw() {
    if (millis() < infoUntilMs + 200) return true;     // so a message also goes away on its own
    if (appMode == AppMode::SETTING && setCategory == SC_PATCH && morphPage) return true;   // live position
    if (appMode == AppMode::SETTING && setInCategory && setCategory == SC_GLITCH) return true;   // live GL
    if (appMode == AppMode::SETTING && setInCategory && setCategory == SC_THEREMIN) return true; // live distance (v0.81)
    if (appMode == AppMode::ALGO && algoRowIsPenv()) return true;                               // the dot (v0.85)
    return appMode == AppMode::PLAY || appMode == AppMode::ARP || appMode == AppMode::LFO || appMode == AppMode::OP;
}

// Does this key frame change a patch value on an edit tab? Used to hand a
// running morph over to editing before the edit lands (update spec 5.1).
// Cursor keys and the FX pad cursor do not count; ARP settings are not in
// the patch, and the IMU routing on SETTING is never morphed.
static bool frameEditsPatch(const KeyFrame &k) {
    bool val = keyDown(k, ',') || keyDown(k, '/') || keyDown(k, '<') || keyDown(k, '?');
    bool ent = k.enterPressed && !k.shift;
    switch (appMode) {
        case AppMode::OP:  return val || ent || keyPressed(k, 'f');   // f: Ratio <-> Fixed (v0.71)
        case AppMode::ALGO: case AppMode::LFO: return val || ent;
        case AppMode::FX: return fxView == FX_PADS ? ent : val;
        default: return false;
    }
}

void uiScreenHandleKeys(const KeyFrame &k) {
    pollMidiLearn();
    // Editing while morphing starts from what is heard: livePatch becomes the
    // patch, morph turns off, and the edit is applied on top of that.
    if (morphRunning() && frameEditsPatch(k)) morphCommitAndStop();
    switch (appMode) {
        case AppMode::OP:      opHandleKeys(k); break;
        case AppMode::ALGO:    algoHandleKeys(k); break;
        case AppMode::ARP:     arpHandleKeys(k); break;
        case AppMode::LFO:     lfoHandleKeys(k); break;
        case AppMode::FX:      fxHandleKeys(k); break;
        case AppMode::SETTING: setHandleKeys(k); break;
        case AppMode::PATCH_BROWSER: browserHandleKeys(k); break;
        case AppMode::NAME_ENTRY:    nameHandleKeys(k); break;
        case AppMode::CONFIRM:       confirmHandleKeys(k); break;
        case AppMode::PLAY:    playHandleKeys(k); break;
        default: break;
    }
}

void uiDrawScreen() {
    { const char *m = morphTakeNotice(); if (m) setInfo(m); }
    { const char *m = patchTakeSaveNotice(); if (m) setInfo(m); }   // v0.72: a save that did not happen
    canvas.startWrite();
    canvas.fillScreen(BLACK);
    drawTabBar(appMode);
    switch (appMode) {
        case AppMode::PLAY:    drawPlayScreen(); break;
        case AppMode::OP:      drawOpScreen(); break;
        case AppMode::ALGO:    drawAlgoScreen(); break;
        case AppMode::ARP:     drawArpScreen(); break;
        case AppMode::LFO:     drawLfoScreen(); break;
        case AppMode::FX:      drawFxScreen(); break;
        case AppMode::SETTING: drawSetScreen(); break;
        case AppMode::PATCH_BROWSER: drawBrowserScreen(); break;
        case AppMode::NAME_ENTRY:    drawNameScreen(); break;
        case AppMode::CONFIRM:       drawConfirmScreen(); break;
        default: break;
    }
    // Short messages show on every screen, not only on SETTING (v0.5) - a
    // morph can switch itself off from any tab.
    if (millis() < infoUntilMs) drawNavHint(infoLine);
    if (helpVisible) drawHelpOverlay();
    canvas.endWrite();
    canvas.pushSprite(0, 0);
}

void uiLoadPatchByIndex(int index) {
    if (patchCount() == 0) scanPatches();
    if (index < 0 || index >= patchCount()) return;
    if (loadPatchFile(patchNameAt(index))) { uiDirty = true; }
}

#ifdef MORPHASE_SCREENSHOT
// Manual-making builds only (never released): the canvas - what the screen
// shows - as a 24-bit BMP, one row at a time (720 bytes on the stack).
#include <SD.h>
void uiScreenshot() {
    if (!sdReady()) { setInfo("Shot: no SD"); return; }
    const char *DIR = MORPHASE_FOLDER "/Shots";
    if (!SD.exists(DIR)) SD.mkdir(DIR);
    char path[48];
    int n = 0;
    do { snprintf(path, sizeof(path), "%s/shot_%03d.bmp", DIR, n++); } while (SD.exists(path) && n < 1000);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { setInfo("Shot: write FAILED"); return; }
    const int W = SCREEN_W, H = SCREEN_H, ROW = W * 3;            // 720: already a multiple of 4
    const uint32_t imgSize = (uint32_t)ROW * H, fileSize = 54 + imgSize;
    uint8_t hdr[54] = { 'B', 'M' };
    auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) hdr[at + i] = (uint8_t)(v >> (8 * i)); };
    put32(2, fileSize); put32(10, 54); put32(14, 40); put32(18, W); put32(22, H);
    hdr[26] = 1; hdr[28] = 24; put32(34, imgSize);
    f.write(hdr, sizeof(hdr));
    uint8_t row[ROW];
    for (int y = H - 1; y >= 0; y--) {                             // BMP rows go bottom-up
        canvas.readRectRGB(0, y, W, 1, row);
        for (int x = 0; x < W; x++) { uint8_t r = row[x * 3]; row[x * 3] = row[x * 3 + 2]; row[x * 3 + 2] = r; }   // RGB -> BGR
        f.write(row, ROW);
    }
    f.close();
    char b[40]; snprintf(b, sizeof(b), "Shot: %s", strrchr(path, '/') + 1);
    setInfo(b);
}
// 5s: the "Shot in 5s" notice (2.5s) is gone by then, so it is not in the picture
static unsigned long shotDueMs = 0;
void uiScreenshotArm() { shotDueMs = millis() + 5000; if (shotDueMs == 0) shotDueMs = 1; setInfo("Shot in 5s"); }
void uiScreenshotPoll() {
    if (shotDueMs && (long)(millis() - shotDueMs) >= 0 && millis() >= infoUntilMs) { shotDueMs = 0; uiScreenshot(); }
}
#endif