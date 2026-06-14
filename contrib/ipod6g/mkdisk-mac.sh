#!/bin/bash
#
# Build a Mac-formatted iPod disk image for the QEMU ipod6g machine: an Apple
# Partition Map (APM) with an Apple_HFS partition containing an HFS+ filesystem
# with a Rockbox install (.rockbox) plus an optional Music/ directory.
#
# This mirrors mkdisk.sh (which builds the Windows MBR+FAT32 layout) but for
# Mac-formatted iPods, to exercise the read-only HFS+ support in Rockbox.
#
# The HFS+ filesystem is created on the host with mkfs.hfsplus (hfsprogs) and
# populated with guestfish (libguestfs, which mounts it with the real Linux
# hfsplus driver inside its appliance); the APM is written by hand.
#
# Usage: mkdisk-mac.sh <out.img> <rockbox.zip> [music-dir] [hfs-size-mb]
#
set -euo pipefail

OUT=${1:?usage: mkdisk-mac.sh out.img rockbox.zip [music-dir] [hfs-size-mb]}
ZIP=${2:?need rockbox.zip}
MUSIC=${3:-}
HFS_MB=${4:-256}

SECT=512
DATA_START_BLK=64                 # leave room for DDR + partition map
HFS_BYTES=$(( HFS_MB * 1024 * 1024 ))
DATA_BLKS=$(( HFS_BYTES / SECT ))
TOTAL_BLKS=$(( DATA_START_BLK + DATA_BLKS ))

export LIBGUESTFS_BACKEND=${LIBGUESTFS_BACKEND:-direct}

TMPD=$(mktemp -d)
HFSIMG=$(mktemp)
cleanup() { rm -rf "$TMPD" "$HFSIMG"; }
trap cleanup EXIT

# 1. Unpack the Rockbox install.
unzip -q "$ZIP" -d "$TMPD"

# 2. Create and format the HFS+ partition image, then populate it.
truncate -s "$HFS_BYTES" "$HFSIMG"
mkfs.hfsplus -v IPOD "$HFSIMG" >/dev/null

GF_CMDS="run
mount /dev/sda /
copy-in \"$TMPD/.rockbox\" /"
if [ -n "$MUSIC" ] && [ -d "$MUSIC" ]; then
    GF_CMDS="$GF_CMDS
mkdir /Music"
    for f in "$MUSIC"/*; do
        GF_CMDS="$GF_CMDS
copy-in \"$f\" /Music"
    done
fi
GF_CMDS="$GF_CMDS
umount /"
printf '%s\n' "$GF_CMDS" | guestfish -a "$HFSIMG"

# 3. Assemble the full disk: DDR + Apple Partition Map + HFS data.
rm -f "$OUT"
truncate -s $(( TOTAL_BLKS * SECT )) "$OUT"

python3 - "$OUT" "$TOTAL_BLKS" "$DATA_START_BLK" "$DATA_BLKS" <<'PY'
import struct, sys
out, total, dstart, dblks = (sys.argv[1], int(sys.argv[2]),
                             int(sys.argv[3]), int(sys.argv[4]))
SECT = 512

def pname(s):  # 32-byte NUL-padded ASCII field
    return s.encode('ascii')[:31].ljust(32, b'\x00')

# Block 0: Driver Descriptor Record (big-endian)
ddr = bytearray(SECT)
struct.pack_into('>HHI', ddr, 0, 0x4552, SECT, total)  # sbSig 'ER', sbBlkSize, sbBlkCount

# Partition map: entry 0 = the map itself, entry 1 = the Apple_HFS data part.
MAP_ENTRIES = 2

def pm_entry(start, count, name, ptype):
    e = bytearray(SECT)
    struct.pack_into('>HHIII', e, 0,
                     0x504D,        # pmSig 'PM'
                     0,             # pmSigPad
                     MAP_ENTRIES,   # pmMapBlkCnt
                     start,         # pmPyPartStart
                     count)         # pmPartBlkCnt
    e[16:48] = pname(name)
    e[48:80] = pname(ptype)
    struct.pack_into('>II', e, 80, 0, count)  # pmLgDataStart, pmDataCnt
    struct.pack_into('>I', e, 88, 0x33)       # pmPartStatus: valid/allocated/readable/writable
    return e

map_self = pm_entry(1, MAP_ENTRIES, "Apple", "Apple_partition_map")
hfs_part = pm_entry(dstart, dblks, "disk", "Apple_HFS")

with open(out, 'r+b') as f:
    f.write(ddr)              # block 0
    f.seek(1 * SECT)
    f.write(map_self)         # block 1
    f.write(hfs_part)         # block 2
PY

# 4. Place the HFS+ filesystem at the data partition offset.
dd if="$HFSIMG" of="$OUT" bs=$SECT seek=$DATA_START_BLK conv=notrunc status=none

echo "wrote $OUT (APM + Apple_HFS, ${HFS_MB} MiB HFS+ at block ${DATA_START_BLK})"
