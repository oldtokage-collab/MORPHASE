# MORPHASE Quick Start Guide

**MORPHASE** is firmware that turns the M5Stack **Cardputer ADV** into a **4-operator FM polyphonic synthesizer**. You play it from a MIDI keyboard, and the Cardputer becomes the editor and controller for the sound. You can also move the sound by tilting the unit or waving your hand over a sensor.

This guide takes you from power-on to your first sound and a quick tour of the main features. For a full description of every setting, see the Reference Guide.

---

## 1. What you need

| Item | Details |
|---|---|
| **Cardputer ADV** | The original Cardputer (non-ADV) is not supported |
| **microSD card** | FAT32. Patches and settings are saved here |
| **MIDI keyboard** | Connected in one of these ways |
| └ Standard | Plug an M5Stack **Unit MIDI** (U187) into the unit's Grove port and connect the keyboard with a 5-pin MIDI cable |
| └ BLE | An app or keyboard that sends Bluetooth MIDI, on iPhone / iPad etc. (Unit MIDI works too). Tested app: [KeyPad](https://www.discodsp.com/en/keypad/) |
| **Earphones** | Recommended. The built-in speaker barely reproduces low notes |

MORPHASE comes in two builds, **Standard** and **BLE**. They differ in whether they accept Bluetooth MIDI and in how many notes can sound at once.

| | Standard | BLE |
|---|---|---|
| MIDI | Unit MIDI (DIN) | Unit MIDI (DIN) + Bluetooth MIDI |
| Notes at once | 12 | 10 |

The startup screen and `SET > Info` show which build you have (the BLE build adds ` BLE`).

---

## 2. Start it up

Insert the microSD card and turn the power on.

**The first time**, a `/MORPHASE` folder is created on the card and **32 factory patches** (ready-made sounds) and 2 factory chains (explained later) are written to it. This takes a few seconds.

After the startup screen, the **PLAY screen** appears. This is MORPHASE's "home screen".

![PLAY screen](images/screens/play.png)

| Area | Shows |
|---|---|
| Top row | Patch name (large), its position in the list (e.g. `1/32`), the algorithm and the number of notes sounding (e.g. `Algo 5  0/12`) |
| Row below | The notes sounding (one box per voice; with ARP On, the ARP's chord) |
| Center left | The output waveform (scope). Above it: GMorph and Feedback values, the FX letters (`C D R X V`), `GL`, the chain state and more |
| Bottom left | Pitch bend bar, ARP and LFO state, the `MIDI` `HOLD` `OCT` `TRN` `THR` indicators |
| Right column | Tilt (IMU) pad and X / Y state, tilt destinations, master volume (`VOL`) |

The tabs `PLAY` `OP` `ALGO` `ARP` `LFO` `FX` `SET` run across the top of the screen.

---

## 3. Connect

### Standard build (Unit MIDI)

1. Set the Unit MIDI DIP switch to **BYPASS** and the unit's Grove 5V switch to **5VOUT**
2. Plug the Unit MIDI into the unit's Grove port (G1 / G2) and connect it to your keyboard's MIDI OUT
3. Play a key. If `MIDI` lights at the bottom left of the PLAY screen, MIDI is coming in

The receive channel starts as **Omni** (all channels).

### BLE build (Bluetooth MIDI)

1. When powered on, the unit waits for a Bluetooth MIDI connection under the name `MORPHASE`. A dim **`b`** appears right of the patch name on the PLAY screen
2. On iPhone / iPad, choose `MORPHASE` **inside the app** (its Bluetooth MIDI connection screen), not in the phone's Bluetooth settings
3. Once connected, it changes to a bright **`B`**

![PLAY screen on the BLE build (B right of the name)](images/screens/play_ble.png)

---

## 4. Play your first sound

Play a few keys. The first sound is `01_EPiano` (electric piano). The harder you play, the brighter and louder it gets.

### Choosing a patch (sound)

- **On the unit**: press `Tab` a few times to reach the **SET** tab → `Enter` on `Patch` → `Enter` on `Load` → choose with `;` `.` and press `Enter`
- **From MIDI**: Program Change 0–31 from your keyboard selects factory patches 01–32

### Five to try first

| Patch | What it sounds like |
|---|---|
| `01_EPiano` | Electric piano. Its character follows how hard you play |
| `04_Organ` | Organ. Sounds as long as you hold the keys |
| `13_MorphLd` | Mono lead. Tilt the unit to change the tone (next chapter) |
| `23_TiltPad` | Wide pad. Tilt to move the tone |
| `25_Kick` | Kick drum. Play low keys around C1–C2 |

### Basic keys

| Key | Action |
|---|---|
| `K` / `L` | Volume down / up (hold to keep changing) |
| `;` / `.` | Octave up / down (PLAY screen) |
| `,` / `/` | Transpose in semitones (PLAY screen) |
| `Z` / `X` | Pitch bend down / up while held |
| `D` | Note hold (keeps the notes you hold sounding until you press it again) |
| `Shift+H` | Show / hide the help for the current screen |
| `G0` (the unit's G0 button) | Jump between the PLAY screen and the last editing tab you used |

![PLAY screen key map](images/play_keymap_en.svg)

---

## 5. Tilt the unit

The Cardputer ADV has a tilt sensor (IMU). **It starts Off**, so turn it on first.

| Key | Action |
|---|---|
| `Shift+A` | X axis (left/right tilt) On / Off |
| `Shift+S` | Y axis (front/back tilt) On / Off |
| `A` / `S` | Freeze (hold) / release the current tilt of that axis |

Once it's on, tilting changes **the tone (waveform) on the X axis and the grit (feedback) on the Y axis** in most patches. Some patches, like `13_MorphLd` and `23_TiltPad`, have their own tilt movements built in. `32_Drone` falls apart as you tilt it.

![SET > IMU](images/screens/imu.png)

What the tilt moves is set by `XTarget` / `YTarget` (destination) and `XDepth` / `YDepth` (depth) in `SET > IMU`.

---

## 6. Add and remove FX

MORPHASE has five effects (Chorus / Delay / Reverb / Crush / Drive). While playing, these keys add or remove them temporarily.

| Key | Effect | Letter on PLAY |
|---|---|---|
| `q` | Chorus (shimmer and width) | `C` |
| `w` | Delay (echo) | `D` |
| `e` | Reverb (room) | `R` |
| `r` | Crush (digital grit) | `X` |
| `t` | Drive (distortion) | `V` |

Effects the patch uses are temporarily removed, and effects it doesn't use are temporarily added. This isn't saved; reloading the patch puts it back.

**MORPHASE does not distort unless you use the distortion effects (Drive, Crush).** When the sound gets too loud, the "output limiter" at the very end only turns the volume down, so nothing clips.

### GLITCH (breaking it up on purpose)

While you hold `g`, the sound breaks up at random (repeating a fragment, playing backwards, chopping, knocking the tone off, and so on). Choose how in `SET > Glitch`.

---

## 7. Change the sound a little

A MORPHASE sound is made of **four operators (OPs)**. An operator is a small oscillator that plays one waveform.

- **Carrier**: an operator that makes the sound you hear
- **Modulator**: an operator that wobbles a carrier's waveform to change its tone (you don't hear it directly)
- **Ratio**: the operator's frequency as a multiple of the note you play

Try it on the **OP tab**.

1. Go to the OP tab with `Tab`. Choose an operator with the number keys `1`–`4` (the screen shows `CARRIER` or `MODULATOR`)
2. Choose a setting with `;` `.` and change it with `,` `/` (hold to move in steps ten times larger)
3. Raising and lowering a modulator's **`Level`** changes the brightness a lot
4. Moving **`Morph`** changes that operator's waveform from a smooth sine towards sharper shapes
5. On the **`A` `D` `S` `R`** rows, an envelope graph (how the sound starts and fades) appears. Play a key and a white dot travels along the graph

![Envelope graph on the OP tab](images/screens/op_env.png)

However much you change, the factory patch stays as it was unless you save.

---

## 8. Save a patch

Save from `SET > Patch`.

- **Save**: overwrite the current patch (asks to confirm)
- **SaveAs**: save under a name (up to 12 characters: letters, digits, `-` `_` `.`)

Factory patches (names starting with digits, like `01_`) **cannot be overwritten**. Choosing Save opens the screen for saving under another name (e.g. `01_EPiano` → `EPiano2`). If you delete or change them by mistake, `SET > Patch > Factory` restores them all.

---

## 9. Try the morph

The feature MORPHASE is named after. It moves the sound smoothly from patch to patch.

1. Open `SET > Patch > Morph` (titled `MORPH > Slots`). Eight slots hold patches (the "Pads" chain at first)
2. Press `Shift+1`–`Shift+8` to move from the current sound to that slot's patch over `Time` (1 second at first). The PLAY screen shows something like `M>3 45%`
3. To try drums, open `Chains >` and Load `02_Drums` (slot 1 = `25_Kick`, 2 = `26_Tom`)

![MORPH > Slots](images/screens/morph_slots.png)

**The smoothest transitions are between patches with the same algorithm** (the way the operators are connected). With different algorithms, the sound switches over part-way.

To switch from your MIDI keyboard's pads, set `SET > MIDI > PC Mode` to `Morph`; Program Change 0–7 then moves to slots 1–8.

---

## 10. What MORPHASE can do

Here is the full picture, including the features you have already tried.

**The FM engine**
- **4-operator FM with 14 algorithms**: 8 built around the classic 4-operator FM layouts, plus 6 with feedback in different places (for sawtooth, growl and metallic sounds)
- **Waveform morphing**: each operator slides smoothly through 8 waveforms, from a sine to sharp shapes. `GMorph` moves all four together
- **Fixed-frequency operators and a pitch envelope**: build percussion such as kicks, toms and metal hits
- **12-note polyphony** (10 on the BLE build). Mono, legato and glide too

**Patch morph: from sound to sound**
- Moves between patches lined up in 8 slots, blending everything: ratios, envelopes, FX
- Two ways: a **chain**, where a knob, tilt or LFO moves the "position", and **glide**, which slides to a chosen patch with `Shift+1–8` or MIDI pads
- Chains can be saved under a name and switched one after another from a MIDI button

**GLITCH: breaking it up on purpose**
- Repeat the last fragment, play it backwards, chop it (Stutter / Reverse / Gate)
- Knock the tone off for a moment (Chaos)
- Break up the arpeggio rhythm (ratchets, octave jumps, running backwards, rests)
- Trigger it only while holding `g`, all the time, or as much as you tilt the unit

**Theremin (M5Stack Unit ToF4M)**
- **Wave your hand** over a distance sensor to move the sound
- Two modes: `Mod`, where hand distance moves the tone and more, and `Play`, where your hand's height plays a note like an instrument. In `Play`, pitch can glide smoothly or snap to one of 49 scales
- Play it along with the keys (chords with your left hand, theremin with your right)

**Move it with your body**
- **Tilt the unit** to change the tone (IMU; just turn it on and it works with most patches)
- Teach it 4 knobs and 4 buttons on a MIDI controller just by moving them (MIDI Learn)

**Arpeggiator**
- 5 patterns (Up / Down / UpDown / Random / AsPlayed) + **STEP**: 16 steps hold "which note of the held chord", so changing the chord keeps the rhythm and changes only the notes
- Latch, swing, sync to external MIDI clock, 8 saved patterns

**FX, and an output that doesn't clip**
- Crush, Chorus, Delay, Reverb, Drive. Add and remove them while playing with `q w e r t`
- An output limiter at the end: even big chords only get turned down in volume, never bent in shape

**32 factory patches, ready to play**
- From keys, bells, bass, leads and pads to drums and noise

**Two builds**
- Standard (Unit MIDI) and BLE, which also plays over Bluetooth MIDI from an iPhone / iPad (tested app: [KeyPad](https://www.discodsp.com/en/keypad/))

---

## 11. When you want to know more

The Reference Guide covers features such as:

- The 14 algorithms, fixed-frequency operators, the pitch envelope
- The arpeggiator (including step patterns) and ARP glitch
- The LFO and the list of modulation destinations
- Assigning MIDI knobs and buttons (MIDI Learn)
- Saving chains and morphing in detail
- **The theremin** (plug in an M5Stack Unit ToF4M and move the tone or pitch with your hand)
- The list of 32 factory patches and what to try on each

---

## 12. Troubleshooting

### Start with the help

On any screen, `Shift+H` shows the keys for that screen. Press `Shift+H` again to hide it.

![Help on the PLAY screen](images/screens/help.png)

### No sound

- Is the master volume (`VOL` at the bottom right of the PLAY screen) at 0? Raise it with `L`
- Is MIDI coming in? Play a key and see whether `MIDI` lights. If not, check the Unit MIDI's orientation, its DIP switch (BYPASS) and the Grove 5V switch (5VOUT)
- Does the receive channel (`SET > MIDI > Channel`) match your keyboard's send channel? (Omni at first)
- Is a microSD card inserted? (Without a card the initial sound plays, but nothing is saved)

### The unit's keys stopped responding

Very rarely, for example after fast typing, the unit's keys can stop responding entirely. This is a known hardware quirk of the Cardputer ADV's keyboard chip (TCA8418). **MORPHASE detects it and restarts itself after about 30 seconds.** Just wait.

### The BLE build won't connect

- Are you connecting from inside the app? (MIDI does not connect from the phone's Bluetooth settings screen)
- If you have registered the same Cardputer as a Bluetooth device with other firmware before, remove that old entry in the phone's Bluetooth settings
- Is `SET > MIDI > BLE` set to `On`? (Changing it needs a restart)

### Still stuck?

Open a GitHub Issue or contact the developer (@Tokagetchi).

---

*MORPHASE is an FM synthesizer made by an individual developer. Have fun moving the sound.*
