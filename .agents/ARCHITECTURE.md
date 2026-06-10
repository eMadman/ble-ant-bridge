# Architecture — BLE-to-ANT+ Data Flow

## Protocol Specifications

### BLE Input: Cycling Power Measurement (0x2A63)

The SmartSpin2k advertises as `"SmartSpin2k"` and exposes CPS service 0x1818.
The Cycling Power Measurement characteristic (0x2A63) sends notifications with this format:

```
Byte 0-1: Flags (uint16, little-endian)
  ├── Bit 0: Pedal Power Balance Present
  ├── Bit 1: Pedal Power Balance Reference  
  ├── Bit 2: Accumulated Torque Present
  ├── Bit 3: Accumulated Torque Source
  ├── Bit 4: Wheel Revolution Data Present
  ├── Bit 5: Crank Revolution Data Present   ← IMPORTANT for cadence
  ├── Bit 6: Extreme Force Magnitudes Present
  ├── Bit 7: Extreme Torque Magnitudes Present
  └── Bits 8-15: Other optional fields

Byte 2-3: Instantaneous Power (sint16, Watts) — ALWAYS PRESENT

If Bit 5 (Crank Revolution Data Present) is set:
  Byte N+0 to N+1: Cumulative Crank Revolutions (uint16)
  Byte N+2 to N+3: Last Crank Event Time (uint16, units: 1/1024 second, rolls over at 64s)
```

**Cadence Calculation:**
```
deltaRevs = currentCrankRevs - previousCrankRevs  // handle uint16 rollover
deltaTime = currentCrankTime - previousCrankTime   // handle uint16 rollover
cadenceRPM = (deltaRevs * 60 * 1024) / deltaTime
```

**SmartSpin2k CPS format (verified from live device):**
The SmartSpin2k sends `flags = 0x0030` — **both bit 4 (Wheel Revolution Data) and bit 5 (Crank Revolution Data) set** — producing a 14-byte packet:

```
Byte 0-1:  Flags = 0x0030 (uint16 LE)
Byte 2-3:  Instantaneous Power (sint16 W)        ← e.g. 0x00AD = 173 W
Byte 4-7:  Cumulative Wheel Revolutions (uint32)  ← bit 4 block: 6 bytes total
Byte 8-9:  Last Wheel Event Time (uint16, 1/2048s)
Byte 10-11: Cumulative Crank Revolutions (uint16) ← bit 5 block: 4 bytes total
Byte 12-13: Last Crank Event Time (uint16, 1/1024s)
```

**Critical parser note:** Because bit 4 is always set, the crank data is never at a fixed
offset. The parser must walk optional fields in flag-bit order and skip the 6-byte wheel
block before reading crank data. See `ble_ant_bridge.ino:cpmNotifyCallback` for the
implemented offset walk (bits 0, 2, 4 each advance the offset before reaching bit 5).

### FTMS Input: Indoor Bike Data (0x2AD2) — ROADMAP

For future implementation. The SmartSpin2k also exposes FTMS service 0x1826.

```
Byte 0-1: Flags (uint16, little-endian)
  ├── Bit 0: More Data (INVERTED: 0 = instantaneous speed present)
  ├── Bit 1: Average Speed Present
  ├── Bit 2: Instantaneous Cadence Present
  ├── Bit 3: Average Cadence Present
  ├── Bit 4: Total Distance Present
  ├── Bit 5: Resistance Level Present
  ├── Bit 6: Instantaneous Power Present     ← Key field
  ├── Bit 7: Average Power Present
  └── Bits 8-12: Energy, HR, etc.

Fields appear IN ORDER based on set flag bits:
  - Instantaneous Speed: uint16, resolution 0.01 km/h
  - Instantaneous Cadence: uint16, resolution 0.5 RPM
  - Instantaneous Power: sint16, Watts
```

### ANT+ Output: Bicycle Power Profile (Device Type 0x0B)

#### Channel Configuration
```
Channel Type:       Master TX (0x10)
Network Number:     0 (ANT+ public network)
Network Key:        ANT_PLUS_NETWORK_KEY (supplied at build time — not in repo)
RF Frequency:       57 (2457 MHz)
Channel Period:     8182 (~4.005 Hz, ~249.5 ms per message)
Device Type:        0x0B (Bicycle Power)
Transmission Type:  0x05
Device Number:      NRF_FICR->DEVICEID[0] & 0xFFFF
```

#### Data Page 0x10 — Standard Power Only (Primary)
```
Byte 0: 0x10                          — Page Number
Byte 1: Update Event Count            — Increments each TX event (0-255, wraps)
Byte 2: Pedal Power                   — 0xFF (not used / unavailable)
Byte 3: Instantaneous Cadence         — RPM (0-254), 0xFF if unavailable
Byte 4: Accumulated Power LSB         — Running sum of instantaneous power
Byte 5: Accumulated Power MSB         — (uint16, wraps at 65535)
Byte 6: Instantaneous Power LSB       — Current power in Watts
Byte 7: Instantaneous Power MSB       — (uint16)
```

**Accumulated Power** increments by `instantaneousPower` at each event count increment.
This allows the receiver to compute average power over any interval:
`avgPower = (accumPower2 - accumPower1) / (eventCount2 - eventCount1)`

#### Data Page 0x50 — Manufacturer's Information (Common Page)
```
Byte 0: 0x50
Byte 1: 0xFF  (reserved)
Byte 2: 0xFF  (reserved)
Byte 3: HW Revision (e.g., 0x01)
Byte 4-5: Manufacturer ID (uint16 LE) — Use 0x00FF for development
Byte 6-7: Model Number (uint16 LE)   — Use 0x0001
```

#### Data Page 0x51 — Product Information (Common Page)
```
Byte 0: 0x51
Byte 1: 0xFF  (reserved)
Byte 2: SW Revision (supplemental, 0xFF if none)
Byte 3: SW Revision (main, e.g., 0x01)
Byte 4-7: Serial Number (uint32 LE) — Use full DEVICEID[0]
```

#### Broadcast Rotation Pattern
```
Messages  1-64:   Page 0x10 (Standard Power)
Message   65:     Page 0x50 (Manufacturer Info)  
Messages  66-129: Page 0x10 (Standard Power)
Message   130:    Page 0x51 (Product Info)
→ Repeat (total period = 130 messages ≈ 32.5 seconds)
```

## State Machine

```mermaid
stateDiagram-v2
    [*] --> INIT
    INIT --> SCANNING : BLE + ANT+ initialized
    SCANNING --> CONNECTING : Found "SmartSpin2k"
    CONNECTING --> DISCOVERING : BLE connected
    DISCOVERING --> SUBSCRIBED : CPS 0x2A63 found & subscribed
    SUBSCRIBED --> RUNNING : First notification received
    RUNNING --> SCANNING : BLE disconnected
    RUNNING --> RUNNING : BLE notification → parse → ANT+ TX
    SCANNING --> SCANNING : Scan timeout → retry (backoff)
```

## Threading Model (FreeRTOS)

The Adafruit Bluefruit + SDAntplus stack runs on FreeRTOS:

- **Main loop() task**: State machine management, LED updates
- **BLE SoftDevice task**: Managed by Bluefruit library (scan callbacks, notify callbacks)
- **ANT+ TX**: Driven by SoftDevice channel events (EVENT_TX in SDAntplus callback)

The BLE notification callback puts parsed data into a shared `BridgeData` struct (protected by a mutex or atomic operations). The ANT+ TX callback reads from this struct when building the next page 0x10.

## Memory Budget (nRF52840: 256KB RAM, 1MB Flash)

| Component | RAM (est.) | Flash (est.) |
|---|---|---|
| S340 SoftDevice | ~48 KB | ~192 KB (0x31000) |
| Bootloader | ~8 KB | ~32 KB |
| Bluefruit52 + FreeRTOS | ~20 KB | ~120 KB |
| SDAntplus | ~4 KB | ~30 KB |
| Application code | ~8 KB | ~40 KB |
| **Total** | **~88 KB / 256 KB** | **~414 KB / 1 MB** |
| **Headroom** | **168 KB** | **610 KB** |

Plenty of headroom for both stacks.
