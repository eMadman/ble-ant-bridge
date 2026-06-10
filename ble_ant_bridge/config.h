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

// ── Bridge data / stale handling ────────────────────────────────────
// If no CPS notification arrives within this window, power is zeroed and
// cadence is set to 0xFF ("unavailable") in the next ANT+ broadcast.
#define STALE_DATA_TIMEOUT_MS           3000

// ── ANT+ Bicycle Power TX (Phase 3) ─────────────────────────────────
// Channel config per ARCHITECTURE.md §"ANT+ Output: Bicycle Power Profile".
// The 8-byte ANT+ network key is NOT here — it lives in untracked secrets.h.
#define ANTPLUS_NETWORK_NUMBER             0       // ANT+ public network
#define ANT_BPWR_CHANNEL_NUMBER            0
#define ANT_BPWR_DEVICE_TYPE            0x0B       // Bicycle Power
#define ANT_BPWR_TRANSMISSION_TYPE     0x05
#define ANT_BPWR_RF_FREQ                  57       // 2457 MHz
#define ANT_BPWR_CHANNEL_PERIOD         8182       // ~4.005 Hz (~249.5 ms/msg)

// Broadcast rotation (130-message cycle): 0x10×64, 0x50, 0x10×64, 0x51.
#define ANT_ROTATION_PERIOD              130
#define ANT_PAGE_POWER                 0x10        // Standard Power Only
#define ANT_PAGE_MANUFACTURER          0x50        // Manufacturer's Info (common)
#define ANT_PAGE_PRODUCT               0x51        // Product Info (common)

// Identity fields for the common pages (development values per ARCHITECTURE.md).
#define ANT_MANUFACTURER_ID          0x00FF        // development
#define ANT_MODEL_NUMBER             0x0001
#define ANT_HW_REVISION              0x01
#define ANT_SW_REVISION_MAIN         0x01
