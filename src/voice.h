// MORPHASE - voice.h
// Voice pool, allocation, sustain, glide, modulation resolution and the
// audioTask itself. audioTask OWNS every voice; loop() only ever posts
// NoteEvents and writes parameter structs (spec 11.1).

#ifndef MORPHASE_VOICE_H
#define MORPHASE_VOICE_H

#include "config.h"

// Event queue: called from loop() only.
bool postNoteEvent(uint8_t type, uint8_t note, uint8_t vel, uint8_t flags);
inline bool postNoteOn(uint8_t note, uint8_t vel, uint8_t flags = EVF_NONE)  { return postNoteEvent(EV_NOTE_ON, note, vel, flags); }
inline bool postNoteOff(uint8_t note, uint8_t flags = EVF_NONE)              { return postNoteEvent(EV_NOTE_OFF, note, 0, flags); }
inline bool postSustain(bool down)                                           { return postNoteEvent(down ? EV_SUSTAIN_ON : EV_SUSTAIN_OFF, 0, 0, EVF_NONE); }
inline bool postAllNotesOff()                                                { return postNoteEvent(EV_ALL_NOTES_OFF, 0, 0, EVF_NONE); }
inline bool postAllSoundOff()                                                { return postNoteEvent(EV_ALL_SOUND_OFF, 0, 0, EVF_NONE); }

// Sustain has two sources - the pedal (CC#64) and the D key hold toggle
// (v0.11). Either one holds; both must be off to release. Routes to the
// arpeggiator's chord hold when the ARP is on. loop() only.
enum SustainSource { SUS_PEDAL = 0, SUS_KEY = 1 };
void sustainInput(int source, bool down);
bool sustainKeyHeld();          // the D toggle state, for the display

// Set up defaults for live control values (call before the task starts).
void voiceEngineInit();

// Asks audioTask to zero the FX lines at the next buffer boundary (patch
// reset / load). The lines belong to audioTask, so loop() never clears
// them directly.
void requestFxClear();

// The audio task entry point (created pinned to AUDIO_TASK_CORE).
void audioTask(void *pvParameters);

// Cumulative DSP diagnostics (spec A). audioTask writes, the UI reads.
extern volatile uint32_t dspOverBudgetCount;   // buffers that ran past the budget
extern volatile uint32_t dspMaxUs;             // worst buffer time seen
void dspResetCounters();                       // Info screen, Enter

// Output scope. audioTask captures one window of the signal that actually
// left the mix bus, but ONLY once the UI has taken the previous one - so
// on any screen that does not draw it, this costs a single bool test per
// buffer. The window starts at a rising zero crossing so the picture
// stands still instead of sliding sideways.
bool scopeTake(int16_t *dst);   // copies SCOPE_POINTS samples; false if none fresh

// Read-only view for the display: which notes are sounding (0 = none).
int  voiceNoteAt(int index);      // MIDI note of voice index, or -1
bool voiceActiveAt(int index);

#endif // MORPHASE_VOICE_H
