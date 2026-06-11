#!/usr/bin/env bash
# Lay the S340 v7.0.1 overlay into an installed Adafruit nRF52 Arduino BSP so the
# "SuperMini nRF52840 (S340)" board can be selected, built, and DFU-flashed.
#
# Idempotent. Re-run after any BSP reinstall/update (which wipes these edits).
#
# Usage:
#   ./bsp-s340/install-bsp-s340.sh
#   ./bsp-s340/install-bsp-s340.sh \
#       --bsp-dir  "$HOME/Library/Arduino15/packages/adafruit/hardware/nrf52/1.7.0" \
#       --s340-include "/path/to/s340_nrf52_7.0.1_API/include"

set -euo pipefail

if [[ "$(uname)" == "Darwin" ]]; then
  BSP_DIR="$HOME/Library/Arduino15/packages/adafruit/hardware/nrf52/1.7.0"
else
  BSP_DIR="$HOME/.arduino15/packages/adafruit/hardware/nrf52/1.7.0"
fi
S340_INCLUDE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bsp-dir)       BSP_DIR="$2";       shift 2 ;;
    --s340-include)  S340_INCLUDE="$2";  shift 2 ;;
    *) echo "Unknown argument: $1"; exit 1 ;;
  esac
done

OVERLAY_DIR="$(cd "$(dirname "$0")" && pwd)"

need_path() {
  if [[ ! -e "$1" ]]; then
    echo "ERROR: $2 not found: $1" >&2
    exit 1
  fi
}

if [[ -z "$S340_INCLUDE" ]]; then
  echo "" >&2
  echo "ERROR: --s340-include is required." >&2
  echo "" >&2
  echo "  Clone the bootloader repo, then re-run:" >&2
  echo "    git clone https://github.com/eMadman/Adafruit_nRF52_Bootloader" >&2
  echo "    ./install-bsp-s340.sh \\" >&2
  echo "        --s340-include <clone-root>/Adafruit_nRF52_Bootloader/lib/softdevice/s340_nrf52_7.0.1/s340_nrf52_7.0.1_API/include" >&2
  echo "" >&2
  exit 1
fi

echo "== bsp-s340 installer =="

need_path "$BSP_DIR"                                    "BSP install dir"
need_path "$BSP_DIR/boards.txt"                         "boards.txt"
need_path "$BSP_DIR/cores/nRF5/linker"                  "cores/nRF5/linker"
need_path "$BSP_DIR/cores/nRF5/nordic/softdevice"       "cores/nRF5/nordic/softdevice"
need_path "$BSP_DIR/variants"                           "variants"
need_path "$S340_INCLUDE" "S340 7.0.1 include dir"

# 1) S340 SoftDevice headers
SD_DEST="$BSP_DIR/cores/nRF5/nordic/softdevice/s340_nrf52_7.0.1_API/include"
mkdir -p "$SD_DEST"
cp -R "$S340_INCLUDE/." "$SD_DEST/"
echo "  [ok] S340 headers -> $SD_DEST"

# 2) Linker script
LD="$OVERLAY_DIR/linker/nrf52840_s340_v7.ld"
need_path "$LD" "overlay linker script"
cp "$LD" "$BSP_DIR/cores/nRF5/linker/"
echo "  [ok] nrf52840_s340_v7.ld -> cores/nRF5/linker/"

# 3) Variant
VAR_SRC="$OVERLAY_DIR/variants/supermini_nrf52840"
VAR_DEST="$BSP_DIR/variants/supermini_nrf52840"
need_path "$VAR_SRC" "overlay variant folder"
mkdir -p "$VAR_DEST"
cp -R "$VAR_SRC/." "$VAR_DEST/"
echo "  [ok] variant supermini_nrf52840 -> variants/"

# 4) boards.txt fragment (append once)
BOARDS_TXT="$BSP_DIR/boards.txt"
FRAGMENT="$OVERLAY_DIR/boards.txt.fragment"
need_path "$FRAGMENT" "boards.txt fragment"
MARKER="superminis340.name="
if grep -qF "$MARKER" "$BOARDS_TXT"; then
  echo "  [skip] boards.txt already contains the superminis340 entry"
else
  printf '\n' >> "$BOARDS_TXT"
  cat "$FRAGMENT" >> "$BOARDS_TXT"
  echo "  [ok] appended superminis340 entry to boards.txt"
fi

# 5) Informational: confirm the stock ANT_LICENSE_KEY wrap is present
BF="$BSP_DIR/libraries/Bluefruit52Lib/src/bluefruit.cpp"
if [[ -f "$BF" ]] && grep -qF "ANT_LICENSE_KEY" "$BF"; then
  echo "  [ok] bluefruit.cpp already supports ANT_LICENSE_KEY (no patch needed)"
else
  echo "  WARNING: bluefruit.cpp does not reference ANT_LICENSE_KEY -- check BSP version (expected 1.7.0)." >&2
fi

echo ""
echo "Done. In Arduino IDE:"
echo "  1. Restart Arduino IDE (if already open)"
echo "  2. Tools > Board > Adafruit nRF52 > 'SuperMini nRF52840 (S340)'"
echo "  3. Select the port; double-tap reset to enter DFU if needed."
echo "  4. Upload ble_ant_bridge/ble_ant_bridge.ino.  NEVER use 'Burn Bootloader'."
