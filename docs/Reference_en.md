# MORPHASE Reference Guide

This guide covers every screen, setting and key in MORPHASE. If this is your first time, work through the Quick Start Guide first.

Names that appear on the screen (tabs, settings, patch names) are written exactly as the screen shows them.

## Contents

1. Basic operation (PLAY screen)
2. How MORPHASE makes sound
3. OP tab
4. ALGO tab
5. ARP tab
6. LFO and IMU (modulation)
7. FX tab
8. GLITCH
9. Patches
10. Patch morph
11. MIDI
12. BLE MIDI (BLE build)
13. SETTING
14. Factory patch list
15. Info screen
16. Troubleshooting and known limits

---

## 1. Basic operation (PLAY screen)

### 1.1 Tabs

The tabs `PLAY` `OP` `ALGO` `ARP` `LFO` `FX` `SET` run across the top of the screen.

| Key | Action |
|---|---|
| `Tab` / `Shift+Tab` | Next tab / previous tab |
| `G0` button | Jump between the PLAY screen and the last editing tab you used |
| `Shift+H` | Show / hide the help for the current screen |

![Help (Shift+H)](images/screens/help.png)

Inside each tab, `;` `.` select a setting and `,` `/` change its value. Hold a value key for 1.5 seconds and it moves in steps ten times larger.

### 1.2 Keys that work on every screen

| Key | Action |
|---|---|
| `K` / `L` | Master volume down / up (hold to keep changing). Saved 3 seconds later |
| `D` | Note hold. Keeps the notes you hold sounding until you press `D` again |
| `Z` / `X` | Pitch bend down / up while held. Range is `SET > MIDI > BendRng` |
| `A` / `S` | Freeze (hold) / release the current tilt of the IMU X / Y axis |
| `Shift+A` / `Shift+S` | IMU X / Y axis On / Off |
| `Shift+V` | ARP On / Off |
| `V` | ARP latch On / Off |
| `Shift+Enter` | Tap tempo (tap 2–4 times) |
| `q` `w` `e` `r` `t` | Temporarily flip Chorus / Delay / Reverb / Crush / Drive (7.4) |
| `g` | GLITCH while held (chapter 8) |
| `Shift+1`–`Shift+8` | Glide to morph slot 1–8 (10.3) |

### 1.3 Keys on the PLAY screen only

| Key | Action |
|---|---|
| `;` / `.` | Octave up / down (−3 to +3) |
| `,` / `/` | Transpose down / up (−12 to +12 semitones) |

Octave and transpose apply to every incoming MIDI note. They are saved 3 seconds later.

### 1.4 Reading the PLAY screen

![PLAY screen](images/screens/play.png)

| Area | Shows |
|---|---|
| Top row | Patch name, its position in the list (`edit` for an unsaved sound not in the list), algorithm, notes sounding / voices available (`mono` for a mono patch). The BLE build shows `b` (waiting, dim) / `B` (connected) right of the name |
| Note row | With ARP Off, one box per voice with the note it plays. With ARP On, the chord the ARP is using (the last note played lights up) |
| Scope | The output waveform. `- silent -` when nothing sounds |
| Text over the scope | Top left `GM` (current GMorph), top right `FB` (Feedback), bottom right `C D R X V` (the FX that are running light up), `GL` (GLITCH: dim = Off, bright = On, inverted = glitching right now), bottom left the morph state (`C1>2 45%` = chain moving, `C paused` = paused, `C off` = chain Off, `M>3 45%` = gliding). Each can be hidden in `SET > Display` |
| `Pitch` | Bar for pitch bend and pitch modulation (semitones) |
| ARP row | `ARP Up 120 1/16 L` (pattern, tempo, rate, latch). `CLK` when following an external clock |
| LFO row | Wave, rate, destination. `~` when it is running |
| Bottom row | `MIDI` (lights on receive; `-DIN` when a Grove theremin has stopped DIN MIDI), `HOLD`, `OCT` / `TRN` (filled when not 0), `THR` / `THP` (theremin On in Mod / Play) |
| Right column | Tilt pad (the dot is the current tilt; a hollow dot means calibrated), X / Y `On` / `Off` / `Hold`, X / Y destinations, master volume `VOL` |

### 1.5 Modulation marks

A value that is being modulated gets a mark next to it.

| Mark | Moved by |
|---|---|
| `*` | IMU (tilting the unit) |
| `~` | LFO |
| `^` | Theremin (Mod mode) |

---

## 2. How MORPHASE makes sound

### 2.1 4-operator FM

A MORPHASE sound is made of **four operators (OP1–OP4)**. An operator is a small oscillator that plays one waveform.

- **Carrier**: an operator you hear
- **Modulator**: an operator that rapidly wobbles (modulates) a carrier's waveform to change its tone. You don't hear it directly
- **Ratio**: the operator's frequency as a multiple of the note you play. A carrier's Ratio sets the pitch, a modulator's Ratio sets the tone (the pattern of overtones)
- **Level**: loudness for a carrier, depth of modulation (brightness, complexity) for a modulator
- **Feedback**: an operator modulating itself. Gives a gritty, sawtooth-like sound

Which operator modulates which is called the **algorithm** (chapter 4).

### 2.2 Eight waveforms and morphing

Each operator's waveform slides smoothly through eight waveforms ordered by brightness (`Morph` 0–100%).

```
Sine → Squeeze → HalfSine → Parabolic → ESquare → Saw2 → ESaw → Square2
(smooth)                                                     (sharp)
```

- **`Morph` on the OP tab**: the waveform of that one operator
- **`GMorph` on the ALGO tab**: shifts all four operators' waveforms together (added to each OP's Morph)

### 2.3 FM sound design tips (learned while making the factory patches)

| Tip | Why |
|---|---|
| **At most one non-integer-ratio modulator per carrier** | With two or more, the sum and difference tones multiply and you get beating and a "gurgling" murk |
| **Put non-integer ratios on one grid (for example steps of 0.5)** | Even inside one pair, sidebands can fall below the played note (e.g. carrier 2.76, modulator 1.41 → 0.06× = a 16Hz beat) |
| **Modulation depth and sense of pitch are not proportional** | In FM, the strength of the carrier's fundamental rises and falls as depth increases. Around Level 40–45 the pitch can actually disappear (found while tuning 31_NoiseSwl) |
| **For noise, keep the modulators low and deep** | The sidebands pack densely into a soft band of noise. Deep modulation with only high pitches sounds like "lots of metal" |
| **Close pitches make a low difference tone** | In metallic sounds, stacking nearby fixed frequencies gives a low "core" (found while tuning HiHat) |
| **A tone that brightens at the attack sounds like brass** | For strings and voices, keep the modulator's envelope steady from the start (found with 18_Strings) |
| **Pushing the waveform to ESaw (Morph 85+) gets grainy up high** | The waveform has a sharp step, which causes aliasing. Around 78 is plenty for a sawtooth character |
| **Polyphonic chords add up** | Four notes are 3–4 times as loud as one. If it gets too loud, the output limiter at the end keeps the level in check (7.5) |

---

## 3. OP tab

![OP tab](images/screens/op_wave.png)

The number keys `1`–`4` choose the operator to edit. The top line shows the operator's role (`CARRIER` / `MODULATOR`, plus `+FB` if it gets feedback) and the algorithm. The second line shows the current waveform name and Morph value (including modulation).

| Setting | Range | Description |
|---|---|---|
| `Morph` | 0–100% | Waveform (2.2). Mark: OpMorph |
| `Ratio` | 0.25–16.00 | Multiple of the played pitch (0.01 steps). Mark: OpRatio |
| `Fixed` | 1Hz – about 8.4kHz (semitone steps) | Shown instead of `Ratio` in fixed-frequency mode, as `220Hz A3` |
| `Detune` | −50 to +50 cents | A slight pitch offset (makes beating). Mark: OpDetun |
| `Level` | 0–100% | Loudness / modulation depth. At 0 the operator stops. Mark: OpLevel |
| `A` | 0–100 | Attack (rise time) |
| `D` | 0–100 | Decay (time to fall from the peak to S) |
| `S` | 0–100 | Sustain (level while the key is held) |
| `R` | 0–100 | Release (time to fade after the key is released) |
| `VelSns` | 0–100% | How much playing strength changes the level. On a modulator, harder playing gives a brighter sound |
| `KScl` | 0–100% | Key scaling. Lowers a modulator's level per octave above C4 (half at 100%). Keeps high notes from getting harsh |

### 3.1 Fixed frequency (`f`)

`f` switches the selected operator between **Ratio ↔ Fixed**. A Fixed operator plays the same pitch regardless of the key, glide or pitch bend (only the pitch envelope affects it). Use it for percussion attacks and metallic sounds. A fixed operator is shown by the color of its number on the top line and `FIX` at the right of the second line. Switching back to Ratio restores the original Ratio value.

### 3.2 Envelope graph

![Envelope graph on the OP tab](images/screens/op_env.png)

When the cursor is on the `A` `D` `S` `R` rows, an envelope graph replaces the waveform, and the second line shows the real times (e.g. `A 0.03s  D 1.30s  S 40  R 0.45s`).

- Times: A is `0.002 + (value/100)² × 3` seconds, D and R are `0.002 + (value/100)² × 8` seconds. Because the value is squared, small values give finer control
- The curve is drawn with the same formula the sound engine uses (the graph and the sound never disagree)
- The width follows time, but short A / D / R segments get a minimum width so they stay visible, and S (while held) is a fixed quarter of the width. Read exact times from the numbers above
- When you play, a white dot follows the **last note played** (theremin notes are not tracked)
- In reality the release starts from "the level at the moment of release" (the graph draws it from S)

---

## 4. ALGO tab

![ALGO tab](images/screens/algo.png)

The algorithm diagram is on the left, and the settings list (scrolling 7 rows at a time) on the right. Filled boxes are carriers, outlined boxes are modulators, and the `FB` mark shows the operator that gets feedback. An operator at Level 0 is drawn dim.

| Setting | Range | Description |
|---|---|---|
| `Algo` | 1–14 | Algorithm (4.1) |
| `Feedbk` | 0–100 | Feedback amount. Most of the change happens around 0–25. Mark: Feedbk |
| `PEnvAmt` | −48 to +48 semitones | Pitch envelope depth (4.2). Mark: PEnvAmt |
| `PEnvDcy` | 0–100 | Time for the pitch envelope to return (same time scale as D) |
| `PEnvVel` | 0–100% | How much playing strength changes the depth |
| `GMorph` | 0–100% | Shifts all four operators' waveforms together (2.2). Mark: GMorph |
| `Voices` | 1–12 (1–10 on the BLE build) | Number of notes that can sound at once. 1 = mono |
| `Glide` | 0–1000ms | Time for the pitch to slide to the next note (portamento). Mark: Glide |
| `GlMode` | Always / Legato | Always = always slide from the previous note; Legato = slide only when you play the next key while still holding the previous one |
| `GlideOn` | On / Off | Use glide or not |

When the BLE build loads a patch with 11–12 voices, `Voices` shows `12 (10)` (the patch value is kept; up to 10 voices sound).

### 4.1 The 14 algorithms

`>` means "modulates", `+` means "sounds side by side".

| # | Name | Routing | Feedback on | Good for |
|---|---|---|---|---|
| 1 | 4>3>2>1 | Series | OP4 | Deep modulation, metal, noise |
| 2 | (4,3)>2>1 | 3 and 4 modulate 2, 2 modulates 1 | OP4 | Brass, leads |
| 3 | 4>2>1+3>1 | 4>2>1 and 3>1 | OP4 | Clavi, complex tones |
| 4 | (4,3,2)>1 | Three modulate one carrier | OP4 | Bass, kick |
| 5 | 4>3 + 2>1 | Two pairs | OP4 | E.Piano, bells, pads |
| 6 | 4>3>2 + 1 | Three-stage series + one carrier | OP4 | Snare (skin + snares) |
| 7 | 4>(3,2,1) | One modulator, three carriers | OP4 | Pads, additive-style |
| 8 | 1 2 3 4 | All four are carriers | OP4 | Organ (drawbars) |
| 9 | SerSaw | 4>3>2>1 | **OP1** (carrier) | Sawtooth leads |
| 10 | Growl | 4>3>2>1 | **OP2** | Growling bass |
| 11 | DualFB | 4>3 + 2>1 | **OP4 and OP2** (separately) | Metal, thick pairs |
| 12 | StkSaw | 3>2>1 + 4 | OP4 | Sawtooth + another sound |
| 13 | Pair+2 | 2>1 + 3 + 4 | **OP2** | A pair + two carriers |
| 14 | Diamond | 4>(3,2)>1 | OP4 | Gongs, complex overtones |

**Patches with the same algorithm morph into each other smoothly** (chapter 10).

### 4.2 Pitch envelope

At the start of a note the pitch is offset by `PEnvAmt` and returns to the played pitch over the `PEnvDcy` time (the "boom" of kicks and toms, laser-like sounds). It also affects fixed-frequency operators. It is not retriggered on legato (mono, playing the next key while still holding the previous one).

![Pitch envelope graph (25_Kick)](images/screens/algo_penv.png)

When the cursor is on the `PEnvAmt` `PEnvDcy` `PEnvVel` rows, a pitch envelope graph replaces the diagram. **The bright line is the shape at velocity 127, the dim line at velocity 40**; raising `PEnvVel` widens the gap between them. When you play, a white dot follows the last note played.

---

## 5. ARP tab

The arpeggiator (plays the chord you hold as a broken chord, automatically).

![ARP tab](images/screens/arp.png)

| Setting | Range | Description |
|---|---|---|
| `Arp` | On / Off | Same as `Shift+V` |
| `Pattern` | Up / Down / UpDown / Random / AsPlayed / STEP | Order of notes. AsPlayed = the order you pressed them; STEP is 5.1 |
| `Latch` | On / Off | Same as `V`. Keeps the chord after you let go |
| `Tempo` | 40–300 BPM | Mark: ArpTmpo |
| `Rate` | 1/4 / 1/8 / 1/8T / 1/16 / 1/16T / 1/32 | Length of one step |
| `Swing` | −50 to +50 | 0 = straight. + = laid back (off-beats late; +50 is a 2:1 shuffle), − = pushed (off-beats early by the same amount at −50). The swing follows the steps as they are played, on-beat / off-beat, so it holds whatever the number of notes in the chord. Mark: ArpSwng |
| `Gate` | 10–200% | Note length. Above 100% notes overlap the next one |
| `Octave` | 1–4 | How many octaves to span |
| `Wrap` | Rest / Wrap / Octave | For STEP, when a step asks for a note number larger than the chord (rest / back to the first note / the same note an octave up) |
| `Length` | 1–16 | Number of STEP steps |
| `ClkSync` | On / Off | Follow the tempo of an incoming MIDI clock |
| `LatchMd` | Add / Chord | Add = keys you press are added in turn (press again to remove); Chord = after releasing everything, the next keys you press make a new chord |
| `OctTrn` | Input / Output | Apply octave / transpose to the incoming notes / to the notes the ARP plays |

Defaults: Up / 120 BPM / 1/16 / Swing 0 / Gate 80% / Octave 1 / Wrap Octave / Length 16 / LatchMd Add / OctTrn Input.

### 5.1 STEP patterns

![Editing the STEP grid](images/screens/arp_step.png)

A 16-step pattern where each step holds not a pitch but "**which note of the held chord**" (1–8 or Off), plus octave, velocity and gate. Change the chord and the same rhythm plays the new notes.

| Action | Result |
|---|---|
| Set `Pattern` to STEP and press `Enter` | Enter the step grid |
| `,` / `/` | Choose a step |
| `;` / `.` | Choose the field (note, octave, velocity, gate) |
| `1`–`8` / `0` | Chord note number / Off |
| `Shift+,` / `Shift+/` | Change the value |
| `Enter` | Back to the list |

Up to 8 patterns can be saved (`SET > ARP Pattern`, 13.7). Assign 8 consecutive MIDI CCs from one base number and you can pick them directly from pads (`SET > MIDI > PtnBase`).

The feature that breaks up the ARP (ARP glitch) is in chapter 8.

---

## 6. LFO and IMU (modulation)

### 6.1 LFO tab

The LFO (a slow repeating wobble) is one LFO shared by all voices.

| Setting | Range | Description |
|---|---|---|
| `Wave` | Sine / Triangle / Saw / Square / S&H | Shape of the wobble (S&H = random steps) |
| `Rate` | 0.1–20.0Hz | Speed |
| `Depth` | 0–100% | Depth. 0 stops it |
| `Target` | Off / destination list (6.3) | What it moves. Control destinations (ArpTmpo etc.) can't be chosen |
| `Delay` | 0–2000ms | When you play from silence, how long until the wobble comes in (it fades in) |

![LFO tab](images/screens/lfo.png)

The right side shows the wobble's shape and a dot at the current position. The bottom line shows the current output value and the destination.

> The screen redraws about 16 times a second, so at fast `Rate` settings (from about 4Hz, clearly above 8Hz) the dot can look slow or even run backwards (like wagon wheels in a film). The LFO itself runs at the set speed.

The LFO swings both positive and negative. The destination value moves up and down around the patch value.

### 6.2 IMU (tilting the unit)

The Cardputer ADV's tilt sensor moves values.

- Turn on the X axis (left/right) / Y axis (front/back) with `Shift+A` / `Shift+S`. **Both start Off**
- Destination and depth are **per-patch** settings. New patches, and factory patches without their own tilt settings, use **X = GMorph 50%, Y = Feedbk 50%**, so just turning the IMU on changes the sound (full tilt covers the whole range)
- Destinations that rest at one end of their range, such as GMorph, Feedbk, OpMorph, PtchMrph and GlProb, move the same way whichever way you tilt. Others, such as Pitch, go up or down with the direction of the tilt
- Control destinations can be chosen too. ArpTmpo, ArpSwng and ArpGate go up or down from their set value with the direction of the tilt. Glide only gets longer than its set value, whichever way you tilt (up to +1000ms)

All IMU settings are in `SET > IMU` (13.4).

### 6.3 Modulation destinations

On any row that picks a destination (IMU XTarget / YTarget, LFO Target, a knob slot's `n Tgt`, theremin Target), a one-line description appears at the bottom of the screen.

| Name | Moves | LFO | IMU / knob / theremin |
|---|---|---|---|
| `GMorph` | The waveforms of all four OPs (tone change within one patch) | ○ | ○ |
| `Feedbk` | Feedback amount (grittier, more sawtooth) | ○ | ○ |
| `Pitch` | Pitch (semitones) | ○ | ○ |
| `Volume` | Patch volume | ○ | ○ |
| `OpMorph` | The waveform of the selected OP | ○ | ○ |
| `OpLevel` | The level of the selected OP (brightness, for a modulator) | ○ | ○ |
| `OpRatio` | The ratio of the selected OP (tone, pitch) | ○ | ○ |
| `OpDetun` | The detune of the selected OP (beating) | ○ | ○ |
| `ArpTmpo` / `ArpSwng` / `ArpGate` | ARP tempo / swing / gate | — | ○ |
| `Glide` | Pitch glide time | — | ○ |
| `ChDepth` / `ChMix` | Chorus depth / amount | ○ | ○ |
| `DlFdbk` / `DlMix` | Delay repeats / amount | ○ | ○ |
| `RvSize` / `RvMix` | Reverb size / amount | ○ | ○ |
| `CrBits` / `CrDown` / `CrMix` | Crush bits / downsampling / amount | ○ | ○ |
| `DrAmt` / `DrMix` | Drive amount / mix | ○ | ○ |
| `PtchMrph` | Chain position (patch to patch, chapter 10) | ○ | ○ |
| `GlProb` | GLITCH probability (chapter 8) | ○ | ○ |
| `PEnvAmt` | Pitch envelope depth | ○ | ○ |

- **`Op…` destinations act on the operator selected on the OP tab** (number keys 1–4)
- **An FX destination has no effect while that FX's Mix is 0 (Off)**
- The names are similar, but **`GMorph` (waveform), `OpMorph` (one OP's waveform) and `PtchMrph` (patch to patch) are different things**

---

## 7. FX tab

### 7.1 The idea

**MORPHASE does not distort unless you use the distortion FX (Drive, Crush).**

- **Drive** is a tool for **adding** distortion (used as a sound)
- When the level is too high, the **output limiter** at the very end **only turns the volume down**, without bending the waveform, so nothing clips (7.5)

### 7.2 Signal flow

```
Voices (up to 12, 10 on BLE) → Crush → Chorus → Delay → Reverb → GLITCH → Drive → Volume → Output limiter → Output
```

### 7.3 Pads and parameters

![FX tab (pads)](images/screens/fx_pads.png)

Five pads in key order (Chorus / Delay / Reverb / Crush / Drive). **An FX with Mix 0 is Off** and costs no processing.

| Action | Result |
|---|---|
| `,` / `/` | Choose a pad |
| `Enter` | On / Off (turning it On resets Mix to its default) |
| `.` | Go to the parameter page |
| `Tab` (on the parameter page) | Back to the pads |

![FX tab (parameters)](images/screens/fx_params.png)

| FX | Parameters | Range | Default | Mix when turned On |
|---|---|---|---|---|
| Chorus (shimmer and width) | `Rate` / `Depth` / `Mix` | 0.1–5.0Hz / 0–20ms / 0–100% | 0.8Hz / 8ms / 0 | 40% |
| Delay (echo) | `Time` / `Fdbk` / `Mix` | 50–600ms / 0–90% / 0–100% | 300ms / 35% / 0 | 30% |
| Reverb (room) | `Size` / `Damp` / `Mix` | 0–100% ×3 | 50 / 50 / 0 | 35% |
| Crush (digital grit) | `Bits` / `Down` / `Mix` | 1–16 / 1–32x / 0–100% | 8 / 4x / 0 | 100% |
| Drive (distortion) | `Amount` / `Mix` | 1.0–5.0x / 0–100% | 2.0x / 0 | 100% |

### 7.4 FX shortcuts (`q` `w` `e` `r` `t`)

| Key | FX | Letter on PLAY |
|---|---|---|
| `q` | Chorus | `C` |
| `w` | Delay | `D` |
| `e` | Reverb | `R` |
| `r` | Crush | `X` |
| `t` | Drive | `V` |

- **An FX the patch has On is temporarily bypassed** (`BYP` on the FX tab), and **an FX the patch has Off is temporarily added at its default Mix** (`TMP`)
- Not saved. Loading a patch clears it
- The switch is a quick, smooth fade (about 23ms). Bypassing Delay or Reverb lets their tails ring out
- Can also be assigned to MIDI buttons (`BypCho` to `BypDrv`, 11.5)

### 7.5 Output limiter

Always active, after the volume.

- It only turns the volume down; it does not bend the waveform (32-sample lookahead)
- Once it turns down, it holds for 250ms and recovers over 400ms (so the level doesn't flutter and muddy a chord). If you play a quiet note right after a big chord, it takes about 0.65 seconds to return to full level
- The amount of reduction is shown by `OutLim … GR` in `SET > Info` (chapter 15)

---

## 8. GLITCH

Deliberately breaks up your playing. The settings are device settings (`SET > Glitch`), not saved in patches.

### 8.1 How to trigger it

| Method | Result |
|---|---|
| Hold `g` | Glitches at the `Hold%` probability |
| Turn `Glitch` On | Keeps glitching at the `Prob` probability without pressing anything |
| Destination `GlProb` | Tilt, a knob, the LFO or the theremin add probability (works even with Glitch Off: "tilt to break it up") |
| MIDI button `Glitch` | Toggle = Glitch On / Off, Momentary = while held (same as `g`) |

### 8.2 Settings (`SET > Glitch`)

![SET > Glitch](images/screens/glitch.png)

| Setting | Range | Default | Description |
|---|---|---|---|
| `Glitch` | On / Off | Off | Glitch all the time |
| `Rate` | 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T | 1/16 | Grid for each decision (ARP tempo, or MIDI clock) |
| `Prob` | 0–100% | 25 | Probability with Glitch On |
| `Hold%` | 0–100% | 100 | Probability while `g` is held |
| `Buffer` | Off / Stutter / Reverse / Gate / Random | Random | How the sound is broken: Stutter = repeat the last fragment, Reverse = play backwards, Gate = chop |
| `Chaos` | Off / Ratio / Timbre / Wild | Ratio | Knock the tone off for a moment: Ratio = ratios, Timbre = waveforms and feedback, Wild = even the algorithm |
| `Length` | 1/32, 1/16, 1/8 | 1/16 | Length of one glitch |
| `Amount` | 0–100% | 50 | Size of the glitch (how fine the repeats are, how far the tone jumps). Also sets the count / length of ARP glitches |
| `Arp` | Off / Ratchet / Octave / Reverse / Rest / Random | Off | ARP glitch (8.3) |

![The moment a glitch happens (GL inverted on PLAY)](images/screens/play_glitch.png)

With both `Buffer` and `Chaos` selected, each glitch is one or the other. While `GL` on the PLAY screen is inverted, a glitch is happening.

### 8.3 ARP glitch

Decided once per ARP step, at the same probability (`Prob` / `Hold%` / `GlProb`). Nothing happens while the ARP is Off (the `Arp` row says `ARP is off`).

| Type | Effect |
|---|---|
| `Ratchet` | Retrigger the same note 2–4 times within the step (`Amount` sets the count; fewer if one hit would be under 15ms) |
| `Octave` | That step jumps an octave up or down (to the other side if it would go out of range) |
| `Reverse` | For 2–8 steps (`Amount`) the ARP runs backwards, then carries on forwards from there |
| `Rest` | Skip that step (the position still advances) |
| `Random` | Pick one of the four each time |

Glitched notes are sent out as they are when ARP MIDI out (`ArpSend`) is On. The pattern itself is not changed.

---

## 9. Patches

A patch is a complete set of sound settings. **ARP, MIDI, IMU On / Off and display settings are device settings** and are not in patches.

### 9.1 `SET > Patch`

| Item | Description |
|---|---|
| `Load` | Load from the list (`;` `.` to choose, `Enter`; `Tab` to go back) |
| `Save` | Overwrite the current patch (asks to confirm) |
| `SaveAs` | Save under a name (up to 12 characters: letters, digits, `-` `_` `.`). Asks before overwriting an existing name |
| `Rename` / `Dup` / `Delete` | Rename / duplicate / delete |
| `Reset` | Back to the initial sound (Init). All FX Off (asks to confirm) |
| `Random` | Make a random sound |
| `Morph >` | The morph screen (chapter 10) |
| `Factory` | Restore all factory patches and factory chains |

### 9.2 Factory patch protection

- The 32 patches whose names start with two digits and `_` (like `01_`) **cannot be overwritten**. Choosing Save opens the screen for saving under another name (e.g. `01_EPiano` → `EPiano2`)
- They can be deleted. `Factory` brings them back
- When a firmware update improves the factory patches, they are rewritten automatically at startup (your own patches are left alone)

### 9.3 List order and Program Change

The list has **the 32 factory patches first** (in 01–32 order), followed by your own patches in name order. MIDI Program Change N loads entry N+1 of the list (0–31 are the factory patches). Up to 128 patches can be saved.

### 9.4 Files

| File | Contents |
|---|---|
| `/MORPHASE/Patch/<name>.json` | Patch (plain text; copy it to share) |
| `/MORPHASE/current.json` | The current sound (kept across power off) |
| `/MORPHASE/settings.json` | Device settings |
| `/MORPHASE/morph.txt` | The chain being edited |
| `/MORPHASE/Chain/<name>.txt` | Saved chains |
| `/MORPHASE/Pattern/0–7.json` | ARP STEP patterns |

**After a patch is loaded (SET Load, Program Change, `Pch+` / `Pch-`), after leaving a SET screen with `Tab` or going back to PLAY with `G0`, and after changing the volume and similar, saving happens once 3 seconds have passed since the last action and nothing is being played (no notes sounding, the ARP stopped)**. Writing to the card pauses MIDI, the ARP and the screen for a moment, so it waits until it can't get in the way. Only the files that changed are written. If the power goes off while you are still playing, or within 3 seconds of the sound stopping, the latest change is not saved. Explicit saves (Save / SaveAs / Rename / Dup / Delete and so on) are written on the spot.

Saving writes to a temporary file first and swaps it in. If the power goes off mid-write, the file is restored at startup.

---

## 10. Patch morph

Moves the sound smoothly from patch to patch. It blends **the whole patch**: ratios, levels, envelopes, FX and more.

### 10.1 `SET > Patch > Morph` (`MORPH > Slots`)

![MORPH > Slots](images/screens/morph_slots.png)

| Item | Description |
|---|---|
| `Chain` | On / Off. Use the chain (10.2) |
| `Pos` | Chain position (0–100%) |
| `Time` | Glide time (10.3) (0–10 seconds, default 1 second) |
| `Chains >` | Save and load chains (10.4) |
| Slots 1–8 | `Enter` or `/` to choose a patch, `,` or `Del` to empty the slot |

A slot whose algorithm differs from the previous slot gets a `!`. Passing through it, the sound switches over halfway. **Patches with the same algorithm connect smoothly.**

The bottom line shows the current state (`Now 1>2 45%  pos 45%` / `Paused …` / `Chain off` / `Glide > 3 45%`).

### 10.2 Chain (blending by position)

The run of filled slots starting from slot 1 is the "chain". One position (0–100%) travels along the whole of it.

- Move the position with `Pos`, or with **the `PtchMrph` destination** (MIDI knob, tilt, LFO, theremin)
- **To move it with a knob, set the knob slot's destination to `PtchMrph`.** CC#1 and CC#74 are fixed to `GMorph` (waveform), so they don't move the chain
- If you load another patch or edit the sound while the chain is moving, the chain **pauses** (`C paused` on PLAY). Moving `Pos`, a `PtchMrph` knob, the tilt or the theremin resumes it (the LFO does not). The pause survives a power cycle

### 10.3 Glide (moving to a slot)

`Shift+1`–`Shift+8` moves from the sound you hear now to that slot's patch over `Time` (`M>3 45%` on PLAY). On arrival it is the same as loading that patch. Press another slot mid-way and it heads there from the current sound. Works with the chain On or Off.

**Also by Program Change**: set `SET > MIDI > PC Mode` to `Morph` and Program Change 0–7 glides to slots 1–8. 8 and above are ignored with the notice `PC 9 ignored (Morph: 0-7)`.

### 10.4 Saving chains (`Chains >`)

![MORPH > Chains](images/screens/morph_chains.png)

Save the patches lined up in the slots (the chain being edited) under a name, as many as you like.

| Action | Result |
|---|---|
| `;` / `.` | Choose a chain (its patch names show below) |
| `,` / `/` | Choose Load / SaveAs / Delete |
| `Enter` | Do it (Load, overwrite and delete ask to confirm) |
| `Tab` | Back to the morph screen |

- A chain stores the patch names in slots 1–8 and `Time`
- If a patch in a loaded chain is not on the card, that slot is left empty and you are told
- **Factory chains** (marked `F`): `01_Pads` (keys → mallets → bell → additive pads) and `02_Drums` (`25_Kick` ⇔ `26_Tom`). Their names can't be saved over, and if deleted `Factory` brings them back. Loading them does not change `Time`

### 10.5 Switching chains from MIDI (`Chain+` / `Chain-`)

Assign `Chain+` / `Chain-` to a MIDI button (11.5) to load the next / previous chain after the last one loaded (from the top of the list if none has been loaded yet; it wraps around at the ends).

> Loading reads eight patches from the card, so the screen and MIDI handling pause for about 0.1–0.2 seconds (the sound does not stop). **Best between songs or sections.**

### 10.6 Similar names, sorted out

| Name | Where | Moves |
|---|---|---|
| `GMorph` | ALGO tab, destination | The **waveforms** of all four OPs (within one patch) |
| `OpMorph` | Destination (OP tab `Morph`) | The **waveform** of the selected OP |
| `PtchMrph` | Destination | **Chain position** (patch to patch) |
| `Glide` (ALGO tab) | Patch | Time for the **pitch** to slide to the next note (ms) |
| `Time` (morph screen) | Device setting | Time to move **from patch to patch** (seconds) |

---

## 11. MIDI

### 11.1 What it receives

Notes (with velocity), pitch bend, control change, program change, MIDI clock (Start / Continue / Stop). The standard build uses Unit MIDI (the unit's Grove port, 31250bps); the BLE build adds Bluetooth MIDI (chapter 12).

### 11.2 `SET > MIDI`

| Setting | Range | Description |
|---|---|---|
| `Channel` | Omni / 1–16 | Receive channel (default Omni) |
| `BendRng` | 1–12 semitones | Pitch bend range |
| `ArpSend` | On / Off | Send ARP notes over MIDI (Unit MIDI OUT) |
| `PtnBase` | Off / CC number | First CC for direct pattern select (5.1) |
| `PC Mode` | Patch / Morph | Program Change loads a patch / glides to a slot (10.3) |
| `CC Slots >` | | Knob slots (11.4) |
| `Switches >` | | Button slots (11.5) |
| `Assigned >` | | List of assignments (in CC order) |
| `BLE` | On / Off | BLE build only. Changing it needs a restart (chapter 12) |

### 11.3 Fixed CCs

| CC | Function |
|---|---|
| 1 (modulation), 74 | `GMorph` |
| 2 (breath) | `Feedbk` |
| 7 | Volume (fader) |
| 11 | Expression |
| 64 | Sustain pedal |
| 120 / 123 | Stop all notes |

### 11.4 Knob slots (4) and MIDI Learn

Each slot has `CC` (number), `Tgt` (destination) and `Dep` (depth 0–100%).

- **MIDI Learn**: press `Enter` on an `n CC` row, and when `Learn... turn a knob` appears, turn the knob you want (cancels after 10 seconds, or press `Enter` again). It learns the CC that actually moved, so you don't need to know the knob's number
- The knob position becomes the destination's value directly (`Volume` works like a fader that turns down; `Pitch` and `PEnvAmt` are 0 at the center)
- A slot that hasn't received its CC yet does nothing
- The same CC can be assigned twice (Learn warns `already used!`)

### 11.5 Button slots (4)

Each slot has `CC`, `Act` (action) and `Mod` (Toggle = flips each press / Moment = only while held).

| Action | Description |
|---|---|
| `Arp` / `Latch` / `Hold` / `Glide` | ARP On/Off / latch / note hold / glide On/Off |
| `Pch+` / `Pch-` | Next / previous patch |
| `Pat+` / `Pat-` | Next / previous ARP pattern |
| `Tap` | Tap tempo |
| `RndPch` / `RndPat` | Random patch / pattern |
| `Panic` | Stop all notes |
| `BypCho` to `BypDrv` | FX shortcuts (7.4) |
| `Glitch` | GLITCH (8.1) |
| `Theremin` | Theremin On / Off |
| `Chain+` / `Chain-` | Next / previous chain (10.5) |

### 11.6 MIDI clock

With `ClkSync` On in the ARP tab, the ARP and GLITCH grids follow an external MIDI clock. Start restarts from the top.

---

## 12. BLE MIDI (BLE build)

![PLAY screen on the BLE build (B right of the name)](images/screens/play_ble.png)

The BLE build appears as a Bluetooth MIDI device named `MORPHASE`. It only receives, and works alongside DIN MIDI. Notes, bend, CCs, MIDI Learn, sustain, Program Change and clock behave exactly as over DIN.

| Item | Description |
|---|---|
| Connecting | On iPhone / iPad, choose `MORPHASE` **inside the app** (its Bluetooth MIDI connection screen), not in the phone's Bluetooth settings |
| Display | `b` (waiting, dim) / `B` (connected) right of the name on PLAY. Info shows `(Adv/Conn/Off/Fail)` and the bytes received |
| On disconnect | All sounding notes stop and the pedal is treated as released (nothing hangs) |
| On / Off | `SET > MIDI > BLE` (default On). Changing it asks whether to restart |
| Tempo | BLE arrives in bundles about every 10ms, so clock is shakier than DIN. DIN is steadier for tempo sync |
| Won't connect | If you have registered the same Cardputer with other firmware before, remove the old entry in the phone's Bluetooth settings (MORPHASE uses its own address) |

Tested with: iPhone and iPad (app [KeyPad](https://www.discodsp.com/en/keypad/)). The BLE build plays up to 10 notes at once and shows fewer colors on screen (256).

---

## 13. SETTING

![SET tab](images/screens/set.png)

On the `SET` tab, choose a category with `;` `.` and open it with `Enter`. Going back with `Tab` saves the settings 3 seconds later, when nothing is being played (9.4).

### 13.1 Patch

See chapters 9 and 10.

### 13.2 Display

| Setting | Description |
|---|---|
| `Theme` | Screen color: Green / Amber / Cyan / Violet / Red |
| `Bright` | Brightness (10–100) |
| `GM/FB` / `MorphPos` / `FX stat` / `GL` | Show / hide each of the texts over the PLAY scope |

### 13.3 MIDI

See chapter 11.

### 13.4 IMU

The left column is the X axis, the right column the Y axis.

![SET > IMU](images/screens/imu.png)

| Setting | Range | Description |
|---|---|---|
| `XTarget` / `YTarget` | Destinations (6.3) | **Patch setting** |
| `XDepth` / `YDepth` | 0–100% | **Patch setting**. Default 50% |
| `XSens` / `YSens` | 50–200% | Sensitivity. At 100%, a 35° tilt covers the full range |
| `XInvert` / `YInvert` | On / Off | Reverse the direction |
| `XCurve` / `YCurve` | Linear / Exp | Exp = gentle for small tilts, strong for large tilts |
| `XDead` / `YDead` | 0–30% | Dead zone around center (default 5%) |
| `Calib` | ON / OFF | `Enter` stores the way you are holding it now as "level" |

The current tilt values and bars are shown below.

### 13.5 Theremin

Turns your hand into a controller with an M5Stack **Unit ToF4M** (distance sensor). A device setting.

**Where to plug in** (`Bus`):

| Port | What goes there |
|---|---|
| The unit's Grove (G1 / G2) | Unit MIDI. DIN MIDI is read only here |
| LoRa Cap Grove (G8 / G9) | ToF, `Bus` = `Cap` (default, recommended). Works together with DIN MIDI. This port is the unit's internal I2C bus, so Unit MIDI can't be used there |
| `Bus` = `Grove` | Without a LoRa Cap, plug the ToF into the unit's Grove. **DIN MIDI is unavailable while the theremin is On** (the BLE build can still use BLE MIDI) |

![SET > Theremin (Mod)](images/screens/theremin_mod.png)

![SET > Theremin (Play)](images/screens/theremin_play.png)

**Settings** (left column is common; the right shows only what the current `Mode` uses):

| Setting | Range (default) | Description |
|---|---|---|
| `Theremin` | On / Off (Off) | Also switchable with the MIDI button `Theremin` |
| `Mode` | Mod / Play (Mod) | Mod = distance is a modulation source; Play = the theremin plays a note |
| `Bus` | Cap / Grove (Cap) | Table above |
| `Near` | 20–200mm (60) | Near end of the range |
| `Far` | Near+100 – 1000mm (250) | Far end of the range. About 280mm under room light |
| `Rescan` | | Look for the sensor again and count the devices on the bus (on Cap, `N shared` includes the unit's own devices) |
| `Target` / `Depth` | Destination (GMorph) / 0–100% (60) | Mod: what to move. The closer the hand, the deeper. Mark `^` |
| `Release` | Hold / Return (Return) | Mod: with no hand, keep the last value / return to 0 |
| `Pitch` | Smooth / Semitone (Smooth) | Play: smooth (a real theremin) / snap to a scale |
| `Scale` | 49 scales (Chromatic) | Play, Semitone: the scale. Tonic is C, moved by transpose. While selected, its full name shows below |
| `Top` | ± octaves from C4 (C5) | Play: the note at `Near` |
| `Span` | 1–4 oct (2) | Play: range downward from there |
| `VolAxis` | Off / X / Y (Y) | Play: the tilt axis that sets the volume (half when level, full tilted one way, silent the other way; freeze with `A` / `S`). In Play, that axis's own destination is paused (`vol` on PLAY) |

The bottom line shows the sensor state (`Off` / `Searching` / `OK` / `Lost`), the distance, a bar for the hand position within the range, and the effect (% in Mod, the note name in Play). Hold your hand where you want the end of the range and set `Near` / `Far` from that.

**Voices in Play**: the last voice is reserved for the theremin, so keys get one voice fewer (11, or 9 on BLE). Theremin notes ignore key notes, the sustain pedal and All Notes Off, and are not sent to the ARP or MIDI out. Even on a mono patch, one key note and the hand's note sound separately. Unplug the sensor and it goes `Lost` in about 2 seconds; plug it back in and it recovers automatically.

### 13.6 Glitch

See chapter 8.

### 13.7 ARP Pattern

| Item | Description |
|---|---|
| `Slot` | Pattern slot (0–7) |
| `Load` / `Save` | Load / save that slot |
| `Random` | Make a playable random pattern |

### 13.8 Info

See chapter 15.

---

## 14. Factory patch list

None of the factory patches distort, except those using Drive (16, 17) and Crush (30). Levels are matched within each group. **Tilt** describes what happens after turning the IMU on with `Shift+A` / `Shift+S` (where nothing is noted, X = waveform, Y = grit).

### Keys (01–04)

| # | Name | Algo | In a word | Try this |
|---|---|---|---|---|
| 01 | `01_EPiano` | 5 | Electric piano (body + tine) | Play hard to hear the tine. Morphs with `07_Marimba` |
| 02 | `02_EPBright` | 5 | Bright E.Piano | Big difference between soft and hard |
| 03 | `03_Clav` | 3 | Snappy clavinet | Play short. Add Drive with `t` for funk |
| 04 | `04_Organ` | 8 | Four sine drawbar organ | Sounds as long as you hold. Tilt X for tone |

### Bells and mallets (05–08)

| # | Name | Algo | In a word | Try this |
|---|---|---|---|---|
| 05 | `05_Bell` | 5 | Bell | Up high. More reverb with `e` |
| 06 | `06_Gong` | 14 | Gong | Long low notes. Long reverb |
| 07 | `07_Marimba` | 5 | Marimba | Morphs with `01_EPiano` (same algorithm) |
| 08 | `08_Glock` | 13 | Glockenspiel | Up high |

### Bass (09–12, mono, Legato)

| # | Name | Algo | In a word | Try this |
|---|---|---|---|---|
| 09 | `09_Bass` | 5 | Basic FM bass | Play legato to slide |
| 10 | `10_Growl` | 10 | Growling bass | LFO growl. Tilt X for growl strength |
| 11 | `11_SawBass` | 12 | Sawtooth bass | Brighter when played hard. `Feedbk` (tilt Y) for roughness |
| 12 | `12_SubBass` | 4 | Deep sub bass | Use earphones |

### Leads (13–16, mono, Legato)

| # | Name | Algo | In a word | Try this |
|---|---|---|---|---|
| 13 | `13_MorphLd` | 7 | Lead with moving waveforms | **Tilt X for waveform** (deep). Slow LFO movement. Delay |
| 14 | `14_SawLead` | 9 | Sawtooth lead | Vibrato after 0.6 s. Delay |
| 15 | `15_Whistle` | 4 | Whistle | Vibrato after 0.7 s |
| 16 | `16_DirtyLd` | 2 | Distorted lead | **Uses Drive**. Remove Drive with `t` for a clean tone |

### Brass, strings, pads (17–20)

| # | Name | Algo | In a word | Try this |
|---|---|---|---|---|
| 17 | `17_Brass` | 2 | Brass | **Edge from Drive**. Brighter when played hard |
| 18 | `18_Strings` | 7 | Strings | Play slowly. Delayed vibrato |
| 19 | `19_WarmPad` | 5 | Warm pad | About 1 s attack. Slow LFO waveform movement |
| 20 | `20_Choir` | 5 | Vocal pad | Vibrato. Chorus and reverb |

### Additive and evolving pads (21–24, all Algo 7)

| # | Name | Algo | In a word | Try this |
|---|---|---|---|---|
| 21 | `21_Additive` | 7 | Stacked sines, organ-like | Morphs with `22`–`24` (same algorithm) |
| 22 | `22_Evolve` | 7 | Pad that keeps changing | Hold it. LFO (0.1Hz) |
| 23 | `23_TiltPad` | 7 | Pad played by tilting | **Tilt X is the star** (deep). No LFO |
| 24 | `24_Shimmer` | 7 | Shimmering pad | 1×, 4× and 8× partials. Long reverb |

### Drums (25–28, polyphonic one-shots)

| # | Name | Algo | In a word | Try this |
|---|---|---|---|---|
| 25 | `25_Kick` | 4 | Kick | C1–C2. Play hard for a deeper drop. Morphs with `26_Tom` (`02_Drums`) |
| 26 | `26_Tom` | 4 | Tom | Pitched: play it across the keys like a set of toms |
| 27 | `27_Snare` | 6 | Snare (skin + snares) | Around C3 |
| 28 | `28_HiHat` | 1 | Hi-hat | Same pitch on every key. Play eighth notes |

### Metal, noise, drone (29–32)

| # | Name | Algo | In a word | Try this |
|---|---|---|---|---|
| 29 | `29_Metallic` | 11 | Metallic overtones | Play chords. Delay |
| 30 | `30_CrushLd` | 9 | Mono lead roughened by Crush | Remove Crush with `r` for a clean lead |
| 31 | `31_NoiseSwl` | 1 | Pitched noise | Swells over about 1.5 s. Breathes with the LFO. **Tilt X for waveform** |
| 32 | `32_Drone` | 7 | Drone | Hold it long. Keeps moving with the LFO. **Tilt X makes GLITCH more likely** (even with Glitch Off) |

---

## 15. Info screen (`SET > Info`)

![SET > Info](images/screens/info.png)

| Line | Shows |
|---|---|
| Line 1 | Version (BLE build: ` BLE` and the connection state `(Adv/Conn/Off/Fail)`) |
| `DSP load` | Time to process the latest audio buffer / limit 23,220us |
| `Peak` | Maximum since startup (or since `Enter`). Safe up to 85% |
| `Over budget` | Number of times the limit was exceeded (red when not 0: the sound may have dropped out) |
| `Voices … heap … min …` | Notes sounding / voices, free memory, lowest free memory since startup |
| `MIDI rx` / `rx … BLE … ov` | Bytes received (BLE build: DIN and BLE, and overflows), SD state |
| `Patches … largest blk …` | Number of patches, largest free memory block (BLE build: `stk`, stack used by Bluetooth processing) |
| `OutLim in … GR …` | Peak level going into the output limiter (0dB = the limit) and the reduction applied (this second) |

`Enter` resets Peak and Over to 0.

---

## 16. Troubleshooting and known limits

### Troubleshooting

| Symptom | Check |
|---|---|
| No sound | Master volume (`VOL`), whether `MIDI` lights, the receive channel, the microSD card |
| Unit MIDI receives nothing | DIP switch BYPASS, Grove 5V switch 5VOUT, theremin `Bus` not set to `Grove` (`-DIN` on PLAY) |
| A knob doesn't move the chain | The knob slot's destination is `PtchMrph` (CC#1 / #74 are GMorph); if PLAY shows `C paused`, move Pos or the knob |
| Tilt changes nothing | IMU turned On with `Shift+A` / `Shift+S`, Depth not 0 in `SET > IMU` |
| Theremin shows `Lost` / `Searching` | Port and `Bus` match, `Rescan` |
| Sound distorts | Drive / Crush in use? (compare by removing them with `t` / `r`) |
| The unit's keys don't respond | See "Keyboard freeze" below |

### Known limits

- **Keyboard freeze**: very rarely, the unit's keys stop responding entirely. This is a known quirk of the Cardputer ADV's keyboard chip (TCA8418). **MORPHASE restarts itself after about 30 seconds and recovers.** Holding the same key for 30 seconds can also trigger the restart
- **LFO dot**: at fast LFO rates the dot can look slow or backwards (6.1)
- **Loading a chain**: the screen and MIDI pause for 0.1–0.2 seconds (10.5)
- **Morphing between different algorithms**: the sound switches over part-way (10.1)
- **Output limiter**: after a big chord, about 0.65 seconds to return to full level (7.5)
- **BLE build**: 10 voices, 256-color screen, clock shakier than DIN. Tested devices: iPhone and iPad (app KeyPad)
- The **original Cardputer** (non-ADV) is not supported
- Up to 128 patches; the chain list shows up to 64 chains

If that doesn't solve it, open a GitHub Issue or contact the developer (@Tokagetchi).
