// MORPHASE - glitch.cpp
// GLITCH (v0.6). See glitch.h.
//
// Threading: loop() only writes single values (tempo, held flags, resync);
// audioTask owns every bit of event state. The per-sample work lives in one
// IRAM function that calls nothing (update spec 4, 0.2): a Buffer event costs
// one or two int16 reads and a few multiplies per sample, and nothing at all
// when no event is running.

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include "glitch.h"
#include "fx.h"

// ---------------------------------------------------------------------------
// loop() side
// ---------------------------------------------------------------------------
static volatile float glBpm = 120.f;
static volatile bool  glKeyHeld = false, glSwHeld = false, glResyncReq = false;
static volatile bool  glEventFlag = false;

void glitchSetBpm(float bpm)         { glBpm = bpm; }
void glitchSetKeyHeld(bool held)     { glKeyHeld = held; }
void glitchSetSwitchHeld(bool held)  { glSwHeld = held; }
void glitchResync()                  { glResyncReq = true; }
bool glitchHeld()                    { return glKeyHeld || glSwHeld; }

// The ARP glitch (v0.82) uses the same effective probability as the buffer
// and Chaos events: Hold% while g / the switch is held, else Prob when
// Glitch is On, plus GlProb modulation (published by audioTask, one buffer
// old). 0..1.
static volatile float glModProbPub = 0.f;
float glitchEffectiveProb() {
    float base = glitchHeld() ? (float)sys.glHold : (sys.glOn ? (float)sys.glProb : 0.f);
    return clampf(base + glModProbPub * 100.f, 0.f, 100.f) / 100.f;
}
bool glitchEventActive()             { return glEventFlag; }

// ---------------------------------------------------------------------------
// audioTask side
// ---------------------------------------------------------------------------
static uint32_t rng = 2463534242u;
static inline uint32_t xorshift32() { uint32_t x = rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; rng = x; return x; }
static inline float rand01()  { return (float)(xorshift32() >> 8) * (1.f / 16777216.f); }   // 0..1
static inline float randU()   { return rand01() * 2.f - 1.f; }                              // -1..1
static inline int   randInt(int n) { return (int)(xorshift32() % (uint32_t)n); }

void glitchInit() {
    rng = esp_random() | 1u;          // seed only; never called in a sample loop
}

// Step grid: ARP tempo, own phase (a grid locked to the ARP would make the
// breaks predictable - the opposite of the point).
static float stepPos = 0.f;           // samples into the current step
static const float RATE_BEATS[(int)GlRate::GL_RATE_COUNT] = { 0.5f, 1.f / 3.f, 0.25f, 1.f / 6.f, 0.125f, 1.f / 12.f };
static const float LEN_BEATS[(int)GlLen::GL_LEN_COUNT]    = { 0.125f, 0.25f, 0.5f };

// Buffer event
enum GlMode : uint8_t { GM_STUTTER = 0, GM_REVERSE, GM_GATE };
struct GlBufEvent {
    uint8_t mode;
    int D;          // event length, samples
    int F;          // fragment length, samples
    int t;          // samples elapsed
    int segStart;   // tape index of the fragment's first sample
};
static GlBufEvent ev;
static bool bufActive = false;
static int  bufPendingStart = -1;     // sample offset in this buffer, -1 none
constexpr int GL_XFADE = 32;          // ~0.7ms: sharp, but no clicks
constexpr int TAPE_SAFE = 24255;      // 550ms: fragment + event must stay clear of the write head

// Chaos event: what to overwrite and with what, decided once
enum ChaosItem : uint8_t { CI_RATIO0 = 0, CI_MORPH0 = 4, CI_DETUNE0 = 8, CI_FEEDBACK = 12, CI_ALGO = 13, CI_COUNT = 14 };
static uint8_t chaosItem[4];
static float   chaosValue[4];
static int     chaosCount = 0;
static int     chaosLeft = 0;         // samples left while running
static bool    chaosRunning = false, chaosNext = false;
static int     chaosNextD = 0;

static int lengthSamples(float bpm) {
    float beat = 60.f / bpm * (float)SAMPLE_RATE;
    return (int)(beat * LEN_BEATS[clampi((int)sys.glLen, 0, (int)GlLen::GL_LEN_COUNT - 1)]);
}

// Pick the Chaos targets and their new values from the sound heard now.
// The pool is built from what MAY move; Voices, glide, pitch, envelopes, OP
// Level, routing and FX are simply not in it (update spec 4, 4.6). Neither
// are FixNote and the pitch envelope (v0.71, update spec 5, 6.2), and a
// fixed operator's Ratio is left out too - it would not be heard, and the
// fixed pitch must not jump.
static void startChaos(const Patch &now, float A) {
    int span = 4;                                          // Ratio
    if (sys.glChaos == GlChaos::TIMBRE) span = 13;         // + Morph, Detune, Feedback
    if (sys.glChaos == GlChaos::WILD)   span = 14;         // + Algorithm
    uint8_t pool[CI_COUNT];
    int poolN = 0;
    for (int i = 0; i < span; i++) {
        if (i < CI_MORPH0 && now.op[i].fixed) continue;
        pool[poolN++] = (uint8_t)i;
    }
    if (poolN == 0) { chaosCount = 0; return; }            // Ratio mode, every operator fixed
    int want = 1 + (int)(A * 3.f); if (want > 4) want = 4; if (want > poolN) want = poolN;
    for (int i = 0; i < want; i++) {                       // partial Fisher-Yates: no duplicates
        int j = i + randInt(poolN - i);
        uint8_t tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
        uint8_t it = pool[i];
        float v;
        if (it < CI_MORPH0) {
            v = clampf(now.op[it].ratio * exp2f(randU() * A * 3.f), 0.25f, 16.f);          // up to +-3 octaves
        } else if (it < CI_DETUNE0) {
            v = clampf(now.op[it - CI_MORPH0].morph + randU() * A * 100.f, 0.f, 100.f);
        } else if (it < CI_FEEDBACK) {
            v = clampf(now.op[it - CI_DETUNE0].detune + randU() * A * 50.f, -50.f, 50.f);
        } else if (it == CI_FEEDBACK) {
            v = now.feedback + rand01() * A * (100.f - now.feedback);                      // upward only
        } else {
            int a = 1 + randInt(NUM_ALGORITHMS - 1);                                        // never the current one
            if (a >= now.algorithm) a++;
            v = (float)a;
        }
        chaosItem[i] = it; chaosValue[i] = v;
    }
    chaosCount = want;
}

static void applyChaos(Patch &e) {
    for (int i = 0; i < chaosCount; i++) {
        uint8_t it = chaosItem[i]; float v = chaosValue[i];
        if (it < CI_MORPH0)        e.op[it].ratio = v;
        else if (it < CI_DETUNE0)  e.op[it - CI_MORPH0].morph = v;
        else if (it < CI_FEEDBACK) e.op[it - CI_DETUNE0].detune = v;
        else if (it == CI_FEEDBACK) e.feedback = v;
        else                       e.algorithm = clampi((int)v, 1, NUM_ALGORITHMS);
    }
}

void AUDIO_IRAM glitchBeginBuffer(int n, bool voicesActive, float modProb, Patch &engine) {
    // 1. Chaos runs on the engine copy only - never on livePatch, so the
    //    morph smoothing cannot absorb it, and the very next buffer after the
    //    event is the untouched sound again (update spec 4, 4.6).
    if (chaosNext) { chaosNext = false; chaosRunning = true; chaosLeft = chaosNextD; }
    if (chaosRunning) {
        applyChaos(engine);
        chaosLeft -= n;
        if (chaosLeft <= 0) chaosRunning = false;          // this buffer was its last
    }

    // 2. Step grid
    float bpm = clampf(glBpm, 20.f, 400.f);
    float stepLen = 60.f / bpm * (float)SAMPLE_RATE * RATE_BEATS[clampi((int)sys.glRate, 0, (int)GlRate::GL_RATE_COUNT - 1)];
    if (stepLen < 64.f) stepLen = 64.f;
    if (glResyncReq) { glResyncReq = false; stepPos = 0.f; }

    // 3. Probability: held beats the always-on mode; GlitchProb adds on top
    //    and works even with Glitch Off ("tilt to break it")
    bool held = glKeyHeld || glSwHeld;
    float base = held ? (float)sys.glHold : (sys.glOn ? (float)sys.glProb : 0.f);
    float prob = clampf(base + modProb * 100.f, 0.f, 100.f) / 100.f;
    glModProbPub = modProb;                                  // for the ARP glitch (v0.82)
    bool bufOn = sys.glBuf != GlBuf::OFF, chaosOn = sys.glChaos != GlChaos::OFF;

    // 4. Boundaries inside this buffer: one decision each, one event at a time
    float k = stepLen - stepPos;                            // first boundary, samples from buffer start
    if (stepPos <= 0.f) k = 0.f;
    while (k < (float)n) {
        bool busy = bufActive || bufPendingStart >= 0 || chaosRunning || chaosNext;
        if (!busy && voicesActive && prob > 0.f && (bufOn || chaosOn) && rand01() < prob) {
            bool doBuf = bufOn && (!chaosOn || (xorshift32() & 1u));
            float A = clampf((float)sys.glAmt / 100.f, 0.f, 1.f);
            int D = lengthSamples(bpm);
            if (doBuf) {
                uint8_t mode;
                switch (sys.glBuf) {
                    case GlBuf::STUTTER: mode = GM_STUTTER; break;
                    case GlBuf::REVERSE: mode = GM_REVERSE; break;
                    case GlBuf::GATE:    mode = GM_GATE; break;
                    default:             mode = (uint8_t)randInt(3); break;     // Random
                }
                int a = sys.glAmt;
                int N = (a < 20) ? 2 : (a < 40) ? 3 : (a < 60) ? 4 : (a < 80) ? 6 : 8;
                if (mode != GM_GATE && D + D / N > TAPE_SAFE) D = TAPE_SAFE * N / (N + 1);   // collision guard
                int F = D / N;
                if (F < 2 * GL_XFADE) { F = 2 * GL_XFADE; D = F * N; }
                ev.mode = mode; ev.D = D; ev.F = F; ev.t = 0;
                bufPendingStart = (int)k;                   // sample-accurate start
            } else {
                startChaos(engine, A);                      // from the sound heard now
                chaosNext = true; chaosNextD = D;           // takes effect from the next buffer
            }
        }
        k += stepLen;
    }
    stepPos += (float)n;
    while (stepPos >= stepLen) stepPos -= stepLen;
    glEventFlag = bufActive || bufPendingStart >= 0 || chaosRunning || chaosNext;
}

// Per-sample, IRAM, no calls. buf points at the first sample to process.
// noinline: a static IRAM function called once can be inlined into its
// caller, which lives in flash - taking the sample loop with it.
static void IRAM_ATTR __attribute__((noinline)) glitchLoop(float *buf, int cnt, const int16_t *tape, GlBufEvent &e) {
    const float S = 1.f / LINE_I16_GAIN, XI = 1.f / (float)GL_XFADE;   // the Delay line's scale (v0.78)
    const int D = e.D, F = e.F, seg = e.segStart;
    int t = e.t, fp = t % F, frag = t / F;                  // once per buffer
    for (int k = 0; k < cnt; k++) {
        float live = buf[k];
        // event fade in / out
        float w = 1.f;
        if (t < GL_XFADE) w = (float)t * XI;
        int rem = D - t;
        if (rem < GL_XFADE) { float w2 = (float)rem * XI; if (w2 < w) w = w2; }
        float out;
        if (e.mode == GM_GATE) {
            float cur = (frag & 1) ? 0.f : 1.f;             // on, off, on ... (duty 50%)
            float g = cur;
            if (frag > 0 && fp < GL_XFADE) { float prev = 1.f - cur; g = prev + (cur - prev) * (float)fp * XI; }
            g = 1.f + (g - 1.f) * w;
            out = live * g;
        } else {
            int idx = (e.mode == GM_STUTTER) ? seg + fp : seg + F - 1 - fp;
            if (idx >= DELAY_BUFFER_SIZE) idx -= DELAY_BUFFER_SIZE;
            float g = (float)tape[idx] * S;
            if (frag > 0 && fp < GL_XFADE) {
                // crossfade from where the previous pass would have continued
                int ci = (e.mode == GM_STUTTER) ? seg + F + fp : seg - 1 - fp;
                if (ci >= DELAY_BUFFER_SIZE) ci -= DELAY_BUFFER_SIZE; else if (ci < 0) ci += DELAY_BUFFER_SIZE;
                float c = (float)tape[ci] * S;
                g = c + (g - c) * (float)fp * XI;
            }
            out = live + (g - live) * w;                    // the fragment replaces the sound, reverb included
        }
        buf[k] = out;
        t++;
        if (++fp >= F) { fp = 0; frag++; }
    }
    e.t = t;
}

void AUDIO_IRAM glitchRenderBuffer(float *buf, int n) {
    if (!bufActive && bufPendingStart < 0) return;          // no event: nothing at all
    const int16_t *tape; int writeIdx;
    fxTapeInfo(&tape, &writeIdx);                           // after the Delay wrote this buffer
    int k0 = 0;
    if (!bufActive) {
        k0 = bufPendingStart;
        // The fragment is the F samples just before the event starts. The
        // Delay has already written this whole buffer, so sample k of it is
        // at writeIdx - n + k on the tape.
        int s = writeIdx - n + k0 - ev.F;
        while (s < 0) s += DELAY_BUFFER_SIZE;
        ev.segStart = s % DELAY_BUFFER_SIZE;
        bufActive = true;
    }
    bufPendingStart = -1;
    int cnt = n - k0;
    int rem = ev.D - ev.t;
    if (rem < cnt) cnt = rem;
    if (cnt > 0) glitchLoop(buf + k0, cnt, tape, ev);
    if (ev.t >= ev.D) bufActive = false;
    glEventFlag = bufActive || chaosRunning || chaosNext;
}
