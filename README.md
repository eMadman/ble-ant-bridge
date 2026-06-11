# SmartSpin2k BLE → ANT+ Bridge

Firmware for an **nRF52840 SuperMini** that BLE-Central-connects to a
[SmartSpin2k](https://github.com/doudar/SmartSpin2k), reads its Cycling Power data, and
re-broadcasts it as an **ANT+ FE-C smart trainer** so Garmin head units (and other ANT+
displays) recognize it natively.

```
 SmartSpin2k (ESP32)          nRF52840 SuperMini              Garmin / ANT+ display
┌────────────────────┐     ┌──────────────────────────┐     ┌──────────────────────┐
│ BLE Peripheral     │────▶│ BLE Central │   ANT+ TX   │────▶│ ANT+ Receiver        │
│ Cycling Power 1818 │ BLE │ (Bluefruit) │  (sd_ant_*) │ ANT+│ pairs as a           │
│ (FTMS 1826)        │     │             │             │     │ "Smart Trainer" (FE-C)│
│                    │     │      S340 SoftDevice       │     │                      │
└────────────────────┘     └──────────────────────────┘     └──────────────────────┘
```

**Status:** live **power + cadence** to a Garmin works today (broadcasts as an FE-C
trainer). **ERG control** (the Garmin commanding a target wattage back to the SmartSpin2k)
is in progress.

> Why this exists: SmartSpin2k advertises power over BLE, but Garmin head units often won't
> use a BLE power source reliably. ANT+ is the protocol Garmin natively trusts, so this
> bridge translates one to the other in hardware. The SmartSpin2k firmware is **not**
> modified.

---

## ⚠️ Read this first

- **NEVER use Arduino IDE's "Burn Bootloader."** This board runs a custom **S340 SoftDevice
  + bootloader**. "Burn Bootloader" overwrites both and bricks the ANT+ side. You flash
  application firmware with the normal **Upload** button (app-only DFU). Re-flashing the
  SoftDevice/bootloader is only ever done deliberately over SWD (Stage 2).
- **Two different "keys" — don't confuse them:**
  - **ANT *license* key** — ships *inside* the Nordic S340 header (`nrf_sdm.h`), it's the
    public Garmin evaluation key, not secret, and you never touch it.
  - **ANT+ *network* key** — the 8-byte radio key you put in `secrets.h`. Covered by the
    ANT+ Adopter Agreement; keep it out of the repo.

---

## What you'll need

### Hardware
| Item | Notes |
|---|---|
| **[nRF52840 SuperMini](https://www.aliexpress.com/item/1005009026511947.html?spm=a2g0o.order_list.order_list_main.17.5aa71802Spcy4r)** | Nice!Nano-pin-compatible. One blue LED on **P0.15**, active-high. |
| **[DAPLink probe](https://www.aliexpress.com/item/1005005303809188.html?spm=a2g0o.productlist.main.1.17d419c8A41kIb&algo_pvid=d5ca1c4a-68a6-4aab-8d23-c3c0a8cfdcf7&pdp_ext_f=%7B%22order%22%3A%224971%22%2C%22spu_best_type%22%3A%22price%22%2C%22eval%22%3A%221%22%2C%22fromPage%22%3A%22search%22%7D&utparam-url=scene%3Asearch%7Cquery_from%3A%7Cx_object_id%3A1005005303809188%7C_p_origin_prod%3A)** (CMSIS-DAP SWD) | One-time, to flash the bootloader + SoftDevice over SWD. J-Link or ST-Link also work (see Stage 2). |
| 4× jumper wires | SWD: **SWDIO, SWCLK, GND, 3V3** (DAPLink ↔ SuperMini SWD pads). |
| 2× USB cables | One for the SuperMini, one for the DAPLink. |
| A **SmartSpin2k** | The BLE power source this bridge talks to. |
| A Garmin (or other ANT+ display) | For final pairing/testing. |

### Accounts
- Register as an **"ANT+ Adopter"** at **[thisisant.com](https://www.thisisant.com/register/)**.
  Approval typically takes about one business day, and you need it for **both**:
  1. Downloading the **S340 v7.0.1 SoftDevice** (Stage 1), and
  2. Obtaining the **8-byte ANT+ network key** (Stage 4).

### Toolchain
- **git**
- **ARM GCC** (`arm-none-eabi-gcc` 9.x or newer) — for building the bootloader
- **Python 3** with `intelhex` and `adafruit-nrfutil`
  (`pip install intelhex adafruit-nrfutil`)
- **pyOCD** (`pip install pyocd`) — drives the DAPLink probe
- **Arduino IDE** with the **Adafruit nRF52 BSP 1.7.0** (installed via Board Manager)

---

## Repo layout

| Path | What it is |
|---|---|
| [`ble_ant_bridge/`](ble_ant_bridge/) | The firmware sketch (`ble_ant_bridge.ino`, `config.h`, `secrets.h.example`, ANT/BLE source). |
| [`bsp-s340/`](bsp-s340/) | Installer that teaches the Adafruit BSP 1.7.0 to build for S340 + the SuperMini. |

The S340 SoftDevice and bootloader are built from the **sibling repo**
`Adafruit_nRF52_Bootloader` (cloned in Stage 2) — they are not part of this repo.

---

## Stage 1 — Obtain the S340 v7.0.1 SoftDevice

The S340 (combined BLE + ANT+) SoftDevice is closed-source and only distributed by Garmin,
so it isn't checked into either repo. With your approved thisisant.com account:

1. Download **SoftDevice S340 v7.0.1** from the ANT+ developer resources and extract it.
2. Clone the bootloader repo (you'll build from it in Stage 2):
   ```bash
   git clone --recurse-submodules https://github.com/eMadman/Adafruit_nRF52_Bootloader
   ```
3. Copy these three items from the extracted download into
   `Adafruit_nRF52_Bootloader/lib/softdevice/s340_nrf52_7.0.1/`, **renaming** as shown:
   | From the download | Rename to |
   |---|---|
   | `ANT_s340_nrf52_7.0.1.API/` (folder) | `s340_nrf52_7.0.1_API/` |
   | `ANT_s340_nrf52_7.0.1.hex` | `s340_nrf52_7.0.1_softdevice.hex` |
   | `License_Agreement_*.pdf` | *(keep as-is)* |

The ANT *license* (evaluation) key now lives at
`…/s340_nrf52_7.0.1/s340_nrf52_7.0.1_API/include/nrf_sdm.h` (around **line 191**). You don't
need to edit it — it's used automatically.

> Reference: the bootloader repo's own
> `lib/softdevice/s340_nrf52_7.0.1/readme.md` documents this same step.

---

## Stage 2 — Build & flash the bootloader + SoftDevice

This puts the S340 SoftDevice and a SuperMini-aware bootloader on the board. **Done once
per board.** The `supermini_nrf52840` board is already defined in the bootloader repo (LED
P0.15, `REGOUT0 = 3V3`, double-reset DFU, USB label `MINIBOOT`).

### Build
From the `Adafruit_nRF52_Bootloader` clone:
```bash
make BOARD=supermini_nrf52840 all
```
Artifacts land in `_build/build-supermini_nrf52840/`. The one you flash is the **merged**
image (SoftDevice + MBR + bootloader in a single hex):
```
_build/build-supermini_nrf52840/supermini_nrf52840_bootloader-<version>_s340_7.0.1.hex
```
`<version>` comes from `git describe` (e.g. `1.0.0`, `g1a2b3c4`). Tab-complete it or list
the directory to get the exact name.

> The `ANT_LICENSE_KEY` does **not** need to be passed to this build — the bootloader
> itself doesn't use ANT+; the application firmware (Stage 5) supplies the license from the
> S340 header. Building plain `all` is enough.

### Wire the DAPLink (SWD)
| DAPLink | → | SuperMini |
|---|---|---|
| SWDIO | → | SWDIO pad |
| SWCLK | → | SWCLK pad |
| GND | → | GND |
| 3V3 / VTref | → | 3V3 |

### Flash (DAPLink → pyOCD)
Erase, then flash the merged hex:
```bash
pyocd erase -t nrf52840 --chip
pyocd flash -t nrf52840 _build/build-supermini_nrf52840/supermini_nrf52840_bootloader-<version>_s340_7.0.1.hex
```

<details>
<summary>Using a J-Link or ST-Link instead?</summary>

- **J-Link** (default flasher): `make BOARD=supermini_nrf52840 flash-sd` then
  `make BOARD=supermini_nrf52840 flash`. (`make … flash` alone is **not** enough on a
  virgin board — it omits the SoftDevice.)
- **pyOCD via make:** add `FLASHER=pyocd`, e.g.
  `make BOARD=supermini_nrf52840 FLASHER=pyocd flash-sd`.
- **ST-Link:** flash the same merged `…_s340_7.0.1.hex` with OpenOCD or `pyocd` (CMSIS-DAP).
</details>

### Verify
Press **reset twice within ~0.5 s** (double-reset). The board should appear as a USB
mass-storage drive labelled **`MINIBOOT`**. That confirms the SoftDevice + bootloader are
live.

---

## Stage 3 — Install the S340 Arduino BSP overlay

Stock Adafruit BSP 1.7.0 only knows S140 (BLE-only). The `bsp-s340/` installer adds an S340
linker script, the SuperMini variant, a board entry, and the S340 headers — so Arduino IDE
can build app-only DFU images for this board. It's idempotent (safe to re-run).

1. Install **Adafruit nRF52 BSP 1.7.0**: Arduino IDE → *Tools ▸ Board ▸ Board Manager*,
   search "Adafruit nRF52", install **1.7.0**.
2. Run the installer, pointing it at the S340 headers inside your **bootloader clone** from
   Stage 1:

   **Windows (PowerShell):**
   ```powershell
   pwsh -File .\bsp-s340\install-bsp-s340.ps1 `
       -S340Include "<clone>\Adafruit_nRF52_Bootloader\lib\softdevice\s340_nrf52_7.0.1\s340_nrf52_7.0.1_API\include"
   ```

   **macOS / Linux (bash):**
   ```bash
   ./bsp-s340/install-bsp-s340.sh \
       --s340-include <clone>/Adafruit_nRF52_Bootloader/lib/softdevice/s340_nrf52_7.0.1/s340_nrf52_7.0.1_API/include
   ```
3. **Restart Arduino IDE**, then select
   **Tools ▸ Board ▸ Adafruit nRF52 ▸ "SuperMini nRF52840 (S340)"**.

> More detail (what each overlay file changes, `sd_fwid` wildcard, RAM tuning) is in
> [`bsp-s340/README.md`](bsp-s340/README.md).

---

## Stage 4 — Configure the firmware

1. **Network key:** copy the template and fill in your 8-byte ANT+ network key (from
   thisisant.com):
   ```bash
   cp ble_ant_bridge/secrets.h.example ble_ant_bridge/secrets.h
   ```
   Edit `ble_ant_bridge/secrets.h` and set:
   ```c
   #define ANT_PLUS_NETWORK_KEY { 0x.., 0x.., 0x.., 0x.., 0x.., 0x.., 0x.., 0x.. }
   ```
   `secrets.h` is git-ignored — it won't be committed.
2. **Target device name:** in [`ble_ant_bridge/config.h`](ble_ant_bridge/config.h), set
   `TARGET_DEVICE_NAME` to your SmartSpin2k's advertised BLE name (default is
   `"SmartSpin2k"`). The other constants (scan timing, ANT FE-C channel, LED blink rates)
   have working defaults.

---

## Stage 5 — Build & flash the firmware

1. Open the sketch **folder** [`ble_ant_bridge/ble_ant_bridge.ino`](ble_ant_bridge/ble_ant_bridge.ino)
   in Arduino IDE (the `.ino` must stay inside its same-named folder alongside `config.h`,
   `secrets.h`, etc.).
2. Confirm the board is **"SuperMini nRF52840 (S340)"**.
3. Click **Upload** (app-only DFU). **Not** "Burn Bootloader."
   - If the IDE can't trigger DFU automatically, **double-reset** the board first, then
     Upload.
4. Open Serial Monitor @ **115200**. You should see the state machine advance:
   `SCANNING → CONNECTING → DISCOVERING → SUBSCRIBED → RUNNING`.
   - LED ~1 Hz blink while scanning; fast/solid once connected.

---

## Stage 6 — Pair & test

1. Power on the SmartSpin2k and start pedaling so it produces power data.
2. Watch the bridge's Serial Monitor reach `RUNNING` and show parsed power/cadence.
3. On your Garmin, add a new sensor — the bridge appears as a **Smart Trainer** (ANT+ FE-C)
   with live **power and cadence**.

> **ERG mode** (the Garmin setting a target wattage that the SmartSpin2k follows) is not
> finished yet.

---

## Troubleshooting

**LED states (firmware):** ~1 Hz blink = scanning for the SmartSpin2k; fast blink / solid =
connected and bridging. Dark = check power / that firmware is running.

**Upload can't find a port / fails to start DFU:** double-reset the board (reset twice
within ~0.5 s) to force DFU, then Upload again.

**Boot log prints a SoftDevice RAM requirement above `0x20006000`:** raise `RAM ORIGIN` in
`bsp-s340/linker/nrf52840_s340_v7.ld` to the printed value, re-run the Stage 3 installer,
and re-flash. (Background in [`bsp-s340/README.md`](bsp-s340/README.md).)

**Bootloader rejects a DFU package over version mismatch:** the board entry uses
`sd_fwid = 0xFFFE` (the "skip SoftDevice check" wildcard) because S340 has no canonical
fwid in `adafruit-nrfutil`. See the `sd_fwid` note in [`bsp-s340/README.md`](bsp-s340/README.md).

**`/usr/bin/arm-none-eabi-gcc: No such file or directory`:** pass the toolchain path, e.g.
`make CROSS_COMPILE=/path/to/gcc-arm/bin/arm-none-eabi- BOARD=supermini_nrf52840 all`.
Confirm `arm-none-eabi-gcc --version` is ≥ 9.x.

**`ModuleNotFoundError: No module named 'intelhex'`:** `pip install intelhex`.

**`nrfjprog: No such file or directory`:** only needed for the J-Link path. Use the DAPLink
+ pyOCD path instead, or install Nordic's nRF Command Line Tools.

---

## Further reading

- [`bsp-s340/README.md`](bsp-s340/README.md) — what the BSP overlay changes and why.
