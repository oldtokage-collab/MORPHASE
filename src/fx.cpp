// MORPHASE - fx.cpp
// Chorus / Delay / Reverb / Limiter on the mix bus.
//
// Lessons carried over from C.P.S. (each one was a real, audible bug there):
//  * the chorus wet path is muted until its delay line has filled, then
//    faded in, so the first note after silence does not click
//  * the chorus read distance is smoothed per sample so a depth change
//    resolved once per buffer cannot step the read position
//  * the chorus LFO phase is NOT reset by clearFxBuffers() - snapping it
//    teleports the read position and clicks
//  * the limiter is x/(1+|x|), never tanhf(), per sample
//  * the reverb hoists all state into locals for the whole block

#include "fx.h"
#include "fm.h"
#include <math.h>
#include <string.h>
#include <esp_attr.h>

// ---------------------------------------------------------------------------
// Buffers (static, sized in config.h)
// ---------------------------------------------------------------------------
// The int16 lines and their headroom (v0.78). The mix bus is before the
// patch Volume, and it is the sum of the voices: about 1.0 per voice at most
// (after the carrier normalisation), so a 4-note chord runs near 3-3.7 and an
// 8-note one at velocity 127 up to 7.2 (measured on every factory patch,
// docs/MORPHASE_REPORT_CLIP_CHECK.md). A line has to hold the most voices x
// one voice's level, so the bus is scaled down before the int16 write and
// back up on the read: 1/16 of the bus = int16 full scale (+24dB over 1.0).
// Until v0.77 the Delay line held 1.0 (every chord clipped it - its echoes,
// and the GLITCH tape, broke up) and the Chorus 8.0 (17_Brass reached -0.9dB).
// The coarser step adds at most -89dBFS of noise to the echoes and -83dBFS to
// the chorus, in their fading tails at master 100. The Reverb needs none:
// Freeverb's 0.015 input gain keeps it at -7dB of full scale or below.
// Chorus: 4KB, int16 since v0.77 (was float 8KB).
static int16_t chorusBuffer[CHORUS_BUFFER_SIZE];          // 4KB
constexpr float CHORUS_I16_GAIN = LINE_I16_GAIN;
static int16_t delayBuffer[DELAY_BUFFER_SIZE];            // 53KB, also the GLITCH tape
static int16_t reverbComb0[1116], reverbComb1[1188], reverbComb2[1277], reverbComb3[1356];
static int16_t reverbComb4[1422], reverbComb5[1491], reverbComb6[1557], reverbComb7[1617];
static int16_t reverbAp0[556], reverbAp1[441], reverbAp2[341], reverbAp3[225];   // ~25KB total

static int   chorusWriteIdx = 0;
static float chorusLfoPhase = 0.f;        // 0..1
static float chorusDelaySmooth = 441.f;   // samples
static int   chorusFillCount = 0;
static float chorusWetGain = 0.f;

static int   delayWriteIdx = 0;

// ---------------------------------------------------------------------------
// Bypass (v0.56). Each effect has a gain that moves to its target (0 bypassed,
// 1 active) linearly across one buffer, so a switch never clicks. Chorus,
// Crush and Limiter fade their wet path; Delay and Reverb close their INPUT
// and let what is already inside ring out.
// ---------------------------------------------------------------------------
volatile uint8_t fxBypassMask = 0;
volatile uint8_t fxBypassMomentary = 0;
volatile uint8_t fxForceMask = 0;
volatile uint8_t fxForceMomentary = 0;

void AUDIO_IRAM fxApplyForce(FxParams &f, uint8_t force) {
    if (!force) return;
    if ((force & (1u << FXP_CHORUS))  && f.chorusMix  <= 0.f) f.chorusMix  = FX_PAD_DEFAULT_MIX[FXP_CHORUS]  / 100.f;
    if ((force & (1u << FXP_DELAY))   && f.delayMix   <= 0.f) f.delayMix   = FX_PAD_DEFAULT_MIX[FXP_DELAY]   / 100.f;
    if ((force & (1u << FXP_REVERB))  && f.reverbMix  <= 0.f) f.reverbMix  = FX_PAD_DEFAULT_MIX[FXP_REVERB]  / 100.f;
    if ((force & (1u << FXP_CRUSH))   && f.crushMix   <= 0.f) f.crushMix   = FX_PAD_DEFAULT_MIX[FXP_CRUSH]   / 100.f;
    if ((force & (1u << FXP_LIMITER)) && f.limiterMix <= 0.f) f.limiterMix = FX_PAD_DEFAULT_MIX[FXP_LIMITER] / 100.f;
}
static float chorusG = 1.f, crushG = 1.f, limiterG = 1.f, reverbSend = 1.f;
static inline float bypTarget(const FxParams &p, int pad) { return (p.bypass & (1u << pad)) ? 0.f : 1.f; }

// Delay runs as a small state machine, because its line doubles as the
// GLITCH tape (v0.6) and as the record of what was played (update spec 4,
// 3.6):
//   ACTIVE  normal: line <- in*send + delayed*fb, wet out
//   TAIL    bypassed: input closed, the echoes ring out
//   TAPE    bypassed and silent (or pad Off): line <- in, no wet - recording only
//   WARMUP  back from TAPE: line <- in, no wet, for one delay time, so the
//           dry sound recorded while bypassed never comes out as an echo
enum DelayState : uint8_t { DS_ACTIVE = 0, DS_TAIL, DS_TAPE, DS_WARMUP };
static uint8_t delayState = DS_ACTIVE;
static float   delaySend = 1.f, delayWet = 1.f;
static int     delayWarmLeft = 0, delayQuietSamples = 0, delayTailSamples = 0;
constexpr float DELAY_QUIET_PEAK = 0.001f;            // -60dB
constexpr int   DELAY_TAIL_MAX = 8 * SAMPLE_RATE;     // 8s: feedback that never dies

static int   reverbCombIdx[NUM_REVERB_COMBS] = {0};
static float reverbCombStore[NUM_REVERB_COMBS] = {0.f};
static int   reverbApIdx[NUM_REVERB_ALLPASS] = {0};

// Forced inline (v0.61): -Os had kept this a real function - 15 call sites,
// 13 calls per sample (the Delay write and every Reverb comb / allpass write).
static inline __attribute__((always_inline)) int16_t toI16(float v) {
    if (v > 32767.f) v = 32767.f; else if (v < -32767.f) v = -32767.f;
    return (int16_t)v;
}

void AUDIO_IRAM fxParamsFromPatch(FxParams &o) {
    const Patch &lp = enginePatch;    // the engine plays enginePatch, never `patch` (v0.5 / v0.6)
    o.chorusRateHz  = clampf(lp.chorusRateHz, 0.1f, 5.f);
    o.chorusDepthMs = clampf(lp.chorusDepthMs, 0.f, 20.f);
    o.chorusMix     = clampf(lp.chorusMix, 0.f, 100.f) / 100.f;
    o.delayTimeMs   = clampf(lp.delayTimeMs, 50.f, 600.f);
    o.delayFeedback = clampf(lp.delayFeedback, 0.f, 90.f) / 100.f;
    o.delayMix      = clampf(lp.delayMix, 0.f, 100.f) / 100.f;
    o.reverbSize    = clampf(lp.reverbSize, 0.f, 100.f) / 100.f;
    o.reverbDamp    = clampf(lp.reverbDamp, 0.f, 100.f) / 100.f;
    o.reverbMix     = clampf(lp.reverbMix, 0.f, 100.f) / 100.f;
    o.limiterDrive  = clampf(lp.limiterDrive, 1.f, 5.f);
    o.limiterMix    = clampf(lp.limiterMix, 0.f, 100.f) / 100.f;
    o.crushMix      = clampf(lp.crushMix, 0.f, 100.f) / 100.f;
    float bits      = clampf(lp.crushBits, 1.f, 16.f);
    o.crushScale    = exp2f(bits - 1.f);
    o.crushInvScale = 1.f / o.crushScale;
    o.crushStep     = 1.f / clampf(lp.crushDown, 1.f, 32.f);
    // Mix below 1% is OFF (spec 6.3)
    if (o.chorusMix  < 0.01f) o.chorusMix  = 0.f;
    if (o.delayMix   < 0.01f) o.delayMix   = 0.f;
    if (o.reverbMix  < 0.01f) o.reverbMix  = 0.f;
    if (o.limiterMix < 0.01f) o.limiterMix = 0.f;
    if (o.crushMix   < 0.01f) o.crushMix   = 0.f;
}

bool AUDIO_IRAM fxHasTail(const FxParams &p) {
    return p.chorusMix > 0.f || p.delayMix > 0.f || p.reverbMix > 0.f;
}

// ---------------------------------------------------------------------------
// Chorus
// ---------------------------------------------------------------------------
static IRAM_ATTR void chorusBlock(float *buf, int n, const FxParams &p) {
    const int16_t *sine = waveSineI16;
    const float lfoInc = p.chorusRateHz / SAMPLE_RATE;
    const float mix = p.chorusMix;
    const float gT = bypTarget(p, FXP_CHORUS), gStep = (gT - chorusG) / (float)n;
    float g = chorusG;
    chorusG = gT;
    for (int k = 0; k < n; k++) {
        float dry = buf[k];
        chorusBuffer[chorusWriteIdx] = toI16(dry * CHORUS_I16_GAIN);   // bypassed or not, the line keeps filling
        if (mix <= 0.f) {
            chorusWriteIdx = (chorusWriteIdx + 1) & (CHORUS_BUFFER_SIZE - 1);
            continue;
        }
        int   li = (int)(chorusLfoPhase * WAVE_TABLE_SIZE) & WAVE_TABLE_MASK;
        float lfo = (float)sine[li] * (1.f / 32767.f);               // -1..1
        float delayMs = 10.f + p.chorusDepthMs * 0.5f * (1.f + lfo); // 10..30ms
        float target = delayMs * 0.001f * SAMPLE_RATE;
        chorusDelaySmooth += (target - chorusDelaySmooth) * 0.002f;
        float readPos = (float)chorusWriteIdx - chorusDelaySmooth;
        if (readPos < 0.f) readPos += CHORUS_BUFFER_SIZE;
        int   i0 = (int)readPos;
        int   i1 = (i0 + 1) & (CHORUS_BUFFER_SIZE - 1);
        float fr = readPos - (float)i0;
        i0 &= (CHORUS_BUFFER_SIZE - 1);
        float wet = ((float)chorusBuffer[i0] * (1.f - fr) + (float)chorusBuffer[i1] * fr) * (1.f / CHORUS_I16_GAIN);

        if (chorusFillCount < CHORUS_BUFFER_SIZE) chorusFillCount++;
        if ((float)chorusFillCount < chorusDelaySmooth) chorusWetGain = 0.f;
        else chorusWetGain += (1.f - chorusWetGain) * 0.0008f;
        wet *= chorusWetGain;

        chorusLfoPhase += lfoInc;
        if (chorusLfoPhase >= 1.f) chorusLfoPhase -= 1.f;
        chorusWriteIdx = (chorusWriteIdx + 1) & (CHORUS_BUFFER_SIZE - 1);
        float m = mix * g; g += gStep;
        buf[k] = dry * (1.f - m) + wet * m;
    }
}

// ---------------------------------------------------------------------------
// Delay (int16 line, integer read offset, recirculating feedback)
// ---------------------------------------------------------------------------
static IRAM_ATTR void delayBlock(float *buf, int n, const FxParams &p) {
    const float mix = p.delayMix, fb = p.delayFeedback;
    int delaySamples = (int)(p.delayTimeMs * 0.001f * SAMPLE_RATE);
    if (delaySamples >= DELAY_BUFFER_SIZE) delaySamples = DELAY_BUFFER_SIZE - 1;
    if (delaySamples < 1) delaySamples = 1;
    int readIdx = delayWriteIdx - delaySamples;
    if (readIdx < 0) readIdx += DELAY_BUFFER_SIZE;

    // Pad Off: record only (this is also the TAPE state). Turning the pad
    // back on goes through WARMUP, so the last delay time of dry playing is
    // not replayed as an echo (update spec 4, 9 - item 5).
    if (mix <= 0.f) {
        for (int k = 0; k < n; k++) {
            delayBuffer[delayWriteIdx] = toI16(buf[k] * LINE_I16_GAIN);
            if (++delayWriteIdx >= DELAY_BUFFER_SIZE) delayWriteIdx = 0;
        }
        delayState = DS_TAPE; delayWet = 0.f; delaySend = 1.f;
        return;
    }

    // State changes, once per buffer
    const bool byp = (p.bypass & (1u << FXP_DELAY)) != 0;
    switch (delayState) {
        case DS_ACTIVE:
            if (byp) { delayState = DS_TAIL; delayQuietSamples = 0; delayTailSamples = 0; }
            break;
        case DS_TAIL:
            if (!byp) delayState = DS_ACTIVE;   // the line holds echoes only: carry on
            else if (delayQuietSamples >= delaySamples || delayTailSamples >= DELAY_TAIL_MAX) delayState = DS_TAPE;
            break;
        case DS_TAPE:
            if (!byp) { delayState = DS_WARMUP; delayWarmLeft = delaySamples; }
            break;
        case DS_WARMUP:
            if (byp) delayState = DS_TAPE;
            else if (delayWarmLeft <= 0) delayState = DS_ACTIVE;   // the read head has passed the release
            break;
    }

    const bool loop = (delayState == DS_ACTIVE || delayState == DS_TAIL);   // the feedback loop runs
    const float sendT = (delayState == DS_ACTIVE) ? 1.f : 0.f;
    const float wetT  = loop ? 1.f : 0.f;
    const float sendStep = (sendT - delaySend) / (float)n, wetStep = (wetT - delayWet) / (float)n;
    float send = delaySend, wet = delayWet, peak = 0.f;
    for (int k = 0; k < n; k++) {
        float dry = buf[k];
        float delayed = delayBuffer[readIdx] * (1.f / LINE_I16_GAIN);
        if (loop) delayBuffer[delayWriteIdx] = toI16((dry * send + delayed * fb) * LINE_I16_GAIN);
        else      delayBuffer[delayWriteIdx] = toI16(dry * LINE_I16_GAIN);   // TAPE / WARMUP: recording only
        float m = mix * wet;
        buf[k] = dry * (1.f - m) + delayed * m;
        float a = fabsf(delayed * m); if (a > peak) peak = a;
        send += sendStep; wet += wetStep;
        if (++delayWriteIdx >= DELAY_BUFFER_SIZE) delayWriteIdx = 0;
        if (++readIdx >= DELAY_BUFFER_SIZE) readIdx = 0;
    }
    delaySend = sendT; delayWet = wetT;
    if (delayState == DS_TAIL) {
        delayTailSamples += n;
        delayQuietSamples = (peak < DELAY_QUIET_PEAK) ? delayQuietSamples + n : 0;
    } else if (delayState == DS_WARMUP) {
        delayWarmLeft -= n;
    }
}

// ---------------------------------------------------------------------------
// Reverb (Freeverb topology, int16 lines, float arithmetic)
// ---------------------------------------------------------------------------
static IRAM_ATTR void reverbBlock(float *buf, int n, const FxParams &p) {
    if (p.reverbMix <= 0.f) { reverbSend = bypTarget(p, FXP_REVERB); return; }
    // Size -> comb feedback 0.70..0.98, Damp -> lowpass 0..0.4 (usable
    // parts of Freeverb's ranges).
    const float fb    = 0.70f + p.reverbSize * 0.28f;
    const float damp  = p.reverbDamp * 0.4f;
    const float damp1 = 1.f - damp;
    const float mix   = p.reverbMix, dry1 = 1.f - mix;
    const float IN_GAIN = 0.015f * 32767.f;      // Freeverb fixed input gain, int16 domain
    const float OUT_GAIN = 1.f / 32767.f;
    const float sT = bypTarget(p, FXP_REVERB), sStep = (sT - reverbSend) / (float)n;
    float send = reverbSend;
    reverbSend = sT;

    int   i0=reverbCombIdx[0],i1=reverbCombIdx[1],i2=reverbCombIdx[2],i3=reverbCombIdx[3];
    int   i4=reverbCombIdx[4],i5=reverbCombIdx[5],i6=reverbCombIdx[6],i7=reverbCombIdx[7];
    float c0=reverbCombStore[0],c1=reverbCombStore[1],c2=reverbCombStore[2],c3=reverbCombStore[3];
    float c4=reverbCombStore[4],c5=reverbCombStore[5],c6=reverbCombStore[6],c7=reverbCombStore[7];
    int   p0=reverbApIdx[0],p1=reverbApIdx[1],p2=reverbApIdx[2],p3=reverbApIdx[3];

    for (int k = 0; k < n; k++) {
        float dry = buf[k];
        float in  = dry * IN_GAIN * send;      // bypass closes the input; the tail rings on
        send += sStep;
        float out = 0.f;
        #define RVB_COMB(N, LEN) { \
            float y = (float)reverbComb##N[i##N]; \
            c##N = c##N * damp + y * damp1; \
            reverbComb##N[i##N] = toI16(in + c##N * fb); \
            if (++i##N >= LEN) i##N = 0; \
            out += y; }
        RVB_COMB(0,1116) RVB_COMB(1,1188) RVB_COMB(2,1277) RVB_COMB(3,1356)
        RVB_COMB(4,1422) RVB_COMB(5,1491) RVB_COMB(6,1557) RVB_COMB(7,1617)
        #undef RVB_COMB
        // Eight combs can sum well past int16 range; scale down before the
        // allpass lines (which are int16 too) and compensate on output.
        out *= 0.125f;
        #define RVB_AP(N, LEN) { \
            float y = (float)reverbAp##N[p##N]; \
            reverbAp##N[p##N] = toI16(out + y * 0.5f); \
            out = y - out; \
            if (++p##N >= LEN) p##N = 0; }
        RVB_AP(0,556) RVB_AP(1,441) RVB_AP(2,341) RVB_AP(3,225)
        #undef RVB_AP
        buf[k] = dry * dry1 + out * (OUT_GAIN * 4.f) * mix;
    }

    reverbCombIdx[0]=i0;reverbCombIdx[1]=i1;reverbCombIdx[2]=i2;reverbCombIdx[3]=i3;
    reverbCombIdx[4]=i4;reverbCombIdx[5]=i5;reverbCombIdx[6]=i6;reverbCombIdx[7]=i7;
    reverbCombStore[0]=c0;reverbCombStore[1]=c1;reverbCombStore[2]=c2;reverbCombStore[3]=c3;
    reverbCombStore[4]=c4;reverbCombStore[5]=c5;reverbCombStore[6]=c6;reverbCombStore[7]=c7;
    reverbApIdx[0]=p0;reverbApIdx[1]=p1;reverbApIdx[2]=p2;reverbApIdx[3]=p3;
}

// ---------------------------------------------------------------------------
// Bitcrush (v0.55): sample-and-hold rate reduction + quantization. IRAM, and
// nothing in the loop calls out: no floorf()/lrintf() (library calls in
// flash). Quantizing uses an offset so the int cast truncates a positive
// number, which equals floor(). The mix bus is not -1..1 - the voices sum
// to several times that before the limiter - so the input is clamped to
// +-16 (v0.62, was +-8 for 8 voices; each voice peaks near +-1) and the
// offset is 2^20, above 16 * 2^15 at 16 bits; a float still resolves 1/8
// of a step at that magnitude, plenty for a truncation.
// ---------------------------------------------------------------------------
static float crushPhase = 0.f;
static float crushHeld  = 0.f;
constexpr float CRUSH_CLAMP  = 16.f;
constexpr float CRUSH_OFFSET = 1048576.f;  // 2^20

void IRAM_ATTR fxCrushBlock(float *buf, int n, const FxParams &p) {
    const float gT = bypTarget(p, FXP_CRUSH);
    if (p.crushMix <= 0.f) { crushG = gT; return; }   // Off: costs nothing
    const float mix = p.crushMix, scale = p.crushScale, inv = p.crushInvScale, step = p.crushStep;
    const float gStep = (gT - crushG) / (float)n;
    float g = crushG;
    crushG = gT;
    float ph = crushPhase, held = crushHeld;
    for (int k = 0; k < n; k++) {
        float x = buf[k];
        ph += step;
        if (ph >= 1.f) {
            ph -= 1.f;
            float c = x;
            if (c > CRUSH_CLAMP) c = CRUSH_CLAMP; else if (c < -CRUSH_CLAMP) c = -CRUSH_CLAMP;
            float v = c * scale + 0.5f + CRUSH_OFFSET;
            held = ((float)(int32_t)v - CRUSH_OFFSET) * inv;
        }
        buf[k] = x + (held - x) * (mix * g);
        g += gStep;
    }
    crushPhase = ph; crushHeld = held;
}

// ---------------------------------------------------------------------------
// Limiter: rational soft clip x/(1+|x|) - one fabsf and one division
// ---------------------------------------------------------------------------
// 1/d for d >= 1 without a division (v0.61). The S3 FPU has no divide
// instruction: "/" calls the ROM __divsf3, once per sample here. Bit-trick
// seed + 3 Newton steps: ~1e-7 relative error, inaudible.
static inline __attribute__((always_inline)) float fastRecip(float d) {
    union { float f; uint32_t i; } u; u.f = d; u.i = 0x7EF311C7u - u.i;
    float r = u.f;
    r = r * (2.f - d * r); r = r * (2.f - d * r); r = r * (2.f - d * r);
    return r;
}

void IRAM_ATTR fxLimiterBlock(float *buf, int n, const FxParams &p) {
    // Bypassed, the signal goes on unlimited; writeOutput()'s int16 clamp
    // still catches it, so it clips rather than wraps (update spec 4, 3.5).
    const float gT = bypTarget(p, FXP_LIMITER);
    if (p.limiterMix <= 0.f) { limiterG = gT; return; }
    const float drive = p.limiterDrive, mix = p.limiterMix;
    const float gStep = (gT - limiterG) / (float)n;
    float g = limiterG;
    limiterG = gT;
    for (int k = 0; k < n; k++) {
        float x = buf[k] * drive;
        float y = x * fastRecip(1.f + fabsf(x));
        float m = mix * g; g += gStep;
        buf[k] = buf[k] * (1.f - m) + y * m;
    }
}

// ---------------------------------------------------------------------------
// IRAM (v0.61, stage 2): the FX sample loops run from internal RAM like the
// FM path. With toI16 inlined, 8 voices + all FX went 15,150 -> 13,120us avg
// and 16,353 -> 13,393us peak; the peak-to-average gap shrank from ~1,200 to
// ~270us, i.e. the cache-miss spikes are gone.
void IRAM_ATTR fxProcessBlock(float *buf, int n, const FxParams &p) {
    chorusBlock(buf, n, p);      // always writes its line even when off
    delayBlock(buf, n, p);       // same
    reverbBlock(buf, n, p);
}

// ---------------------------------------------------------------------------
// Output limiter (v0.72, stage 2 of the two-stage design). The Drive curve
// above shapes the sound; this one only protects it: a gain, never a curve,
// so it cannot add distortion of its own. It sits in writeOutput(), after
// the volume, where the int16 limit is known. Before v0.72 a sum that went
// over simply hit the int16 clamp and split.
//
// Look-ahead: the signal leaves one 32-sample block late. The gain for the
// block being sent is ramped (one multiply-add per sample) to a value that
// is already low enough for the block that comes next, so the gain is down
// before a peak arrives. It may only fall within the two blocks it spans,
// then recovers with a ~150ms time constant. One reciprocal per block.
// ---------------------------------------------------------------------------
constexpr int   OUTLIM_BLOCK      = 32;            // also the look-ahead
constexpr float OUTLIM_THRESHOLD  = 31000.f;       // just under int16 full scale (-0.5dB)
constexpr float OUTLIM_RELEASE_MS = 400.f;         // recovery time constant (150 before v0.87)
// v0.87: once pulled down, the gain is held this long before it recovers.
// Without it the gain dipped at every peak of a dense chord and crept back in
// between - a gain wobbling at the beat rate, heard as distortion (Init, 10
// notes at velocity 127: energy off the notes -25.7dB; hold 250ms + release
// 400ms: -35.7dB). Below the threshold nothing changes, bit for bit.
constexpr int   OUTLIM_HOLD_BLOCKS = (int)(0.250f * SAMPLE_RATE / OUTLIM_BLOCK);
// 1 - exp(-a) with a = block / (release * rate); a is ~0.005, where it equals a
constexpr float OUTLIM_REL_COEF   = (float)OUTLIM_BLOCK / (OUTLIM_RELEASE_MS * 0.001f * (float)SAMPLE_RATE);

static float outLimDelay[OUTLIM_BLOCK];            // the block waiting to be sent
static float outLimGain = 1.f;                     // gain at the start of the next block sent
static float outLimPrevNeed = 1.f;                 // what the delayed block itself needs
static int   outLimHold = 0;                       // blocks left before the gain may recover (v0.87)
// Info meters (v0.72): written by audioTask per buffer, read by the UI.
static float outLimPeakAcc = 0.f, outLimMinGainAcc = 1.f;
static int   outLimMeterBufs = 0;
volatile float outLimPeak1s = 0.f;                 // largest |sample| into the limiter, int16 units
volatile float outLimMinGain1s = 1.f;              // lowest gain it applied

static inline __attribute__((always_inline)) float outLimRecip(float d) {
    union { float f; uint32_t i; } u; u.f = d; u.i = 0x7EF311C7u - u.i;
    float r = u.f;
    r = r * (2.f - d * r); r = r * (2.f - d * r); r = r * (2.f - d * r);
    return r;
}

static_assert(AUDIO_BUFFER_SIZE % OUTLIM_BLOCK == 0, "the output stage works in whole blocks");

// v0.77: volume, limiter and the int16 conversion in one pass per block
// (they were three passes over the buffer before). Same arithmetic in the
// same order, so the output is bit-identical. `mix` is used as scratch.
// Measured on the device (worst case, same source otherwise): three passes
// Peak 19,659us, one pass 19,367us - about 290us saved.
int IRAM_ATTR __attribute__((noinline)) fxOutputStage(int16_t *out, float *mix, int n, float volGain) {
    float peakAll = 0.f, minG = 1.f;
    float g = outLimGain, prevNeed = outLimPrevNeed;
    int outPeak = 0;
    for (int b = 0; b + OUTLIM_BLOCK <= n; b += OUTLIM_BLOCK) {
        float *x = mix + b;
        int16_t *o = out + b;
        float peak = 0.f;
        for (int i = 0; i < OUTLIM_BLOCK; i++) {
            float s = x[i] * volGain;
            x[i] = s;
            float a = fabsf(s); if (a > peak) peak = a;
        }
        if (peak > peakAll) peakAll = peak;
        float need = (peak > OUTLIM_THRESHOLD) ? OUTLIM_THRESHOLD * outLimRecip(peak) : 1.f;
        // End of the ramp: recover toward 1, but never above what the block
        // being sent or the block after it needs.
        // Recover toward 1 only once the hold has run out; a block that needs
        // the gain lower starts the hold again.
        float gEnd = (outLimHold > 0) ? g : g + (1.f - g) * OUTLIM_REL_COEF;
        if (outLimHold > 0) outLimHold--;
        if (need < g || prevNeed < g) outLimHold = OUTLIM_HOLD_BLOCKS;
        if (gEnd > need) gEnd = need;
        if (gEnd > prevNeed) gEnd = prevNeed;
        float step = (gEnd - g) * (1.f / (float)OUTLIM_BLOCK);
        for (int i = 0; i < OUTLIM_BLOCK; i++) {
            float in = x[i];
            float s = outLimDelay[i] * g;
            g += step;
            outLimDelay[i] = in;
            // int16 clamp: now only catches what the limiter could not
            if (s > 32767.f) s = 32767.f; else if (s < -32767.f) s = -32767.f;
            int16_t si = (int16_t)s;
            o[i] = si;
            int a = si < 0 ? -si : si;
            if (a > outPeak) outPeak = a;
        }
        g = gEnd;
        if (g < minG) minG = g;
        prevNeed = need;
    }
    outLimGain = g; outLimPrevNeed = prevNeed;
    // One-second meters for the Info screen
    if (peakAll > outLimPeakAcc) outLimPeakAcc = peakAll;
    if (minG < outLimMinGainAcc) outLimMinGainAcc = minG;
    if (++outLimMeterBufs >= SAMPLE_RATE / AUDIO_BUFFER_SIZE) {
        outLimPeak1s = outLimPeakAcc; outLimMinGain1s = outLimMinGainAcc;
        outLimPeakAcc = 0.f; outLimMinGainAcc = 1.f; outLimMeterBufs = 0;
    }
    return outPeak;
}

// Empties the look-ahead when the engine stops, so the next note does not
// start with the last 0.7ms of the previous one.
void fxClearOutputLimiter() {
    memset(outLimDelay, 0, sizeof(outLimDelay));
    outLimGain = 1.f; outLimPrevNeed = 1.f; outLimHold = 0;
}

// Reverb only: its lines are skipped while Mix is 0, so they hold whatever
// was there when it went off. Chorus and Delay keep writing theirs.
void clearReverbBuffers() {
    memset(reverbComb0, 0, sizeof(reverbComb0)); memset(reverbComb1, 0, sizeof(reverbComb1));
    memset(reverbComb2, 0, sizeof(reverbComb2)); memset(reverbComb3, 0, sizeof(reverbComb3));
    memset(reverbComb4, 0, sizeof(reverbComb4)); memset(reverbComb5, 0, sizeof(reverbComb5));
    memset(reverbComb6, 0, sizeof(reverbComb6)); memset(reverbComb7, 0, sizeof(reverbComb7));
    memset(reverbAp0, 0, sizeof(reverbAp0)); memset(reverbAp1, 0, sizeof(reverbAp1));
    memset(reverbAp2, 0, sizeof(reverbAp2)); memset(reverbAp3, 0, sizeof(reverbAp3));
    for (int i = 0; i < NUM_REVERB_COMBS; i++) reverbCombStore[i] = 0.f;
}

void fxTapeInfo(const int16_t **tape, int *writeIdx) {
    *tape = delayBuffer;
    *writeIdx = delayWriteIdx;
}

void clearFxBuffers() {
    memset(chorusBuffer, 0, sizeof(chorusBuffer));
    memset(delayBuffer, 0, sizeof(delayBuffer));
    chorusWriteIdx = 0; delayWriteIdx = 0;
    crushPhase = 0.f; crushHeld = 0.f;
    delayState = DS_ACTIVE; delaySend = 1.f; delayWet = 1.f; delayWarmLeft = 0;
    delayQuietSamples = 0; delayTailSamples = 0;
    chorusFillCount = 0; chorusWetGain = 0.f;
    // chorusLfoPhase deliberately untouched (see header comment)
    memset(reverbComb0, 0, sizeof(reverbComb0)); memset(reverbComb1, 0, sizeof(reverbComb1));
    memset(reverbComb2, 0, sizeof(reverbComb2)); memset(reverbComb3, 0, sizeof(reverbComb3));
    memset(reverbComb4, 0, sizeof(reverbComb4)); memset(reverbComb5, 0, sizeof(reverbComb5));
    memset(reverbComb6, 0, sizeof(reverbComb6)); memset(reverbComb7, 0, sizeof(reverbComb7));
    memset(reverbAp0, 0, sizeof(reverbAp0)); memset(reverbAp1, 0, sizeof(reverbAp1));
    memset(reverbAp2, 0, sizeof(reverbAp2)); memset(reverbAp3, 0, sizeof(reverbAp3));
    for (int i = 0; i < NUM_REVERB_COMBS; i++) { reverbCombIdx[i] = 0; reverbCombStore[i] = 0.f; }
    for (int i = 0; i < NUM_REVERB_ALLPASS; i++) reverbApIdx[i] = 0;
    fxClearOutputLimiter();
}
