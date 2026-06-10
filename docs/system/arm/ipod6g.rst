Apple iPod Classic 6G (``ipod6g``)
==================================

The ``ipod6g`` machine models the 6th generation iPod Classic, built
around the Samsung S5L8702 SoC (ARM926EJ-S).  It is sufficient to run
the Rockbox firmware: the real Rockbox bootloader is loaded at the
IRAM base (as the boot ROM would) and boots ``rockbox.ipod`` from the
FAT32 partition of the ATA disk.

Emulated devices:

- Dual ARM PL192 vectored interrupt controllers
- S5L8702 timer block (microsecond counter, kernel tick)
- Clock/PLL controller, GPIO (with LCD-type and PATA board straps)
- I2C with PCF50635 PMU (battery, hold switch) and codec stub
- LCD interface controller (320x240 RGB565 panel, command set 2/3)
- Click wheel (host keys: Up=MENU, Down=PLAY, Left/Right,
  Enter=SELECT, PgUp/PgDn or ]/[ = wheel scroll)
- ATA host controller (PATA mode, LBA28/LBA48 + controller DMA)
- Dual PL080 DMA controllers, I2S audio sink (samples discarded,
  consumption paced at the sample rate)

Boot::

  qemu-system-arm -M ipod6g -kernel bootloader.bin \
      -drive file=ipod.img,format=raw,if=ide

``-kernel`` accepts either the raw Rockbox bootloader (``bootloader.bin``)
or a scrambled ``rockbox.ipod`` image (detected by its ``ip6g`` header)
for direct-to-DRAM boot.  See ``contrib/ipod6g/`` for scripts that build
the disk image and drive the machine over QMP.

Running in a browser
--------------------

The machine can also be built to WebAssembly using QEMU's emscripten
support (wasm64 + TCI) and run on an HTML canvas with keyboard and
click-wheel input.  See ``contrib/ipod6g-web/README.rst`` for the
toolchain recipe, bundle build and a COOP/COEP-aware dev server.
