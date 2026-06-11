#pragma once
/*
 * bridge_core.h — the translation seam between the FreeRTOS tasks.
 *
 * CPS data path (Phase A): BLE notify callback → bridgeUpdateFromCps()
 * → shared BridgeData → ANT+ TX reads via bridgeSnapshot().
 *
 * Control data path (Phase B): ANT+ RX handler (loop() task) →
 * bridgeSetControl() → shared ControlCommand → BLE write path consumes
 * via bridgeConsumeControl() (also loop() task). Both sides run in the
 * same task, but the critical section is kept for consistency with
 * BridgeData and to guard against any future task topology change.
 */

#include <stdint.h>

// ── CPS → ANT+ TX ────────────────────────────────────────────────────

struct BridgeData {
    int16_t  instantaneousPower;   // Watts (sint16, from CPS 0x2A63)
    uint8_t  cadence;              // RPM, 0xFF = unavailable
    uint16_t speedMmps;            // 0.001 m/s (FE-C page 16 speed); 0xFFFF = unavailable
    uint32_t lastUpdateMs;         // millis() of the last BLE notification
    bool     valid;               // true once at least one notification is stored
};

// Publish freshly parsed CPS power/cadence/speed. Called from the BLE notify
// callback. Cadence ≥ 255 is clamped to 0xFF (ANT+ "unavailable"); speed is
// in 0.001 m/s units (0xFFFF = unavailable).
void bridgeUpdateFromCps(int16_t power, uint16_t cadence, uint16_t speedMmps);

// Return a consistent snapshot for the ANT+ TX path. Called from loop().
BridgeData bridgeSnapshot();

// ── Garmin → SmartSpin2k control (Phase B) ───────────────────────────

enum class ControlMode : uint8_t {
    NONE       = 0,
    ERG        = 1,   // target power in Watts
    RESISTANCE = 2,   // resistance level (0-100)
    SIMULATION = 3,   // wind/grade simulation parameters
};

struct SimParams {
    int16_t  windSpeedMms;     // 0.001 m/s units (ANT+ page 51 bytes 1-2)
    int16_t  gradeHundredths;  // 0.01% units (bytes 3-4)
    uint8_t  crCoeff;          // 5×10^-5 units (byte 5)
    uint8_t  cwaCoeff;         // 0.01 kg/m units (byte 6)
};

struct ControlCommand {
    ControlMode mode;
    bool        pending;
    uint16_t    targetPowerW;   // ERG mode
    uint8_t     resistancePct;  // RESISTANCE mode (0-100)
    SimParams   sim;            // SIMULATION mode
};

// Set the pending control command. Called from ANT+ RX handler (loop() task).
void bridgeSetControl(const ControlCommand& cmd);

// Atomically read and clear the pending command. Returns true if a command
// was waiting. Called from the BLE write path (loop() task).
bool bridgeConsumeControl(ControlCommand* out);

// Non-destructive read of the pending command. Used by the BLE write path to
// decide whether the command is currently safe to apply (e.g. defer ERG until
// the rider is actually moving) without burning the queue slot.
bool bridgePeekControl(ControlCommand* out);
