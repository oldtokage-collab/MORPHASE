// MORPHASE - fx.h
// One FX chain on the mix bus: Crush -> Chorus -> Delay -> Reverb -> Limiter.
// v0.56: each effect can be bypassed from the keyboard (q w e r t) or a MIDI
// switch without touching the patch. Bypass is applied here, after morph,
// modulation and everything else, and every change is ramped over one
// buffer so it never clicks.
// All processing is float on the mix buffer; Delay and Reverb store their
// lines as int16 (spec 6.4). Everything here runs inside audioTask.

#ifndef MORPHASE_FX_H
#define MORPHASE_FX_H

#include "config.h"

// Effective parameters for one buffer, resolved by the voice manager from
// the patch (Mix == 0 means the effect is off and costs nothing).
struct FxParams {
    float chorusRateHz;
    float chorusDepthMs;
    float chorusMix;      // 0..1
    float delayTimeMs;
    float delayFeedback;  // 0..0.9
    float delayMix;       // 0..1
    float reverbSize;     // 0..1
    float reverbDamp;     // 0..1
    float reverbMix;      // 0..1
    float limiterDrive;   // 1..5
    float limiterMix;     // 0..1
    // Bitcrush (v0.55). The transcendental part is resolved here, per buffer,
    // so the sample loop only adds, multiplies and compares.
    float crushMix;       // 0..1 (0 = off)
    float crushScale;     // 2^(bits-1): quantization steps per half range
    float crushInvScale;
    float crushStep;      // 1/down: sample-and-hold phase increment
    uint8_t bypass;       // effective bypass mask for this buffer (bit = FxPad)
};

// Bypass state (v0.56). loop() writes these single bytes; audioTask reads them
// once per buffer. Never saved: a patch load clears the mask.
extern volatile uint8_t fxBypassMask;        // q w e r t keys and Toggle switches
extern volatile uint8_t fxBypassMomentary;   // Momentary switches while held: flips those bits
inline uint8_t fxBypassEffective() { return fxBypassMask ^ fxBypassMomentary; }

// Temporary On (v0.57): the same keys on a pad that is Off in the patch turn
// it on at its default Mix, for the performance only. Never saved; a patch
// load clears it like the bypass. Together the two make every key a plain
// "flip this effect" switch.
extern volatile uint8_t fxForceMask;
extern volatile uint8_t fxForceMomentary;
inline uint8_t fxForceEffective() { return fxForceMask ^ fxForceMomentary; }
// Gives the pads that are Off in this buffer's parameters but forced on their
// default Mix. The other parameters (delay time, reverb size ...) are the
// patch's. Applied after modulation, so a forced pad is not modulated.
void fxApplyForce(FxParams &f, uint8_t force);

// Processes n samples in place (buf holds roughly -1..1 values).
void fxProcessBlock(float *buf, int n, const FxParams &p);

// Bitcrush alone (first stage, v0.55). Runs from IRAM.
void fxCrushBlock(float *buf, int n, const FxParams &p);

// Drive alone (last stage before Volume). The x/(1+|x|) curve: a sound,
// switched with the Drive pad / t (v0.72: named Limiter before).
void fxLimiterBlock(float *buf, int n, const FxParams &p);

// Output stage (v0.77, one pass): volume (volGain = int16 units per mix unit)
// -> output limiter (v0.72: gain only, 32-sample look-ahead, always on) ->
// int16 clamp into out. Returns the output peak. In IRAM; mix is scratch.
int fxOutputStage(int16_t *out, float *mix, int n, float volGain);
void fxClearOutputLimiter();         // engine stop (from clearFxBuffers)
extern volatile float outLimPeak1s;      // Info: largest |sample| in, last second (int16 units)
extern volatile float outLimMinGain1s;   // Info: lowest gain applied, last second

// Zeroes every delay line (spec 6.5 / 9.4).
void clearFxBuffers();

// True when a time-based effect is on and a tail may need to ring out.
bool fxHasTail(const FxParams &p);

// Copies the patch's FX section into an FxParams (used once per buffer).
void fxParamsFromPatch(FxParams &out);
void clearReverbBuffers();          // Mix rising from 0 (v0.5)
// Delay and Chorus line scale (v0.78): int16 full scale = 16x the mix bus's 1.0,
// the headroom for the most voices at once (see fx.cpp). The GLITCH tape is the
// Delay line, so glitch.cpp reads it with the same scale.
constexpr float LINE_I16_GAIN = 32768.f / 16.f;

// The Delay line as the GLITCH tape (v0.6): read-only. writeIdx is where the
// next sample will go, i.e. just after the newest one. Call once per buffer.
void fxTapeInfo(const int16_t **tape, int *writeIdx);

#endif // MORPHASE_FX_H
