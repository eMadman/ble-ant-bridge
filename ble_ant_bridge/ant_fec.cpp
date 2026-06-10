/*
 * ant_fec.cpp — ANT+ FE-C trainer TX engine (see ant_fec.h).
 *
 * Page byte layouts are transcribed from ARCHITECTURE.md §"ANT+ Output: FE-C".
 * Channel parameters live in config.h. Phase A transmits trainer data only;
 * the bidirectional master channel (0x10) leaves room for Phase B control RX.
 */

#include "ant_fec.h"
#include "config.h"
#include "bridge_core.h"
#include "secrets.h"     // ANT_PLUS_NETWORK_KEY — untracked; build fails to link without it

#include <Arduino.h>
#include <bluefruit.h>   // pulls in the SoftDevice headers via the S340 BSP

extern "C" {
#include "ant_interface.h"
#include "ant_parameters.h"
}

// The 8-byte ANT+ network key exists only in the untracked secrets.h.
static const uint8_t kAntNetworkKey[8] = ANT_PLUS_NETWORK_KEY;

// Rotation + running accumulators. Touched only from service()/begin(), both of
// which run in the loop() task, so no locking is needed here.
static uint32_t msgCounter   = 0;    // total broadcasts loaded (drives rotation)
static uint8_t  eventCount   = 0;    // page 25 byte 1 — wraps at 256
static uint16_t accumPower   = 0;    // page 25 bytes 3-4 — wraps at 65536
static uint8_t  bgIndex      = 0;    // background page cursor: 0→54, 1→80, 2→81
static uint32_t deviceSerial = 0;    // full DEVICEID[0] — page 81 serial number
static uint32_t startMs      = 0;    // for page 16 elapsed time (0.25 s units)

// Phase B: last received control command state for page 71 (Command Status).
// cmdResponsePending is set by handleControlPage() and cleared by loadNextBroadcast()
// after it inserts page 71 into the broadcast stream.
static bool    cmdResponsePending = false;
static uint8_t lastCmdPage        = 0xFF;  // page number of last received control page
static uint8_t lastCmdStatus      = 0x02;  // 0=Pass, 1=Fail, 2=NotSupported
static uint8_t lastCmdData[4]     = { 0xFF, 0xFF, 0xFF, 0xFF };  // echoed in page 71

static void buildPageGeneralFE(uint8_t* buf, const BridgeData& d, bool stale);
static void buildPageTrainer(uint8_t* buf, const BridgeData& d, bool stale);
static void buildPageCapabilities(uint8_t* buf);
static void buildPageManufacturer(uint8_t* buf);
static void buildPageProduct(uint8_t* buf);
static void buildPageCommandStatus(uint8_t* buf);
static void handleControlPage(const uint8_t* data);
static uint32_t loadNextBroadcast();

bool AntFec::begin() {
    deviceSerial = NRF_FICR->DEVICEID[0];
    uint16_t deviceNumber = (uint16_t)(deviceSerial & 0xFFFF);
    if (deviceNumber == 0) deviceNumber = 1;   // 0 is not a valid ANT master device number

    startMs = millis();

    uint32_t err;

    // Network key for network 0 (ANT+ public). sd_ant_enable() is intentionally
    // skipped — the S340 default of one ANT channel is sufficient.
    err = sd_ant_network_address_set(ANTPLUS_NETWORK_NUMBER, kAntNetworkKey);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] network_address_set failed: 0x%lX\n", (unsigned long)err); return false; }

    // CHANNEL_TYPE_MASTER (0x10) = bidirectional master. Unlike the BPWR build's
    // MASTER_TX_ONLY (0x50), this keeps an RX window after each TX so Phase B can
    // receive Garmin's acknowledged control pages. We accept the small coexistence
    // cost (the RX guard band) because FE-C is inherently bidirectional.
    err = sd_ant_channel_assign(ANT_FEC_CHANNEL_NUMBER, CHANNEL_TYPE_MASTER, ANTPLUS_NETWORK_NUMBER, 0);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] channel_assign failed: 0x%lX\n", (unsigned long)err); return false; }

    err = sd_ant_channel_id_set(ANT_FEC_CHANNEL_NUMBER, deviceNumber, ANT_FEC_DEVICE_TYPE, ANT_FEC_TRANSMISSION_TYPE);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] channel_id_set failed: 0x%lX\n", (unsigned long)err); return false; }

    err = sd_ant_channel_radio_freq_set(ANT_FEC_CHANNEL_NUMBER, ANT_FEC_RF_FREQ);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] radio_freq_set failed: 0x%lX\n", (unsigned long)err); return false; }

    err = sd_ant_channel_period_set(ANT_FEC_CHANNEL_NUMBER, ANT_FEC_CHANNEL_PERIOD);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] channel_period_set failed: 0x%lX\n", (unsigned long)err); return false; }

    // Max TX power (+8 dBm on nRF52840) helps the Garmin detect the sensor
    // when BLE activity causes some ANT+ transmissions to be preempted.
    err = sd_ant_channel_radio_tx_power_set(ANT_FEC_CHANNEL_NUMBER, RADIO_TX_POWER_LVL_5, 0);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] tx_power_set failed: 0x%lX\n", (unsigned long)err); return false; }

    err = sd_ant_channel_open(ANT_FEC_CHANNEL_NUMBER);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] channel_open failed: 0x%lX\n", (unsigned long)err); return false; }

    // Seed the first broadcast; EVENT_TX fires after each transmission so
    // service() can load the next page in the rotation.
    uint32_t seedErr = loadNextBroadcast();
    if (seedErr != NRF_SUCCESS) {
        Serial.printf("[ANT] seed broadcast failed: 0x%lX\n", (unsigned long)seedErr);
    }

    Serial.printf("[ANT] FE-C TX started - dev# %u (type 0x%02X, trainer)\n",
                  deviceNumber, ANT_FEC_DEVICE_TYPE);
    return true;
}

void AntFec::service() {
    uint8_t channel;
    uint8_t eventCode;
    uint8_t msgBuf[MESG_BUFFER_SIZE];
    static uint32_t txCount    = 0;
    static uint32_t lastLogTxN = 0;

    // Drain every queued ANT event on our channel.
    // EVENT_TX  → load next broadcast payload.
    // EVENT_RX  → parse Garmin's acknowledged control page (48/49/51); sets
    //             cmdResponsePending so loadNextBroadcast() inserts page 71.
    while (sd_ant_event_get(&channel, &eventCode, msgBuf) == NRF_SUCCESS) {
        if (channel == ANT_FEC_CHANNEL_NUMBER) {
            if (eventCode == EVENT_TX) {
                loadNextBroadcast();
                txCount++;
            } else if (eventCode == EVENT_RX) {
                handleControlPage(msgBuf + MESG_DATA_OFFSET);   // payload starts at byte 3
            }
        }
    }

    // Print "[ANT] TX count" once at the start and every ~32 s so the serial
    // output confirms FE-C is actually broadcasting.
    if (txCount != lastLogTxN && (txCount == 1 || txCount % 128 == 0)) {
        Serial.printf("[ANT] TX count: %lu\n", (unsigned long)txCount);
        lastLogTxN = txCount;
    }
}

// Select the next page, build it, and hand it to the SoftDevice as the payload
// for the upcoming channel period. Rotation: pages 16 ↔ 25 alternate each message;
// every ANT_FEC_BACKGROUND_INTERVAL messages a background page (54/80/81) is
// substituted, cycling so all three are seen well within the receiver's window.
// Returns the SoftDevice error code so begin() can detect a failed seed.
static uint32_t loadNextBroadcast() {
    uint8_t payload[ANT_STANDARD_DATA_PAYLOAD_SIZE];

    msgCounter++;
    BridgeData d = bridgeSnapshot();
    bool stale = !d.valid || ((millis() - d.lastUpdateMs) > STALE_DATA_TIMEOUT_MS);

    // Page 71 (Command Status) takes priority over the normal background rotation
    // so Garmin's ERG UI gets prompt acknowledgement of each control command.
    if (cmdResponsePending) {
        buildPageCommandStatus(payload);
        cmdResponsePending = false;
    } else if (msgCounter % ANT_FEC_BACKGROUND_INTERVAL == 0) {
        switch (bgIndex) {
            case 0:  buildPageCapabilities(payload); break;   // page 54
            case 1:  buildPageManufacturer(payload); break;   // page 80 (0x50)
            default: buildPageProduct(payload);      break;   // page 81 (0x51)
        }
        bgIndex = (uint8_t)((bgIndex + 1) % 3);
    } else if (msgCounter & 1) {
        buildPageGeneralFE(payload, d, stale);                // page 16
    } else {
        buildPageTrainer(payload, d, stale);                  // page 25
    }

    return sd_ant_broadcast_message_tx(ANT_FEC_CHANNEL_NUMBER, ANT_STANDARD_DATA_PAYLOAD_SIZE, payload);
}

// Page 16 (0x10) — General FE Data. Equipment type, elapsed time, FE state.
// Speed is reported invalid (0xFFFF): Garmin computes virtual speed from power
// in trainer mode, so the bridge need not synthesize one.
static void buildPageGeneralFE(uint8_t* buf, const BridgeData& d, bool stale) {
    (void)d;
    uint8_t feState  = stale ? FE_STATE_READY : FE_STATE_IN_USE;
    uint8_t elapsed  = (uint8_t)((millis() - startMs) / 250u);  // 0.25 s units, wraps

    buf[0] = ANT_FEC_PAGE_GENERAL;             // 0x10 — page number
    buf[1] = ANT_FEC_EQUIPMENT_TYPE;           // 0x19 — trainer / stationary bike
    buf[2] = elapsed;                          // elapsed time (0.25 s units)
    buf[3] = 0xFF;                             // distance traveled — disabled
    buf[4] = 0xFF;                             // speed LSB — invalid
    buf[5] = 0xFF;                             // speed MSB — invalid (0xFFFF)
    buf[6] = 0xFF;                             // heart rate — not available
    // byte7 low nibble = capabilities (0: no HR, distance disabled, real speed);
    //       high nibble = FE state.
    buf[7] = (uint8_t)(feState << 4);
}

// Page 25 (0x19) — Specific Trainer Data. Cadence + accumulated/instantaneous
// power (12-bit) + FE state. Event count and accumulated power advance on every
// page-25 TX so the receiver can derive average power over any interval.
static void buildPageTrainer(uint8_t* buf, const BridgeData& d, bool stale) {
    int16_t  power   = stale ? 0 : d.instantaneousPower;
    uint8_t  cadence = stale ? 0xFF : d.cadence;
    if (power < 0) power = 0;                   // FE-C instantaneous power is unsigned
    if (power > 4094) power = 4094;             // 0xFFF reserved for "invalid"
    uint16_t instPower = stale ? 0x0FFF : (uint16_t)power;

    eventCount++;                              // one event per trainer-page transmission
    if (!stale) accumPower += (uint16_t)power; // uint16 wrap at 65536 is intended

    uint8_t feState = stale ? FE_STATE_READY : FE_STATE_IN_USE;

    buf[0] = ANT_FEC_PAGE_TRAINER;             // 0x19 — page number
    buf[1] = eventCount;                       // update event count
    buf[2] = cadence;                          // instantaneous cadence (RPM) or 0xFF
    buf[3] = (uint8_t)(accumPower & 0xFF);     // accumulated power LSB
    buf[4] = (uint8_t)(accumPower >> 8);       // accumulated power MSB
    buf[5] = (uint8_t)(instPower & 0xFF);      // instantaneous power LSB (low 8 bits)
    // byte6 low nibble = instantaneous power MSB (upper 4 of 12 bits);
    //       high nibble = trainer status bit field (0 — no calibration pending).
    buf[6] = (uint8_t)((instPower >> 8) & 0x0F);
    // byte7 low nibble = flags (0 — operating at target / no power limit);
    //       high nibble = FE state.
    buf[7] = (uint8_t)(feState << 4);
}

// Page 54 (0x36) — FE Capabilities. Phase B advertises all three control modes.
static void buildPageCapabilities(uint8_t* buf) {
    buf[0] = ANT_FEC_PAGE_CAPABILITIES;        // 0x36
    buf[1] = 0xFF;                             // reserved
    buf[2] = 0xFF;                             // reserved
    buf[3] = 0xFF;                             // reserved
    buf[4] = 0xFF;                             // reserved
    buf[5] = 0xFF;                             // max resistance LSB — N/A
    buf[6] = 0xFF;                             // max resistance MSB — N/A (0xFFFF)
    buf[7] = FEC_CAP_BASIC_RESISTANCE | FEC_CAP_TARGET_POWER | FEC_CAP_SIMULATION;
}

// Common Page 80 (0x50) — Manufacturer's Information.
static void buildPageManufacturer(uint8_t* buf) {
    buf[0] = ANT_PAGE_MANUFACTURER;            // 0x50
    buf[1] = 0xFF;                             // reserved
    buf[2] = 0xFF;                             // reserved
    buf[3] = ANT_HW_REVISION;                  // HW revision
    buf[4] = (uint8_t)(ANT_MANUFACTURER_ID & 0xFF);
    buf[5] = (uint8_t)(ANT_MANUFACTURER_ID >> 8);
    buf[6] = (uint8_t)(ANT_MODEL_NUMBER & 0xFF);
    buf[7] = (uint8_t)(ANT_MODEL_NUMBER >> 8);
}

// Common Page 81 (0x51) — Product Information. Serial = full DEVICEID[0].
static void buildPageProduct(uint8_t* buf) {
    buf[0] = ANT_PAGE_PRODUCT;                 // 0x51
    buf[1] = 0xFF;                             // reserved
    buf[2] = 0xFF;                             // SW revision (supplemental) — none
    buf[3] = ANT_SW_REVISION_MAIN;             // SW revision (main)
    buf[4] = (uint8_t)(deviceSerial & 0xFF);   // serial number LSB
    buf[5] = (uint8_t)(deviceSerial >> 8);
    buf[6] = (uint8_t)(deviceSerial >> 16);
    buf[7] = (uint8_t)(deviceSerial >> 24);    // serial number MSB
}

// Page 71 (0x47) — Command Status. TX response echoing the last received control
// page so Garmin's ERG UI can confirm the command was accepted.
static void buildPageCommandStatus(uint8_t* buf) {
    buf[0] = ANT_FEC_PAGE_COMMAND_STATUS;  // 0x47
    buf[1] = lastCmdPage;                  // page number of last received control page
    buf[2] = 0xFF;                         // sequence number — N/A for FE-C masters
    buf[3] = lastCmdStatus;               // 0=Pass, 1=Fail, 2=NotSupported
    buf[4] = lastCmdData[0];
    buf[5] = lastCmdData[1];
    buf[6] = lastCmdData[2];
    buf[7] = lastCmdData[3];
}

// Parse a control page received from Garmin (EVENT_RX). Extracts the target
// value, publishes it to bridge_core, and primes a page-71 response broadcast.
// data[0] is the ANT+ page number; units follow the FE-C Device Profile spec.
static void handleControlPage(const uint8_t* data) {
    ControlCommand cmd = {};

    switch (data[0]) {
        case ANT_FEC_PAGE_TARGET_POWER: {    // 0x31 — ERG: 0.25 W units, bytes 6-7
            uint16_t raw  = (uint16_t)(data[6] | ((uint16_t)data[7] << 8));
            cmd.mode         = ControlMode::ERG;
            cmd.targetPowerW = raw / 4;      // 0.25 W resolution → W
            lastCmdPage      = data[0];
            lastCmdStatus    = 0x00;         // Pass
            lastCmdData[0]   = 0xFF;
            lastCmdData[1]   = 0xFF;
            lastCmdData[2]   = data[6];      // echo raw target power
            lastCmdData[3]   = data[7];
            bridgeSetControl(cmd);
            cmdResponsePending = true;
            Serial.printf("[ANT] Page 49 ERG target: %u W\n", cmd.targetPowerW);
            break;
        }
        case ANT_FEC_PAGE_BASIC_RESISTANCE: {  // 0x30 — byte 7: 0.5% units (0-200=0-100%)
            cmd.mode          = ControlMode::RESISTANCE;
            cmd.resistancePct = data[7] / 2;  // 0.5% units → 0-100%
            lastCmdPage       = data[0];
            lastCmdStatus     = 0x00;
            lastCmdData[0]    = 0xFF; lastCmdData[1] = 0xFF; lastCmdData[2] = 0xFF;
            lastCmdData[3]    = data[7];
            bridgeSetControl(cmd);
            cmdResponsePending = true;
            Serial.printf("[ANT] Page 48 resistance: %u%%\n", cmd.resistancePct);
            break;
        }
        case ANT_FEC_PAGE_TRACK_RESISTANCE: {  // 0x33 — simulation: wind + grade + Cr + CwA
            cmd.mode                  = ControlMode::SIMULATION;
            cmd.sim.windSpeedMms      = (int16_t)(data[1] | ((uint16_t)data[2] << 8));
            cmd.sim.gradeHundredths   = (int16_t)(data[3] | ((uint16_t)data[4] << 8));
            cmd.sim.crCoeff           = data[5];
            cmd.sim.cwaCoeff          = data[6];
            lastCmdPage               = data[0];
            lastCmdStatus             = 0x00;
            lastCmdData[0]            = data[3];  // echo grade LSB/MSB + coefficients
            lastCmdData[1]            = data[4];
            lastCmdData[2]            = data[5];
            lastCmdData[3]            = data[6];
            bridgeSetControl(cmd);
            cmdResponsePending = true;
            Serial.printf("[ANT] Page 51 simulation: grade=%.2f%%\n",
                          cmd.sim.gradeHundredths * 0.01f);
            break;
        }
        default:
            break;   // unknown page — ignore silently
    }
}
