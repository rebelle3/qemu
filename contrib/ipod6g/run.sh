#!/bin/bash
# Launch the iPod Classic 6G machine.
#
# Usage: run.sh <bootloader.bin> <disk.img> [extra qemu args...]
# Headless example (QMP on a unix socket for drive.py):
#   run.sh bootloader.bin ipod.img -display none -qmp unix:/tmp/qmp.sock,server,wait=off
set -euo pipefail
BOOT=${1:?usage: run.sh bootloader.bin disk.img [qemu args]}
DISK=${2:?need disk image}
shift 2
QEMU=${QEMU:-$(dirname "$0")/../../build/qemu-system-arm}
exec "$QEMU" -M ipod6g \
    -kernel "$BOOT" \
    -drive file="$DISK",format=raw,if=ide \
    -serial none "$@"
