/*
 * bridge_core.cpp — shared BridgeData store (see bridge_core.h).
 *
 * Mutual exclusion uses taskENTER_CRITICAL()/taskEXIT_CRITICAL() (raises BASEPRI
 * to configMAX_SYSCALL_INTERRUPT_PRIORITY) — NOT noInterrupts(), which would
 * globally mask the SoftDevice and is forbidden while it is running. The guarded
 * region is just a small struct copy, so it is extremely short.
 */

#include "bridge_core.h"
#include <Arduino.h>   // millis() + FreeRTOS critical-section macros (nRF52 core)

static BridgeData    gData    = { 0, 0xFF, 0xFFFF, 0, false };
static ControlCommand gControl = { ControlMode::NONE, false, 0, 0, { 0, 0, 0, 0 } };

void bridgeUpdateFromCps(int16_t power, uint16_t cadence, uint16_t speedMmps) {
    uint8_t  cad = (cadence > 254) ? 0xFF : (uint8_t)cadence;
    uint32_t now = millis();

    taskENTER_CRITICAL();
    gData.instantaneousPower = power;
    gData.cadence            = cad;
    gData.speedMmps          = speedMmps;
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

void bridgeSetControl(const ControlCommand& cmd) {
    taskENTER_CRITICAL();
    gControl         = cmd;
    gControl.pending = true;
    taskEXIT_CRITICAL();
}

bool bridgeConsumeControl(ControlCommand* out) {
    bool got = false;
    taskENTER_CRITICAL();
    if (gControl.pending) {
        *out             = gControl;
        gControl.pending = false;
        got              = true;
    }
    taskEXIT_CRITICAL();
    return got;
}

bool bridgePeekControl(ControlCommand* out) {
    bool got = false;
    taskENTER_CRITICAL();
    if (gControl.pending) {
        *out = gControl;
        got  = true;
    }
    taskEXIT_CRITICAL();
    return got;
}
