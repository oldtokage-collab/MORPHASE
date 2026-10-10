// MORPHASE - glitch.h
// GLITCH (v0.6, update spec 4). Unpredictable breaks played on purpose.
// Two kinds of event, one at a time:
//   Buffer - Stutter / Reverse a fragment of what was just heard (read from
//            the Delay line, which records continuously - no new buffer),
//            or Gate what is sounding now
//   Chaos  - throw FM parameters (Ratio, Morph, Detune, Feedback, Algorithm)
//            for a moment, on the engine's per-buffer copy of the patch only
// Events are decided on a tempo grid (ARP tempo / MIDI clock, own phase).
// Device settings (SETTING > Glitch), never part of a patch.

#ifndef MORPHASE_GLITCH_H
#define MORPHASE_GLITCH_H

#include "config.h"

// ---- loop() side: single values only, read by audioTask once per buffer ----
void glitchSetBpm(float bpm);
void glitchSetKeyHeld(bool held);        // the g key
void glitchSetSwitchHeld(bool held);     // MIDI switch Glitch, Momentary
void glitchResync();                     // MIDI Start
bool glitchHeld();                       // g key or switch
bool glitchEventActive();                // for the PLAY screen
float glitchEffectiveProb();             // 0..1, Prob / Hold% + GlProb (the ARP glitch, v0.82)

// ---- audioTask side ----------------------------------------------------------
void glitchInit();
// Once per buffer, after the engine copy of the patch is made and before
// note events: advances the step grid, decides events and applies a running
// Chaos event to `engine`. modProb is GlitchProb modulation, 0..1.
void glitchBeginBuffer(int n, bool voicesActive, float modProb, Patch &engine);
// After Reverb, before the Limiter: renders a running Buffer event in place.
void glitchRenderBuffer(float *buf, int n);

#endif // MORPHASE_GLITCH_H
