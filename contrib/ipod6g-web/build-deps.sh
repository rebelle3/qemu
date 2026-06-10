#!/bin/bash
# Cross-build QEMU's wasm dependencies, replaying
# tests/docker/dockerfiles/emsdk-wasm64-cross.docker (emsdk 4.0.10) with
# GitHub-mirror sources (canonical hosts unreachable from this sandbox).
# Deviations from the recipe: pixman 0.42.2 (Ubuntu pool tarball; recipe
# pins 0.44.2) and meson 1.11 (QEMU's bundled wheel; recipe pins 1.5.0).
set -euxo pipefail

source /opt/wasm/emsdk/emsdk_env.sh
export PATH="/home/user/qemu/build/pyvenv/bin:$PATH"

export TARGET=/opt/wasm/target
export CPATH="$TARGET/include"
export PKG_CONFIG_PATH="$TARGET/lib/pkgconfig"
export EM_PKG_CONFIG_PATH="$PKG_CONFIG_PATH"
export CFLAGS="-O3 -pthread -DWASM_BIGINT -sMEMORY64=1"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-sWASM_BIGINT -sASYNCIFY=1 -L$TARGET/lib -sMEMORY64=1"
SRC=/opt/wasm/src

cross_meson() {  # $1 = output file, extra c_args appended via $2
    local extra="${2:-}"
    cat > "$1" <<EOT
[host_machine]
system = 'emscripten'
cpu_family = 'wasm64'
cpu = 'wasm64'
endian = 'little'

[binaries]
c = 'emcc'
cpp = 'em++'
ar = 'emar'
ranlib = 'emranlib'
pkgconfig = ['pkg-config', '--static']

[built-in options]
c_args = [$(printf "'%s', " $CFLAGS $extra | sed 's/, $//')]
cpp_args = [$(printf "'%s', " $CFLAGS $extra | sed 's/, $//')]
objc_args = [$(printf "'%s', " $CFLAGS $extra | sed 's/, $//')]
c_link_args = [$(printf "'%s', " $LDFLAGS | sed 's/, $//')]
cpp_link_args = [$(printf "'%s', " $LDFLAGS | sed 's/, $//')]
EOT
}

echo "=== zlib ==="
cd $SRC/zlib
emconfigure ./configure --prefix=$TARGET --static
emmake make install -j$(nproc)

echo "=== libffi ==="
# use the release tarball (ships ./configure); the v3.5.2 tarball lacks
# some automake helper scripts, so copy them from the host automake
cd $SRC/libffi
for f in missing depcomp install-sh compile; do
    [ -f $f ] || cp /usr/share/automake-1.1*/$f .
done
test -x ./configure || autoreconf -fiv
emconfigure ./configure --host=wasm64-unknown-linux \
    --prefix=$TARGET --enable-static \
    --disable-shared --disable-dependency-tracking \
    --disable-builddir --disable-multi-os-directory \
    --disable-raw-api --disable-docs
emmake make install SUBDIRS='include' -j$(nproc)

echo "=== pixman ==="
cd $SRC/pixman
cross_meson /opt/wasm/cross-pixman.meson
rm -rf _build
meson setup _build --prefix=$TARGET --cross-file=/opt/wasm/cross-pixman.meson \
    --default-library=static \
    --buildtype=release -Dtests=disabled -Dopenmp=disabled -Dgtk=disabled -Dlibpng=disabled
meson install -C _build

echo "=== glib: res_query stub ==="
mkdir -p /opt/wasm/stub && cd /opt/wasm/stub
cat > res_query.c <<'EOT'
#include <netdb.h>
int res_query(const char *name, int class,
              int type, unsigned char *dest, int len)
{
    h_errno = HOST_NOT_FOUND;
    return -1;
}
EOT
emcc ${CFLAGS} -c res_query.c -fPIC -o libresolv.o
emar rcs libresolv.a libresolv.o
mkdir -p $TARGET/lib/
cp libresolv.a $TARGET/lib/

echo "=== glib: pcre2 subproject assembly ==="
# wrapdb.mesonbuild.com is unreachable, so assemble the subproject by
# hand: release tarball + wrapdb meson overlay as a plain directory
# subproject, with a minimal wrap keeping the [provide] mapping (without
# it, meson silently falls back to the host pcre2 whose headers the
# emscripten compiler cannot see).
cd $SRC/glib/subprojects
if [ ! -d pcre2 ]; then
    curl -sL https://github.com/mesonbuild/wrapdb/releases/download/pcre2_10.44-2/pcre2-10.44.tar.bz2 -o pcre2.tar.bz2 \
      || curl -sL https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.44/pcre2-10.44.tar.bz2 -o pcre2.tar.bz2
    curl -sL -o pcre2-patch.zip \
      https://github.com/mesonbuild/wrapdb/releases/download/pcre2_10.44-2/pcre2_10.44-2_patch.zip
    tar xjf pcre2.tar.bz2
    unzip -oq pcre2-patch.zip      # version-matched meson overlay
    mv pcre2-10.44 pcre2
fi
cat > pcre2.wrap <<'WRAP'
[wrap-file]
directory = pcre2

[provide]
libpcre2-8 = libpcre2_8
libpcre2-16 = libpcre2_16
libpcre2-32 = libpcre2_32
libpcre2-posix = libpcre2_posix
WRAP

echo "=== glib ==="
cd $SRC/glib
cross_meson /opt/wasm/cross-glib.meson "-Wno-incompatible-function-pointer-types"
rm -rf _build
meson setup _build --prefix=$TARGET --cross-file=/opt/wasm/cross-glib.meson \
    --default-library=static --buildtype=release --force-fallback-for=pcre2 \
    -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dnls=disabled \
    -Dtests=false -Dglib_debug=disabled -Dglib_assert=false -Dglib_checks=false
sed -i -E "/#define HAVE_POSIX_SPAWN 1/d" ./_build/config.h
sed -i -E "/#define HAVE_PTHREAD_GETNAME_NP 1/d" ./_build/config.h
meson install -C _build

echo "=== DONE: $(ls $TARGET/lib/pkgconfig)"
