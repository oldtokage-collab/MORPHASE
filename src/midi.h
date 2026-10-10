// MORPHASE - midi.h
// Serial MIDI (M5Stack Unit MIDI U187 on the Grove port, DIP = BYPASS).
// Receive: notes, bend, CC, program change, clock. Send: ARP notes only.

#ifndef MORPHASE_MIDI_H
#define MORPHASE_MIDI_H

#include "config.h"

void midiBegin();
void midiPoll();                       // drain the UART, called from loop()
void midiDinSuspend();                 // Theremin on Grove (v0.81): give GPIO1/2 away
void midiDinResume();
bool midiDinActive();

// Transmit (ARP note out, spec 7.3)
void midiSendNoteOn(uint8_t note, uint8_t vel);
void midiSendNoteOff(uint8_t note);

// Clock sync state (spec 5.5)
bool  midiClockLocked();               // ticks are arriving
float midiClockBpm();                  // EMA-smoothed tempo

// Indicators
bool  midiRxActive();                  // something arrived in the last 300ms
uint32_t midiRxByteCount();

// Program Change: -1 when nothing is pending; loop() consumes it.
int  midiTakeProgramRequest();

// ---------------------------------------------------------------------------
// MIDI Learn (v0.3). Nobody knows which CC number their knob sends, so the
// setting is made by moving the control rather than by typing a number.
// A CC is taken only once it actually moves, not on the first message (v0.33).
// While learning, incoming CC is captured and NOT passed on to the sound.
// ---------------------------------------------------------------------------
void midiLearnBegin();
void midiLearnCancel();
bool midiLearnActive();
int  midiLearnTakeResult();     // CC number once captured, else -1 (consumes it)

#endif // MORPHASE_MIDI_H
