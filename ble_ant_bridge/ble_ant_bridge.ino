/*
 * ble_ant_bridge.ino — Phase 1: BLE Central scan → connect → subscribe.
 *
 * Scans for a peripheral advertising the Complete Local Name "SmartSpin2k",
 * connects, discovers Cycling Power Service (0x1818), subscribes to the Cycling
 * Power Measurement characteristic (0x2A63), and dumps each notification to
 * Serial. Reconnects with exponential backoff on disconnect.
 *
 * Board: "SuperMini nRF52840 (S340)" (see bsp-s340/). BLE-only for now — the
 * S340 ANT+ side is wired up in Phase 3. No CPS field parsing yet: Phase 2 adds
 * the power/cadence decode. This phase only proves notifications flow.
 *
 * Pass criteria:
 *   [1] Serial: "[BLE] scanning for 'SmartSpin2k'"
 *   [2] Serial: "[BLE] connected" then "[BLE] subscribed to CPM 0x2A63"
 *   [3] Serial: "[CPM] <n> bytes: ..." lines stream while the SmartSpin2k runs
 */

#include <bluefruit.h>
#include "config.h"

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

static void startScanning();
static void scanCallback(ble_gap_evt_adv_report_t* report);
static void connectCallback(uint16_t connHandle);
static void disconnectCallback(uint16_t connHandle, uint8_t reason);
static void cpmNotifyCallback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len);
static void serviceLed();

void setup() {
    Serial.begin(SERIAL_BAUD);
    while (!Serial && millis() < SERIAL_WAIT_MS) delay(10);

    Serial.println("[BLE] BLE→ANT+ bridge — Phase 1 (BLE Central)");

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

    startScanning();
}

void loop() {
    // Backoff-driven re-scan after a disconnect.
    if (bridgeState == BridgeState::RECONNECT_WAIT && millis() >= nextScanAtMs) {
        startScanning();
    }
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

    bridgeState = BridgeState::SUBSCRIBED;
    reconnectBackoffMs = RECONNECT_BACKOFF_MIN_MS;   // healthy link → reset backoff
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

// CPM notification handler. Phase 1: raw dump + always-present power word.
// Full flag/crank decode + cadence math is Phase 2.
static void cpmNotifyCallback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    (void)chr;
    bridgeState = BridgeState::RUNNING;

    if (len < 4) {
        Serial.printf("[CPM] short notification (%u bytes)\n", len);
        return;
    }

    // Bytes 0-1: Flags (uint16 LE); Bytes 2-3: Instantaneous Power (sint16 LE, W).
    uint16_t flags = data[0] | (data[1] << 8);
    int16_t  power = (int16_t)(data[2] | (data[3] << 8));
    bool crankPresent = flags & (1 << 5);

    Serial.printf("[CPM] %u bytes: ", len);
    for (uint16_t i = 0; i < len; i++) Serial.printf("%02X ", data[i]);
    Serial.printf("| power=%d W flags=0x%04X crank=%s\n",
                  power, flags, crankPresent ? "yes" : "no");
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
