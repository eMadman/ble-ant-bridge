#pragma once
/*
 * ant_power_tx.h — ANT+ Bicycle Power (Device Type 0x0B) master transmitter,
 * built directly on the S340 sd_ant_* SoftDevice API.
 *
 * Coexists with Adafruit Bluefruit, which owns sd_softdevice_enable(). We do NOT
 * call sd_ant_enable(): after the SoftDevice is up the S340 defaults to one ANT
 * channel, which is all a single BPWR sensor needs (no extra RAM, no BSP patch).
 *
 * EVENT_TX is serviced by polling sd_ant_event_get() from loop() — Bluefruit only
 * drains BLE/SOC events, so ANT events sit in their own queue until we pull them.
 */

namespace AntPowerTx {

// Set the network key, configure + open the BPWR master channel, and seed the
// first broadcast. Call once, AFTER Bluefruit.begin(). Returns false (and logs
// "[ANT] …") if any SoftDevice call fails.
bool begin();

// Drain queued ANT events; on EVENT_TX advance the page rotation and load the
// next broadcast payload. Call every loop() pass — cheap when nothing is queued.
void service();

}  // namespace AntPowerTx
