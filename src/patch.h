// MORPHASE - patch.h
// Patch / pattern / settings storage on the SD card, defaults, factory
// patches, reset and randomize.
//
// SD layout (spec 9.1):
//   /MORPHASE/settings.json      everything (sound + session + device)
//   /MORPHASE/Patch/<name>.json  sound only (patch keys)
//   /MORPHASE/Pattern/N.json     ARP step pattern slots 0-7

#ifndef MORPHASE_PATCH_H
#define MORPHASE_PATCH_H

#include "config.h"

// Defaults
void patchInitDefaults(Patch &p);          // the "Init" sound (algorithm 4, OP1 only)
void sysInitDefaults();
void patchResetTone();                     // current patch -> Init; FX mixes zeroed; FX lines cleared (spec 9.4)
void patchRandomize();

// SD
bool sdInit();
// v0.92T2: the theme number copied into NVS for the boot splash. Read at
// power-on (-1 = none yet / out of range); written from the deferred save
// only when it differs from sys.theme.
int  nvsReadTheme();
bool nvsThemeStale();
void nvsWriteTheme();
bool sdReady();
void ensureFolders();
void ensureFactoryPatches();               // boot: whole set into an empty folder; all rewritten when the set version rises (v0.75)
int  writeFactoryPatches(bool overwrite);  // count written, -1 on an SD error; SETTING > Patch > Factory restores all (overwrite)
// Boot only (v0.89): set while setup() does its SD work, so the splash keeps
// moving - 0 / 2 around the factory install, 1 per file written, listed or read.
extern void (*bootProgress)(int phase);
bool patchIsFactoryName(const char *name); // v0.75: factory names cannot be saved over from the device
// v0.76: the demo morph chains, patch names in slot order. which: 0 = the pads
// chain (also written to a card with no morph.txt), 1 = the drum pair (v0.79).
constexpr int DEMO_CHAIN_COUNT = 2;
int  factoryDemoChain(const char *const **names, int which = 0);
const char *factoryDemoLabel(int which);            // "Pads" / "Drums"

// Chains (v0.83): /MORPHASE/Chain/<name>.txt - 8 patch names (blank = Off)
// and an optional time=<ms> line. 01_Pads / 02_Drums are factory chains.
bool chainIsFactoryName(const char *name);
int  chainList(char (*out)[PATCH_NAME_LEN + 1], int maxN);   // factory first, then by name
bool chainRead(const char *name, char (*slots)[PATCH_NAME_LEN + 1], int *timeMs);   // timeMs -1 = none
bool chainWrite(const char *name, const char *const *slots, int timeMs);           // timeMs -1 = none
bool chainDelete(const char *name);
int  writeFactoryChains(bool overwrite);                     // count, -1 on an SD error

// One writer for every file (v0.72): build the text, then commit. Too big or
// an SD error aborts without touching the old file and leaves a notice for
// the UI ("<what> NOT saved: too big / SD error").
void        fileWriteBegin();
void        fileWritef(const char *fmt, ...);
bool        fileWriteCommit(const char *path, const char *what);
void        fileRecoverTmp(const char *path);     // boot: a lone <path>.tmp becomes <path>
const char *patchTakeSaveNotice();                // UI: the last failure message, once

// Settings (whole state)
bool saveSettings();
// v0.90T3: "save soon" - leaving a screen sets this instead of saving on the
// spot; main.cpp's deferredPerformSave() writes 3 s after the last request.
extern volatile bool saveRequested;
extern uint32_t patchLoadSeq;                    // v0.90T2: +1 per patch loaded into `patch` (deferred save)
bool loadSettings();

// Patch bank
int         scanPatches();                 // fills the name list, returns count
int         patchCount();
const char *patchNameAt(int index);
int         patchIndexOfName(const char *name);   // -1 if absent
bool        savePatchFile(const char *name);      // current sound -> file; sets patch.name
bool        loadPatchFile(const char *name);
bool        loadPatchInto(const char *name, Patch &out);  // chain slots: never touches `patch`
bool        deletePatchFile(const char *name);
bool        renamePatchFile(const char *oldName, const char *newName);
bool        patchNameValid(const char *name);

// Pattern bank (16 steps + length + chordWrap)
bool savePatternSlot(int slot);
bool loadPatternSlot(int slot);

// Diagnostics ring log (spec 11.1): record cheaply, dump on request.
void diagLog(const char *fmt, ...);
void diagDump();

#endif // MORPHASE_PATCH_H
