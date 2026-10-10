// MORPHASE - splash.cpp
// Boot splash (v0.89, MORPHASE_UPDATE_SPLASH_v0_89.md; the logo v0.92,
// MORPHASE_UPDATE_v0_92.md; one morph and power-on start v0.92T2,
// MORPHASE_UPDATE_v0_92T2.md; the flat line first v0.92T3).
//
//   line             a flat line in the theme colour while the SD card is
//                    looked for - SD.begin() blocks and can't draw frames,
//                    and a still line doesn't show the stalls (v0.92T3)
//   morph    1.2 s  the wave grows out of the line (0.25 s) and the eight
//                   engine waveforms morph in a band, scrolling
//                   left to right (morphPreviewSample() - the same tables and
//                   blend the operators use). Once. Only while the factory
//                   patches are being written does it keep going back and
//                   forth, until they are done.
//   settle   0.3 s  the wave keeps its shape, shrinks and drops to the
//                   height of the logo's underline (v0.92T3: it used to
//                   turn back to a sine, which read as a second morph)
//   underline 0.3 s the wave goes; the logo's underline is drawn in from the
//                   left - sine first, square last, the way the morph runs
//   letters  0.2 s  the logo's letters drawn in from the left, and
//                   "4-OP FM POLYSYNTH <version>" under them
//   hold    >=1.6 s it stays - and for as long as the boot's SD work is
//                   still going (a big patch folder takes longer)
//   glitch   ~0.1 s three frames, then the PLAY screen
//
// The stages are stepped from two places: during the SD work, through
// bootProgress (patch.cpp calls it per file and while it waits for the
// card), and from splashFinish() once that work is done. They run on
// animation time - it only moves when a frame is drawn, so a long wait on
// the card can't skip a stage (v0.89T2).
//
// No buffer at all (v0.89T2): each frame erases the previous wave by drawing
// it again in black - recomputed from its parameters, not stored - then draws
// the new one straight to the panel. v0.89T1's band sprite left a hole in the
// heap once freed (largest block 47K -> 39K on the standard build). The logo
// is two 1-bit bitmaps in flash (splash_logo.h, 810 bytes each), drawn 1:1.
//
// Any key or G0 skips to the PLAY screen (after the SD work is done).

#include "splash.h"
#include "splash_logo.h"
#include "config.h"
#include "fm.h"
#include "ui_common.h"
#include <M5Cardputer.h>
#include <driver/gpio.h>

#define SPL_BAND_Y     18
#define SPL_BAND_H     60
#define SPL_FRAME_MS   33
#define SPL_MORPH_MS   1200      // one sweep Sine -> Square2
#define SPL_GROW_MS    250       // the wave grows out of the flat line
#define SPL_SETTLE_MS  300       // smaller, down to the underline
#define SPL_LINE_MS    300       // the underline drawn in
#define SPL_LETTER_MS  200       // the letters drawn in
#define SPL_HOLD_MS    1600      // it stays, at least this long (v0.92T3: 1.2 s + the settle's 0.1)
#define SPL_INST_Y     100
#define SPL_SCROLL     24        // table points per frame

// The logo, centred across; with the line of text under it the group sits a
// little above the middle of the 135-pixel screen.
#define SPL_LOGO_X     ((SCREEN_W - SPLASH_LOGO_W) / 2)     // 14
#define SPL_LOGO_Y     38
#define SPL_SUB_Y      (SPL_LOGO_Y + SPLASH_LOGO_H + 10)    // 78
#define SPL_WAVE_MID0  (SPL_BAND_Y + SPL_BAND_H / 2)        // the band's centre
#define SPL_WAVE_MID1  (SPL_LOGO_Y + (SPLASH_LOGO_WAVE_Y0 + SPLASH_LOGO_WAVE_Y1) / 2)   // the underline's
#define SPL_AMP_PX     (SPL_BAND_H / 2 - 4)
#define SPL_AMP_END    (3.f / SPL_AMP_PX)                    // the underline swings about 3 px

enum SplStage : uint8_t { ST_FLAT, ST_MORPH, ST_SETTLE, ST_LINE, ST_LETTERS, ST_HOLD };

struct WaveDraw { float m, a; uint32_t phase; int dx, mid; bool shown; };
static WaveDraw prevWave = { 0, 0, 0, 0, 0, false };
static unsigned long lastFrame = 0, animMs = 0, stageStart = 0, holdStartMs = 0;
static bool skipped = false, installing = false;
static SplStage stage = ST_MORPH;
static float lastMorph = 0.f, settleFrom = 0.f;
static uint32_t phase = 0;
static int revealCol = 0;

static bool anyKeyDown() {
    M5Cardputer.update();
    return M5Cardputer.Keyboard.isPressed() || M5Cardputer.BtnA.isPressed()
        || gpio_get_level(GPIO_NUM_0) == 0;          // G0 read directly as well (v0.89T2)
}

// One pass of the wave (2 px thick) and its centre line, in colour c (the
// centre line in cl).
static void strokeWave(const WaveDraw &w, uint16_t c, uint16_t cl) {
    auto &d = M5Cardputer.Display;
    d.drawFastHLine(0, w.mid, SCREEN_W, cl);
    int py = 0;
    for (int x = 0; x < SCREEN_W; x++) {
        // two periods across the screen, scrolling right
        int idx = (int)(((uint32_t)x * 2u * WAVE_TABLE_SIZE / SCREEN_W + WAVE_TABLE_SIZE * 4 - w.phase) % WAVE_TABLE_SIZE);
        int y = w.mid - (int)(morphPreviewSample(w.m, idx) * w.a * SPL_AMP_PX);
        int sx = x + w.dx;
        if (x && sx > 0 && sx < SCREEN_W) {
            d.drawLine(sx - 1, py, sx, y, c);
            d.drawLine(sx - 1, py + 1, sx, y + 1, c);
        }
        py = y;
    }
}

static void eraseWave() {
    if (!prevWave.shown) return;
    M5Cardputer.Display.startWrite();
    strokeWave(prevWave, BLACK, BLACK);
    M5Cardputer.Display.endWrite();
    prevWave.shown = false;
}

static void drawWave(float m, float a, int dx, int mid = SPL_WAVE_MID0) {
    auto &d = M5Cardputer.Display;
    WaveDraw w = { m, a, phase, dx, mid, true };
    d.startWrite();
    if (prevWave.shown) strokeWave(prevWave, BLACK, BLACK);
    strokeWave(w, uiColor, uiDim);
    d.endWrite();
    prevWave = w;
}

static void drawInstalling(bool on) {
    auto &d = M5Cardputer.Display;
    d.fillRect(0, SPL_INST_Y, SCREEN_W, 10, BLACK);
    if (!on) return;
    const char *s = "Installing factory patches...";
    d.setTextSize(1); d.setTextColor(uiMuted, BLACK);
    d.setCursor((SCREEN_W - (int)strlen(s) * 6) / 2, SPL_INST_Y); d.print(s);
}

// Columns [c0, c1) of a logo bitmap, pixel by pixel (the reveal).
static void drawLogoCols(const uint8_t *bits, int c0, int c1, uint16_t c) {
    auto &d = M5Cardputer.Display;
    const int stride = (SPLASH_LOGO_W + 7) / 8;
    d.startWrite();
    for (int x = c0; x < c1 && x < SPLASH_LOGO_W; x++)
        for (int y = 0; y < SPLASH_LOGO_H; y++)
            if (bits[y * stride + (x >> 3)] & (0x80 >> (x & 7))) d.drawPixel(SPL_LOGO_X + x, SPL_LOGO_Y + y, c);
    d.endWrite();
}

static void drawSub(uint16_t fg, uint16_t bg) {
    auto &d = M5Cardputer.Display;
    const char *sub = "4-OP FM POLYSYNTH " MORPHASE_VERSION MORPHASE_EDITION;
    d.setTextSize(1); d.setTextColor(fg, bg);
    d.setCursor((SCREEN_W - (int)strlen(sub) * 6) / 2, SPL_SUB_Y); d.print(sub);
}

// The whole logo and the line under it, shifted by dx (glitch frames).
static void drawLogo(int dx, bool inverted) {
    auto &d = M5Cardputer.Display;
    uint16_t fg = inverted ? BLACK : uiColor, bg = inverted ? uiColor : BLACK;
    d.fillRect(0, SPL_LOGO_Y - 2, SCREEN_W, SPL_SUB_Y + 10 - (SPL_LOGO_Y - 2), bg);
    d.drawBitmap(SPL_LOGO_X + dx, SPL_LOGO_Y, SPLASH_LOGO_LETTERS, SPLASH_LOGO_W, SPLASH_LOGO_H, fg);
    d.drawBitmap(SPL_LOGO_X + dx, SPL_LOGO_Y, SPLASH_LOGO_WAVE, SPLASH_LOGO_W, SPLASH_LOGO_H, fg);
    drawSub(fg, bg);
}

// Back-and-forth through the eight waveforms (Sine at 0 and every 2.4 s).
static float morphAt(unsigned long ms) {
    unsigned long p = ms % (2 * SPL_MORPH_MS);
    return (p < SPL_MORPH_MS ? p : 2 * SPL_MORPH_MS - p) * 100.f / SPL_MORPH_MS;
}

// True when a frame is due; advances the animation clock by the time since
// the last frame, at most one frame's worth (time spent blocked on the SD
// card does not count).
static bool frameDue() {
    unsigned long now = millis(), dt = now - lastFrame;
    if (dt < SPL_FRAME_MS) return false;
    lastFrame = now;
    animMs += dt > SPL_FRAME_MS ? SPL_FRAME_MS : dt;
    phase = (phase + SPL_SCROLL) % WAVE_TABLE_SIZE;
    return true;
}

static void enterStage(SplStage s) { stage = s; stageStart = animMs; revealCol = 0; }

static float stageK(unsigned long ms) {
    float k = (float)(animMs - stageStart) / ms;
    return k > 1.f ? 1.f : k;
}

// One step of the splash: checks for a skip, and draws the next frame of
// whichever stage it is in when a frame is due.
static void splashTick() {
    if (skipped) return;
    if (anyKeyDown()) { skipped = true; return; }
    if (!frameDue()) return;
    switch (stage) {
        case ST_FLAT:
            break;              // the line stays until splashStartWave()
        case ST_MORPH: {
            lastMorph = morphAt(animMs);
            float g = (float)animMs / SPL_GROW_MS;
            drawWave(lastMorph, g < 1.f ? g : 1.f, 0);
            // one sweep - longer only while the factory patches are written
            if (animMs >= SPL_MORPH_MS && !installing) { settleFrom = lastMorph; enterStage(ST_SETTLE); }
            break;
        }
        case ST_SETTLE: {
            // the same shape, smaller, and down to the underline's height
            float k = stageK(SPL_SETTLE_MS);
            drawWave(settleFrom, 1.f - k * (1.f - SPL_AMP_END), 0,
                     SPL_WAVE_MID0 + (int)((SPL_WAVE_MID1 - SPL_WAVE_MID0) * k + 0.5f));
            if (k >= 1.f) { eraseWave(); enterStage(ST_LINE); }
            break;
        }
        case ST_LINE: {
            // the underline, drawn in from the left
            int to = (int)(SPLASH_LOGO_W * stageK(SPL_LINE_MS) + 0.5f);
            drawLogoCols(SPLASH_LOGO_WAVE, revealCol, to, uiColor); revealCol = to;
            if (to >= SPLASH_LOGO_W) { drawSub(uiColor, BLACK); enterStage(ST_LETTERS); }
            break;
        }
        case ST_LETTERS: {
            int to = (int)(SPLASH_LOGO_W * stageK(SPL_LETTER_MS) + 0.5f);
            drawLogoCols(SPLASH_LOGO_LETTERS, revealCol, to, uiColor); revealCol = to;
            if (to >= SPLASH_LOGO_W) { holdStartMs = millis(); enterStage(ST_HOLD); }
            break;
        }
        case ST_HOLD:
            break;              // it stays as it is
    }
}

void splashBegin() {
    M5Cardputer.Display.fillScreen(BLACK);
    prevWave.shown = false; skipped = false; installing = false; phase = 0; animMs = 0;
    lastFrame = millis();
    enterStage(ST_FLAT);
    drawWave(0.f, 0.f, 0);          // the flat line, in the theme colour
}

// The SD card has been looked for: the wave grows out of the line and the
// morph starts, on a fresh animation clock.
void splashStartWave() {
    animMs = 0;
    lastFrame = millis() - SPL_FRAME_MS;
    enterStage(ST_MORPH);
    splashTick();
}

// Called during the boot SD work (patch.cpp's bootProgress): while the card
// is looked for, around and between the factory files, and once per file
// while the patch list is scanned and the chain patches are read. A factory
// install that starts after the morph already gave way to the logo just
// shows its line under the logo; the hold lasts until it is done.
void splashFactoryProgress(int p) {
    if (p == 0) { installing = true; drawInstalling(true); }
    else if (p == 2) { installing = false; drawInstalling(false); }
    splashTick();
}

void splashFinish() {
    // The SD work is done: play what is left, then hold at least SPL_HOLD_MS
    // from when the letters were complete
    installing = false;
    while (!skipped && !(stage == ST_HOLD && millis() - holdStartMs >= SPL_HOLD_MS)) { splashTick(); delay(1); }
    if (!skipped) {
        // Glitch: three frames - the logo jolted sideways, flickering
        static const int  DX[3]  = { 14, -9, 5 };
        static const bool INV[3] = { true, false, false };
        for (int f = 0; f < 3; f++) { drawLogo(DX[f], INV[f]); delay(SPL_FRAME_MS); }
    }
    if (skipped) {
        // Let go of the key first, so it does nothing on the PLAY screen
        // (G0 acts on release there). At most a second.
        unsigned long w = millis();
        while (anyKeyDown() && millis() - w < 1000) delay(5);
        M5Cardputer.update();
    }
    M5Cardputer.Display.fillScreen(BLACK);
}
