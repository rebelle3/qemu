# iPod Classic 6G in your browser

QEMU (compiled to WebAssembly) emulating the Samsung S5L8702 iPod
Classic 6G, running the real Rockbox bootloader and firmware.

Built from [`rebelle3/qemu` branch `claude/vibrant-tesla-7e1qia`](https://github.com/rebelle3/qemu/tree/claude/vibrant-tesla-7e1qia)
(see `contrib/ipod6g-web/` for the build recipe). QEMU and Rockbox are
GPL-2.0; sources for everything in this bundle live on that branch and
at https://git.rockbox.org/ .

~65 MB download; boot takes a minute or two (interpreted CPU).
Controls: arrows = MENU/PLAY/PREV/NEXT, Enter = SELECT, `[`/`]` or the
mouse wheel = scroll, or use the on-screen click wheel. The first
press only wakes the backlight.

Best on desktop Chrome/Firefox or Android Chrome. The
`coi-serviceworker` shim provides the cross-origin isolation that
SharedArrayBuffer needs on GitHub Pages (one automatic reload on first
visit).
