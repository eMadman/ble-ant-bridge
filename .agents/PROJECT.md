# SmartSpin2k ANT+ Bridge — Project Documentation

## What Is This?

A hardware bridge that solves a Garmin compatibility issue. The SmartSpin2k (ESP32-based smart trainer controller) broadcasts fitness data via BLE, but Garmin head units often fail to recognize it for power data. This nRF52840-based bridge receives BLE data from the SmartSpin2k and re-broadcasts it as ANT+ Bicycle Power — the protocol Garmin natively understands.

## Architecture

```
SmartSpin2k (ESP32)          nRF52840 SuperMini              Garmin Head Unit
┌─────────────────┐     ┌──────────────────────────┐     ┌──────────────────┐
│ BLE Peripheral   │────▶│ BLE Central  │  ANT+ TX  │────▶│ ANT+ Receiver    │
│ CPS 0x1818       │     │ (Bluefruit52)│(SDAntplus)│     │ BPWR Display     │
│ FTMS 0x1826      │     │              │           │     │                  │
│ Name: SmartSpin2k│     │    S340 SoftDevice       │     │                  │
└─────────────────┘     └──────────────────────────┘     └──────────────────┘
```

## Technology Stack

| Layer | Technology | Notes |
|---|---|---|
| **MCU** | nRF52840 SuperMini (Cortex-M4F) | Nice!Nano pin-compatible |
| **SoftDevice** | S340 v7.0.1 (BLE5 + ANT+) | Replaces stock S140 (BLE-only) |
| **Framework** | Arduino (Adafruit nRF52 BSP, modified for S340) | FreeRTOS included |
| **BLE Library** | Adafruit Bluefruit52 | Central role, connects to SmartSpin2k |
| **ANT+ Library** | SDAntplus (orrmany) | Channel management; BPWR TX is custom |
| **SWD Programmer** | ST-Link V2 clone via OpenOCD | Alternative: Pi Pico with Picoprobe |

## Key Design Decisions

1. **CPS over FTMS for PoC**: We subscribe to CPS (0x2A63) characteristic only. Simpler parsing, direct power + crank revolution data. FTMS is a roadmap item.
2. **Adafruit BSP as base**: Only documented S340 integration path for Arduino. Custom board variant with SuperMini pin map.
3. **Hardware-derived ANT+ Device ID**: Lower 16 bits of `NRF_FICR->DEVICEID[0]` for uniqueness.
4. **ANT+ BPWR TX from scratch**: SDAntplus lacks a Bicycle Power profile. We implement Data Pages 0x10 (Standard Power Only) and common pages 0x50/0x51 manually.

## File Structure

```
ble-ant-bridge/
├── .agents/
│   ├── PROJECT.md              ← You are here
│   ├── ARCHITECTURE.md         ← Detailed data flow & protocol specs
│   ├── CODING_GUIDELINES.md    ← Code style, patterns, constraints
│   └── ROADMAP.md              ← Future work items
├── ble_ant_bridge.ino          ← Main sketch (setup/loop, state machine)
├── ble_parser.h/.cpp           ← BLE CPS data parser
├── ant_power_tx.h/.cpp         ← ANT+ Bicycle Power TX profile
├── bridge_core.h/.cpp          ← Translation pipeline (BLE→ANT+)
├── config.h                    ← Pin defs, constants, ANT+ config
└── README.md                   ← User-facing documentation
```

## External References

| Resource | URL | Purpose |
|---|---|---|
| SmartSpin2k Source | https://github.com/doudar/SmartSpin2k | BLE service implementation reference |
| SDAntplus Library | https://github.com/orrmany/SDAntplus | ANT+ Arduino library for S340 |
| S340 Bootloader Guide | https://blogarak.wordpress.com/2020/03/15/s340-softdevice-adafruit-nrf52840-express-feather/ | Step-by-step S340 flashing |
| S340 Arduino IDE Integration | https://blogarak.wordpress.com/2020/03/29/arduino-ide-integration-for-the-nrf52840-feather-express-with-s340/ | BSP modification guide |
| Forked Bootloader (S340) | https://github.com/orrmany/Adafruit_nRF52_Bootloader/tree/s340-for-nrf52840-Feather | Ready-made S340 bootloader |
| ANT+ BPWR Spec | https://www.thisisant.com/ (requires login) | Bicycle Power device profile |
| Adafruit nRF52 BSP | https://github.com/adafruit/Adafruit_nRF52_Arduino | Arduino core to modify |

## Critical Constraints

- **NO NimBLE**: SmartSpin2k uses NimBLE on ESP32, but our bridge MUST use Adafruit Bluefruit library (talks to S340 SoftDevice directly)
- **S340 Memory Map**: Application flash starts at 0x31000 (not 0x26000 like S140). All linker scripts must reflect this.
- **ANT+ License Key (≠ network key — EXCEPTION, hardcoded on purpose)**: The S340 requires an ANT+ evaluation license key to be provided during `sd_softdevice_enable()` (the S340 enable is **3-arg**: clock, fault handler, license). In S340 7.0.1 the key is **already `#define`d** as `ANT_LICENSE_KEY` in `nrf_sdm.h:191` (the header even `#error`s if it's absent) — there is nothing to uncomment. **This is NOT the ANT+ network key** — they are two unrelated tokens:
  - *ANT license key* (this one): a SoftDevice enablement token shipped **inside the Nordic S340 headers**. It's a fixed value that comes with the SoftDevice, it's the public Garmin evaluation key, and it is present-by-default in `nrf_sdm.h`. Seeing it in source is correct — **do not** try to externalize it.
  - *ANT+ network key* (the other one): the radio key for the ANT+ public network, covered by the ANT+ Adopter Agreement. This one is **never** committed — supplied at build time. See CODING_GUIDELINES.md → "ANT+ Network Key".
  - Rule of thumb: if it lives in a Nordic/`nrf_*` header it's the license key (leave it); if it's an 8-byte array you feed to your ANT+ channel config, it's the network key (keep it out of the repo).
- **Radio TDM**: The S340 handles BLE/ANT+ time-division multiplexing automatically. No manual radio arbitration needed.
- **CircuitPython is dead**: Replacing S140 with S340 permanently removes CircuitPython/UF2 stock firmware compatibility.
- **No modificatoins to SmartSpin2k firmware**: We can't modify SmartSpin2k firmware.
