# bsp-s340 — S340 v7.0.1 overlay for the Adafruit nRF52 Arduino BSP

Teaches the stock **Adafruit nRF52 Arduino BSP 1.7.0** to build and DFU-flash
apps for the **S340 v7.0.1** SoftDevice (combined BLE + ANT+) on the
**SuperMini nRF52840**, instead of the default S140 (BLE-only).

## Prerequisites

- **Arduino IDE** with the **Adafruit nRF52 BSP 1.7.0** installed
  (`Tools > Board > Board Manager`, search "Adafruit nRF52", install 1.7.0)
- A SuperMini nRF52840 already flashed with an S340 SoftDevice + compatible bootloader
  (see the main project README)
- The **Adafruit nRF52 Bootloader** repo cloned locally — place your S340 7.0.1 API
  headers inside (lib\softdevice\s340_nrf52_7.0).
  ```
  git clone https://github.com/eMadman/Adafruit_nRF52_Bootloader
  ```

> **Never use "Burn Bootloader"** in Arduino IDE — it will overwrite the S340 SoftDevice
> and bootloader. Use only the normal **Upload** (app-only DFU).

## Install

Run the installer once after installing or updating the BSP. It is idempotent — safe to re-run.

**Windows (PowerShell):**
```powershell
pwsh -File .\bsp-s340\install-bsp-s340.ps1 `
    -S340Include "<clone-root>\Adafruit_nRF52_Bootloader\lib\softdevice\s340_nrf52_7.0.1\s340_nrf52_7.0.1_API\include"
```

Override the BSP path if your Arduino15 folder is non-standard:
```powershell
pwsh -File .\bsp-s340\install-bsp-s340.ps1 `
    -BspDir   "<...>\Arduino15\packages\adafruit\hardware\nrf52\1.7.0" `
    -S340Include "<...>\s340_nrf52_7.0.1_API\include"
```

**macOS / Linux (bash):**
```bash
./bsp-s340/install-bsp-s340.sh \
    --s340-include <clone-root>/Adafruit_nRF52_Bootloader/lib/softdevice/s340_nrf52_7.0.1/s340_nrf52_7.0.1_API/include
```

Override the BSP path if needed:
```bash
./bsp-s340/install-bsp-s340.sh \
    --bsp-dir    "$HOME/.arduino15/packages/adafruit/hardware/nrf52/1.7.0" \
    --s340-include "<...>/s340_nrf52_7.0.1_API/include"
```

After installing, restart Arduino IDE if it was already open, then select:
**Tools > Board > Adafruit nRF52 > "SuperMini nRF52840 (S340)"**

## What it changes

| Overlay file | Installed into BSP | Why |
|---|---|---|
| `linker/nrf52840_s340_v7.ld` | `cores/nRF5/linker/` | App `FLASH ORIGIN=0x31000` (MBR + S340), ends at `0xF4000` (bootloader region) |
| `variants/supermini_nrf52840/` | `variants/` | Single LED P0.15 active-high; `USE_LFRC` (no 32 kHz crystal) |
| `boards.txt.fragment` | appended to `boards.txt` | `superminis340` board entry; `build.sd_name=s340` + `build.sd_version=7.0.1` |
| S340 7.0.1 headers (from bootloader repo) | `cores/nRF5/nordic/softdevice/s340_nrf52_7.0.1_API/include/` | `nrf_sdm.h` with `ANT_LICENSE_KEY` + 3-arg `sd_softdevice_enable` |

`platform.txt` already builds the SoftDevice include path from `build.sd_name` /
`build.sd_version`, so pointing those variables at `s340` / `7.0.1` routes the
compiler to the S340 headers automatically — no `platform.txt` patch required.

Stock BSP 1.7.0 wraps `sd_softdevice_enable` in `#ifdef ANT_LICENSE_KEY`, and the
S340 `nrf_sdm.h` defines that macro, so the correct 3-arg ANT form is selected
automatically — no `bluefruit.cpp` patch required.

## Notes

### DFU `sd_fwid = 0xFFFE`

The boards fragment sets `build.sd_fwid=0xFFFE`, which becomes `--sd-req 0xFFFE`
in the DFU package — the "skip SoftDevice requirement check" wildcard. S340 has no
canonical firmware-id in the `adafruit-nrfutil` table, so the wildcard avoids a
spurious version-mismatch rejection by the bootloader. To use a strict check instead,
replace `0xFFFE` with the real S340 7.0.1 fwid (the uint16 LE at flash offset `0x300C`
of `s340_nrf52_7.0.1_softdevice.hex`).

### RAM origin tuning

`RAM ORIGIN=0x20006000` in the linker script is a starting reservation. S340 with ANT
may need more RAM than S140. If the boot serial log prints a SoftDevice RAM requirement
above `0x20006000`, raise `ORIGIN` in `linker/nrf52840_s340_v7.ld` to the printed
value, re-run the installer, and re-flash.
