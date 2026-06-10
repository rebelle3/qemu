/*
 * Browser front-end for the wasm build of the QEMU ipod6g machine.
 *
 * Loads the ES6 module produced by emscripten (qemu-system-arm.js),
 * preloads the Rockbox bootloader and disk image into MEMFS, starts the
 * machine, then blits frames from the wasm heap to the canvas and
 * forwards key/button events to the guest click wheel.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

const status = (m) => { document.getElementById('status').textContent = m; };

/* QKeyCode numeric values (qapi/ui.json order is ABI-stable per release;
 * resolved at build time into qkeycodes.json by build-web.sh). */
const QKEY = await (await fetch('./qkeycodes.json')).json();

const KEYMAP = {
    ArrowUp: QKEY.up, ArrowDown: QKEY.down,
    ArrowLeft: QKEY.left, ArrowRight: QKEY.right,
    Enter: QKEY.ret,
    BracketLeft: QKEY.bracket_left, BracketRight: QKEY.bracket_right,
    PageUp: QKEY.pgup, PageDown: QKEY.pgdn,
};

async function fetchInto(FS, url, path) {
    status(`fetching ${url} …`);
    const r = await fetch(url);
    if (!r.ok) throw new Error(`fetch ${url}: ${r.status}`);
    const data = new Uint8Array(await r.arrayBuffer());
    FS.writeFile(path, data);
    return data.length;
}

const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d');

const Module = {
    arguments: [
        '-M', 'ipod6g',
        '-kernel', '/bootloader.bin',
        '-drive', 'file=/ipod.qcow2,format=qcow2,if=ide',
        '-display', 'none',
        '-audio', 'none',
        '-monitor', 'none',
        '-serial', 'none',
    ],
    locateFile: (f) => './' + f,
    print: (t) => console.log('[qemu]', t),
    printErr: (t) => console.warn('[qemu]', t),
};

status('loading qemu-system-arm.wasm …');
const factory = (await import('./qemu-system-arm.js')).default;

/* Preload guest images before main() runs. */
Module.preRun = [async function () {
    const FS = Module.FS;
    Module.addRunDependency('guest-images');
    await fetchInto(FS, './bootloader.bin', '/bootloader.bin');
    await fetchInto(FS, './ipod.qcow2', '/ipod.qcow2');
    status('booting Rockbox … (TCI interpreter: allow up to a minute)');
    Module.removeRunDependency('guest-images');
}];

const qemu = await factory(Module);

/* ---- display: poll the frame the shim publishes in MEMFS ---- */
let lastFrame = -1;
let image = null;

function blit() {
    qemu._qemu_wasm_request_frame();
    try {
        const d = qemu.FS.readFile('/fbdump');
        const info = new Uint32Array(d.buffer, d.byteOffset, 4);
        const [frame, w, h] = info;
        if (frame !== lastFrame && w > 0 && h > 0) {
            lastFrame = frame;
            if (!image || image.width !== w || image.height !== h) {
                canvas.width = w; canvas.height = h;
                image = ctx.createImageData(w, h);
                status('running — click the screen, then use the keys below');
            }
            image.data.set(d.subarray(16, 16 + w * h * 4));
            ctx.putImageData(image, 0, 0);
        }
    } catch (e) { /* no frame yet */ }
    requestAnimationFrame(blit);
}
requestAnimationFrame(blit);

/* ---- input ---- */
function key(qcode, down) { qemu._qemu_wasm_key_event(qcode, down ? 1 : 0); }

canvas.addEventListener('keydown', (e) => {
    const q = KEYMAP[e.code]; if (q === undefined) return;
    e.preventDefault(); key(q, true);
});
canvas.addEventListener('keyup', (e) => {
    const q = KEYMAP[e.code]; if (q === undefined) return;
    e.preventDefault(); key(q, false);
});
canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    const q = e.deltaY > 0 ? QKEY.bracket_right : QKEY.bracket_left;
    key(q, true); setTimeout(() => key(q, false), 30);
}, { passive: false });

function bindButton(id, qcode) {
    const el = document.getElementById(id);
    el.addEventListener('pointerdown', () => key(qcode, true));
    el.addEventListener('pointerup', () => key(qcode, false));
}
bindButton('b-menu', QKEY.up);
bindButton('b-play', QKEY.down);
bindButton('b-prev', QKEY.left);
bindButton('b-next', QKEY.right);
bindButton('b-select', QKEY.ret);

canvas.focus();
