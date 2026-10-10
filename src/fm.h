// MORPHASE - fm.h
// Operators, waveform tables, waveform morphing and the fourteen fixed
// algorithms. Everything here runs inside audioTask; nothing in this file
// touches loop()-owned state.

#ifndef MORPHASE_FM_H
#define MORPHASE_FM_H

#include "config.h"

// Per-operator runtime state (owned by a Voice).
struct OpState {
    float    phase;     // 0..1 cycles
    float    inc;       // phase increment per sample (refreshed every ENV_BLOCK)
    float    env;       // current envelope level 0..1
    float    envStep;   // per-sample linear step toward the next block value
    float    stagePos;  // 0..1 progress within the current stage
    float    relStart;  // env level captured when RELEASE began (spec 3.3)
    float    amp;       // effective level 0..1 (level * velocity * key scale)
    float    velKs;     // velocity * key-scale factor fixed at note-on
    float    out;       // last output sample (feedback; any OP since v0.7)
    float    outPrev;   // output before that (feedback averages two samples)
    EnvStage stage;
};

// Everything renderVoice() needs that is resolved once per buffer by the
// voice manager (spec 4.6: per-sample code only READS effective values).
struct RenderContext {
    int          algorithm;        // 1-NUM_ALGORITHMS
    float        feedback;         // 0..1 effective
    const float *table[NUM_OPS];   // morphed table per OP (WAVE_TABLE_SIZE+1 entries)
    bool         opOn[NUM_OPS];    // level > 0 -> processed at all
    float        opLevel[NUM_OPS]; // 0..1 from the patch
    float        opLevelAdd[NUM_OPS];   // OP_LEVEL modulation, added after velocity/key scaling
    // ratio * 2^(detune/1200), interpolated across the buffer so that
    // modulating ratio or detune sweeps the phase increment smoothly
    // instead of stepping it once per buffer (v0.2).
    float        ratioMulStart[NUM_OPS];
    float        ratioMulStep[NUM_OPS];
    float        attackRate[NUM_OPS];  // stagePos advance per ENV_BLOCK
    float        decayRate[NUM_OPS];
    float        releaseRate[NUM_OPS];
    float        sustain[NUM_OPS];     // 0..1
    float        pitchMulStart;    // global pitch multiplier at buffer start
    float        pitchMulStep;     // added per ENV_BLOCK block
    float        glideCoef;        // 0..1 per ENV_BLOCK block (1 = instant)
    float        carrierScale;     // 1/sqrt(carrier count)
    // v0.71. For a fixed operator ratioMul* carry its whole phase increment
    // (Hz / SAMPLE_RATE x Detune x OP_RATIO modulation) instead of a ratio,
    // so a morph of FixNote is walked per block like any ratio.
    uint8_t      fixedMask;        // bit o = OP o+1 is fixed
    float        penvAmt;          // semitones, modulation included
    float        penvRate;         // stagePos advance per ENV_BLOCK (operator Decay curve)
};

// Voice-level state that the renderer advances.
struct VoiceRender {
    OpState op[NUM_OPS];
    float   ratioNow[NUM_OPS];   // running ratio multiplier (interpolated per block)
    float   freqCur;       // Hz after glide
    float   freqTarget;    // Hz
    float   glideCap;      // per-block glide coefficient at most this (1 = none). The theremin's
                           // voice (v0.81) follows its 33ms readings smoothly even with Glide off
    float   pitchMul;      // running pitch multiplier (interpolated per block)
    bool    gate;          // key (or pedal) still holding this voice
    // Pitch envelope (v0.71). Started in startVoice() next to opNoteOn(),
    // so it restarts exactly when the operator envelopes do (not on legato).
    bool    penvOn;        // still moving
    float   penvPos;       // 0..1 along the decay
    float   penvVelK;      // velocity factor, frozen at note-on
    float   penvMul;       // 2^(offset/12) for this block, 1 when off
};

// Restarts the pitch envelope of one voice (note-on / retrigger).
void penvNoteOn(VoiceRender &v, float velK);

// Wave tables --------------------------------------------------------------
void buildWaveTables();                                    // once at boot, RAM
// The int16 sine (WAVE_TABLE_SIZE + 1 entries, full scale 32767) for the
// Chorus LFO. A plain pointer, so the Chorus loop in IRAM calls nothing in
// flash. v0.77: a 256-point float table was tried first; the coarser steps
// moved the chorus delay enough to change every sample, so the LFO reads the
// same 1024 points as before (one conversion per sample, no extra RAM).
extern const int16_t *const waveSineI16;

// Builds the four morphed tables for the current effective morph values
// (0..100 each) into scratch RAM. Called once per buffer (spec 3.6).
void buildMorphTables(const float effMorph[NUM_OPS]);
const float *morphTable(int opIndex);                      // WAVE_TABLE_SIZE + 1 entries

// A single blended sample for display previews (0..WAVE_TABLE_SIZE-1).
float morphPreviewSample(float morph0to100, int index);

// Rendering ----------------------------------------------------------------
// Adds n samples of this voice into out[] (float, roughly -1..1 per voice).
// gainStart/gainStep implement the 2ms steal fade: gain is applied per
// sample and stepped; pass 1,0 for no fade.
void renderVoice(VoiceRender &v, const RenderContext &ctx, float *out, int n,
                 float gainStart, float gainStep);

// True when every carrier of the current algorithm has reached IDLE.
bool voiceIsSilent(const VoiceRender &v, int algorithm);

// Envelope helpers used by the voice manager
void opNoteOn(OpState &op, bool retrigger);
void opNoteOff(OpState &op);
void opKill(OpState &op);
float envTimeToRate(float param0to100, float maxSeconds);  // stagePos advance per block

#endif // MORPHASE_FM_H
