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
- [x] Modify Adafruit Arduino BSP (boards.txt, linker) for S340 — see `bsp-s340/` overlay; verified by flashing `verify_ble` (advertises as BLE-ANT-TEST)

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

### Phase 4: Integration & Polish ⬜
- [x] Wire BLE parser output → ANT+ transmitter input
- [x] Implement stale data handling (no BLE update >3s → power=0)
- [ ] Add LED status indicators
- [x] End-to-end test: SmartSpin2k → bridge → Garmin head unit ✅
- [ ] Measure latency (target: <500ms BLE-to-ANT+)

---

## Phase 2: ANT+ FE-C — Full Smart Trainer Control (Post-PoC) ⬜

**Goal**: Replace ANT+ BPWR (data-only) with ANT+ FE-C (bidirectional).
Garmin sees the SmartSpin2k as a full controllable smart trainer.

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

**Implementation Tasks:**
- [ ] Research ANT+ FE-C specification (Device Type 0x11) in detail
- [ ] Change ANT+ channel from BPWR Master TX to FE-C Master TX+RX
- [ ] Implement FE-C TX pages: 16 (General FE), 25 (Trainer Data), 50, 51, 54
- [ ] Implement FE-C RX handlers for control pages: 48, 49, 51
- [ ] Implement BLE FTMS Control Point write path (bridge → SmartSpin2k)
  - Subscribe to FTMS Control Point responses (indications)
  - Write SetTargetPower / SetTargetResistance / SetSimulationParams
- [ ] Implement FE state machine (Ready, In Use, Finished, Paused)
- [ ] Handle calibration flow (SpinDown via FE-C ↔ FTMS SpinDownControl)
- [ ] Parse FTMS Indoor Bike Data (0x2AD2) for full data set (speed, cadence, power, resistance)
- [ ] End-to-end test: Garmin Follow-a-Course → SmartSpin2k resistance changes
- [ ] End-to-end test: Garmin ERG workout → SmartSpin2k target power

**Complexity note**: FE-C is ~3-5x the PoC effort. The PoC validates the hard parts
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
