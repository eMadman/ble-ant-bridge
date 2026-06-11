# Roadmap — BLE-ANT+ Bridge

## Current Milestone: PoC (Proof of Concept)

### Phase 0: Hardware Preparation ✅
- [x] Register at thisisant.com, download S340 v7.0.1 hex + headers
- [x] prepare Pi Pico with debugprobe
- [x] Install toolchain: GNU ARM, OpenOCD, adafruit-nrfutil
- [x] Clone & modify Adafruit nRF52 Bootloader for S340
- [x] Create SuperMini board variant (Nice!Nano pin map + S340)
- [x] Flash S340 SoftDevice + custom bootloader via OpenOCD
- [x] Verify: board enumerates (verify it appears as a mass storage drive)
- [x] Modify Adafruit Arduino BSP (boards.txt, linker) for S340 — see `bsp-s340/` overlay (verified by flashing a minimal BLE test sketch advertising as BLE-ANT-TEST)

### Phase 1: BLE Central — Scan & Connect ✅
- [x] Create project skeleton (ble_ant_bridge.ino, config.h)
- [x] Implement BLE scan with filter for "SmartSpin2k" name
- [x] Implement connect → service discovery → characteristic subscription
- [x] Verify: serial output shows CPS notifications from SmartSpin2k
- [x] Implement reconnection on disconnect (with backoff)

### Phase 2: BLE Data Parsing ✅
- [x] Implement CPS 0x2A63 parser (power + crank revolution data)
- [x] Implement cadence calculation from crank revolution deltas
- [x] Handle uint16 rollover for crank revolutions and event time
- [x] Verify: serial output shows correct power (W) and cadence (RPM)

### Phase 3: ANT+ Bicycle Power TX ✅
- [x] Initialize ANT+ channel (raw sd_ant_* — no library; CHANNEL_TYPE_MASTER_TX_ONLY)
- [x] Implement Data Page 0x10 (Standard Power Only)
- [x] Implement Data Pages 0x50, 0x51 (common pages)
- [x] Implement broadcast rotation pattern (130-message cycle)
- [x] Verify: Garmin sees "Bicycle Power" sensor (dev# from FICR)
- [x] Verify: power value updates in real-time

### Phase 4: Integration & Polish ✅ (closed 2026-06-10)
- [x] Wire BLE parser output → ANT+ transmitter input
- [x] Implement stale data handling (no BLE update >3s → power=0)
- [x] End-to-end test: SmartSpin2k → bridge → Garmin head unit ✅
- [~] ~~Add LED status indicators~~ — **deferred (won't-do this milestone)**; the
  basic scan/connect/run LED in `serviceLed()` is enough for the PoC.
- [~] ~~Measure latency (target: <500ms BLE-to-ANT+)~~ — **deferred (won't-do this
  milestone)**; perceived latency is acceptable in end-to-end testing.

---

## Post-PoC Milestone: ANT+ FE-C — Full Smart Trainer Control ⬜

**Goal**: Replace ANT+ BPWR (data-only) with ANT+ FE-C (bidirectional).
Garmin sees the SmartSpin2k as a full controllable smart trainer.

**Confirmed scope decisions (2026-06-10):**
- **Replace BPWR** with FE-C (don't run both) — FE-C trainer page 25 carries
  power+cadence. `ant_power_tx.*` stays in-tree but unwired from the `.ino`.
- **TX first (Phase A), then control (Phase B).**
- **Keep CPS 0x2A63** as the power/cadence source; page-16 speed = invalid
  (Garmin computes virtual speed in trainer mode). FTMS is added in Phase B for
  the control-write path only.
- **ERG first** for control (target power); resistance + simulation follow.

### FE-C Phase A: Trainer Data TX ✅ (verified 2026-06-10)
- [x] New `ant_fec.*` module: bidirectional Master channel, Device Type 0x11,
      RF 57, period 8192 (raw `sd_ant_*`, no library; `sd_ant_enable()` skipped)
- [x] TX Page 16 (General FE Data) + Page 25 (Specific Trainer Data)
- [x] TX Page 54 (FE Capabilities — advertise ERG)
- [x] Reuse common pages 80/81; implement 16↔25 + background rotation
- [x] Swap `.ino` from `AntPowerTx` → `AntFec`
- [x] Verify: Garmin pairs as **Smart Trainer** with live power + cadence ✅
- [x] Verify: Garmin shows ERG target-power UI and sends control command ✅
      (bridge correctly ignores it — RX handler not yet wired; Phase B)

### FE-C Phase B: Garmin → Trainer Control (ERG) 🔧 (in progress 2026-06-10)

**What this enables on Garmin:**
- ERG mode (Garmin sets target wattage → SmartSpin2k adjusts resistance)
- Simulation mode (Garmin sends grade % → SmartSpin2k simulates climb)
- Follow a Course (GPX elevation auto-controls resistance)
- Follow a Workout (structured workout power targets)
- Manual resistance % control from Garmin

**ANT+ FE-C ↔ SmartSpin2k FTMS Control Point Mapping:**

| ANT+ FE-C Page | Direction | FTMS Equivalent | SmartSpin2k Support |
|---|---|---|---|
| Page 16 (General FE Data) | TX → Garmin | — (constructed from SS2k data) | N/A |
| Page 25 (Trainer Data) | TX → Garmin | Indoor Bike Data (power, cadence) | ✅ |
| Page 48 (Basic Resistance) | RX ← Garmin | SetTargetResistanceLevel (0x04) | ✅ Verified |
| Page 49 (Target Power / ERG) | RX ← Garmin | SetTargetPower (0x05) | ✅ Verified |
| Page 51 (Track/Simulation) | RX ← Garmin | SetIndoorBikeSimulationParameters (0x11) | ✅ Verified |
| Page 54 (FE Capabilities) | TX → Garmin | FTMS Feature flags | ✅ |
| Page 55 (User Config) | RX ← Garmin | — (rider weight, wheel size) | Partial |

> Note: the channel + TX pages 16/25/54 land in **Phase A** above. Phase B adds the
> RX (control) path and the BLE write-back. ERG (page 49) ships first; basic
> resistance (48) and simulation (51) follow once the write path is proven.

**Phase B implementation tasks (ERG first):**
- [x] FE-C RX in `AntFec::service()`: handle EVENT_RX; parse Page 49 (ERG,
      0.25 W units → W = val/4), Page 48 (resistance), Page 51 (simulation);
      reply Page 71 (Command Status) on next broadcast slot
- [x] `bridge_core` reverse channel: `ControlCommand` (mode + target values +
      pending flag), `bridgeSetControl()` / `bridgeConsumeControl()`, critical-section guarded
- [x] BLE FTMS write path: `ftms` / `ftmsCP` client objects; discover 0x1826 +
      0x2AD9 in `connectCallback`; enable indications; Request Control (0x00);
      `serviceControl()` in loop() writes 0x05 / 0x04 / 0x11 on each new command
- [x] Page 54 capabilities updated: advertise all three modes (bits 0+1+2)
- [ ] Verify ERG end to end: Garmin ERG workout → SmartSpin2k target power
- [ ] Verify basic resistance (48→0x04) and simulation (51→0x11)
- [ ] FE state machine (Ready, In Use, Finished, Paused)
- [ ] (Stretch) calibration flow (SpinDown via FE-C ↔ FTMS SpinDownControl)
- [ ] (Stretch) parse FTMS Indoor Bike Data 0x2AD2 for full data set / real speed

**Complexity note**: FE-C is ~3-5x the PoC effort. The PoC validated the hard parts
(S340 setup, BLE Central, ANT+ channel management) first.

---

## Future Roadmap Items

### nRF Connect SDK Migration (If Arduino Path Unstable)
- [ ] Evaluate NCS + ANT Wireless SDK add-on
- [ ] Port BLE Central logic to Zephyr BLE API
- [ ] Port ANT+ TX to NCS ANT API
- [ ] Compare stability, flash size, debugging experience

### Power Optimization
- [ ] Reduce BLE connection interval when data rate allows
- [ ] Implement sleep mode when SmartSpin2k not found for >5 min
- [ ] Measure battery life (if using LiPo on SuperMini)

### Multi-Sensor Support
- [ ] Bridge Heart Rate (BLE HRS → ANT+ HRM)
- [ ] Bridge Speed/Cadence (BLE CSC → ANT+ BSC)

### Configuration
- [ ] BLE peripheral name configurable (not just "SmartSpin2k")
- [ ] ANT+ device number configurable via BLE characteristic
- [ ] Store config in nRF52840 flash (FDS or LittleFS)
