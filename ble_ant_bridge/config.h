#pragma once
/*
 * config.h — compile-time configuration for the BLE→ANT+ bridge.
 * All tunable constants live here (CODING_GUIDELINES: "All configuration
 * constants in config.h").
 */

// ── Target peripheral ───────────────────────────────────────────────
// The SmartSpin2k advertises with this exact Complete Local Name.
#define TARGET_DEVICE_NAME "SmartSpin2k"

// ── BLE service / characteristic UUIDs (16-bit Bluetooth SIG) ────────
#define UUID16_SVC_CYCLING_POWER 0x1818
#define UUID16_CHR_CYCLING_POWER_MEASURE 0x2A63

// ── Scan parameters (units of 0.625 ms) ─────────────────────────────
#define SCAN_INTERVAL 160 // 100 ms
#define SCAN_WINDOW 80    // 50 ms
#define SCAN_ACTIVE true  // active scan → get scan-response names

// ── Reconnect backoff (ms) ──────────────────────────────────────────
// On disconnect we wait before re-scanning, doubling each failed cycle
// up to the cap. Reset to MIN once subscribed.
#define RECONNECT_BACKOFF_MIN_MS 1000
#define RECONNECT_BACKOFF_MAX_MS 30000

// ── Serial ──────────────────────────────────────────────────────────
#define SERIAL_BAUD 115200
#define SERIAL_WAIT_MS 3000 // wait up to this long for USB CDC
// Throttle the steady-state [CPM] log to this cadence (movement edges still
// print).
#define CPM_LOG_INTERVAL_MS 2000

// ── Status LED ──────────────────────────────────────────────────────
// LED_BUILTIN (P0.15) and LED_STATE_ON come from the supermini_nrf52840
// variant.
#define LED_BLINK_SCAN_MS 500 // ~1 Hz: scanning
#define LED_BLINK_BUSY_MS 125 // ~4 Hz: connecting/discovering

// ── Bridge data / stale handling ────────────────────────────────────
// If no CPS notification arrives within this window, power is zeroed and
// cadence is set to 0xFF ("unavailable") in the next ANT+ broadcast.
#define STALE_DATA_TIMEOUT_MS 3000

// ── FTMS ERG control timing ─────────────────────────────────────────
// SS2k will ACK Start (0x07) immediately but appears to need several CPS data
// updates worth of "Training" state before it will actually engage closed-loop
// ERG when SetTargetPower (0x05) lands. The previously-working session had
// ~75 s of pedaling between Start and the first SetTargetPower; back-to-back
// writes in the same loop tick leave SS2k acking the target but never spinning
// up the PID loop. Hold the first ERG write off for this long after Start.
#define ERG_POST_START_DELAY_MS 2000

// Once an ERG target has been written, repeat it on this cadence as long as the
// rider keeps pedaling. Garmin only re-sends Page 49 on workout-state changes,
// so if SS2k drops a target write (which it has, twice now) nothing else will
// nudge it back into ERG mode without our help.
#define ERG_RESEND_INTERVAL_MS 5000

// ── ANT+ public network ─────────────────────────────────────────────
// The 8-byte ANT+ network key is NOT here — it lives in untracked secrets.h.
#define ANTPLUS_NETWORK_NUMBER 0 // ANT+ public network

// ── Phase B diagnostic logging ──────────────────────────────────────
// Temporary: dump every ANT event + unknown control pages + FE state
// transitions + every FTMS indication so we can see what Garmin actually
// sends during an ERG workout. Flip to 0 (or strip) once the RX path is
// confirmed working end to end.
#define ANT_RX_DEBUG 0

// ── ANT+ Bicycle Power TX (LEGACY — superseded by FE-C, kept unwired) ─
// Channel config per ARCHITECTURE.md §"ANT+ Output: Bicycle Power Profile".
// ant_power_tx.* still references these but is no longer included by the .ino.
#define ANT_BPWR_CHANNEL_NUMBER 0
#define ANT_BPWR_DEVICE_TYPE 0x0B // Bicycle Power
#define ANT_BPWR_TRANSMISSION_TYPE 0x05
#define ANT_BPWR_RF_FREQ 57          // 2457 MHz
#define ANT_BPWR_CHANNEL_PERIOD 8182 // ~4.005 Hz (~249.5 ms/msg)

// Broadcast rotation (130-message cycle): 0x10×64, 0x50, 0x10×64, 0x51.
#define ANT_ROTATION_PERIOD 130
#define ANT_PAGE_POWER 0x10        // Standard Power Only
#define ANT_PAGE_MANUFACTURER 0x50 // Manufacturer's Info (common)
#define ANT_PAGE_PRODUCT 0x51      // Product Info (common)

// ── FTMS service / characteristic UUIDs (16-bit Bluetooth SIG) ───────
// Used in Phase B to write ERG + resistance + simulation targets to
// SmartSpin2k's Fitness Machine Control Point.
#define UUID16_SVC_FTMS 0x1826
#define UUID16_CHR_FTMS_CONTROL_POINT 0x2AD9

// FTMS Control Point opcodes (FTMS spec v1.0 §4.16).
// Per spec, CCCD indications must be enabled before writing any opcode.
#define FTMS_OP_REQUEST_CONTROL 0x00
#define FTMS_OP_START_RESUME 0x07 // no parameters
#define FTMS_OP_STOP_PAUSE 0x08   // param: 0x01=stop, 0x02=pause
#define FTMS_STOP_PARAM_STOP 0x01
#define FTMS_OP_SET_RESISTANCE_LEVEL                                           \
  0x04 // uint8, 0.1-unit resolution (0-100 = 0-10.0)
#define FTMS_OP_SET_TARGET_POWER 0x05 // uint16 LE, Watts
#define FTMS_OP_SET_SIMULATION_PARAMS                                          \
  0x11 // wind(int16) + grade(int16) + Cr + CwA

// ── ANT+ FE-C Trainer TX (Phase A) ──────────────────────────────────
// Fitness Equipment Control, Device Type 0x11. Channel is BIDIRECTIONAL
// MASTER (0x10, not TX-only) so Phase B can receive Garmin control pages;
// this reintroduces the RX guard band BPWR avoided — accepted for control.
// Config per ARCHITECTURE.md §"ANT+ Output: FE-C".
#define ANT_FEC_CHANNEL_NUMBER 0
#define ANT_FEC_DEVICE_TYPE 0x11 // Fitness Equipment (FE-C)
#define ANT_FEC_TRANSMISSION_TYPE 0x05
#define ANT_FEC_RF_FREQ 57          // 2457 MHz (same as all ANT+)
#define ANT_FEC_CHANNEL_PERIOD 8192 // exactly 4.00 Hz (0x2000)
#define ANT_FEC_EQUIPMENT_TYPE 0x19 // 25 = Trainer / Stationary Bike

// FE-C TX data pages.
#define ANT_FEC_PAGE_GENERAL 0x10      // 16 — General FE Data
#define ANT_FEC_PAGE_TRAINER 0x19      // 25 — Specific Trainer Data
#define ANT_FEC_PAGE_CAPABILITIES 0x36 // 54 — FE Capabilities

// FE State (page 16/25 byte 7, high nibble): READY when no fresh data,
// IN_USE while live CPS data is streaming.
#define FE_STATE_READY 0x1
#define FE_STATE_IN_USE 0x2

// FE Capabilities bit field (page 54 byte 7). Phase A advertises ERG only;
// basic-resistance (bit0) and simulation (bit2) light up in Phase B.
#define FEC_CAP_BASIC_RESISTANCE (1u << 0)
#define FEC_CAP_TARGET_POWER (1u << 1) // ERG
#define FEC_CAP_SIMULATION (1u << 2)

// ── ANT+ FE-C Control Pages (Phase B — RX ← Garmin) ─────────────────
// Garmin sends these as acknowledged messages to the bidirectional master.
#define ANT_FEC_PAGE_BASIC_RESISTANCE                                          \
  0x30 // 48 — resistance % (0.5% units, byte 7)
#define ANT_FEC_PAGE_TARGET_POWER                                              \
  0x31 // 49 — ERG target (0.25W units, bytes 6-7)
#define ANT_FEC_PAGE_TRACK_RESISTANCE                                          \
  0x33 // 51 — simulation (wind + grade + Cr + CwA)
#define ANT_FEC_PAGE_COMMAND_STATUS                                            \
  0x47 // 71 — TX response echoing last command

// Rotation: pages 16 ↔ 25 alternate every message; every Nth message a
// background page is substituted, cycling through [54, 80, 81].
#define ANT_FEC_BACKGROUND_INTERVAL 64 // every 64 msgs send a bg page

// Identity fields for the common pages (development values per
// ARCHITECTURE.md).
#define ANT_MANUFACTURER_ID 0x00FF // development
#define ANT_MODEL_NUMBER 0x0001
#define ANT_HW_REVISION 0x01
#define ANT_SW_REVISION_MAIN 0x01
