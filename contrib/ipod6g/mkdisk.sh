#!/bin/bash
#
# Build a FAT32 disk image for the QEMU ipod6g machine: an MBR with a
# single FAT32 partition containing a Rockbox install (.rockbox) plus a
# Music/ directory of sample tracks.
#
# Usage: mkdisk.sh <out.img> <rockbox.zip> [music-dir] [size-mb]
#
set -euo pipefail

OUT=${1:?usage: mkdisk.sh out.img rockbox.zip [music-dir] [size-mb]}
ZIP=${2:?need rockbox.zip}
MUSIC=${3:-}
SIZE_MB=${4:-256}

PART_OFFSET_SECT=2048              # 1 MiB aligned
SECT=512
PART_SECT=$(( SIZE_MB * 1024 * 1024 / SECT - PART_OFFSET_SECT ))

rm -f "$OUT"
truncate -s "${SIZE_MB}M" "$OUT"

# Write an MBR with a single FAT32-LBA (type 0x0c) bootable partition.
python3 - "$OUT" "$PART_OFFSET_SECT" "$PART_SECT" <<'PY'
import struct, sys
out, start, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
mbr = bytearray(512)
# CHS fields are ignored by LBA-aware code; use 0xFE/0xFF placeholders.
entry = struct.pack('<B3sB3sII', 0x80, b'\xfe\xff\xff', 0x0c,
                    b'\xfe\xff\xff', start, count)
mbr[446:446+16] = entry
mbr[510] = 0x55; mbr[511] = 0xaa
with open(out, 'r+b') as f:
    f.write(mbr)
PY

# Format the partition region into a standalone FAT image, then dd it in.
FATIMG=$(mktemp)
truncate -s $(( PART_SECT * SECT )) "$FATIMG"
mkfs.vfat -F 32 -n IPOD "$FATIMG" >/dev/null

# Populate: unzip .rockbox and copy sample music.
TMPD=$(mktemp -d)
unzip -q "$ZIP" -d "$TMPD"
mcopy -i "$FATIMG" -s "$TMPD/.rockbox" ::/
if [ -n "$MUSIC" ] && [ -d "$MUSIC" ]; then
    mmd -i "$FATIMG" ::/Music
    mcopy -i "$FATIMG" -s "$MUSIC"/* ::/Music/
fi

dd if="$FATIMG" of="$OUT" bs=$SECT seek=$PART_OFFSET_SECT conv=notrunc status=none
rm -rf "$TMPD" "$FATIMG"
echo "wrote $OUT (${SIZE_MB} MiB, FAT32 + .rockbox$( [ -n "$MUSIC" ] && echo ' + Music' ))"
