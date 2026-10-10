// MORPHASE - ui_screens.h
// The seven tabs plus the SETTING sub-screens.

#ifndef MORPHASE_UI_SCREENS_H
#define MORPHASE_UI_SCREENS_H

#include "ui_common.h"

void uiScreensInit();                          // builds the item tables
void uiScreenHandleKeys(const KeyFrame &k);    // per-screen key handling (after global keys)
void uiDrawScreen();                           // full redraw of the current screen into the canvas
void uiOnModeEntered(AppMode m);               // reset per-screen sub-state (FX pad view etc.)
bool uiScreenWantsPeriodicRedraw();            // PLAY / ARP animate
bool uiScreenConsumesTab();                    // FX param page and sub-screens use Tab themselves

// Program Change from MIDI (loop() calls this with the requested index)
void uiLoadPatchByIndex(int index);

// Called from the MIDI layer. These live here because the actions they run
// are the same ones the keys and the menus run - the point is that a switch
// on a controller and a key on the Cardputer go through ONE implementation,
// not two that can drift apart (spec B.8).
void applySwitchAction(SwitchAction a, bool high, bool rising, bool momentary);
void uiFxBypassKey(int pad);       // q w e r t (v0.56): toggle one effect's bypass, with a message
void selectPatternSlot(int slot);   // rising edge only: this touches the card

#ifdef MORPHASE_SCREENSHOT
// Manual-making builds only (env morphase_shot / morphase_ble_shot, never
// released): Shift+P saves the screen 5 seconds later as
// /MORPHASE/Shots/shot_NNN.bmp - time to hold a chord, g or a tilt first.
void uiScreenshotArm();
void uiScreenshotPoll();         // after each redraw: takes the shot when due
#endif

#endif // MORPHASE_UI_SCREENS_H
