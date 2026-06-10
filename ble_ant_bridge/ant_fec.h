#pragma once
/*
 * ant_fec.h — ANT+ FE-C (Fitness Equipment Control, Device Type 0x11) trainer
 * transmitter, built directly on the S340 sd_ant_* SoftDevice API. Replaces the
 * legacy BPWR transmitter (ant_power_tx.*) so Garmin head units pair to the
 * SmartSpin2k as a controllable smart trainer rather than a bare power sensor.
 *
 * Coexists with Adafruit Bluefruit, which owns sd_softdevice_enable(). We do NOT
 * call sd_ant_enable(): one ANT channel is the S340 default after the SoftDevice
 * is up (no extra RAM, RAM origin 0x20006000 unchanged).
 *
 * Unlike BPWR (which used a TX-only master), the FE-C channel is a BIDIRECTIONAL
 * master (CHANNEL_TYPE_MASTER 0x10) so a future control phase can receive Garmin's
 * acknowledged control pages. Phase A only transmits trainer data pages.
 *
 * EVENT_TX is serviced by polling sd_ant_event_get() from loop() — Bluefruit only
 * drains BLE/SOC events, so ANT events sit in their own queue until we pull them.
 */

namespace AntFec {

// Set the network key, configure + open the FE-C master channel, and seed the
// first broadcast. Call once, AFTER Bluefruit.begin(). Returns false (and logs
// "[ANT] …") if any SoftDevice call fails.
bool begin();

// Drain queued ANT events; on EVENT_TX advance the page rotation and load the
// next broadcast payload. Call every loop() pass — cheap when nothing is queued.
void service();

}  // namespace AntFec
