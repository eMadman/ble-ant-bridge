# Coding Guidelines — BLE-ANT+ Bridge

## Language & Framework

- **Language**: C++ (Arduino dialect)
- **Framework**: Arduino Core for nRF52 (Adafruit BSP, modified for S340)
- **Standard**: C++11 (what the Adafruit nRF52 toolchain supports)
- **IDE**: Arduino IDE or PlatformIO (both supported via Adafruit BSP)

## Code Style

### Naming Conventions
```cpp
// Constants: UPPER_SNAKE_CASE
#define ANT_DEVICE_TYPE_BPWR    0x0B
#define ANT_CHANNEL_PERIOD      8182
// Network key is NOT defined in tracked source — see "ANT+ Network Key" below.
extern const uint8_t ANT_PLUS_NETWORK_KEY[8];   // supplied at build time

// Classes: PascalCase
class AntPowerTransmitter { ... };

// Methods/Functions: camelCase
void parseNotification(const uint8_t* data, size_t len);

// Member variables: camelCase with no prefix
int16_t instantaneousPower;
uint8_t updateEventCount;

// Local variables: camelCase
uint16_t crankRevDelta = currentRevs - previousRevs;
```

### File Organization
- Each `.h` file has a corresponding `.cpp` file
- Use `#pragma once` for include guards
- Keep `ble_ant_bridge.ino` minimal — delegate to modules
- All configuration constants in `config.h`

### Comments
- Use `//` for inline comments
- Use `/* ... */` for multi-line block comments
- Document all public functions with a brief description
- Document byte-level protocol parsing with offset annotations

```cpp
// Parse Cycling Power Measurement (0x2A63)
// Bytes 0-1: Flags (uint16 LE)
// Bytes 2-3: Instantaneous Power (sint16 LE, Watts)
// If flags bit 5: Cumulative Crank Revs (uint16) + Last Crank Event Time (uint16, 1/1024s)
void parseCPS(const uint8_t* data, size_t len) { ... }
```

## Patterns

### BLE Data Handling
```cpp
// Always validate data length before parsing
if (len < 4) {
    Serial.println("[BLE] CPS notification too short");
    return;
}

// Use little-endian extraction helpers
uint16_t flags = data[0] | (data[1] << 8);
int16_t power = (int16_t)(data[2] | (data[3] << 8));
```

### ANT+ Page Construction
```cpp
// Build page 0x10 in a uint8_t[8] buffer
void buildPage16(uint8_t* buf, uint8_t eventCount, uint8_t cadence,
                 uint16_t accumPower, uint16_t instPower) {
    buf[0] = 0x10;                          // Page number
    buf[1] = eventCount;                     // Wrapping counter
    buf[2] = 0xFF;                           // Pedal power (unused)
    buf[3] = cadence;                        // RPM or 0xFF
    buf[4] = (uint8_t)(accumPower & 0xFF);   // Accumulated power LSB
    buf[5] = (uint8_t)(accumPower >> 8);     // Accumulated power MSB
    buf[6] = (uint8_t)(instPower & 0xFF);    // Instantaneous power LSB
    buf[7] = (uint8_t)(instPower >> 8);      // Instantaneous power MSB
}
```

### Rollover Handling
```cpp
// uint16 rollover-safe delta
uint16_t safeDelta16(uint16_t current, uint16_t previous) {
    return (uint16_t)(current - previous);  // C unsigned arithmetic handles wrap
}
```

### State Management
```cpp
// Use an enum for bridge state
enum class BridgeState : uint8_t {
    INIT,
    SCANNING,
    CONNECTING,
    DISCOVERING,
    SUBSCRIBED,
    RUNNING
};

// Global state variable
volatile BridgeState bridgeState = BridgeState::INIT;
```

## Critical "Do Not Do" List

1. **DO NOT use NimBLE** — it doesn't talk to S340 SoftDevice. Use Bluefruit52 only.
2. **DO NOT block in callbacks** — BLE notify and ANT+ event callbacks must return quickly. Copy data and process in loop().
3. **DO NOT use `delay()`** for timing — use `millis()` comparisons or FreeRTOS tasks.
4. **DO NOT assume CPS notification rate** — it varies. Design for 1-4 Hz input, 4 Hz ANT+ output.
5. **DO NOT forget ANT+ common pages** — Garmin devices may reject sensors that don't rotate in pages 0x50 and 0x51.
6. **DO NOT hardcode the ANT+ device number** — derive from `NRF_FICR->DEVICEID[0]`.
7. **DO NOT commit the ANT+ network key** — see "ANT+ Network Key" below. Supply it at build time; keep it out of tracked source and out of these docs.

## ANT+ Network Key

The ANT+ network key is part of the ANT+ Documents covered by the ANT+ Adopter
Agreement (clause c: do not distribute any part of the Documents outside your
organization). To keep this repo publishable, the key is **never hardcoded in
tracked source or written into project docs**.

- Define it in an **untracked** header (e.g. `secrets.h`, listed in `.gitignore`)
  or inject it via a build flag / PlatformIO `build_flags` / environment value.
- Tracked code only references it as `extern const uint8_t ANT_PLUS_NETWORK_KEY[8];`
  and fails to link if the builder hasn't provided it.
- Provide a `secrets.example.h` with a zeroed/placeholder key so the build shape
  is obvious without shipping the real value.
- Each builder is responsible for being their own ANT+ Adopter and sourcing the
  key from thisisant.com. We do not distribute it for them.

> **Don't confuse this with the ANT license key.** The S340 also needs an *ANT
> license key* (`ANT_LICENSE_KEY` in `nrf_sdm.h`) passed to `sd_softdevice_enable()`.
> That one is a Nordic SoftDevice enablement token, ships hardcoded in the Nordic
> headers, and is *meant* to be in source — leave it alone. The rule above applies
> only to the 8-byte **network key** you feed to the ANT+ channel config. See
> PROJECT.md → "Critical Constraints" for the full distinction.

## Debugging

### Serial Output
- Use `Serial.begin(115200)` in setup
- Prefix log lines with module tag: `[BLE]`, `[ANT]`, `[BRIDGE]`, `[STATE]`
- In production, disable verbose logging to save CPU time

### LED Indicators (SuperMini)
| State | Blue LED (P0.15) | Red LED (P0.17) |
|---|---|---|
| Scanning | Slow blink 1Hz | Off |
| Connected + data | Solid | Off |
| Connected + stale | Solid | Slow blink |
| Error | Off | Fast blink |

## Testing Strategy

1. **Unit test parsing**: Feed known CPS byte sequences, verify extracted power/cadence
2. **BLE integration**: Connect to real SmartSpin2k, verify serial output shows correct values
3. **ANT+ integration**: Use ANT+ USB stick + SimulANT+ or Garmin to verify reception
4. **End-to-end**: Full chain from SmartSpin2k pedaling to Garmin power display
