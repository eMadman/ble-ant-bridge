/*
 * bridge_core.cpp — shared BridgeData store (see bridge_core.h).
 *
 * Mutual exclusion uses taskENTER_CRITICAL()/taskEXIT_CRITICAL() (raises BASEPRI
 * to configMAX_SYSCALL_INTERRUPT_PRIORITY) — NOT noInterrupts(), which would
 * globally mask the SoftDevice and is forbidden while it is running. The guarded
 * region is just an 8-byte struct copy, so it is extremely short.
 */

#include "bridge_core.h"
#include <Arduino.h>   // millis() + FreeRTOS critical-section macros (nRF52 core)

static BridgeData gData = { 0, 0xFF, 0, false };

void bridgeUpdateFromCps(int16_t power, uint16_t cadence) {
    uint8_t  cad = (cadence > 254) ? 0xFF : (uint8_t)cadence;
    uint32_t now = millis();

    taskENTER_CRITICAL();
    gData.instantaneousPower = power;
    gData.cadence            = cad;
    gData.lastUpdateMs       = now;
    gData.valid              = true;
    taskEXIT_CRITICAL();
}

BridgeData bridgeSnapshot() {
    BridgeData copy;
    taskENTER_CRITICAL();
    copy = gData;
    taskEXIT_CRITICAL();
    return copy;
}
