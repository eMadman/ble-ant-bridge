<#
.SYNOPSIS
  Lay the S340 v7.0.1 overlay into an installed Adafruit nRF52 Arduino BSP so the
  "SuperMini nRF52840 (S340)" board can be selected, built, and DFU-flashed.

.DESCRIPTION
  Idempotent. Re-run after any BSP reinstall/update (which wipes these edits).
  Copies:
    - S340 7.0.1 SoftDevice headers -> cores/nRF5/nordic/softdevice/s340_nrf52_7.0.1_API/include
    - nrf52840_s340_v7.ld           -> cores/nRF5/linker/
    - variants/supermini_nrf52840/  -> variants/
    - boards.txt.fragment           -> appended to boards.txt (once)

  It does NOT patch bluefruit.cpp: stock BSP 1.7.0 already wraps
  sd_softdevice_enable() in `#ifdef ANT_LICENSE_KEY` (the 3-arg ANT form), and the
  S340 nrf_sdm.h defines ANT_LICENSE_KEY, so the right call is selected automatically.

.PARAMETER BspDir
  The installed BSP folder. Defaults to the standard Arduino IDE location.

.PARAMETER S340Include
  The S340 7.0.1 API include folder in the sibling bootloader repo.

.EXAMPLE
  pwsh -File .\install-bsp-s340.ps1
#>
[CmdletBinding()]
param(
  [string]$BspDir      = "$env:LOCALAPPDATA\Arduino15\packages\adafruit\hardware\nrf52\1.7.0",
  [string]$S340Include = "D:\git\ss2k\bridge_project\Adafruit_nRF52_Bootloader\lib\softdevice\s340_nrf52_7.0.1\s340_nrf52_7.0.1_API\include"
)

$ErrorActionPreference = 'Stop'
$overlay = $PSScriptRoot

function Need-Path([string]$p, [string]$what) {
  if (-not (Test-Path -LiteralPath $p)) {
    throw "$what not found: $p"
  }
}

Write-Host "== bsp-s340 installer ==" -ForegroundColor Cyan
Need-Path $BspDir      "BSP install dir"
Need-Path $S340Include "S340 7.0.1 include dir"
Need-Path (Join-Path $BspDir 'boards.txt')                       "boards.txt"
Need-Path (Join-Path $BspDir 'cores\nRF5\linker')                "cores/nRF5/linker"
Need-Path (Join-Path $BspDir 'cores\nRF5\nordic\softdevice')     "cores/nRF5/nordic/softdevice"
Need-Path (Join-Path $BspDir 'variants')                         "variants"

# 1) S340 SoftDevice headers (whole include tree, incl. include/nrf52 + ANT headers)
$sdDest = Join-Path $BspDir 'cores\nRF5\nordic\softdevice\s340_nrf52_7.0.1_API\include'
New-Item -ItemType Directory -Force -Path $sdDest | Out-Null
Copy-Item -Path (Join-Path $S340Include '*') -Destination $sdDest -Recurse -Force
Write-Host "  [ok] S340 headers -> $sdDest"

# 2) Linker
$ld = Join-Path $overlay 'linker\nrf52840_s340_v7.ld'
Need-Path $ld "overlay linker script"
Copy-Item -Path $ld -Destination (Join-Path $BspDir 'cores\nRF5\linker\') -Force
Write-Host "  [ok] nrf52840_s340_v7.ld -> cores/nRF5/linker/"

# 3) Variant
$varSrc  = Join-Path $overlay 'variants\supermini_nrf52840'
$varDest = Join-Path $BspDir  'variants\supermini_nrf52840'
Need-Path $varSrc "overlay variant folder"
New-Item -ItemType Directory -Force -Path $varDest | Out-Null
Copy-Item -Path (Join-Path $varSrc '*') -Destination $varDest -Recurse -Force
Write-Host "  [ok] variant supermini_nrf52840 -> variants/"

# 4) boards.txt fragment (append once)
$boardsTxt = Join-Path $BspDir 'boards.txt'
$fragment  = Join-Path $overlay 'boards.txt.fragment'
Need-Path $fragment "boards.txt fragment"
$marker = 'superminis340.name='
if (Select-String -LiteralPath $boardsTxt -SimpleMatch $marker -Quiet) {
  Write-Host "  [skip] boards.txt already contains the superminis340 entry"
} else {
  Add-Content -LiteralPath $boardsTxt -Value "`r`n"
  Add-Content -LiteralPath $boardsTxt -Value (Get-Content -LiteralPath $fragment -Raw)
  Write-Host "  [ok] appended superminis340 entry to boards.txt"
}

# 5) Informational: confirm the stock ANT_LICENSE_KEY wrap is present
$bf = Join-Path $BspDir 'libraries\Bluefruit52Lib\src\bluefruit.cpp'
if ((Test-Path -LiteralPath $bf) -and
    (Select-String -LiteralPath $bf -SimpleMatch 'ANT_LICENSE_KEY' -Quiet)) {
  Write-Host "  [ok] bluefruit.cpp already supports ANT_LICENSE_KEY (no patch needed)"
} else {
  Write-Warning "bluefruit.cpp does not reference ANT_LICENSE_KEY -- check BSP version (expected 1.7.0)."
}

Write-Host ""
Write-Host "Done. In Arduino IDE:" -ForegroundColor Green
Write-Host "  1. Tools > Board > Adafruit nRF52 > 'SuperMini nRF52840 (S340)'"
Write-Host "  2. Select the COM port; put the board in DFU (double-tap reset) if needed."
Write-Host "  3. Upload verify_ble/verify_ble.ino.  NEVER use 'Burn Bootloader'."
Write-Host "  (Restart Arduino IDE if the board does not appear in the menu.)"
