/*
 * ant_power_tx.cpp — ANT+ Bicycle Power TX engine (see ant_power_tx.h).
 *
 * Page byte layouts are transcribed verbatim from ARCHITECTURE.md
 * §"ANT+ Output: Bicycle Power Profile". Channel parameters live in config.h.
 */

#include "ant_power_tx.h"
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
static uint16_t msgCounter   = 0;    // 1..ANT_ROTATION_PERIOD
static uint8_t  eventCount   = 0;    // page 0x10 byte 1 — wraps at 256
static uint16_t accumPower   = 0;    // page 0x10 bytes 4-5 — wraps at 65536
static uint32_t deviceSerial = 0;    // full DEVICEID[0] — page 0x51 serial number

static void buildPagePower(uint8_t* buf, const BridgeData& d);
static void buildPageManufacturer(uint8_t* buf);
static void buildPageProduct(uint8_t* buf);
static uint32_t loadNextBroadcast();

bool AntPowerTx::begin() {
    deviceSerial = NRF_FICR->DEVICEID[0];
    uint16_t deviceNumber = (uint16_t)(deviceSerial & 0xFFFF);
    if (deviceNumber == 0) deviceNumber = 1;   // 0 is not a valid ANT master device number

    uint32_t err;

    // Network key for network 0 (ANT+ public). sd_ant_enable() is intentionally
    // skipped — the S340 default of one ANT channel is sufficient.
    err = sd_ant_network_address_set(ANTPLUS_NETWORK_NUMBER, kAntNetworkKey);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] network_address_set failed: 0x%lX\n", (unsigned long)err); return false; }

    // MASTER_TX_ONLY (0x50) = TX_NOT_RX | NO_TX_GUARD_BAND: eliminates the RX
    // guard window that CHANNEL_TYPE_MASTER (0x10) opens after each TX, freeing
    // that radio slot for BLE and improving coexistence on the S340.
    err = sd_ant_channel_assign(ANT_BPWR_CHANNEL_NUMBER, CHANNEL_TYPE_MASTER_TX_ONLY, ANTPLUS_NETWORK_NUMBER, 0);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] channel_assign failed: 0x%lX\n", (unsigned long)err); return false; }

    err = sd_ant_channel_id_set(ANT_BPWR_CHANNEL_NUMBER, deviceNumber, ANT_BPWR_DEVICE_TYPE, ANT_BPWR_TRANSMISSION_TYPE);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] channel_id_set failed: 0x%lX\n", (unsigned long)err); return false; }

    err = sd_ant_channel_radio_freq_set(ANT_BPWR_CHANNEL_NUMBER, ANT_BPWR_RF_FREQ);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] radio_freq_set failed: 0x%lX\n", (unsigned long)err); return false; }

    err = sd_ant_channel_period_set(ANT_BPWR_CHANNEL_NUMBER, ANT_BPWR_CHANNEL_PERIOD);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] channel_period_set failed: 0x%lX\n", (unsigned long)err); return false; }

    // Max TX power (+8 dBm on nRF52840) helps the Garmin detect the sensor
    // when BLE activity causes some ANT+ transmissions to be preempted.
    err = sd_ant_channel_radio_tx_power_set(ANT_BPWR_CHANNEL_NUMBER, RADIO_TX_POWER_LVL_5, 0);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] tx_power_set failed: 0x%lX\n", (unsigned long)err); return false; }

    err = sd_ant_channel_open(ANT_BPWR_CHANNEL_NUMBER);
    if (err != NRF_SUCCESS) { Serial.printf("[ANT] channel_open failed: 0x%lX\n", (unsigned long)err); return false; }

    // Seed the first broadcast; EVENT_TX fires after each transmission so
    // service() can load the next page in the rotation.
    uint32_t seedErr = loadNextBroadcast();
    if (seedErr != NRF_SUCCESS) {
        Serial.printf("[ANT] seed broadcast failed: 0x%lX\n", (unsigned long)seedErr);
    }

    Serial.printf("[ANT] BPWR TX started - dev# %u (type 0x%02X, %u-msg rotation)\n",
                  deviceNumber, ANT_BPWR_DEVICE_TYPE, ANT_ROTATION_PERIOD);
    return true;
}

void AntPowerTx::service() {
    uint8_t channel;
    uint8_t eventCode;
    uint8_t msgBuf[MESG_BUFFER_SIZE];
    static uint32_t txCount    = 0;
    static uint32_t lastLogTxN = 0;

    // Drain every queued ANT event; only our channel's EVENT_TX loads a new payload.
    while (sd_ant_event_get(&channel, &eventCode, msgBuf) == NRF_SUCCESS) {
        if (channel == ANT_BPWR_CHANNEL_NUMBER && eventCode == EVENT_TX) {
            loadNextBroadcast();
            txCount++;
        }
    }

    // Print "[ANT] TX N" once at the start and every 130 transmissions (~32 s)
    // so the serial output confirms ANT is actually broadcasting.
    if (txCount != lastLogTxN && (txCount == 1 || txCount % ANT_ROTATION_PERIOD == 0)) {
        Serial.printf("[ANT] TX count: %lu (page rot %u/130)\n", (unsigned long)txCount, msgCounter);
        lastLogTxN = txCount;
    }
}

// Select the next page per the 130-message rotation, build it, and hand it to the
// SoftDevice as the payload for the upcoming channel period. Returns the SoftDevice
// error code so begin() can detect a failed seed.
static uint32_t loadNextBroadcast() {
    uint8_t payload[ANT_STANDARD_DATA_PAYLOAD_SIZE];

    if (msgCounter >= ANT_ROTATION_PERIOD) msgCounter = 0;
    msgCounter++;   // 1..ANT_ROTATION_PERIOD

    if (msgCounter == 65) {
        buildPageManufacturer(payload);             // common page 0x50
    } else if (msgCounter == ANT_ROTATION_PERIOD) { // 130 → common page 0x51
        buildPageProduct(payload);
    } else {
        BridgeData d = bridgeSnapshot();
        buildPagePower(payload, d);                 // power page 0x10
    }

    return sd_ant_broadcast_message_tx(ANT_BPWR_CHANNEL_NUMBER, ANT_STANDARD_DATA_PAYLOAD_SIZE, payload);
}

// Data Page 0x10 — Standard Power Only. Event count + accumulated power advance on
// every power-page TX so the receiver can derive average power over any interval.
static void buildPagePower(uint8_t* buf, const BridgeData& d) {
    bool stale = !d.valid || ((millis() - d.lastUpdateMs) > STALE_DATA_TIMEOUT_MS);
    int16_t power   = stale ? 0 : d.instantaneousPower;
    uint8_t cadence = stale ? 0xFF : d.cadence;
    if (power < 0) power = 0;                  // BPWR instantaneous power is unsigned
    uint16_t instPower = (uint16_t)power;

    eventCount++;                              // one event per power-page transmission
    accumPower += instPower;                   // uint16 wrap at 65536 is intended

    buf[0] = ANT_PAGE_POWER;                   // 0x10 — page number
    buf[1] = eventCount;                       // update event count
    buf[2] = 0xFF;                             // pedal power — not used
    buf[3] = cadence;                          // instantaneous cadence (RPM) or 0xFF
    buf[4] = (uint8_t)(accumPower & 0xFF);     // accumulated power LSB
    buf[5] = (uint8_t)(accumPower >> 8);       // accumulated power MSB
    buf[6] = (uint8_t)(instPower & 0xFF);      // instantaneous power LSB
    buf[7] = (uint8_t)(instPower >> 8);        // instantaneous power MSB
}

// Data Page 0x50 — Manufacturer's Information (common page).
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

// Data Page 0x51 — Product Information (common page). Serial = full DEVICEID[0].
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
