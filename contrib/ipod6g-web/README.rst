iPod Classic 6G in the browser (QEMU → WebAssembly)
===================================================

This directory packages the ``ipod6g`` machine as a static web page:
QEMU is compiled to WebAssembly with emscripten and drives an HTML
canvas; Rockbox runs unmodified inside it.

No QEMU sources are modified for this: the browser glue
(``wasm-shim.c``) is compiled separately against the tree and linked
into the emscripten build via ``--extra-ldflags``.  The shim exposes a
shared RGBA framebuffer (filled on the QEMU main loop via bottom
halves) and a key-injection entry point; ``ipod6g.js`` polls the one
and feeds the other.

Toolchain
---------

Use the emsdk version pinned by QEMU's CI recipe
(``tests/docker/dockerfiles/emsdk-wasm64-cross.docker``)::

  git clone https://github.com/emscripten-core/emsdk.git
  ./emsdk/emsdk install 4.0.10 && ./emsdk/emsdk activate 4.0.10
  source ./emsdk/emsdk_env.sh

Cross-build the dependencies (zlib 1.3.1, libffi 3.5.2, pixman,
glib 2.84.0) into a prefix, following the same dockerfile.  A
transcript of the exact commands used (with GitHub-mirror sources and
two small deviations: pixman 0.42.2, meson from QEMU's bundled wheel)
is kept in ``build-deps.sh``.

Build the bundle
----------------

Needs a *native* QEMU build (for ``qemu-img``) and the Rockbox
artifacts from ``contrib/ipod6g/README.rst``::

  BOOTLOADER=.../bootloader.bin \
  ROCKBOX_ZIP=.../rockbox.zip \
  MUSIC_DIR=.../music \
  WASM_TARGET=/opt/wasm/target \
  contrib/ipod6g-web/build-web.sh

This configures ``build-wasm/`` with::

  emconfigure configure --target-list=arm-softmmu --static --cpu=wasm64 \
      --wasm64-32bit-address-limit --disable-tools --disable-docs \
      --enable-tcg-interpreter --extra-ldflags=.../wasm-shim.o

(``--wasm64-32bit-address-limit`` = ``-sMEMORY64=2``: runs on stock
browsers without the Memory64 flag) and stages
``dist/``: ``qemu-system-arm.{js,wasm}``, the qcow2 disk, bootloader,
``index.html``, ``ipod6g.js``, ``qkeycodes.json``.

Run
---

SharedArrayBuffer (required by ``-sPROXY_TO_PTHREAD``) needs
cross-origin isolation, so serve with the provided helper, not a plain
file server::

  contrib/ipod6g-web/serve.py 8080 contrib/ipod6g-web/dist

then open http://127.0.0.1:8080/ .  Boot takes a little while — the
CPU is interpreted (TCI).  The first key press only wakes the
backlight, as on real hardware.

Headless smoke test (no browser)::

  node contrib/ipod6g-web/run-node.mjs contrib/ipod6g-web/dist 45

Notes
-----

- Audio is intentionally silent (the I2S sink discards samples); the
  playback clock still advances at real time.
- The wasm build interprets guest code; expect roughly a 5-15x
  slowdown over native TCG.  The Rockbox UI remains responsive.
- Browser keys: arrows = MENU/PLAY/PREV/NEXT, Enter = SELECT,
  ``[``/``]`` or the mouse wheel = scroll; on-screen wheel buttons
  work on touch devices.
