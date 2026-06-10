/*
 * Headless smoke test for the wasm build: boot the ipod6g machine under
 * node for a fixed time and report whether it kept running and whether
 * the shim produced framebuffer updates.
 *
 * Usage: node run-node.mjs <dist-dir> [seconds]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import { readFileSync, writeFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { resolve } from 'node:path';

const dist = resolve(process.argv[2] ?? 'dist');
const seconds = Number(process.argv[3] ?? 45);
/* extra: comma-separated timed actions "at:<sec>:key:<qcode-name>" */
const actions = (process.argv[4] ?? '').split(',').filter(Boolean);

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

const QKEY = JSON.parse(
    readFileSync(resolve(dist, 'qkeycodes.json'), 'utf8'));
function crc32(buf) {
    let c, crc = 0xffffffff;
    for (let i = 0; i < buf.length; i++) {
        c = (crc ^ buf[i]) & 0xff;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        crc = (crc >>> 8) ^ c;
    }
    return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
}
function savePng(name) {
    const f = readFrame();
    if (!f) { console.log('[shot] no frame for', name); return; }
    const { w, h, rgba } = f;
    const raw = Buffer.alloc((w * 4 + 1) * h);
    for (let y = 0; y < h; y++) {
        raw[y * (w * 4 + 1)] = 0;
        rgba.subarray(y * w * 4, (y + 1) * w * 4)
            .forEach((v, i) => { raw[y * (w * 4 + 1) + 1 + i] = v; });
    }
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
    ihdr[8] = 8; ihdr[9] = 6;
    const png = Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
        chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)),
        chunk('IEND', Buffer.alloc(0))]);
    writeFileSync(name, png);
    console.log('[shot]', name, `${w}x${h}`);
}

for (const a of actions) {
    const p = a.split(':');
    const at = Number(p[1]);
    if (p[2] === 'key') {
        setTimeout(() => {
            console.log(`[act] key ${p[3]}`);
            qemu._qemu_wasm_key_event(QKEY[p[3]], 1);
            setTimeout(() => qemu._qemu_wasm_key_event(QKEY[p[3]], 0), 150);
        }, at * 1000);
    } else if (p[2] === 'shot') {
        setTimeout(() => savePng(`/tmp/${p[3]}.png`), at * 1000);
    }
}

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
