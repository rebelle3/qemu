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

# --- 1+2. QEMU (two passes for the shim) -----------------------------
# configure's compiler checks link with --extra-ldflags, so give them an
# empty placeholder object first; compile the real shim (which needs the
# generated headers from the configured build dir) before the final make.
mkdir -p "$BUILD"
[ -f "$BUILD/wasm-shim.o" ] || \
    ( echo > "$BUILD/empty-shim.c" && \
      emcc -O2 -pthread -sMEMORY64=2 -c "$BUILD/empty-shim.c" \
           -o "$BUILD/wasm-shim.o" )

cd "$BUILD"
if [ ! -f config-host.mak ]; then
    emconfigure "$QEMU_SRC/configure" \
        --target-list=arm-softmmu \
        --static --cpu=wasm64 --wasm64-32bit-address-limit \
        --disable-tools --disable-docs --enable-tcg-interpreter \
        --extra-ldflags="$BUILD/wasm-shim.o -L$WASM_TARGET/lib"
fi

# first pass: full build with the placeholder shim (also generates the
# headers the real shim needs, e.g. config-poison.h)
emmake make -j"$(nproc)"

# compile the real shim and relink
emcc -O2 -pthread -sMEMORY64=2 -DWASM_BIGINT \
    -I"$QEMU_SRC/include" -I"$BUILD" \
    $(pkg-config --cflags glib-2.0 pixman-1) \
    -c "$HERE/wasm-shim.c" -o "$BUILD/wasm-shim.o"
emmake make -j"$(nproc)"

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
m = re.search(r"'name': 'QKeyCode'.*?'data':\s*\[(.*?)\]", text, re.S)
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
