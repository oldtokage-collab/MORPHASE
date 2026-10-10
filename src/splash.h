// MORPHASE - splash.h
// Boot splash (v0.89): the engine's own waveforms morphing in a band,
// then the name, a short glitch, and the PLAY screen. Runs in setup()
// before the canvas, the audio task and BLE exist, and uses no buffer:
// it draws straight to the panel (v0.89T2).

#ifndef MORPHASE_SPLASH_H
#define MORPHASE_SPLASH_H

void splashBegin();                     // at power-on: a flat line in the theme colour (NVS copy)
void splashStartWave();                 // once the SD card has been looked for: the wave and the morph
void splashFactoryProgress(int phase);  // bootProgress hook: 0 install starts, 1 one file, 2 install done
void splashFinish();                    // plays the rest (or skips it on a key)

#endif // MORPHASE_SPLASH_H
