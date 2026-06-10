#!/bin/bash
#
# Assemble the browser bundle for the QEMU ipod6g machine.
#
# Prerequisites (see README.rst):
#  - emsdk activated (emcc on PATH)
#  - wasm-built deps in $WASM_TARGET (default /opt/wasm/target)
#  - a NATIVE QEMU build dir (for qemu-img) at $QEMU_NATIVE_BUILD
#  - Rockbox artifacts: bootloader.bin + rockbox.zip
#
# Steps:
#  1. compile wasm-shim.c against the QEMU tree
#  2. configure+build QEMU for emscripten (out-of-tree, no source changes),
#     linking the shim via --extra-ldflags
#  3. build a slim FAT32 disk and convert to qcow2
#  4. generate qkeycodes.json from the QAPI schema
#  5. stage everything into $OUT (default ./dist)
#
set -euxo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
QEMU_SRC=${QEMU_SRC:-$(cd "$HERE/../.." && pwd)}
WASM_TARGET=${WASM_TARGET:-/opt/wasm/target}
BUILD=${BUILD:-$QEMU_SRC/build-wasm}
OUT=${OUT:-$HERE/dist}
QEMU_NATIVE_BUILD=${QEMU_NATIVE_BUILD:-$QEMU_SRC/build}
BOOTLOADER=${BOOTLOADER:?set BOOTLOADER=path/to/bootloader.bin}
ROCKBOX_ZIP=${ROCKBOX_ZIP:?set ROCKBOX_ZIP=path/to/rockbox.zip}
MUSIC_DIR=${MUSIC_DIR:-}
DISK_MB=${DISK_MB:-64}

export PKG_CONFIG_LIBDIR="$WASM_TARGET/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
export EM_PKG_CONFIG_PATH="$PKG_CONFIG_PATH"

# --- 1. QEMU ----------------------------------------------------------
mkdir -p "$BUILD"
cd "$BUILD"
if [ ! -f config-host.mak ]; then
    emconfigure "$QEMU_SRC/configure" \
        --target-list=arm-softmmu \
        --static --cpu=wasm64 --wasm64-32bit-address-limit \
        --disable-tools --disable-docs --enable-tcg-interpreter
fi
emmake make -j"$(nproc)"

# --- 2. shim ----------------------------------------------------------
# The browser glue is linked in a separate final link: meson machine
# files cannot carry an extra object (the in-tree emscripten cross file
# owns c_link_args), so replay ninja's exact link command with the shim
# object prepended to the response file.
emcc -O2 -pthread -sMEMORY64=2 -DWASM_BIGINT \
    -I"$QEMU_SRC/include" -I"$BUILD" \
    $(pkg-config --cflags glib-2.0 pixman-1) \
    -c "$HERE/wasm-shim.c" -o "$BUILD/wasm-shim.o"

# response files are transient; -d keeprsp makes ninja leave them behind
rm -f qemu-system-arm.js qemu-system-arm.wasm
ninja -d keeprsp qemu-system-arm.js
LINKCMD=$(ninja -t commands qemu-system-arm.js | tail -1)
RSP=$(echo "$LINKCMD" | grep -o '@[^ ]*' | tr -d '@')
PREFIX=${LINKCMD%%@*}
( printf 'wasm-shim.o ' ; cat "$RSP" ) > shim-link.rsp
$PREFIX @shim-link.rsp

# --- 3. disk ---------------------------------------------------------
"$HERE/../ipod6g/mkdisk.sh" "$BUILD/ipod-web.img" "$ROCKBOX_ZIP" \
    "$MUSIC_DIR" "$DISK_MB"
"$QEMU_NATIVE_BUILD/qemu-img" convert -O qcow2 -c \
    "$BUILD/ipod-web.img" "$BUILD/ipod.qcow2"

# --- 4. qkeycodes ----------------------------------------------------
python3 - "$QEMU_SRC" "$BUILD/qkeycodes.json" <<'PY'
import json, re, sys
src, out = sys.argv[1], sys.argv[2]
text = open(src + '/qapi/ui.json').read()
m = re.search(r"'enum': 'QKeyCode'.*?'data':\s*\[(.*?)\]", text, re.S)
names = re.findall(r"'([a-z0-9_\-]+)'", m.group(1))
codes = {n.replace('-', '_'): i for i, n in enumerate(names)}
json.dump(codes, open(out, 'w'))
print('qkeycodes:', len(codes))
PY

# --- 5. stage --------------------------------------------------------
mkdir -p "$OUT"
cp "$BUILD"/qemu-system-arm.js "$OUT/" 2>/dev/null || true
cp "$BUILD"/qemu-system-arm*.wasm "$OUT/" 2>/dev/null || \
    cp "$BUILD"/qemu-system-arm.wasm "$OUT/"
for f in "$BUILD"/qemu-system-arm.worker.js "$BUILD"/qemu-system-arm.ww.js; do
    [ -f "$f" ] && cp "$f" "$OUT/"
done
cp "$BUILD/qkeycodes.json" "$BUILD/ipod.qcow2" "$OUT/"
cp "$BOOTLOADER" "$OUT/bootloader.bin"
cp "$HERE/index.html" "$HERE/ipod6g.js" "$OUT/"
echo "Bundle ready in $OUT — serve with: $HERE/serve.py 8080 $OUT"
