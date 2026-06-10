/*
 * Audio-path verification: boot under node with -audiodev wav writing
 * into MEMFS, play the first sample track, then pull the capture out
 * and report it. SPDX-License-Identifier: GPL-2.0-or-later
 */
import { readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const dist = resolve(process.argv[2] ?? 'dist');
const factory = (await import(resolve(dist, 'qemu-system-arm.js'))).default;
const QKEY = JSON.parse(readFileSync(resolve(dist, 'qkeycodes.json'), 'utf8'));

const Module = {
    arguments: [
        '-M', 'ipod6g,audiodev=snd0',
        '-audiodev', 'wav,id=snd0,path=/capture.wav',
        '-kernel', '/bootloader.bin',
        '-drive', 'file=/ipod.qcow2,format=qcow2,if=ide',
        '-display', 'none', '-monitor', 'none', '-serial', 'none',
    ],
    locateFile: (f) => resolve(dist, f),
    print: (t) => console.log('[q]', t),
    printErr: (t) => { if (!/mprotect|madvise/.test(t)) console.error('[q!]', t); },
    preRun: [function () {
        Module.FS.writeFile('/bootloader.bin',
                            readFileSync(resolve(dist, 'bootloader.bin')));
        Module.FS.writeFile('/ipod.qcow2',
                            readFileSync(resolve(dist, 'ipod.qcow2')));
    }],
};
const qemu = await factory(Module);
console.log('booting...');

const key = (q) => {
    qemu._qemu_wasm_key_event(q, 1);
    setTimeout(() => qemu._qemu_wasm_key_event(q, 0), 150);
};
setTimeout(() => { console.log('[act] select'); key(QKEY.ret); }, 60000);
setTimeout(() => { console.log('[act] select'); key(QKEY.ret); }, 64000);
setTimeout(() => { console.log('[act] select (play)'); key(QKEY.ret); }, 68000);

setTimeout(() => {
    try {
        const wav = qemu.FS.readFile('/capture.wav');
        writeFileSync('/tmp/capture-wasm.wav', wav);
        console.log('captured', wav.length, 'bytes -> /tmp/capture-wasm.wav');
    } catch (e) {
        console.log('no capture:', e.message);
    }
    process.exit(0);
}, 100000);
