// MORPHASE - theremin.h
// Theremin (v0.81, instruction (6) section 1): a VL53L1X distance sensor
// (Unit ToF4M). Everything here runs in loop(); audioTask only reads the
// values published in `live` (thModNorm / thGate / thHz / thVol / thReserve).
//
//   Mod  - the hand's distance is one more modulation source, routed like
//          the IMU (near = 1).
//   Play - one note of the current patch on a voice of its own; the
//          distance sets the pitch (C.P.S. v0.999-v0.9993).
//
// Bus Cap   = the LoRa Cap's Grove (G8/G9): the board's own bus (Wire1,
//             keyboard and IMU), shared and never begun or ended here.
// Bus Grove = the main Grove (G1/G2): the Unit MIDI pins, so DIN MIDI is
//             stopped while the theremin is on there.

#ifndef MORPHASE_THEREMIN_H
#define MORPHASE_THEREMIN_H

#include "config.h"

void thereminInit();                 // setup(), after the settings are loaded
void thereminUpdate();               // every loop() pass
void thereminSetEnabled(bool on);    // SETTING row, MIDI switch
void thereminRescan();               // SETTING > Theremin > Rescan
void thereminSettingsChanged();      // after Bus / Mode edits

// IMU axis that sets the Play volume, while it does (0 none, 1 X, 2 Y).
// That axis's own destinations are muted meanwhile (spec 1.5).
int  thereminVolAxis();

// Display
enum ThState : uint8_t { TH_OFF = 0, TH_SEARCHING, TH_OK, TH_LOST };
ThState     thereminState();
const char *thereminStateName();
int         thereminDistanceMm();    // the last reading, 0 when none
float       thereminBarValue();      // 0..1 of the window (near = 1), -1 when no hand
const char *thereminScanLabel();     // "3 shared" / "1 found" / "--"
const char *thereminNoteName();      // Play: the note sounding, "" when none

// Scales for Pitch = Semitone (C.P.S. SCALES[], same order: append only,
// the index is saved). The root is C, moved by Transpose.
struct ThScale { const char *name; const char *shortName; const char *group; uint8_t len; int8_t iv[12]; };
extern const ThScale TH_SCALES[];
extern const int TH_SCALE_COUNT;

// Top: semitones from C4 to the top of the range, in octave steps
constexpr int TH_TOP_MIN = -24, TH_TOP_MAX = 36;
constexpr int TH_NEAR_MIN = 20, TH_NEAR_MAX = 200;
constexpr int TH_FAR_MAX = 1000, TH_FAR_GAP = 100;   // Far >= Near + 100

#endif // MORPHASE_THEREMIN_H
