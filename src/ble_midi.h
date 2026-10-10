// MORPHASE - ble_midi.h
// BLE MIDI input (v0.77, BLE build only: MORPHASE_BLE). BLE is one more way
// for MIDI bytes to arrive and nothing else: the packets lose their BLE
// timestamps here and the plain bytes go through midi.cpp's parser (its own
// parser state), so every MIDI feature works the same over BLE.
#pragma once
#include <stdint.h>

#ifdef MORPHASE_BLE
enum BleMidiState : uint8_t { BLE_STATE_OFF = 0, BLE_STATE_ADV, BLE_STATE_CONN, BLE_STATE_FAIL };

// setup(): starts the BT stack. Only called when the setting is On; with it
// Off the stack never starts (no BT task, no CPU, no runtime memory).
void bleMidiBegin();

// loop(), from midiPoll(): the next byte, timestamps removed. False when
// nothing is waiting.
bool bleMidiRead(uint8_t *b);
// loop(): true once after each disconnect (all notes off, sustain up).
bool bleMidiTakeDisconnect();

BleMidiState bleMidiState();
const char  *bleMidiStateName();     // "Off" / "Adv" / "Conn" / "Fail"
uint32_t     bleMidiRxBytes();       // MIDI bytes received (after the timestamps)
uint32_t     bleMidiOverflows();     // bytes dropped because the ring was full
uint32_t     bleMidiHostStackUsed(); // host task: deepest stack use so far, bytes (0 = not started)
uint32_t     bleMidiHostStackSize(); // host task: its stack size, bytes
#endif
