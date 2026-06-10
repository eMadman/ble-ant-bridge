# CLAUDE.md — SmartSpin2k BLE→ANT+ Bridge

nRF52840 SuperMini firmware that BLE-Central-connects to a SmartSpin2k (Cycling Power,
CPS `0x2A63`) and re-broadcasts power/cadence as **ANT+ Bicycle Power** so Garmin head units
recognize it. Arduino-dialect C++.

Detailed specs live in `.agents/` — read these before non-trivial work:
- `.agents/PROJECT.md` — overview, stack, critical constraints
- `.agents/ARCHITECTURE.md` — BLE/ANT byte layouts, broadcast rotation, state machine
- `.agents/CODING_GUIDELINES.md` — style, "do not do" list, secrets policy
- `.agents/ROADMAP.md` — phased task list (current: Phase 0 — modify Arduino BSP for S340)

## Toolchain (authoritative)
- **IDE:** Arduino IDE (inside Google Antigravity, a VS Code fork). `pioarduino` is also installed
  but Arduino IDE is the chosen path for the nRF52/S340 build.
- **Board package:** Adafruit nRF52 BSP **1.7.0** (BLE API 7.x family — matches S340 7.0.1's BLE side).
- **Upload:** app-only DFU via the on-device bootloader (adafruit-nrfutil).
  **NEVER use "Burn Bootloader"** — it would overwrite the working S340 SoftDevice + bootloader.

## Hardware / SoftDevice state
- **SoftDevice: S340 v7.0.1** (combined BLE + ANT+), already flashed with a custom S340 bootloader
  built in the **sibling repo** `D:\git\ss2k\bridge_project\Adafruit_nRF52_Bootloader`.
- **App flash start = `0x31000`** = MBR `0x1000` + `SD_FLASH_SIZE 0x30000` (S340 `nrf_sdm.h:144`).
  App FLASH ends at the release bootloader region `0xF4000` → LENGTH `0xC3000`.
  RAM origin starts at `0x20006000` (tune to the boot-time SoftDevice RAM requirement).
- **ANT license:** the **public Garmin evaluation key** is already
  `#define ANT_LICENSE_KEY "3831-521d-7df9-24d8-eff3-467b-225f-a00e"` in S340 `nrf_sdm.h:191`.
  Not secret, not per-device. S340's `sd_softdevice_enable` is **3-arg** (clock, fault handler,
  license) — the BSP's `bluefruit.cpp` call must pass `ANT_LICENSE_KEY`.
- **Board:** SuperMini nRF52840, Nice!Nano-pin-compatible. **One LED on P0.15, active-high.**
  Requires `UICR REGOUT0 = 3V3`.
- **S340 headers source:**
  `…\Adafruit_nRF52_Bootloader\lib\softdevice\s340_nrf52_7.0.1\s340_nrf52_7.0.1_API\include\`.

## Critical constraints
- Use Adafruit **Bluefruit** (talks to S340 SoftDevice). **Never NimBLE.**
- The **ANT+ network key** (8-byte, needed in Phase 3) is the *secret* token — never commit it;
  supply via an untracked `secrets.h` or build flag. It is **distinct** from the in-header ANT
  *license* key above.
- Don't block in BLE-notify / ANT-event callbacks; no `delay()` (use `millis()`/FreeRTOS);
  derive the ANT+ device number from `NRF_FICR->DEVICEID[0]`; always rotate ANT+ common pages
  `0x50`/`0x51` or Garmin may reject the sensor.

## Working style
This developer keeps the workspace intentionally minimal and has a limited usage window. **Ask for
paths/tools rather than crawling the machine** for installs or config that may not exist. Reading
specific files (e.g. the sibling bootloader repo) is fine. Prefer concrete, sourced values over
exploratory searching.
