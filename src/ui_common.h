// MORPHASE - ui_common.h
// Canvas, theme, key input, tab management, menu-list machinery and the
// help overlay. All drawing goes to the off-screen canvas and is pushed
// with pushSprite(); nothing draws to the panel directly (spec 8.5).

#ifndef MORPHASE_UI_COMMON_H
#define MORPHASE_UI_COMMON_H

#include "config.h"
#include <M5Cardputer.h>

// ---------------------------------------------------------------------------
// Canvas / theme
// ---------------------------------------------------------------------------
extern M5Canvas canvas;
extern uint16_t uiColor;       // accent colour of the current theme
extern uint16_t uiDim;         // darker version for guides / dividers - not for text
extern uint16_t uiMuted;       // v0.85: about 55% - soft text, a second (dim) line in a graph
extern uint16_t uiHighlight;   // playhead / attention colour

void uiInitCanvas();
void applyUiTheme();           // recomputes uiColor & co from sys.theme
void applyUiBrightness();

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------
extern AppMode appMode;
extern AppMode lastEditMode;   // G0 toggles PLAY <-> this
extern bool    helpVisible;
extern bool    uiDirty;        // a redraw is wanted

void uiSetMode(AppMode m);
AppMode uiNextTab(AppMode m, bool backwards);

// ---------------------------------------------------------------------------
// Keys - one snapshot per loop() pass
// ---------------------------------------------------------------------------
struct KeyFrame {
    bool changed;                 // Keyboard.isChange() this pass
    bool shift, tab, enter, del, fn;
    char word[12]; int wordLen;   // characters currently reported
    char pressed[12]; int pressedLen;   // characters that were NOT down last pass
    bool tabPressed, enterPressed, delPressed;
};
void uiPollKeys(KeyFrame &k);
bool keyDown(const KeyFrame &k, char c);       // held (either case is NOT folded)
bool keyPressed(const KeyFrame &k, char c);    // edge

// Hold-to-repeat helper (spec 8.2): true on the press and then on every
// repeat tick while held.
struct RepeatKey { bool prev; unsigned long heldMs; unsigned long lastMs; };
bool repeatFire(RepeatKey &r, bool nowDown);
unsigned long repeatHeldFor(const RepeatKey &r);   // ms held, 0 when up

// ---------------------------------------------------------------------------
// Menu items
// ---------------------------------------------------------------------------
enum ItemType : uint8_t { IT_FLOAT = 0, IT_INT, IT_ENUM, IT_BOOL, IT_ACTION, IT_CUSTOM, IT_CC };

struct MenuItem {
    const char *name;             // <= 8 chars (spec 8.6)
    ItemType    type;
    void       *ptr;              // float* / int* / uint8_t* (enum) / bool*
    float       lo, hi, step;
    const char *fmt;              // IT_FLOAT printf format
    const char *const *names;     // IT_ENUM
    int         nameCount;
    void      (*action)();        // IT_ACTION: Enter
    const char *(*label)();       // IT_CUSTOM; on IT_FLOAT / IT_INT, replaces the value text (v0.71, v0.90)
    void      (*inc)();           // IT_CUSTOM
    void      (*dec)();           // IT_CUSTOM
    ModTarget   modOf;            // which modulation target this row IS (markers)
    int         skipValue;        // IT_ENUM: first value to step over (0 = none)
    int         skipEnd;          // IT_ENUM: last value of that run (== skipValue for one)
};

MenuItem itemFloat(const char *name, float *p, float lo, float hi, float step, const char *fmt, ModTarget modOf = ModTarget::OFF);
MenuItem itemInt(const char *name, int *p, int lo, int hi);
MenuItem itemEnum(const char *name, uint8_t *p, const char *const *names, int count, int minIndex = 0,
                  int skipValue = 0, int skipEnd = -1);   // skipEnd -1: skip skipValue alone
MenuItem itemBool(const char *name, bool *p);
MenuItem itemAction(const char *name, void (*action)());
MenuItem itemCustom(const char *name, const char *(*label)(), void (*inc)(), void (*dec)());
// A MIDI CC number: -1 shows as "Off", and Enter starts MIDI Learn (the
// screen handles that, since it owns the field being learned).
MenuItem itemCc(const char *name, int *p);

// Generic value handling
void        itemIncrement(MenuItem &it, bool fast);
void        itemDecrement(MenuItem &it, bool fast);
const char *itemLabel(const MenuItem &it);      // static buffer
void        itemEnter(MenuItem &it);            // Enter: toggle / action / cycle

// Navigation state for one list screen
struct ListNav {
    int sel;
    RepeatKey up, down, inc, dec;
};
// Applies ; . , / Enter to a list; returns true when something changed.
bool listHandleKeys(ListNav &nav, MenuItem *items, int count, const KeyFrame &k);

// Drawing helpers
void drawTabBar(AppMode cur);
void drawItemList(MenuItem *items, int count, int sel, int startY, int splitCol, int rowH);
void drawNavHint(const char *text);
void drawHelpOverlay();
void drawBar(int x, int y, int w, int h, float frac, uint16_t color);
void drawCenteredText(int y, const char *text, uint16_t color);

// Modulation markers: '*' IMU, '~' LFO, for rows whose target matches
const char *modMarker(ModTarget t);

#endif // MORPHASE_UI_COMMON_H
