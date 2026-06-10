QEMU iPod Classic 6G (Rockbox) helper scripts
=============================================

Build guest software (in a Rockbox tree, with arm-none-eabi-gcc)::

  mkdir build-boot && cd build-boot
  ../tools/configure --target=ipod6g --type=b --compiler-prefix=arm-none-eabi-
  make -j$(nproc)              # -> bootloader.bin
  mkdir ../build-fw && cd ../build-fw
  ../tools/configure --target=ipod6g --type=n --compiler-prefix=arm-none-eabi-
  make -j$(nproc) zip          # -> rockbox.zip (.rockbox install)

Create the iPod disk (MBR + FAT32 with .rockbox and sample music)::

  ./mkmusic.py music
  ./mkdisk.sh ipod.img path/to/rockbox.zip music 256

Run::

  ./run.sh path/to/bootloader.bin ipod.img \
      -display none -qmp unix:/tmp/qmp.sock,server,wait=off

Drive it headless (screendumps + click-wheel input)::

  ./drive.py /tmp/qmp.sock shot:menu ret ret shot:browser \
      bracket_right ret sleep:2 shot:nowplaying

Keys: Up=MENU, Down=PLAY, Left/Right, Enter=SELECT,
PgUp/]=scroll clockwise, PgDn/[=scroll anticlockwise.
