// MORPHASE - ui_common.cpp
// Canvas, theme, keys, menu machinery, tab bar, help overlay.

#include "ui_common.h"
#include "arp.h"
#include <string.h>
#include <stdio.h>

M5Canvas canvas(&M5Cardputer.Display);
uint16_t uiColor = 0x07E0;
uint16_t uiDim = 0x0200;
uint16_t uiMuted = 0x0400;     // v0.85: soft TEXT and second lines (uiDim is for dividers only)
uint16_t uiHighlight = 0xF81F;

volatile int selectedOp = 0;   // OP tab selection; the OP_* mod targets follow it

AppMode appMode = AppMode::PLAY;
AppMode lastEditMode = AppMode::OP;
bool helpVisible = false;
bool uiDirty = true;

// ---------------------------------------------------------------------------
void uiInitCanvas() {
#if defined(MORPHASE_BLE)
    // BLE build (v0.77): 8-bit colour frees 32.4KB of heap for the BT stack.
    // The UI is nearly one colour per theme, so 256 colours lose nothing
    // visible (checked on the device). pushSprite converts to the panel.
    canvas.setColorDepth(8);
#else
    canvas.setColorDepth(16);
#endif
    if (!canvas.createSprite(SCREEN_W, SCREEN_H)) {
        Serial.println("[Canvas] createSprite FAILED - the screen will stay blank");
    }
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
}

void applyUiTheme() {
    switch (sys.theme) {
        case UiTheme::AMBER:  uiColor = M5Cardputer.Display.color565(255, 170, 30);  uiDim = M5Cardputer.Display.color565(70, 45, 0);  uiMuted = M5Cardputer.Display.color565(140, 94, 17);  break;
        case UiTheme::CYAN:   uiColor = M5Cardputer.Display.color565(40, 220, 230);  uiDim = M5Cardputer.Display.color565(0, 55, 60);  uiMuted = M5Cardputer.Display.color565(22, 121, 127); break;
        case UiTheme::VIOLET: uiColor = M5Cardputer.Display.color565(190, 120, 255); uiDim = M5Cardputer.Display.color565(50, 25, 70); uiMuted = M5Cardputer.Display.color565(110, 70, 150); break;
        // Red = C.P.S.'s Ember (v0.56): green near zero reads as deep red, not vermilion
        case UiTheme::RED:    uiColor = M5Cardputer.Display.color565(225, 0, 35);    uiDim = M5Cardputer.Display.color565(64, 0, 10);  uiMuted = M5Cardputer.Display.color565(140, 0, 22); break;   // red alone: a little brighter (60%)
        case UiTheme::GREEN:
        default:              uiColor = M5Cardputer.Display.color565(60, 230, 90);   uiDim = M5Cardputer.Display.color565(0, 64, 0);   uiMuted = M5Cardputer.Display.color565(33, 127, 50); break;
    }
    uiHighlight = M5Cardputer.Display.color565(170, 40, 255);   // purple playhead (spec 8.4)
    uiDirty = true;
}

void applyUiBrightness() {
    int b = clampi(sys.brightness, 10, 100);
    M5Cardputer.Display.setBrightness((uint8_t)(b * 255 / 100));
}

// ---------------------------------------------------------------------------
void uiSetMode(AppMode m) {
    if (m == appMode) return;
    appMode = m;
    if (m != AppMode::PLAY && m <= AppMode::SETTING) lastEditMode = m;
    helpVisible = false;
    uiDirty = true;
}

AppMode uiNextTab(AppMode m, bool backwards) {
    int i = (int)m;
    if (i >= NUM_TABS) i = (int)AppMode::SETTING;   // sub-screens fold back to SETTING
    i = backwards ? (i + NUM_TABS - 1) % NUM_TABS : (i + 1) % NUM_TABS;
    return (AppMode)i;
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------
static char prevWord[12];
static int  prevWordLen = 0;
static bool prevTab = false, prevEnter = false, prevDel = false;

void uiPollKeys(KeyFrame &k) {
    k.changed = M5Cardputer.Keyboard.isChange();
    auto s = M5Cardputer.Keyboard.keysState();
    k.shift = s.shift; k.tab = s.tab; k.enter = s.enter; k.del = s.del; k.fn = s.fn;
    k.wordLen = 0;
    for (char c : s.word) { if (k.wordLen < 11) k.word[k.wordLen++] = c; }
    k.word[k.wordLen] = 0;
    k.pressedLen = 0;
    for (int i = 0; i < k.wordLen; i++) {
        bool was = false;
        for (int j = 0; j < prevWordLen; j++) if (prevWord[j] == k.word[i]) { was = true; break; }
        if (!was && k.pressedLen < 11) k.pressed[k.pressedLen++] = k.word[i];
    }
    k.pressed[k.pressedLen] = 0;
    k.tabPressed   = k.tab && !prevTab;
    k.enterPressed = k.enter && !prevEnter;
    k.delPressed   = k.del && !prevDel;
    memcpy(prevWord, k.word, sizeof(prevWord)); prevWordLen = k.wordLen;
    prevTab = k.tab; prevEnter = k.enter; prevDel = k.del;
}

bool keyDown(const KeyFrame &k, char c)    { for (int i = 0; i < k.wordLen; i++) if (k.word[i] == c) return true; return false; }
bool keyPressed(const KeyFrame &k, char c) { for (int i = 0; i < k.pressedLen; i++) if (k.pressed[i] == c) return true; return false; }

bool repeatFire(RepeatKey &r, bool nowDown) {
    unsigned long t = millis();
    bool fire = false;
    if (!nowDown) { r.prev = false; r.heldMs = 0; return false; }
    if (!r.prev) { r.prev = true; r.heldMs = t; r.lastMs = t; return true; }
    if (r.heldMs == 0) return false;                      // hold began on another screen
    if (t - r.heldMs < MENU_REPEAT_DELAY_MS) return false;
    if (t - r.lastMs < MENU_REPEAT_RATE_MS) return false;
    r.lastMs = t; fire = true;
    return fire;
}

unsigned long repeatHeldFor(const RepeatKey &r) { return (r.prev && r.heldMs) ? millis() - r.heldMs : 0; }

// ---------------------------------------------------------------------------
// Menu items
// ---------------------------------------------------------------------------
static MenuItem blankItem(const char *name, ItemType t) {
    MenuItem it;
    memset(&it, 0, sizeof(it));
    it.name = name; it.type = t; it.modOf = ModTarget::OFF;
    return it;
}
MenuItem itemFloat(const char *name, float *p, float lo, float hi, float step, const char *fmt, ModTarget modOf) {
    MenuItem it = blankItem(name, IT_FLOAT); it.ptr = p; it.lo = lo; it.hi = hi; it.step = step; it.fmt = fmt; it.modOf = modOf; return it;
}
MenuItem itemInt(const char *name, int *p, int lo, int hi) {
    MenuItem it = blankItem(name, IT_INT); it.ptr = p; it.lo = (float)lo; it.hi = (float)hi; it.step = 1.f; return it;
}
MenuItem itemEnum(const char *name, uint8_t *p, const char *const *names, int count, int minIndex,
                  int skipValue, int skipEnd) {
    MenuItem it = blankItem(name, IT_ENUM); it.ptr = p; it.names = names; it.nameCount = count;
    it.lo = (float)minIndex; it.skipValue = skipValue; it.skipEnd = (skipEnd < 0) ? skipValue : skipEnd;
    return it;
}
MenuItem itemCc(const char *name, int *p) {
    MenuItem it = blankItem(name, IT_CC); it.ptr = p; it.lo = -1.f; it.hi = 127.f; it.step = 1.f; return it;
}
MenuItem itemBool(const char *name, bool *p) { MenuItem it = blankItem(name, IT_BOOL); it.ptr = p; return it; }
MenuItem itemAction(const char *name, void (*action)()) { MenuItem it = blankItem(name, IT_ACTION); it.action = action; return it; }
MenuItem itemCustom(const char *name, const char *(*label)(), void (*inc)(), void (*dec)()) {
    MenuItem it = blankItem(name, IT_CUSTOM); it.label = label; it.inc = inc; it.dec = dec; return it;
}

static void itemStep(MenuItem &it, int dir, bool fast) {
    switch (it.type) {
        case IT_FLOAT: {
            float *p = (float *)it.ptr;
            float st = it.step * (fast ? 10.f : 1.f);
            *p = clampf(*p + st * dir, it.lo, it.hi);
            // keep on the step grid so 0.01 increments do not drift
            float q = (*p - it.lo) / it.step;
            q = (float)(long)(q + (q >= 0 ? 0.5f : -0.5f));
            *p = clampf(it.lo + q * it.step, it.lo, it.hi);
            break;
        }
        case IT_INT:
        case IT_CC: {
            int *p = (int *)it.ptr;
            int st = fast ? 5 : 1;
            *p = clampi(*p + st * dir, (int)it.lo, (int)it.hi);
            break;
        }
        case IT_ENUM: {
            uint8_t *p = (uint8_t *)it.ptr;
            int v = (int)*p + dir;
            int lo = (int)it.lo;
            // A list can carry a boundary marker, or a whole run of values,
            // that is not a choice here (the LFO skips the control targets).
            for (int guard = 0; guard < it.nameCount + 2; guard++) {
                if (v < lo) v = it.nameCount - 1;
                if (v >= it.nameCount) v = lo;
                if (it.skipValue == 0 || v < it.skipValue || v > it.skipEnd) break;
                v += (dir >= 0) ? 1 : -1;
            }
            *p = (uint8_t)v;
            break;
        }
        case IT_BOOL: { bool *p = (bool *)it.ptr; *p = !*p; break; }
        case IT_CUSTOM: if (dir > 0) { if (it.inc) it.inc(); } else { if (it.dec) it.dec(); } break;
        case IT_ACTION: default: break;
    }
}

void itemIncrement(MenuItem &it, bool fast) { itemStep(it, +1, fast); }
void itemDecrement(MenuItem &it, bool fast) { itemStep(it, -1, fast); }

void itemEnter(MenuItem &it) {
    switch (it.type) {
        case IT_ACTION: if (it.action) it.action(); break;
        case IT_BOOL:   itemStep(it, +1, false); break;
        case IT_ENUM:   itemStep(it, +1, false); break;
        default: break;
    }
}

const char *itemLabel(const MenuItem &it) {
    static char buf[20];
    switch (it.type) {
        case IT_FLOAT:
            // A float row may draw its own text and still step like a float,
            // hold-to-x10 included (v0.71: the fixed-frequency row)
            if (it.label) return it.label();
            snprintf(buf, sizeof(buf), it.fmt ? it.fmt : "%.0f", *(const float *)it.ptr); return buf;
        case IT_INT:   if (it.label) return it.label();     // shown differently from what is stored (v0.90: Swing)
                       snprintf(buf, sizeof(buf), "%d", *(const int *)it.ptr); return buf;
        case IT_CC: {
            int v = *(const int *)it.ptr;
            if (v < 0) return "Off";
            snprintf(buf, sizeof(buf), "%d", v); return buf;
        }
        case IT_ENUM: {
            int v = *(const uint8_t *)it.ptr;
            if (v < 0 || v >= it.nameCount) return "?";
            return it.names[v];
        }
        case IT_BOOL:   return *(const bool *)it.ptr ? "On" : "Off";
        case IT_ACTION: return "";
        case IT_CUSTOM: return it.label ? it.label() : "";
        default: return "";
    }
}

bool listHandleKeys(ListNav &nav, MenuItem *items, int count, const KeyFrame &k) {
    bool changed = false;
    if (count <= 0) return false;
    bool up   = keyDown(k, ';') || keyDown(k, ':');
    bool down = keyDown(k, '.') || keyDown(k, '>');
    bool dec  = keyDown(k, ',') || keyDown(k, '<');
    bool inc  = keyDown(k, '/') || keyDown(k, '?');
    if (repeatFire(nav.up, up))     { nav.sel = (nav.sel + count - 1) % count; changed = true; }
    if (repeatFire(nav.down, down)) { nav.sel = (nav.sel + 1) % count; changed = true; }
    nav.sel = clampi(nav.sel, 0, count - 1);
    // Acceleration: after 1.5s of holding a value key, steps get 10x
    if (repeatFire(nav.dec, dec)) { itemDecrement(items[nav.sel], repeatHeldFor(nav.dec) > 1500); changed = true; }
    if (repeatFire(nav.inc, inc)) { itemIncrement(items[nav.sel], repeatHeldFor(nav.inc) > 1500); changed = true; }
    if (k.enterPressed && !k.shift) { itemEnter(items[nav.sel]); changed = true; }
    return changed;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void drawTabBar(AppMode cur) {
    static const char *labels[NUM_TABS] = { "PLAY", "OP", "ALGO", "ARP", "LFO", "FX", "SET" };
    canvas.fillRect(0, 0, SCREEN_W, 11, BLACK);
    int ci = (int)cur; if (ci >= NUM_TABS) ci = (int)AppMode::SETTING;
    for (int i = 0; i < NUM_TABS; i++) {
        int x = i * TAB_W;
        bool act = (i == ci);
        canvas.drawRect(x, 0, TAB_W, 11, uiColor);
        if (act) { canvas.fillRect(x + 1, 1, TAB_W - 2, 9, WHITE); canvas.setTextColor(BLACK, WHITE); }
        else     canvas.setTextColor(uiColor, BLACK);
        int lx = x + (TAB_W - (int)strlen(labels[i]) * 6) / 2;
        canvas.setCursor(lx, 2); canvas.print(labels[i]);
    }
    canvas.setTextColor(uiColor, BLACK);
}

const char *modMarker(ModTarget t) {
    if (t == ModTarget::OFF) return "";
    bool imu = (sys.imuXEnabled && patch.imuXTarget == t && patch.imuXDepth > 0.f) ||
               (sys.imuYEnabled && patch.imuYTarget == t && patch.imuYDepth > 0.f);
    bool lfo = (patch.lfoTarget == t && patch.lfoDepth > 0.f);
    // ^ = the theremin in Mod (v0.81), a hand held over the sensor
    bool th = sys.thOn && sys.thMode == ThMode::MOD && sys.thTarget == t && sys.thDepth > 0.f;
    static char b[5];
    int n = 0;
    if (imu || lfo || th) b[n++] = ' ';
    if (imu) b[n++] = '*';
    if (lfo) b[n++] = '~';
    if (th)  b[n++] = '^';
    b[n] = 0;
    return b;
}

void drawItemList(MenuItem *items, int count, int sel, int startY, int splitCol, int rowH) {
    const int LX = 5, RX = 123;
    bool twoCol = (splitCol > 0 && splitCol < count);
    canvas.setTextColor(uiColor, BLACK);
    if (twoCol) {
        int rows = splitCol > (count - splitCol) ? splitCol : (count - splitCol);
        canvas.drawFastVLine(119, startY - 2, rows * rowH + 4, uiDim);
    }
    for (int i = 0; i < count; i++) {
        int x, y;
        if (twoCol) { bool left = i < splitCol; x = left ? LX : RX; y = startY + (left ? i : i - splitCol) * rowH; }
        else        { x = LX; y = startY + i * rowH; }
        canvas.setCursor(x, y);
        if (items[i].type == IT_ACTION) canvas.printf("%c%s", (i == sel) ? '>' : ' ', items[i].name);
        else canvas.printf("%c%-8s%s%s", (i == sel) ? '>' : ' ', items[i].name, itemLabel(items[i]), modMarker(items[i].modOf));
    }
}

void drawNavHint(const char *text) {
    canvas.fillRect(0, 125, SCREEN_W, 10, BLACK);
    canvas.setTextColor(uiColor, BLACK);
    canvas.setCursor((SCREEN_W - (int)strlen(text) * 6) / 2, 126);
    canvas.print(text);
}

void drawBar(int x, int y, int w, int h, float frac, uint16_t color) {
    canvas.drawRect(x, y, w, h, color);
    int fw = (int)((w - 2) * clampf(frac, 0.f, 1.f) + 0.5f);
    if (fw > 0) canvas.fillRect(x + 1, y + 1, fw, h - 2, color);
}

void drawCenteredText(int y, const char *text, uint16_t color) {
    canvas.setTextColor(color, BLACK);
    canvas.setCursor((SCREEN_W - (int)strlen(text) * 6) / 2, y);
    canvas.print(text);
}

// Help overlay (Shift+H toggles). The clear always covers the largest box
// so switching to a shorter one never leaves the old frame behind.
void drawHelpOverlay() {
    constexpr int HELP_BOX_MAX_H = 112;
    canvas.fillRect(2, 13, 236, HELP_BOX_MAX_H, BLACK);
    canvas.setTextColor(uiColor, BLACK);
    const char *lines[11]; int n = 0;
    int boxH = 112;
    switch (appMode) {
        case AppMode::OP:
            lines[n++] = "=== OP HELP (Sh+H) ===";
            lines[n++] = "1-4: select operator";
            lines[n++] = ";/. row   ,// value";
            lines[n++] = "hold ,// 1.5s: x10 steps";
            lines[n++] = "Level 0 = operator off";
            lines[n++] = "f: fixed frequency on/off";
            lines[n++] = "A D S R rows: envelope graph";
            lines[n++] = " (dot = the note playing)";
            lines[n++] = "K/L: volume  D: hold notes";
            lines[n++] = "Tab/Sh+Tab: tabs  G0: PLAY";
            break;
        case AppMode::ALGO:              // v0.85 (showed the PLAY help before)
            lines[n++] = "=== ALGO HELP (Sh+H) ===";
            lines[n++] = "Algo 1-14: how the 4 OPs link";
            lines[n++] = "Filled box = carrier (heard)";
            lines[n++] = "FB: the OP that feeds back";
            lines[n++] = "PEnv rows: pitch env graph";
            lines[n++] = " bright v127  dim v40  dot=note";
            lines[n++] = "Voices 1 = mono (GlMode Legato)";
            lines[n++] = "Glide: pitch slide in ms - not";
            lines[n++] = " the morph Time (SET>Patch>Morph)";
            lines[n++] = "Tab/Sh+Tab: tabs  G0: PLAY";
            break;
        case AppMode::ARP:
            lines[n++] = "=== ARP HELP (Sh+H) ===";
            lines[n++] = "Sh+V:Arp on/off  V:Latch";
            lines[n++] = "LatchMd Add: keys add/remove";
            lines[n++] = "LatchMd Chord: held keys=chord";
            lines[n++] = "Sh+Enter: TAP TEMPO (x2-4)";
            lines[n++] = "ClkSync On: follow MIDI clock";
            lines[n++] = "Pattern STEP: Enter=focus grid";
            lines[n++] = " ,//:step ;/.:field 1-8:note";
            lines[n++] = " 0:off  Sh+,//(< >):value";
            lines[n++] = "Gate>100%: notes overlap";
            lines[n++] = "SET>Pattern>Random: new pattern";
            break;
        case AppMode::LFO:
            lines[n++] = "=== LFO HELP (Sh+H) ===";
            lines[n++] = "LFO: one for all voices";
            lines[n++] = "Delay: fade-in after note-on";
            lines[n++] = "Bipolar: 0 = patch value, +/-";
            lines[n++] = "Op* targets act on the OP the";
            lines[n++] = " OP tab has selected (keys 1-4)";
            lines[n++] = "Right: shape + live phase dot";
            lines[n++] = "FX targets act only if Mix>0";
            lines[n++] = "PtchMrph: set Pos ~50% to swing";
            lines[n++] = "* IMU  ~ LFO  ^ Theremin (Mod)";
            lines[n++] = "IMU settings: SETTING > IMU";
            break;
        case AppMode::FX:
            lines[n++] = "=== FX HELP (Sh+H) ===";
            lines[n++] = ",//: pad  Enter: on/off";
            lines[n++] = ".: edit params";
            lines[n++] = "Tab in params: back to pads";
            lines[n++] = "Mix 0 = effect off";
            lines[n++] = "q w e r t: flip C D R X V";   // V = Drive since v0.72
            lines[n++] = " On:BYP Off:TMP (not saved)";
            lines[n++] = "K/L: volume";
            break;
        case AppMode::SETTING:
            lines[n++] = "=== SET HELP (Sh+H) ===";
            lines[n++] = "Enter: open category  Tab: back";
            lines[n++] = "Save asks before overwriting";
            lines[n++] = "Browser/Name: Enter=ok Tab=back";
            lines[n++] = "Theremin ^=Mod; Grove: no DIN";
            lines[n++] = "PC Mode: Patch=load Morph=Sh+1-8";
            lines[n++] = " (same algorithm = smooth glide)";
            lines[n++] = "Sw Chain+/-: between songs (0.2s)";
            lines[n++] = "Info: Enter resets peak/over";
            lines[n++] = "MIDI: Enter on CC = Learn (move)";
#ifdef MORPHASE_BLE
            lines[n++] = "MIDI>BLE: On/Off, then restart";
#endif
            break;
        case AppMode::PLAY:
        default:
            lines[n++] = "=== HELP (Sh+H) ===";
            lines[n++] = ";/.: octave  ,//: transpose";
            lines[n++] = "Tab/Sh+Tab: tabs  G0: PLAY/EDIT";
            lines[n++] = "K/L: volume -/+ (hold=repeat)";
            lines[n++] = "D: hold notes (toggle)";
            lines[n++] = "Z/X bend  A/S IMU hold  Sh+A/S on";
            lines[n++] = "Sh+V Arp  V Latch  Sh+Ent tap";
            lines[n++] = "g (hold): GLITCH";
            lines[n++] = "q w e r t: flip FX C D R X V";
            lines[n++] = "Sh+1-8: glide to morph slot";
#ifdef MORPHASE_BLE
            lines[n++] = "b/B by the name: BLE wait/linked";
#endif
            break;
    }
    boxH = 8 + n * 10 + 4;
    canvas.drawRect(2, 13, 236, boxH, uiColor);
    for (int i = 0; i < n; i++) { canvas.setCursor(6, 17 + i * 10); canvas.print(lines[i]); }
}
