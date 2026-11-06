#!/bin/sh
# Compile the display firmware and write it to the board on USB.
#
#   sugarsense-display/tools/flash.sh              compile + flash
#   sugarsense-display/tools/flash.sh --compile    compile only
#
# Needs: brew install arduino-cli esptool
#        arduino-cli core install esp32:esp32
#        arduino-cli lib install "GFX Library for Arduino" ArduinoJson
# On an Apple Silicon Mac without Rosetta, arduino-cli's bundled ctags is an
# Intel binary and the build stops with "bad CPU type": see ../README.md.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$HERE/../firmware/sugarsense_display"
BUILD="${SS_DISPLAY_BUILD:-${TMPDIR:-/tmp}/sugarsense-display-build}"
FQBN='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi'

mkdir -p "$BUILD"
# Library code keeps the path of its source file for its error messages. Map
# your home folder away so the firmware file never carries your user name.
MAP="-ffile-prefix-map=/$HOME=. -ffile-prefix-map=$HOME=."
arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" \
  --build-property "compiler.c.extra_flags=$MAP" \
  --build-property "compiler.cpp.extra_flags=$MAP" \
  --build-property "compiler.S.extra_flags=$MAP" \
  "$SKETCH"
[ "$1" = "--compile" ] && exit 0

PORT="${SS_DISPLAY_PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no /dev/cu.usbmodem* port: is the display plugged in?"; exit 1; }
# The board hangs off the ESP32-S3's native USB: only a watchdog reset restarts it.
esptool --chip esp32s3 --port "$PORT" --baud 921600 --after watchdog-reset write-flash \
  0x0 "$BUILD/sugarsense_display.ino.bootloader.bin" \
  0x8000 "$BUILD/sugarsense_display.ino.partitions.bin" \
  0xe000 "$BUILD/boot_app0.bin" \
  0x10000 "$BUILD/sugarsense_display.ino.bin"
