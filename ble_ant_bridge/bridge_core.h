#pragma once
/*
 * bridge_core.h — the translation seam between the two FreeRTOS tasks.
 *
 * The BLE notify callback (Bluefruit task) publishes parsed CPS data via
 * bridgeUpdateFromCps(); the ANT+ TX path (Arduino loop() task) reads a
 * consistent copy via bridgeSnapshot(). The single backing struct is guarded
 * by a short FreeRTOS critical section, so the two tasks never observe a
 * half-written value.
 */

#include <stdint.h>

struct BridgeData {
    int16_t  instantaneousPower;   // Watts (sint16, from CPS 0x2A63)
    uint8_t  cadence;              // RPM, 0xFF = unavailable
    uint32_t lastUpdateMs;         // millis() of the last BLE notification
    bool     valid;               // true once at least one notification is stored
};

// Publish freshly parsed CPS power/cadence. Called from the BLE notify callback.
// Cadence ≥ 255 is clamped to 0xFF (ANT+ "unavailable").
void bridgeUpdateFromCps(int16_t power, uint16_t cadence);

// Return a consistent snapshot for the ANT+ TX path. Called from loop().
BridgeData bridgeSnapshot();
