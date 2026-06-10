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
static bool                    ftmsCPReady          = false;  // true once CCCD + Request Control succeed
static bool                    trainingActive       = false;  // true between Start and Stop writes
static uint32_t                trainingActiveSinceMs = 0;     // millis() when Start was written; gates ERG settle delay
static uint16_t                lastErgTargetW       = 0;      // last SetTargetPower we wrote; 0 = none active
static uint32_t                lastErgWriteMs       = 0;      // millis() of last SetTargetPower write; drives periodic refresh

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
static void ftmsCpIndicateCallback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len);
static void serviceControl();
static void serviceTrainingState();
static void serviceErgRefresh();
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

    // FTMS client (control sink): indication callback receives SS2k's response to
    // Request Control — ftmsCPReady is set only after that ACK arrives.
    ftms.begin();
    ftmsCP.setNotifyCallback(ftmsCpIndicateCallback);
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
    AntFec::service();         // drain ANT events: EVENT_TX → TX, EVENT_RX → control
    serviceTrainingState();    // send Stop/Start to SS2k on stale transitions
    serviceControl();          // consume pending ControlCommand → write to FTMS CP
    serviceErgRefresh();       // periodically re-write last ERG target while moving
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
            // ftmsCPReady is set in ftmsCpIndicateCallback once SS2k ACKs [0x80,0x00,0x01]
            Serial.println("[BLE] FTMS Request Control sent — awaiting SS2k ACK");
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
    ftmsCPReady           = false;
    trainingActive        = false;
    trainingActiveSinceMs = 0;
    lastErgTargetW        = 0;
    lastErgWriteMs        = 0;
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

    // Throttle steady-state CPM logging: at most once per CPM_LOG_INTERVAL_MS,
    // but always print when cadence crosses the 0 boundary so movement
    // start/stop edges aren't lost to the throttle.
    static uint32_t lastCpmLogMs  = 0;
    static bool     prevCpmMoving = false;
    bool     cpmMoving = cadence > 0;
    uint32_t nowMs     = millis();
    if (cpmMoving != prevCpmMoving || (nowMs - lastCpmLogMs) >= CPM_LOG_INTERVAL_MS) {
        Serial.printf("[CPM] %4d W  %3u RPM\n", power, cadence);
        lastCpmLogMs  = nowMs;
        prevCpmMoving = cpmMoving;
    }
}

// FTMS CP indication callback. SS2k responds [0x80, opcode, result] to each
// control point write. We gate ftmsCPReady on the ACK to Request Control (0x00)
// so no target writes go out before SS2k has accepted control ownership.
static void ftmsCpIndicateCallback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    (void)chr;
#if ANT_RX_DEBUG
    // Dump every indication SS2k sends so we can see whether SetTargetPower
    // writes (op 0x05) are being ACKed (result 0x01) or rejected.
    if (len >= 3 && data[0] == 0x80) {
        Serial.printf("[FTMS] indication op=0x%02X result=0x%02X\n",
                      data[1], data[2]);
    } else {
        Serial.printf("[FTMS] indication (%u bytes, header=0x%02X)\n",
                      len, len ? data[0] : 0);
    }
#endif
    if (len >= 3 && data[0] == 0x80 && data[1] == FTMS_OP_REQUEST_CONTROL && data[2] == 0x01) {
        ftmsCPReady = true;
        Serial.println("[BLE] FTMS 0x2AD9 ready — ERG control enabled");
    }
}

// Start on the first real movement (power > 0 or cadence > 0), Stop only when
// the BLE notification stream goes stale. We deliberately do NOT Stop on a
// transient rest (rider coasting between intervals) — CPS keeps publishing
// 0 W / 0 RPM notifications and we want ERG to resume the moment they pedal
// again. The end-of-workout release is driven by Garmin's Page 48 resistance=0
// path in serviceControl() and by BLE disconnect.
static void serviceTrainingState() {
    if (!ftmsCPReady) return;

    static bool prevStale  = true;
    static bool prevMoving = false;
    BridgeData d = bridgeSnapshot();
    bool stale  = !d.valid || ((millis() - d.lastUpdateMs) > STALE_DATA_TIMEOUT_MS);
    // Cadence > 0, NOT power > 0. A rider leaning on the cranks produces a power
    // spike with no rotation (cadence == 0 because dRevs == 0 in the CPS parser),
    // and SS2k will register the ensuing Start+SetTargetPower against a
    // stationary trainer and lock the knob at the low-resistance default. Real
    // rotation — the BLE CPS reporting cadence > 0 — is what we actually need
    // before telling SS2k to enter ERG.
    bool moving = !stale && d.cadence != 0xFF && d.cadence > 0;

    if (!prevMoving && moving && !trainingActive) {
        // rest → moving: rider actually started pedaling — only now is it safe
        // to put SS2k into training state. (Previously we tripped on the first
        // CPS notification, which arrives at 0 W / 0 RPM the instant we
        // subscribe and could leave SS2k sitting in "Started" for tens of
        // seconds before any real movement.)
        uint8_t startBuf = FTMS_OP_START_RESUME;
        ftmsCP.write(&startBuf, 1);
        trainingActive        = true;
        trainingActiveSinceMs = millis();
        Serial.println("[FTMS] Start (movement detected)");
    } else if (!prevStale && stale && trainingActive) {
        // BLE notification stream went silent for STALE_DATA_TIMEOUT_MS — most
        // likely a stalled connection. Release SS2k so it doesn't hold the
        // last ERG target indefinitely.
        uint8_t buf[2] = { FTMS_OP_STOP_PAUSE, FTMS_STOP_PARAM_STOP };
        ftmsCP.write(buf, sizeof(buf));
        trainingActive        = false;
        trainingActiveSinceMs = 0;
        lastErgTargetW        = 0;
        Serial.println("[FTMS] Stop (data stale)");
    }
    prevStale  = stale;
    prevMoving = moving;
}

// Consume the pending ControlCommand from bridge_core and write the matching
// FTMS opcode to SmartSpin2k's Control Point. Silently no-ops when no command
// is waiting or FTMS was not discovered on the current connection.
//
// Movement gating: SS2k will ACK SetTargetPower/Resistance writes at the BLE
// layer even when the rider is stationary, but its firmware never engages
// closed-loop control until cadence picks up — the ERG target is silently
// dropped. So we peek at the pending command and defer non-release writes
// until trainingActive is set (which serviceTrainingState() only flips on real
// movement). The release path (resistance=0, Garmin's end-of-workout signal)
// is honored immediately so we can let SS2k go even from a stopped state.
static void serviceControl() {
    if (!ftmsCPReady) return;

    ControlCommand cmd;
    if (!bridgePeekControl(&cmd)) return;

    bool isRelease = (cmd.mode == ControlMode::RESISTANCE && cmd.resistancePct == 0);

    // Hold non-release commands in the queue until the rider is actually moving.
    // serviceTrainingState() owns the Start handshake and flips trainingActive
    // on the first rest→moving edge; once that happens we consume + write.
    if (!isRelease && !trainingActive) return;

    // Settle delay: SS2k will ACK SetTargetPower the instant Start is written
    // but apparently won't spin up its ERG PID loop unless Start has had a
    // moment in the "Training" state with cadence data flowing. Without this
    // gap the SS2k acks both commands and ignores the target. The previously-
    // working session had ~75 s between Start and SetTargetPower; even 2 s is
    // enough to match the regime where ERG engages.
    if (!isRelease &&
        (millis() - trainingActiveSinceMs) < ERG_POST_START_DELAY_MS) {
        return;
    }

    bridgeConsumeControl(&cmd);   // now safe to burn the queue slot

    // Page 48 resistance=0 is Garmin's "release trainer" signal at workout end,
    // not a real resistance target — forward as Stop rather than SetResistance 0
    // (SS2k firmware avoids position 0 and would hunt against the floor stop).
    if (isRelease) {
        if (trainingActive) {
            uint8_t buf[2] = { FTMS_OP_STOP_PAUSE, FTMS_STOP_PARAM_STOP };
            ftmsCP.write(buf, sizeof(buf));
            trainingActive        = false;
            trainingActiveSinceMs = 0;
            lastErgTargetW        = 0;
            Serial.println("[FTMS] Stop (resistance 0 → release)");
        }
        return;
    }

    switch (cmd.mode) {
        case ControlMode::ERG: {
            // SetTargetPower (0x05): opcode + uint16 LE watts
            uint8_t buf[3] = {
                FTMS_OP_SET_TARGET_POWER,
                (uint8_t)(cmd.targetPowerW & 0xFF),
                (uint8_t)(cmd.targetPowerW >> 8)
            };
            ftmsCP.write(buf, sizeof(buf));
            lastErgTargetW = cmd.targetPowerW;   // cache for serviceErgRefresh()
            lastErgWriteMs = millis();
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

// Periodically re-write the last ERG target so SS2k doesn't quietly drop it.
// Garmin only re-transmits Page 49 on workout-state changes, so if SS2k acks
// our first SetTargetPower but never engages its PID loop (we've watched it
// do this from a freshly-woken state), nothing else will kick it back into
// ERG. While the rider is actively pedaling and we have a cached target, push
// the same value again every ERG_RESEND_INTERVAL_MS — cheap, idempotent, and
// gives SS2k repeated chances to enter closed-loop control.
static void serviceErgRefresh() {
    if (!ftmsCPReady || !trainingActive || lastErgTargetW == 0) return;

    BridgeData d = bridgeSnapshot();
    bool stale  = !d.valid || ((millis() - d.lastUpdateMs) > STALE_DATA_TIMEOUT_MS);
    bool moving = !stale && d.cadence != 0xFF && d.cadence > 0;
    if (!moving) return;

    if ((millis() - lastErgWriteMs) < ERG_RESEND_INTERVAL_MS) return;

    uint8_t buf[3] = {
        FTMS_OP_SET_TARGET_POWER,
        (uint8_t)(lastErgTargetW & 0xFF),
        (uint8_t)(lastErgTargetW >> 8)
    };
    ftmsCP.write(buf, sizeof(buf));
    lastErgWriteMs = millis();
    Serial.printf("[FTMS] SetTargetPower %u W (refresh)\n", lastErgTargetW);
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
