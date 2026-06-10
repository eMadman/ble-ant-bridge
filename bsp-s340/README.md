# bsp-s340 — S340 v7.0.1 overlay for the Adafruit nRF52 Arduino BSP

Teaches the stock **Adafruit nRF52 Arduino BSP 1.7.0** to build and DFU-flash
apps for the **S340 v7.0.1** SoftDevice (combined BLE + ANT+) on the
**SuperMini nRF52840**, instead of the default S140 (BLE-only).

The device is already flashed (in the sibling repo
`D:\git\ss2k\bridge_project\Adafruit_nRF52_Bootloader`) with the S340 SoftDevice
+ a custom S340 bootloader. This overlay only changes the **host build toolchain**
so a compiled sketch lands at the right flash origin against the right headers.

## What it changes

| Overlay file | Installed into BSP | Why |
|---|---|---|
| `linker/nrf52840_s340_v7.ld` | `cores/nRF5/linker/` | App FLASH `ORIGIN=0x31000` (MBR 0x1000 + SD 0x30000), `END=0xF4000` (custom bootloader) |
| `variants/supermini_nrf52840/` | `variants/` | Single LED P0.15 active-high; **`USE_LFRC`** (no 32 kHz crystal) |
| `boards.txt.fragment` | appended to `boards.txt` | `superminis340` board; `build.sd_name=s340` + `build.sd_version=7.0.1` |
| S340 7.0.1 headers (from sibling repo) | `cores/nRF5/nordic/softdevice/s340_nrf52_7.0.1_API/include/` | The `nrf_sdm.h` with `ANT_LICENSE_KEY` + 3-arg `sd_softdevice_enable` |

### Why no `platform.txt` patch
`platform.txt` builds the SoftDevice include path from variables:
`-I{nordic.path}/softdevice/{build.sd_name}_nrf52_{build.sd_version}_API/include`.
Setting `build.sd_name=s340` / `build.sd_version=7.0.1` (in the boards fragment)
redirects it to the S340 headers — no file patch required.

### Why no `bluefruit.cpp` patch
Stock BSP 1.7.0 already wraps the SoftDevice enable call:
```cpp
#ifdef ANT_LICENSE_KEY
  VERIFY_STATUS( sd_softdevice_enable(&clock_cfg, nrf_error_cb, ANT_LICENSE_KEY), false );
#else
  VERIFY_STATUS( sd_softdevice_enable(&clock_cfg, nrf_error_cb), false );
#endif
```
The S340 `nrf_sdm.h` defines `ANT_LICENSE_KEY`, so the 3-arg ANT form is selected
automatically once the include path points at the S340 headers.

## Install / re-install

```powershell
pwsh -File .\bsp-s340\install-bsp-s340.ps1
```

Idempotent. **Re-run after any BSP reinstall or update** — those wipe the edits.
Override paths if your Arduino15 or sibling repo lives elsewhere:

```powershell
pwsh -File .\bsp-s340\install-bsp-s340.ps1 `
  -BspDir "<...>\Arduino15\packages\adafruit\hardware\nrf52\1.7.0" `
  -S340Include "<...>\s340_nrf52_7.0.1_API\include"
```

Then in Arduino IDE: **Tools > Board > Adafruit nRF52 > "SuperMini nRF52840 (S340)"**,
pick the COM port, and **Upload** (never *Burn Bootloader*).

## DFU `sd_fwid` = `0xFFFE`

The boards fragment sets `build.sd_fwid=0xFFFE`, which becomes `--sd-req 0xFFFE`
in the DFU package — the "skip SoftDevice requirement check" wildcard. S340 has no
canonical firmware-id in the `adafruit-nrfutil` table, so the wildcard avoids a
spurious version-mismatch rejection by the on-device bootloader. If you want a
strict check instead, replace it with the real S340 7.0.1 fwid (the uint16 LE at
flash offset `0x300C` of `s340_nrf52_7.0.1_softdevice.hex`).

## RAM origin tuning

`RAM ORIGIN=0x20006000` in the linker is a starting reservation. S340 with ANT
may need more RAM than S140. If the boot serial log prints a SoftDevice RAM
requirement above `0x20006000`, raise `ORIGIN` in `nrf52840_s340_v7.ld` to the
printed value, re-run the installer, and re-flash.

## Verify

Flash `verify_ble/verify_ble.ino`. Pass = Serial @115200 prints
`[VERIFY] Advertising as 'BLE-ANT-TEST'`, the P0.15 LED blinks ~1 Hz, and
nRF Connect sees / can connect+disconnect `BLE-ANT-TEST`.
