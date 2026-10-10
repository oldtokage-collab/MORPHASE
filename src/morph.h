// MORPHASE - morph.h
// Patch morph (v0.5). Up to eight patches, named on the card, in slots 1-8.
// The engine never reads `patch` for the sound: audioTask rebuilds
// `livePatch` at the top of every buffer, and modulation offsets are then
// built on top of that copy from zero, exactly as before. Nothing here ever
// writes into `patch` except an explicit commit or a finished glide.
//
// Two ways to play the slots, usable side by side (v0.51):
//   Chain - one position 0..1 runs along slots 1..N and is moved by hand
//           (Pos), a MIDI knob, a tilt or the LFO (target PtchMrph).
//   Glide - Shift+1..8 moves from the sound heard now straight to that
//           slot's patch over Morph Time, as in C.P.S., and then that patch
//           is loaded. No slot in between is passed through.

#ifndef MORPHASE_MORPH_H
#define MORPHASE_MORPH_H

#include "config.h"

constexpr int MORPH_SLOTS = 8;
constexpr int MORPH_TIME_MAX_MS = 10000;

// ---- loop() side -----------------------------------------------------------
void morphInit();                            // after SD + settings: read the chain, load its patches
void morphPoll();                            // every loop(): finishes a glide that has arrived
const char *morphSlotName(int i);            // "" when the slot is Off
void morphSetSlot(int i, const char *name);  // "" = Off. Saves the chain file and reloads it
void morphSetChain(const char *const *names, int n);   // v0.76: all slots at once (the rest Off)
int  morphChainLength();                     // leading run of loaded slots
int  morphSlotAlgorithm(int i);              // 0 when the slot is not loaded
bool morphChainRunning();                    // chain on, not paused, loaded and at least 2 slots
bool morphChainPaused();                     // chain on but paused by a patch change (v0.52)
bool morphGliding();                         // a Shift+number glide is under way
bool morphRunning();                         // either of the two: livePatch differs from `patch`
void morphSetEnabled(bool on);               // the chain
void morphGlideTo(int slot);                 // Shift+1..8
void morphProgramChange(int pc);             // PC Mode Morph: PC 0-7 -> morphGlideTo, others ignored with a notice

// Edits and saves start from what is heard: livePatch becomes `patch`,
// morph stops, and the UI is told. The name becomes "Morph" so a plain Save
// can never overwrite a chain patch with the in-between sound.
void morphCommitAndStop();
// A patch is about to replace `patch` (Load, Program Change, Pch+/-,
// Random, Reset): morph stops first, and the UI is told.
// "Stops" never switches the Chain setting off (v0.52): the chain is
// PAUSED, and resumes as soon as its position is moved by hand - Pos, a
// PtchMrph knob or a tilt. The LFO does not resume it: it moves all the
// time and would undo the pause at once.
void morphStopForPatchChange();
const char *morphTakeNotice();               // a short message for the UI, once; nullptr if none

// The card changed under the chain (update spec 5.3)
void morphOnPatchSaved(const char *name);
void morphOnPatchDeleted(const char *name);
void morphOnPatchRenamed(const char *oldName, const char *newName);
void morphOnFactoryRewritten();      // v0.75: factory files rewritten - chain slots re-read

// ---- audioTask side ----------------------------------------------------------
// Builds `live` for this buffer: a glide if one runs, else the chain at
// position `pos` (0..1), else a copy of `patch`.
void morphBuildLivePatch(Patch &live, float pos);

#endif // MORPHASE_MORPH_H
