/*
 * Headless smoke test for the wasm build: boot the ipod6g machine under
 * node for a fixed time and report whether it kept running and whether
 * the shim produced framebuffer updates.
 *
 * Usage: node run-node.mjs <dist-dir> [seconds]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const dist = resolve(process.argv[2] ?? 'dist');
const seconds = Number(process.argv[3] ?? 45);

const factory = (await import(resolve(dist, 'qemu-system-arm.js'))).default;

const Module = {
    arguments: [
        '-M', 'ipod6g',
        '-kernel', '/bootloader.bin',
        '-drive', 'file=/ipod.qcow2,format=qcow2,if=ide',
        '-display', 'none', '-audio', 'none',
        '-monitor', 'none', '-serial', 'none',
    ],
    locateFile: (f) => resolve(dist, f),
    print: (t) => console.log('[qemu]', t),
    printErr: (t) => console.error('[qemu!]', t),
    preRun: [function () {
        Module.FS.writeFile('/bootloader.bin',
                            readFileSync(resolve(dist, 'bootloader.bin')));
        Module.FS.writeFile('/ipod.qcow2',
                            readFileSync(resolve(dist, 'ipod.qcow2')));
    }],
};

const qemu = await factory(Module);
console.log('module instantiated, letting the machine run...');

function readFrame() {
    try {
        const d = qemu.FS.readFile('/fbdump');
        const i = new Uint32Array(d.buffer, d.byteOffset, 4);
        return { frame: i[0], w: i[1], h: i[2],
                 rgba: d.subarray(16, 16 + i[1] * i[2] * 4) };
    } catch (e) {
        return null;
    }
}

let frames = 0, lastFrame = -1, last = null;

const poll = setInterval(() => {
    qemu._qemu_wasm_request_frame();
    const f = readFrame();
    if (f && f.frame !== lastFrame) { lastFrame = f.frame; frames++; last = f; }
}, 200);

setTimeout(() => {
    clearInterval(poll);
    console.log(`alive after ${seconds}s; fb updates=${frames} ` +
                `last=${last ? last.w + 'x' + last.h : 'none'}`);
    process.exit(frames > 1 ? 0 : 1);
}, seconds * 1000);
