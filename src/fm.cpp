// MORPHASE - fm.cpp
// Waveform tables, waveform morphing, envelopes and the fourteen algorithms.
//
// Performance rules honoured here (spec 11.2):
//  * no trig / pow / div / tanh inside the per-sample loops - the only
//    per-sample work is table reads, multiply-adds and a wrap
//  * phase accumulators are float, never double (single-precision FPU)
//  * tables are generated into RAM at boot, never const arrays in flash
//  * the algorithm switch sits OUTSIDE the sample loop; each case owns its
//    own loop so routing costs nothing per sample (spec 3.4)

#include "fm.h"
#include <math.h>
#include <string.h>
#include <esp_attr.h>

// IRAM (v0.53). The per-sample FM path runs from internal RAM, not from
// flash through the instruction cache. Measured on v0.52: with the code
// unchanged, only shifting the link layout moved the 8-voice load between
// 20,000 and 22,000us (avg), because the hot loops collided in the cache
// depending on where they landed. In IRAM they cannot. renderVoice and
// voiceAdvanceBlock live there; opTick is forced inline (v0.54, below).

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------
// One guard entry past the end so linear interpolation never needs a
// second wrap: table[WAVE_TABLE_SIZE] == table[0].
// v0.77: the 8 source waveforms are int16 (16.4KB, half of float). They are
// only read when a morph table is rebuilt (once per buffer at most) and by
// the OP preview - never per sample - so the conversion costs per buffer.
// The difference is the int16 rounding (1/65534 of full scale).
static int16_t waveTables[NUM_WAVEFORMS][WAVE_TABLE_SIZE + 1]; // 8 x 2KB = 16.4KB
static float morphTables[NUM_OPS][WAVE_TABLE_SIZE + 1];        // 4 x 4.1KB = 16.4KB scratch
const int16_t *const waveSineI16 = waveTables[0];              // Chorus LFO
constexpr float WAVE_I16_SCALE = 1.f / 32767.f;

// Carrier masks, bit0 = OP1 (spec 3.4 table)
const uint8_t ALGO_CARRIER_MASK[NUM_ALGORITHMS] = {
    0x1,  // 1: 4->3->2->1
    0x1,  // 2: (4,3)->2->1
    0x1,  // 3: 4->2->1 + 3->1
    0x1,  // 4: (4,3,2)->1
    0x5,  // 5: 4->3 + 2->1        carriers OP3, OP1
    0x3,  // 6: 4->3->2 + 1        carriers OP2, OP1
    0x7,  // 7: 4->(3,2,1)         carriers OP3, OP2, OP1
    0xF,  // 8: 1 2 3 4            all carriers
    // v0.7 (update spec 5): same shapes or new ones, feedback elsewhere
    0x1,  // 9:  4->3->2->1        FB on OP1 (the carrier)
    0x1,  // 10: 4->3->2->1        FB on OP2
    0x5,  // 11: 4->3 + 2->1       carriers OP3, OP1; FB on OP4 and OP2
    0x9,  // 12: 3->2->1 + 4       carriers OP4, OP1; FB on OP4
    0xD,  // 13: 2->1 + 3 + 4      carriers OP4, OP3, OP1; FB on OP2
    0x1   // 14: 4->(3,2)->1       diamond; FB on OP4
};

// Which operators feed back on themselves (bit0 = OP1). 1-8 keep OP4.
// v0.7: the feedback amount is still the one `feedback` value; only its
// place moves, so CC#2 / IMU / LFO `Feedback` mean the same everywhere.
const uint8_t ALGO_FB_MASK[NUM_ALGORITHMS] = {
    0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8,
    0x1,  // 9
    0x2,  // 10
    0xA,  // 11: two loops, separate state, shared amount
    0x8,  // 12
    0x2,  // 13
    0x8   // 14
};

static void normalizeTable(float *t) {
    // Remove DC (a DC offset in a modulator would detune its carrier), then
    // scale the peak to exactly 1.0.
    double sum = 0;
    for (int i = 0; i < WAVE_TABLE_SIZE; i++) sum += t[i];
    float dc = (float)(sum / WAVE_TABLE_SIZE);
    float peak = 0.f;
    for (int i = 0; i < WAVE_TABLE_SIZE; i++) {
        t[i] -= dc;
        float a = fabsf(t[i]);
        if (a > peak) peak = a;
    }
    if (peak < 1e-6f) peak = 1.f;
    for (int i = 0; i < WAVE_TABLE_SIZE; i++) t[i] /= peak;
    t[WAVE_TABLE_SIZE] = t[0];
}

static float waveSample(int w, float x) {
    const float TWO_PI_F = 6.283185307f;
    float sx = sinf(TWO_PI_F * x);
    switch (w) {
        case 0: return sx;                                                  // Sine
        case 1: return sinf(TWO_PI_F * (x + 0.6f * sx));                    // Squeeze - self phase-distorted sine
        case 2: return (x < 0.5f) ? sx : 0.f;                               // HalfSine - second half zero
        case 3: if (x < 0.5f) { float u = (x - 0.25f) / 0.25f; return 1.f - u * u; }   // Parabolic -
                else          { float u = (x - 0.75f) / 0.25f; return -(1.f - u * u); } // two arches
        case 4: return tanhf(2.5f * sx);                                    // ESquare - tanh-driven sine
        case 5: return sinf(TWO_PI_F * (x + 0.8f * sinf(2.f * TWO_PI_F * x)));  // Saw2 - 2nd-harmonic PD
        case 6: return 2.f * ((expf(3.f * x) - 1.f) / (expf(3.f) - 1.f)) - 1.f; // ESaw - exponential saw
        default: return tanhf(4.f * sinf(TWO_PI_F * (x + 0.3f * sx)));      // Square2 - PD + tanh
    }
}

void buildWaveTables() {
    // Each waveform is made in float in a scratch table, normalized, then
    // stored as int16.
    float *t = morphTables[0];
    for (int w = 0; w < NUM_WAVEFORMS; w++) {
        for (int i = 0; i < WAVE_TABLE_SIZE; i++) t[i] = waveSample(w, (float)i / WAVE_TABLE_SIZE);
        normalizeTable(t);
        for (int i = 0; i <= WAVE_TABLE_SIZE; i++) {
            float v = t[i] * 32767.f;
            waveTables[w][i] = (int16_t)(v < 0.f ? v - 0.5f : v + 0.5f);   // round to nearest
        }
    }
    // Scratch tables start as plain sine so a preview before the first
    // audio buffer shows something sensible.
    for (int o = 0; o < NUM_OPS; o++)
        for (int i = 0; i <= WAVE_TABLE_SIZE; i++) morphTables[o][i] = (float)waveTables[0][i] * WAVE_I16_SCALE;
}

const float *morphTable(int opIndex) {
    return morphTables[clampi(opIndex, 0, NUM_OPS - 1)];
}

// Continuous blend across the fixed chain. morph 0..100 spans the 8
// waveforms in brightness order; the two neighbours are cross-faded.
static AUDIO_IRAM void blendInto(float *dst, float morph0to100) {
    float pos = clampf(morph0to100, 0.f, 100.f) * (NUM_WAVEFORMS - 1) / 100.f;
    int   a   = (int)pos;
    if (a >= NUM_WAVEFORMS - 1) { a = NUM_WAVEFORMS - 1; }
    float frac = pos - (float)a;
    int   b    = (a + 1 < NUM_WAVEFORMS) ? a + 1 : a;
    const int16_t *ta = waveTables[a];
    const int16_t *tb = waveTables[b];
    if (frac < 0.0005f) {
        for (int i = 0; i <= WAVE_TABLE_SIZE; i++) dst[i] = (float)ta[i] * WAVE_I16_SCALE;
        return;
    }
    // The int16 scale is folded into the two weights: two conversions per
    // entry are all the int16 tables add.
    const float fa = (1.f - frac) * WAVE_I16_SCALE, fb = frac * WAVE_I16_SCALE;
    for (int i = 0; i <= WAVE_TABLE_SIZE; i++) dst[i] = (float)ta[i] * fa + (float)tb[i] * fb;
}

void AUDIO_IRAM buildMorphTables(const float effMorph[NUM_OPS]) {
    // Same morph value -> same table; skip the blend when nothing moved.
    static float lastMorph[NUM_OPS] = {-1.f, -1.f, -1.f, -1.f};
    for (int o = 0; o < NUM_OPS; o++) {
        float m = clampf(effMorph[o], 0.f, 100.f);
        if (fabsf(m - lastMorph[o]) < 0.0001f) continue;
        lastMorph[o] = m;
        blendInto(morphTables[o], m);
    }
}

float morphPreviewSample(float morph0to100, int index) {
    float pos = clampf(morph0to100, 0.f, 100.f) * (NUM_WAVEFORMS - 1) / 100.f;
    int   a   = (int)pos; if (a >= NUM_WAVEFORMS - 1) a = NUM_WAVEFORMS - 1;
    int   b   = (a + 1 < NUM_WAVEFORMS) ? a + 1 : a;
    float frac = pos - (float)a;
    index &= WAVE_TABLE_MASK;
    return ((float)waveTables[a][index] * (1.f - frac) + (float)waveTables[b][index] * frac) * WAVE_I16_SCALE;
}

// ---------------------------------------------------------------------------
// Envelopes (block rate)
// ---------------------------------------------------------------------------
float envTimeToRate(float param0to100, float maxSeconds) {
    float p = clampf(param0to100, 0.f, 100.f) / 100.f;
    float seconds = 0.002f + p * p * maxSeconds;
    return (float)ENV_BLOCK / (seconds * SAMPLE_RATE);
}

void AUDIO_IRAM opNoteOn(OpState &op, bool retrigger) {
    if (!retrigger || op.stage == EnvStage::IDLE) {
        op.env = 0.f;
    }
    // A retrigger starts the attack from the CURRENT level (no click, no
    // restart from zero); stagePos maps that level back onto the attack
    // curve so the rise continues smoothly.
    op.stage    = EnvStage::ATTACK;
    op.stagePos = 0.f;
    if (op.env > 0.f) {
        // attack curve: env = 1 - (1-pos)^2  ->  pos = 1 - sqrt(1-env)
        float e = clampf(op.env, 0.f, 0.999f);
        op.stagePos = 1.f - sqrtf(1.f - e);
    }
    op.envStep = 0.f;
    op.out = 0.f; op.outPrev = 0.f;
    if (!retrigger) op.phase = 0.f;
}

void opNoteOff(OpState &op) {
    if (op.stage == EnvStage::IDLE || op.stage == EnvStage::RELEASE) return;
    op.stage    = EnvStage::RELEASE;
    op.stagePos = 0.f;
    op.relStart = op.env;       // decay from the REAL level, never from sustain (spec 3.3)
}

void opKill(OpState &op) {
    op.stage = EnvStage::IDLE;
    op.env = 0.f; op.envStep = 0.f; op.stagePos = 0.f;
    op.out = 0.f; op.outPrev = 0.f;
}

// Advances one operator envelope by one block and sets the per-sample
// step so env lands on the new value at the end of `count` samples.
static inline void envAdvanceBlock(OpState &op, const RenderContext &ctx, int opIndex, int count, bool gate) {
    float next = op.env;
    switch (op.stage) {
        case EnvStage::ATTACK: {
            op.stagePos += ctx.attackRate[opIndex];
            if (op.stagePos >= 1.f) {
                next = 1.f; op.stage = EnvStage::DECAY; op.stagePos = 0.f;
            } else {
                float r = 1.f - op.stagePos;
                next = 1.f - r * r;
            }
            break;
        }
        case EnvStage::DECAY: {
            float sus = ctx.sustain[opIndex];
            op.stagePos += ctx.decayRate[opIndex];
            if (op.stagePos >= 1.f) {
                next = sus; op.stage = EnvStage::SUSTAIN; op.stagePos = 0.f;
            } else {
                float r = 1.f - op.stagePos;
                next = sus + (1.f - sus) * r * r;
            }
            break;
        }
        case EnvStage::SUSTAIN:
            next = ctx.sustain[opIndex];
            break;
        case EnvStage::RELEASE: {
            op.stagePos += ctx.releaseRate[opIndex];
            if (op.stagePos >= 1.f) {
                next = 0.f; op.stage = EnvStage::IDLE; op.stagePos = 0.f;
            } else {
                float r = 1.f - op.stagePos;
                next = op.relStart * r * r;
            }
            break;
        }
        case EnvStage::IDLE:
        default:
            next = 0.f;
            break;
    }
    (void)gate;
    op.envStep = (next - op.env) / (float)count;
}

bool AUDIO_IRAM voiceIsSilent(const VoiceRender &v, int algorithm) {
    uint8_t mask = ALGO_CARRIER_MASK[clampi(algorithm, 1, NUM_ALGORITHMS) - 1];
    for (int o = 0; o < NUM_OPS; o++) {
        if (!((mask >> o) & 1)) continue;
        if (v.op[o].stage != EnvStage::IDLE) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Pitch envelope (v0.71)
// ---------------------------------------------------------------------------
void AUDIO_IRAM penvNoteOn(VoiceRender &v, float velK) {
    v.penvOn = true; v.penvPos = 0.f; v.penvVelK = velK; v.penvMul = 1.f;
}

// 2^x by multiplies only - exp2f lives in flash (libm) and must not be called
// from IRAM. Nearest integer through the exponent bits, the remainder
// (-0.5..0.5) by the degree-5 Taylor series of 2^f: worst error ~4e-6
// relative, under 0.01 cent. |x| <= 4 here (48 semitones), so the exponent
// never leaves the normal range.
static inline __attribute__((always_inline)) float fastExp2(float x) {
    int   i = (int)(x + 64.5f) - 64;         // round to nearest for x > -64
    float f = x - (float)i;                  // -0.5..0.5
    float p = 1.f + f * (0.693147182f + f * (0.240226507f + f * (0.0555041087f
                  + f * (0.00961812911f + f * 0.00133335581f))));
    union { float fl; int32_t in; } u;
    u.in = (i + 127) << 23;
    return p * u.fl;
}

// One block of the pitch envelope: the operator Decay curve (1-pos)^2 times
// the live amount, so a modulated Amt changes the fall while it sounds.
// Stops for good once under 0.01 semitone, and costs one branch after that.
static inline __attribute__((always_inline)) void penvAdvanceBlock(VoiceRender &v, const RenderContext &ctx) {
    if (!v.penvOn) return;
    v.penvPos += ctx.penvRate;
    float r = 1.f - v.penvPos;
    float semi = (v.penvPos >= 1.f) ? 0.f : ctx.penvAmt * v.penvVelK * r * r;
    if (fabsf(semi) < 0.01f) { v.penvOn = false; v.penvMul = 1.f; return; }
    v.penvMul = fastExp2(semi * (1.f / 12.f));
}

// ---------------------------------------------------------------------------
// Per-block voice update: glide, pitch, increments, envelopes, amps
// ---------------------------------------------------------------------------
static IRAM_ATTR void voiceAdvanceBlock(VoiceRender &v, const RenderContext &ctx, int count) {
    // Glide (exponential toward the target, snapped when close)
    if (v.freqCur != v.freqTarget) {
        float gc = (ctx.glideCoef < v.glideCap) ? ctx.glideCoef : v.glideCap;   // the slower of the two
        if (gc >= 0.999f) v.freqCur = v.freqTarget;
        else {
            v.freqCur += (v.freqTarget - v.freqCur) * gc;
            if (fabsf(v.freqCur - v.freqTarget) < 0.05f) v.freqCur = v.freqTarget;
        }
    }
    // Pitch modulation interpolated across the buffer (one powf per buffer
    // in the voice manager, linear steps here).
    v.pitchMul += ctx.pitchMulStep;
    penvAdvanceBlock(v, ctx);
    float pe = v.penvMul;
    float base = v.freqCur * v.pitchMul * (1.f / SAMPLE_RATE) * pe;
    for (int o = 0; o < NUM_OPS; o++) {
        OpState &op = v.op[o];
        // Ratio is interpolated the same way as pitch: the modulated value
        // is resolved once per buffer and walked here, 32 samples at a
        // time, so a fast tilt on OP_RATIO cannot step the increment.
        v.ratioNow[o] += ctx.ratioMulStep[o];
        // A fixed operator (v0.71) already carries its increment: only the
        // pitch envelope reaches it - not the key, glide, bend or Pitch.
        op.inc = (((ctx.fixedMask >> o) & 1) ? pe : base) * v.ratioNow[o];
        // OP_LEVEL modulation is added AFTER velocity and key scaling, so
        // it reaches the operator even on a softly played note.
        op.amp = clampf(ctx.opLevel[o] * op.velKs + ctx.opLevelAdd[o], 0.f, 1.f);
        envAdvanceBlock(op, ctx, o, count, v.gate);
    }
}

// ---------------------------------------------------------------------------
// The per-sample operator: one table read with linear interpolation.
// `mod` is a phase offset in cycles; +8 keeps the int conversion positive
// for any modulation this engine can produce (|mod| stays well under 8).
// ---------------------------------------------------------------------------
// Forced inline (v0.54). -Os left this as a real function despite
// `static inline`: 32 call sites in renderVoice, 32,768 calls per buffer at
// 8 voices, each paying call8/entry/retw and a store/reload of the phase
// through memory. Inlined, the phase stays in a register. Measured 8 voices
// + all FX: 17,950 -> 14,680us avg, for +828 bytes of IRAM.
static inline __attribute__((always_inline)) float opTick(float &ph, float inc, float mod, const float *tbl) {
    ph += inc;
    if (ph >= 1.f) ph -= 1.f;
    float p  = (ph + mod + 8.f) * (float)WAVE_TABLE_SIZE;
    int   ip = (int)p;
    float fr = p - (float)ip;
    ip &= WAVE_TABLE_MASK;
    float a = tbl[ip];
    return a + (tbl[ip + 1] - a) * fr;
}

// Local working copies of one operator for a block (keeps the hot loop on
// registers instead of struct fields).
#define OP_LOAD(N) \
    float ph##N = v.op[N].phase, inc##N = v.op[N].inc, env##N = v.op[N].env, \
          es##N = v.op[N].envStep, amp##N = v.op[N].amp; \
    const float *tb##N = ctx.table[N]; const bool on##N = ctx.opOn[N];
#define OP_STORE(N) \
    v.op[N].phase = ph##N; v.op[N].env = env##N;

// Operator output for this sample. A silent operator (level 0) costs one
// branch on a loop-invariant flag and nothing else (spec 3.4).
#define OP_OUT(N, MODIN) \
    (on##N ? (env##N += es##N, opTick(ph##N, inc##N, (MODIN), tb##N) * env##N * amp##N) : 0.f)

// OP4 with feedback: the average of its last two outputs modulates its own
// phase (the DX7 approach - averaging keeps high feedback from turning
// into noise instantly).
#define OP4_FB_OUT() \
    (on3 ? (env3 += es3, fbOut = opTick(ph3, inc3, (fbOut + fbPrev) * fbAmt, tb3) * env3 * amp3, \
            fbPrev = fbLast, fbLast = fbOut, fbOut) : 0.f)

// Feedback on any other operator (v0.7, algorithms 9-14). The very same
// rule as OP4_FB_OUT - average of the last two outputs, same scale - plus
// the modulation arriving from above, so a fed-back operator in the middle
// of a chain hears both. Its state lives in that operator's out / outPrev,
// which opNoteOn / opKill already clear for every operator, so it resets
// exactly when OP4's does. 1-8 still use OP4_FB_OUT untouched.
#define FB_LOAD(N) \
    float fbL##N = v.op[N].out, fbP##N = v.op[N].outPrev, fbO##N = fbL##N;
#define FB_STORE(N) \
    v.op[N].out = fbL##N; v.op[N].outPrev = fbP##N;
#define OP_FB_OUT(N, MODIN) \
    (on##N ? (env##N += es##N, fbO##N = opTick(ph##N, inc##N, (MODIN) + (fbO##N + fbP##N) * fbAmt, tb##N) * env##N * amp##N, \
              fbP##N = fbL##N, fbL##N = fbO##N, fbO##N) : 0.f)

void IRAM_ATTR renderVoice(VoiceRender &v, const RenderContext &ctx, float *out, int n,
                 float gainStart, float gainStep) {
    const float D = FM_MOD_DEPTH;
    const float fbAmt = ctx.feedback * FM_FB_DEPTH;
    const float cs = ctx.carrierScale;
    float gain = gainStart;
    // Feedback state lives in OP4 (index 3)
    float fbLast = v.op[3].out, fbPrev = v.op[3].outPrev, fbOut = fbLast;

    int done = 0;
    switch (clampi(ctx.algorithm, 1, NUM_ALGORITHMS)) {
    // ---------------------------------------------------------------
    case 1: // 4->3->2->1
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP4_FB_OUT();
                float o3 = OP_OUT(2, o4 * D);
                float o2 = OP_OUT(1, o3 * D);
                float o1 = OP_OUT(0, o2 * D);
                out[done + k] += o1 * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        break;
    // ---------------------------------------------------------------
    case 2: // (4,3)->2->1
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP4_FB_OUT();
                float o3 = OP_OUT(2, 0.f);
                float o2 = OP_OUT(1, (o4 + o3) * D);
                float o1 = OP_OUT(0, o2 * D);
                out[done + k] += o1 * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        break;
    // ---------------------------------------------------------------
    case 3: // 4->2->1 + 3->1
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP4_FB_OUT();
                float o2 = OP_OUT(1, o4 * D);
                float o3 = OP_OUT(2, 0.f);
                float o1 = OP_OUT(0, (o2 + o3) * D);
                out[done + k] += o1 * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        break;
    // ---------------------------------------------------------------
    case 4: // (4,3,2)->1
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP4_FB_OUT();
                float o3 = OP_OUT(2, 0.f);
                float o2 = OP_OUT(1, 0.f);
                float o1 = OP_OUT(0, (o4 + o3 + o2) * D);
                out[done + k] += o1 * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        break;
    // ---------------------------------------------------------------
    case 5: // 4->3 + 2->1
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP4_FB_OUT();
                float o3 = OP_OUT(2, o4 * D);
                float o2 = OP_OUT(1, 0.f);
                float o1 = OP_OUT(0, o2 * D);
                out[done + k] += (o3 + o1) * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        break;
    // ---------------------------------------------------------------
    case 6: // 4->3->2 + 1
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP4_FB_OUT();
                float o3 = OP_OUT(2, o4 * D);
                float o2 = OP_OUT(1, o3 * D);
                float o1 = OP_OUT(0, 0.f);
                out[done + k] += (o2 + o1) * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        break;
    // ---------------------------------------------------------------
    case 7: // 4->(3,2,1)
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP4_FB_OUT();
                float m  = o4 * D;
                float o3 = OP_OUT(2, m);
                float o2 = OP_OUT(1, m);
                float o1 = OP_OUT(0, m);
                out[done + k] += (o3 + o2 + o1) * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        break;
    // ---------------------------------------------------------------
    // ---------------------------------------------------------------
    // v0.7 (update spec 5, section 2). One loop each, like 1-8.
    // ---------------------------------------------------------------
    case 9: { // 4->3->2->1, FB on OP1 (the carrier)
        FB_LOAD(0)
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP_OUT(3, 0.f);
                float o3 = OP_OUT(2, o4 * D);
                float o2 = OP_OUT(1, o3 * D);
                float o1 = OP_FB_OUT(0, o2 * D);
                out[done + k] += o1 * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        FB_STORE(0)
        break;
    }
    // ---------------------------------------------------------------
    case 10: { // 4->3->2->1, FB on OP2
        FB_LOAD(1)
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP_OUT(3, 0.f);
                float o3 = OP_OUT(2, o4 * D);
                float o2 = OP_FB_OUT(1, o3 * D);
                float o1 = OP_OUT(0, o2 * D);
                out[done + k] += o1 * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        FB_STORE(1)
        break;
    }
    // ---------------------------------------------------------------
    case 11: { // 4->3 + 2->1, FB on OP4 and OP2 (own state, shared amount)
        FB_LOAD(1)
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP4_FB_OUT();
                float o3 = OP_OUT(2, o4 * D);
                float o2 = OP_FB_OUT(1, 0.f);
                float o1 = OP_OUT(0, o2 * D);
                out[done + k] += (o3 + o1) * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        FB_STORE(1)
        break;
    }
    // ---------------------------------------------------------------
    case 12: // 3->2->1 + 4, FB on OP4 (a carrier here)
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP4_FB_OUT();
                float o3 = OP_OUT(2, 0.f);
                float o2 = OP_OUT(1, o3 * D);
                float o1 = OP_OUT(0, o2 * D);
                out[done + k] += (o4 + o1) * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        break;
    // ---------------------------------------------------------------
    case 13: { // 2->1 + 3 + 4, FB on OP2
        FB_LOAD(1)
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP_OUT(3, 0.f);
                float o3 = OP_OUT(2, 0.f);
                float o2 = OP_FB_OUT(1, 0.f);
                float o1 = OP_OUT(0, o2 * D);
                out[done + k] += (o4 + o3 + o1) * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        FB_STORE(1)
        break;
    }
    // ---------------------------------------------------------------
    case 14: // diamond 4->(3,2)->1, FB on OP4
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP4_FB_OUT();
                float m  = o4 * D;
                float o3 = OP_OUT(2, m);
                float o2 = OP_OUT(1, m);
                float o1 = OP_OUT(0, (o3 + o2) * D);
                out[done + k] += o1 * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        break;
    // ---------------------------------------------------------------
    case 8: // 1 2 3 4 - pure additive (four morphing oscillators)
    default:
        while (done < n) {
            int cnt = n - done; if (cnt > ENV_BLOCK) cnt = ENV_BLOCK;
            voiceAdvanceBlock(v, ctx, cnt);
            OP_LOAD(0) OP_LOAD(1) OP_LOAD(2) OP_LOAD(3)
            for (int k = 0; k < cnt; k++) {
                float o4 = OP4_FB_OUT();
                float o3 = OP_OUT(2, 0.f);
                float o2 = OP_OUT(1, 0.f);
                float o1 = OP_OUT(0, 0.f);
                out[done + k] += (o4 + o3 + o2 + o1) * cs * gain; gain += gainStep;
            }
            OP_STORE(0) OP_STORE(1) OP_STORE(2) OP_STORE(3)
            done += cnt;
        }
        break;
    }
    v.op[3].out = fbLast; v.op[3].outPrev = fbPrev;
}

#undef OP_LOAD
#undef OP_STORE
#undef OP_OUT
#undef OP4_FB_OUT
#undef FB_LOAD
#undef FB_STORE
#undef OP_FB_OUT
