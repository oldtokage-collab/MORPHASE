// MORPHASE - ble_midi.cpp
// BLE MIDI input (v0.77, BLE build only). See ble_midi.h.
//
// Decisions from BLE stage 0 (docs/MORPHASE_REPORT_BLE_STAGE0_FINAL.md) and
// stage 1 (docs/MORPHASE_BLE_MIDI_STAGE1.docx):
//  * NimBLE-Arduino 2.5.1, peripheral only, one connection; the host task
//    runs on core 1 (audioTask owns core 0). The BT controller itself is
//    prebuilt in the Arduino core and runs on core 0 - the reason the BLE
//    build plays 10 voices.
//  * MORPHASE's own BLE address: a random static address made from the
//    chip's BT MAC (fixed per unit, never the public address). Phones keep
//    pairing keys per address; other firmware on the same Cardputer
//    (Meshtastic, ...) may have paired on the public one, and the phone then
//    drops the link right after connecting ("Peer removed pairing
//    information"). It also never overwrites those keys.
//  * No pairing, no encryption. No connection-interval request (the spec's
//    7.5-15ms breaks Apple's rule; the phone's own choice worked).
//  * The write callback (NimBLE host task) only strips the timestamps and
//    queues the bytes; loop() takes them in midiPoll(), where DIN MIDI is read.
#ifdef MORPHASE_BLE
#include "ble_midi.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>

#define BLE_MIDI_SERVICE_UUID "03B80E5A-EDE8-4B33-A751-6CE34EC4C700"
#define BLE_MIDI_CHAR_UUID    "7772E5DB-3868-4112-A1A9-F2669D106BF3"
static const char *BLE_NAME = "MORPHASE";

// ---- byte ring: host task in, loop() out ----------------------------------
constexpr int BLE_RING = 512;                       // a power of two
static uint8_t  ring[BLE_RING];
static volatile uint16_t ringHead = 0, ringTail = 0;   // head: writer, tail: reader
static portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t rxBytes = 0, overflows = 0;

static volatile BleMidiState state = BLE_STATE_OFF;
static volatile bool disconnected = false;

// Apple BLE MIDI packet -> plain MIDI bytes. byte 0 is the header (bit 7
// set, bit 6 clear). After it, a byte with bit 7 set is a timestamp unless
// the byte before it was one - then it is a status (or realtime) byte. Data
// bytes go through as they are, which also covers running status with its
// timestamp left out, and SysEx running on over several packets.
static void queuePacket(const uint8_t *p, size_t n) {
    if (n < 2 || (p[0] & 0xC0) != 0x80) return;    // not a BLE MIDI packet
    bool prevTs = false;
    portENTER_CRITICAL(&ringMux);
    uint16_t h = ringHead;
    for (size_t i = 1; i < n; i++) {
        uint8_t b = p[i];
        if (b & 0x80) {
            if (!prevTs) { prevTs = true; continue; }   // timestamp
            prevTs = false;
        } else {
            prevTs = false;
        }
        uint16_t next = (h + 1) & (BLE_RING - 1);
        if (next == ringTail) { overflows++; continue; }   // full: drop and count
        ring[h] = b; h = next;
        rxBytes++;
    }
    ringHead = h;
    portEXIT_CRITICAL(&ringMux);
}

bool bleMidiRead(uint8_t *b) {
    bool got = false;
    portENTER_CRITICAL(&ringMux);
    if (ringTail != ringHead) {
        *b = ring[ringTail];
        ringTail = (ringTail + 1) & (BLE_RING - 1);
        got = true;
    }
    portEXIT_CRITICAL(&ringMux);
    return got;
}

bool bleMidiTakeDisconnect() {
    if (!disconnected) return false;
    disconnected = false;
    return true;
}

BleMidiState bleMidiState() { return state; }
const char *bleMidiStateName() {
    switch (state) {
        case BLE_STATE_ADV:  return "Adv";
        case BLE_STATE_CONN: return "Conn";
        case BLE_STATE_FAIL: return "Fail";
        default:             return "Off";
    }
}
// NimBLE's host task stack (v0.78, kept on Info at the spec chat's request):
// running out of it crashes rather than glitching, so the deepest use so far
// is shown. ESP-IDF counts stacks in bytes. Sized by measurement in v0.77
// (2,088 of 3,072 used at the deepest with fast playing and reconnects).
uint32_t bleMidiHostStackSize() { return CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE; }
uint32_t bleMidiHostStackUsed() {
    static TaskHandle_t h = nullptr;
    if (!h && state != BLE_STATE_OFF) h = xTaskGetHandle("nimble_host");
    if (!h) return 0;
    return bleMidiHostStackSize() - (uint32_t)uxTaskGetStackHighWaterMark(h);
}
uint32_t bleMidiRxBytes()   { return rxBytes; }
uint32_t bleMidiOverflows() { return overflows; }

// ---- NimBLE callbacks (host task, core 1) ---------------------------------
class MidiServerCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &) override { state = BLE_STATE_CONN; }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
        Serial.printf("[BLE] disconnected (reason 0x%X)\n", reason);
        disconnected = true;
        state = BLE_STATE_ADV;
        NimBLEDevice::startAdvertising();
    }
};

class MidiCharCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
        const NimBLEAttValue &v = c->getValue();
        queuePacket(v.data(), v.size());
    }
    // READ answers with no data, as BLE MIDI asks (the last write is not echoed)
    void onRead(NimBLECharacteristic *c, NimBLEConnInfo &) override { c->setValue((const uint8_t *)"", 0); }
};

// MORPHASE's own address (see the top). NimBLE takes it little-endian; a
// static random address has the two top bits set.
static void makeOwnAddr(uint8_t out[6]) {
    static const uint8_t k[6] = {'M', 'O', 'R', 'P', 'H', 'S'};
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);                 // mac[0] = most significant
    for (int i = 0; i < 6; i++) out[i] = mac[5 - i] ^ k[i];
    out[5] |= 0xC0;
}

static bool fail(const char *step) {
    Serial.printf("[BLE] start failed at %s (heap %u, largest %u)\n", step,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    state = BLE_STATE_FAIL;
    return false;
}

static bool startStack() {
    if (!NimBLEDevice::init(BLE_NAME)) return fail("init");
    uint8_t addr[6];
    makeOwnAddr(addr);
    if (!NimBLEDevice::setOwnAddr(addr) || !NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM)) return fail("address");
    Serial.printf("[BLE] address %02X:%02X:%02X:%02X:%02X:%02X (random static)\n",
                  addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    NimBLEServer *srv = NimBLEDevice::createServer();
    if (!srv) return fail("server");
    srv->setCallbacks(new MidiServerCb());
    NimBLEService *svc = srv->createService(BLE_MIDI_SERVICE_UUID);
    if (!svc) return fail("service");
    NimBLECharacteristic *ch = svc->createCharacteristic(
        BLE_MIDI_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    if (!ch) return fail("characteristic");
    ch->setCallbacks(new MidiCharCb());
    if (!srv->start()) return fail("server start");   // starts the services (2.x)

    // Advertisement: flags + the 128-bit service UUID (21 of 31 bytes); the
    // name rides in the scan response.
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->enableScanResponse(true);
    NimBLEAdvertisementData advData, scanData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.addServiceUUID(svc->getUUID());
    scanData.setName(BLE_NAME);
    if (!adv->setAdvertisementData(advData) || !adv->setScanResponseData(scanData)) return fail("advertising data");
    if (!adv->start()) return fail("advertising");
    state = BLE_STATE_ADV;
    return true;
}

void bleMidiBegin() {
    uint32_t h0 = ESP.getFreeHeap(), b0 = ESP.getMaxAllocHeap();
    bool ok = startStack();
    Serial.printf("[BLE] %s: heap %u -> %u, largest %u -> %u\n", ok ? "advertising as MORPHASE" : "not started",
                  (unsigned)h0, (unsigned)ESP.getFreeHeap(), (unsigned)b0, (unsigned)ESP.getMaxAllocHeap());
}
#endif
