/*
 * ble_ant_bridge.ino — BLE CPS parser → ANT+ FE-C trainer TX + ERG control.
 *
 * Phase A (TX): BLE CPS 0x2A63 → bridge_core → ANT+ FE-C pages 16/25/54/80/81.
 * Phase B (RX/control):
 *   - ANT+ RX EVENT_RX → handleControlPage() parses Garmin's acknowledged
 *     pages 48/49/51 → bridgeSetControl() → ControlCommand in bridge_core.
 *   - loop() serviceControl() → bridgeConsumeControl() → FTMS 0x2AD9 write
 *     (SetTargetPower 0x05 / SetResistance 0x04 / SetSimulation 0x11).
 *   - Page 71 (Command Status) broadcast to confirm receipt to Garmin.
 * (Legacy BPWR transmitter, ant_power_tx.*, remains in-tree but unwired.)
 *
 * Board: "SuperMini nRF52840 (S340)" (see bsp-s340/).
 *
 * Pass criteria Phase B:
 *   [1] "[BLE] FTMS 0x2AD9 ready" in serial after connecting to SmartSpin2k
 *   [2] "[ANT] Page 49 ERG target: NNN W" when Garmin sends an ERG command
 *   [3] "[FTMS] SetTargetPower NNN W" confirming the write to SmartSpin2k
 *   [4] SmartSpin2k resistance changes to match Garmin's ERG target
 */

#include <bluefruit.h>
#include "config.h"
#include "bridge_core.h"
#include "ant_fec.h"

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

// BLE Central GATT client objects — CPS (data source) + FTMS (control sink).
static BLEClientService        cps(UUID16_SVC_CYCLING_POWER);
static BLEClientCharacteristic cpm(UUID16_CHR_CYCLING_POWER_MEASURE);

static BLEClientService        ftms(UUID16_SVC_FTMS);
static BLEClientCharacteristic ftmsCP(UUID16_CHR_FTMS_CONTROL_POINT);
static bool                    ftmsCPReady = false;  // true once CCCD + Request Control succeed

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
static void serviceControl();
static void serviceLed();

void setup() {
    Serial.begin(SERIAL_BAUD);
    while (!Serial && millis() < SERIAL_WAIT_MS) delay(10);

    Serial.println("[BLE] BLE→ANT+ bridge — FE-C trainer TX (Phase A)");

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, 1 - LED_STATE_ON);

    // 0 peripheral, 1 central connection.
    Bluefruit.begin(0, 1);
    Bluefruit.setTxPower(4);
    Bluefruit.setName("BLE-ANT-Bridge");
    Bluefruit.autoConnLed(false);   // we drive the status LED ourselves

    // CPS client (data source): notify handler registered before begin().
    cps.begin();
    cpm.setNotifyCallback(cpmNotifyCallback);
    cpm.begin();

    // FTMS client (control sink): write-only use, no notify handler needed.
    ftms.begin();
    ftmsCP.begin();

    Bluefruit.Central.setConnectCallback(connectCallback);
    Bluefruit.Central.setDisconnectCallback(disconnectCallback);

    // Scanner: we filter by name inside the callback, so don't auto-restart here
    // (backoff is driven from loop()).
    Bluefruit.Scanner.setRxCallback(scanCallback);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.setInterval(SCAN_INTERVAL, SCAN_WINDOW);
    Bluefruit.Scanner.useActiveScan(SCAN_ACTIVE);
    Bluefruit.Scanner.filterRssi(-80);   // ignore very weak adv to cut noise

    // ANT+ FE-C trainer transmitter — must come after Bluefruit.begin() so the
    // S340 SoftDevice is already enabled. BLE bridging continues even if it fails.
    if (!AntFec::begin()) {
        Serial.println("[ANT] FE-C init FAILED — continuing BLE-only");
    }

    startScanning();
}

void loop() {
    // Backoff-driven re-scan after a disconnect.
    if (bridgeState == BridgeState::RECONNECT_WAIT && millis() >= nextScanAtMs) {
        startScanning();
    }
    AntFec::service();       // drain ANT events: EVENT_TX → TX, EVENT_RX → control
    serviceControl();        // consume pending ControlCommand → write to FTMS CP
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

    // FTMS 0x1826 — optional control write path. Not finding it is non-fatal;
    // the bridge still forwards power + cadence via ANT+ without ERG control.
    ftmsCPReady = false;
    if (ftms.discover(connHandle) && ftmsCP.discover()) {
        // FTMS spec requires indications to be enabled before writing opcodes.
        if (ftmsCP.enableNotify()) {
            uint8_t reqCtrl = FTMS_OP_REQUEST_CONTROL;
            ftmsCP.write(&reqCtrl, 1);
            ftmsCPReady = true;
            Serial.println("[BLE] FTMS 0x2AD9 ready — ERG control enabled");
        } else {
            Serial.println("[BLE] FTMS CP indicate enable failed — control disabled");
        }
    } else {
        Serial.println("[BLE] FTMS 0x1826 not found — control disabled");
    }
}

// Disconnected: clear FTMS state and schedule a backoff re-scan (no blocking here).
static void disconnectCallback(uint16_t connHandle, uint8_t reason) {
    (void)connHandle;
    ftmsCPReady = false;
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

// Consume the pending ControlCommand from bridge_core and write the matching
// FTMS opcode to SmartSpin2k's Control Point. Silently no-ops when no command
// is waiting or FTMS was not discovered on the current connection.
static void serviceControl() {
    ControlCommand cmd;
    if (!ftmsCPReady || !bridgeConsumeControl(&cmd)) return;

    switch (cmd.mode) {
        case ControlMode::ERG: {
            // SetTargetPower (0x05): opcode + uint16 LE watts
            uint8_t buf[3] = {
                FTMS_OP_SET_TARGET_POWER,
                (uint8_t)(cmd.targetPowerW & 0xFF),
                (uint8_t)(cmd.targetPowerW >> 8)
            };
            ftmsCP.write(buf, sizeof(buf));
            Serial.printf("[FTMS] SetTargetPower %u W\n", cmd.targetPowerW);
            break;
        }
        case ControlMode::RESISTANCE: {
            // SetTargetResistanceLevel (0x04): opcode + uint8 (0-100, 0.1-unit res.)
            uint8_t buf[2] = { FTMS_OP_SET_RESISTANCE_LEVEL, cmd.resistancePct };
            ftmsCP.write(buf, sizeof(buf));
            Serial.printf("[FTMS] SetResistance %u%%\n", cmd.resistancePct);
            break;
        }
        case ControlMode::SIMULATION: {
            // SetIndoorBikeSimulationParameters (0x11): wind(int16) + grade(int16) + Cr + CwA
            uint8_t buf[7] = {
                FTMS_OP_SET_SIMULATION_PARAMS,
                (uint8_t)((uint16_t)cmd.sim.windSpeedMms & 0xFF),
                (uint8_t)((uint16_t)cmd.sim.windSpeedMms >> 8),
                (uint8_t)((uint16_t)cmd.sim.gradeHundredths & 0xFF),
                (uint8_t)((uint16_t)cmd.sim.gradeHundredths >> 8),
                cmd.sim.crCoeff,
                cmd.sim.cwaCoeff
            };
            ftmsCP.write(buf, sizeof(buf));
            Serial.printf("[FTMS] SetSimulation grade=%.2f%%\n",
                          cmd.sim.gradeHundredths * 0.01f);
            break;
        }
        default: break;
    }
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
