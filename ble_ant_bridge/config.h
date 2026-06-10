#pragma once
/*
 * config.h — compile-time configuration for the BLE→ANT+ bridge.
 * All tunable constants live here (CODING_GUIDELINES: "All configuration
 * constants in config.h").
 */

// ── Target peripheral ───────────────────────────────────────────────
// The SmartSpin2k advertises with this exact Complete Local Name.
#define TARGET_DEVICE_NAME                 "SmartSpin2k"

// ── BLE service / characteristic UUIDs (16-bit Bluetooth SIG) ────────
#define UUID16_SVC_CYCLING_POWER           0x1818
#define UUID16_CHR_CYCLING_POWER_MEASURE   0x2A63

// ── Scan parameters (units of 0.625 ms) ─────────────────────────────
#define SCAN_INTERVAL                      160     // 100 ms
#define SCAN_WINDOW                         80     // 50 ms
#define SCAN_ACTIVE                        true    // active scan → get scan-response names

// ── Reconnect backoff (ms) ──────────────────────────────────────────
// On disconnect we wait before re-scanning, doubling each failed cycle
// up to the cap. Reset to MIN once subscribed.
#define RECONNECT_BACKOFF_MIN_MS          1000
#define RECONNECT_BACKOFF_MAX_MS         30000

// ── Serial ──────────────────────────────────────────────────────────
#define SERIAL_BAUD                     115200
#define SERIAL_WAIT_MS                    3000    // wait up to this long for USB CDC

// ── Status LED ──────────────────────────────────────────────────────
// LED_BUILTIN (P0.15) and LED_STATE_ON come from the supermini_nrf52840 variant.
#define LED_BLINK_SCAN_MS                 500     // ~1 Hz: scanning
#define LED_BLINK_BUSY_MS                 125     // ~4 Hz: connecting/discovering
