/*
 * ble_ant_bridge.ino — Phase 3: BLE CPS parser → ANT+ Bicycle Power TX.
 *
 * Phase 1 (scan → connect → subscribe) and Phase 2 (CPS 0x2A63 power + cadence
 * parser) feed parsed data into a shared BridgeData (bridge_core). Phase 3 adds
 * an ANT+ Bicycle Power master transmitter (ant_power_tx) so Garmin head units
 * recognize the SmartSpin2k as a "Bicycle Power" sensor:
 *   - sd_ant_* channel: Master TX, Device Type 0x0B, RF 57, period 8182 (~4 Hz).
 *   - Data Page 0x10 (Standard Power) rotated with common pages 0x50/0x51.
 *   - EVENT_TX serviced by polling sd_ant_event_get() from loop().
 *
 * Board: "SuperMini nRF52840 (S340)" (see bsp-s340/).
 *
 * Pass criteria:
 *   [1] Serial: "[BLE] subscribed to CPM 0x2A63" and "[ANT] BPWR TX started"
 *   [2] "[CPM] NNN W  MM RPM" lines stream while SmartSpin2k runs
 *   [3] An ANT+ receiver (USB stick / Garmin) sees "Bicycle Power" with live power
 */

#include <bluefruit.h>
#include "config.h"
#include "bridge_core.h"
#include "ant_power_tx.h"

// CODING_GUIDELINES state enum (subset used this phase).
enum class BridgeState : uint8_t {
    INIT,
    SCANNING,
    CONNECTING,
    DISCOVERING,
    SUBSCRIBED,
    RUNNING,
    RECONNECT_WAIT
};

static volatile BridgeState bridgeState = BridgeState::INIT;

// BLE Central GATT client objects.
static BLEClientService        cps(UUID16_SVC_CYCLING_POWER);
static BLEClientCharacteristic cpm(UUID16_CHR_CYCLING_POWER_MEASURE);

// Reconnect backoff bookkeeping.
static uint32_t reconnectBackoffMs = RECONNECT_BACKOFF_MIN_MS;
static uint32_t nextScanAtMs       = 0;

// Crank revolution state for cadence calculation (reset on each new connection).
static bool     prevCrankValid         = false;
static uint16_t prevCumCrankRevs       = 0;
static uint16_t prevLastCrankEventTime = 0;

static void startScanning();
static void scanCallback(ble_gap_evt_adv_report_t* report);
static void connectCallback(uint16_t connHandle);
static void disconnectCallback(uint16_t connHandle, uint8_t reason);
static void cpmNotifyCallback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len);
static void serviceLed();

void setup() {
    Serial.begin(SERIAL_BAUD);
    while (!Serial && millis() < SERIAL_WAIT_MS) delay(10);

    Serial.println("[BLE] BLE→ANT+ bridge — Phase 2 (CPS parser)");

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 1 - LED_STATE_ON);

    // 0 peripheral, 1 central connection.
    Bluefruit.begin(0, 1);
    Bluefruit.setTxPower(4);
    Bluefruit.setName("BLE-ANT-Bridge");
    Bluefruit.autoConnLed(false);   // we drive the status LED ourselves

    // GATT client: register the characteristic's notify handler, then init the
    // service + characteristic so they can be discovered after connect.
    cps.begin();
    cpm.setNotifyCallback(cpmNotifyCallback);
    cpm.begin();

    Bluefruit.Central.setConnectCallback(connectCallback);
    Bluefruit.Central.setDisconnectCallback(disconnectCallback);

    // Scanner: we filter by name inside the callback, so don't auto-restart here
    // (backoff is driven from loop()).
    Bluefruit.Scanner.setRxCallback(scanCallback);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.setInterval(SCAN_INTERVAL, SCAN_WINDOW);
    Bluefruit.Scanner.useActiveScan(SCAN_ACTIVE);
    Bluefruit.Scanner.filterRssi(-80);   // ignore very weak adv to cut noise

    // ANT+ Bicycle Power transmitter — must come after Bluefruit.begin() so the
    // S340 SoftDevice is already enabled. BLE bridging continues even if it fails.
    if (!AntPowerTx::begin()) {
        Serial.println("[ANT] BPWR init FAILED — continuing BLE-only");
    }

    startScanning();
}

void loop() {
    // Backoff-driven re-scan after a disconnect.
    if (bridgeState == BridgeState::RECONNECT_WAIT && millis() >= nextScanAtMs) {
        startScanning();
    }
    AntPowerTx::service();   // drain ANT EVENT_TX → load next BPWR page
    serviceLed();
}

// Begin (or resume) scanning indefinitely.
static void startScanning() {
    bridgeState = BridgeState::SCANNING;
    Serial.printf("[BLE] scanning for '%s'\n", TARGET_DEVICE_NAME);
    Bluefruit.Scanner.start(0);   // 0 = no timeout
}

// Called for each advertising report. Connect only if the Complete/Short Local
// Name matches the target; otherwise keep scanning.
static void scanCallback(ble_gap_evt_adv_report_t* report) {
    uint8_t nameBuf[32 + 1] = { 0 };

    bool haveName =
        Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME,
                                            nameBuf, sizeof(nameBuf) - 1) ||
        Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME,
                                            nameBuf, sizeof(nameBuf) - 1);

    if (haveName && strcmp((const char*)nameBuf, TARGET_DEVICE_NAME) == 0) {
        Serial.printf("[BLE] found '%s' — connecting\n", nameBuf);
        bridgeState = BridgeState::CONNECTING;
        Bluefruit.Central.connect(report);
        return;   // scanner is paused; connectCallback (or disconnect) resumes flow
    }

    // Not our device — resume scanning for the next report.
    Bluefruit.Scanner.resume();
}

// Connected: discover CPS 0x1818, then CPM 0x2A63, then enable notifications.
static void connectCallback(uint16_t connHandle) {
    Serial.println("[BLE] connected");
    bridgeState = BridgeState::DISCOVERING;

    if (!cps.discover(connHandle)) {
        Serial.println("[BLE] CPS 0x1818 not found — disconnecting");
        Bluefruit.disconnect(connHandle);
        return;
    }

    if (!cpm.discover()) {
        Serial.println("[BLE] CPM 0x2A63 not found — disconnecting");
        Bluefruit.disconnect(connHandle);
        return;
    }

    if (!cpm.enableNotify()) {
        Serial.println("[BLE] could not enable CPM notifications — disconnecting");
        Bluefruit.disconnect(connHandle);
        return;
    }

    prevCrankValid = false;   // reset cadence state on fresh connection
    bridgeState = BridgeState::SUBSCRIBED;
    reconnectBackoffMs = RECONNECT_BACKOFF_MIN_MS;
    Serial.println("[BLE] subscribed to CPM 0x2A63 — waiting for notifications");
}

// Disconnected: schedule a backoff re-scan (no blocking here).
static void disconnectCallback(uint16_t connHandle, uint8_t reason) {
    (void)connHandle;
    Serial.printf("[BLE] disconnected, reason = 0x%02X\n", reason);

    nextScanAtMs = millis() + reconnectBackoffMs;
    Serial.printf("[BLE] re-scan in %lu ms\n", (unsigned long)reconnectBackoffMs);

    // Exponential backoff up to the cap, for the *next* failure.
    reconnectBackoffMs *= 2;
    if (reconnectBackoffMs > RECONNECT_BACKOFF_MAX_MS) {
        reconnectBackoffMs = RECONNECT_BACKOFF_MAX_MS;
    }
    bridgeState = BridgeState::RECONNECT_WAIT;
}

// CPS 0x2A63 parser — Bluetooth SIG spec §3.65 (Cycling Power Measurement).
// Optional field layout (flag bit → bytes appended after the fixed 4-byte header):
//   0: Pedal Power Balance (1 byte)       1: Pedal Power Balance Reference (flag only)
//   2: Accumulated Torque (2 bytes)       3: Accumulated Torque Source (flag only)
//   4: Wheel Revolution Data (6 bytes: uint32 cumWheelRevs + uint16 lastWheelEventTime/2048s)
//   5: Crank Revolution Data (4 bytes: uint16 cumCrankRevs + uint16 lastCrankEventTime/1024s)
static void cpmNotifyCallback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    (void)chr;
    bridgeState = BridgeState::RUNNING;

    if (len < 4) {
        Serial.printf("[CPM] short notification (%u bytes) — ignored\n", len);
        return;
    }

    uint16_t flags = (uint16_t)(data[0] | (data[1] << 8));
    int16_t  power = (int16_t)(data[2] | (data[3] << 8));

    // Walk optional fields to reach the crank data offset.
    uint8_t offset = 4;
    if (flags & (1u << 0)) offset += 1;   // Pedal Power Balance
    if (flags & (1u << 2)) offset += 2;   // Accumulated Torque
    if (flags & (1u << 4)) offset += 6;   // Wheel Revolution Data

    uint16_t cadence = 0;

    if ((flags & (1u << 5)) && ((uint16_t)(offset + 4) <= len)) {
        uint16_t cumCrank  = (uint16_t)(data[offset]     | (data[offset + 1] << 8));
        uint16_t crankTime = (uint16_t)(data[offset + 2] | (data[offset + 3] << 8));

        if (prevCrankValid) {
            uint16_t dRevs = (uint16_t)(cumCrank  - prevCumCrankRevs);
            uint16_t dTime = (uint16_t)(crankTime - prevLastCrankEventTime);
            if (dRevs > 0 && dTime > 0) {
                // crankTime resolution = 1/1024 s  →  RPM = dRevs × 60 × 1024 / dTime
                cadence = (uint16_t)((uint32_t)dRevs * 60u * 1024u / dTime);
            }
            // dRevs == 0: crank stopped → cadence stays 0
        }

        prevCumCrankRevs       = cumCrank;
        prevLastCrankEventTime = crankTime;
        prevCrankValid         = true;
    }

    // Publish to the shared store; the ANT+ TX path reads it on each EVENT_TX.
    bridgeUpdateFromCps(power, cadence);

    Serial.printf("[CPM] %4d W  %3u RPM\n", power, cadence);
}

// Non-blocking status LED: blink while scanning/connecting, solid when running.
static void serviceLed() {
    static uint32_t last = 0;
    uint32_t now = millis();

    switch (bridgeState) {
        case BridgeState::RUNNING:
        case BridgeState::SUBSCRIBED:
            digitalWrite(LED_BUILTIN, LED_STATE_ON);   // solid on
            break;

        case BridgeState::CONNECTING:
        case BridgeState::DISCOVERING:
            if (now - last >= LED_BLINK_BUSY_MS) { last = now; digitalToggle(LED_BUILTIN); }
            break;

        default:   // SCANNING / RECONNECT_WAIT / INIT
            if (now - last >= LED_BLINK_SCAN_MS) { last = now; digitalToggle(LED_BUILTIN); }
            break;
    }
}
