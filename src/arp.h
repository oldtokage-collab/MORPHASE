// MORPHASE - arp.h
// Arpeggiator (including the STEP pattern). Runs in loop() on millisecond
// timing and posts NoteEvents to the voice engine.

#ifndef MORPHASE_ARP_H
#define MORPHASE_ARP_H

#include "config.h"

void arpInitDefaults();          // fills `arp` with spec defaults
void arpPatternDefaults();       // 16 steps to their defaults, length 16
void arpRandomizePattern();      // musically-constrained random step pattern

// Note input (only called when arp.on)
void arpNoteOn(uint8_t note, uint8_t vel);
void arpNoteOff(uint8_t note);
void arpSustain(bool down);      // pedal holds the chord while down
void arpPanic();                 // all pending notes off, chord cleared

// Chord rebuild - THE single entry point (spec 5.6). Every path that
// changes the chord calls this directly; no dirty flags.
void rebuildArpChord();

void arpSetOn(bool on);
void arpToggleLatch();
void arpTapTempo();
void arpResync();                // MIDI Start/Continue: restart at step 0
void arpUpdate();                // call every loop() pass

// Display helpers
int   arpChordSize();
int   arpCurrentStep();          // step index being played (STEP), or sequence index
bool  arpIsRunning();
float arpEffectiveBpm();         // clock or local, whichever is in use
int   arpLastNote();             // last note fired (-1 none)
int   arpChordNote(int i);       // i-th note of the chord the ARP works from, as heard
int   arpLastChordIndex();       // chord member fired last (-1 none) - lights the PLAY row

#endif // MORPHASE_ARP_H
