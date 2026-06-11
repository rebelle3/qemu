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
                status('running');
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

const SCROLL_KEYS = new Map();   /* qcode -> clicks, filled once QKEY known */
SCROLL_KEYS.set(QKEY.bracket_right, 4).set(QKEY.pgup, 4);
SCROLL_KEYS.set(QKEY.bracket_left, -4).set(QKEY.pgdn, -4);

canvas.addEventListener('keydown', (e) => {
    const q = KEYMAP[e.code]; if (q === undefined) return;
    e.preventDefault();
    if (SCROLL_KEYS.has(q)) return kbScroll(SCROLL_KEYS.get(q));
    key(q, true);
});
canvas.addEventListener('keyup', (e) => {
    const q = KEYMAP[e.code]; if (q === undefined) return;
    e.preventDefault();
    if (SCROLL_KEYS.has(q)) return;
    key(q, false);
});
canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    kbScroll(e.deltaY > 0 ? 4 : -4);
}, { passive: false });

/*
 * The centre SELECT is a real button (not part of the touch ring):
 * immediate press/release, and it never starts a rotation.
 */
{
    const el = document.getElementById('b-select');
    el.addEventListener('pointerdown', (e) => {
        e.stopPropagation(); key(QKEY.ret, true);
    });
    el.addEventListener('pointerup', (e) => {
        e.stopPropagation(); key(QKEY.ret, false);
    });
}

/* ---- the touch ring: rotation + tap, like the real thing ----
 * The whole ring (including the printed MENU/PLAY/PREV/NEXT labels) is
 * the capacitive sensor: the finger's angle around the centre feeds
 * absolute positions (0..95, clockwise, 0 = 12 o'clock) to the
 * emulated sensor, and Rockbox computes the scroll deltas itself.
 * A touch that starts on a label and lifts without rotating counts as
 * a tap of that button; once it rotates, the tap is cancelled. */
const wheelEl = document.getElementById('wheel');
const hasWheelApi = typeof qemu._qemu_wasm_wheel === 'function';

/*
 * Keyboard / mouse-wheel scrolling: synthesize a paced finger rotation.
 * Rockbox's driver derives velocity from packet timestamps, so the
 * device's instant 3-packet key burst is dropped; a real-time click
 * stream (like the touch ring sends) always registers.
 */
let kbPos = 0;
let kbQueue = 0;
let kbTimer = null;
function kbScroll(clicks) {
    if (!hasWheelApi) {          /* old builds: device key path */
        const q = clicks > 0 ? QKEY.bracket_right : QKEY.bracket_left;
        key(q, true); setTimeout(() => key(q, false), 30);
        return;
    }
    kbQueue += clicks;
    if (kbTimer !== null) return;
    qemu._qemu_wasm_wheel(kbPos, 1);              /* anchor */
    kbTimer = setInterval(() => {
        if (kbQueue === 0) {
            qemu._qemu_wasm_wheel(kbPos, 0);      /* lift */
            clearInterval(kbTimer); kbTimer = null;
            return;
        }
        const d = kbQueue > 0 ? 1 : -1;
        kbQueue -= d;
        kbPos = (kbPos + 96 + d) % 96;
        qemu._qemu_wasm_wheel(kbPos, 1);
    }, 50);   /* gentle pacing: a slow (TCI) guest must drain each packet */
}
const RING_BUTTONS = {
    'b-menu': QKEY.up, 'b-play': QKEY.down,
    'b-prev': QKEY.left, 'b-next': QKEY.right,
};
const TAP_SLOP_CLICKS = 2;      /* movement before a tap becomes a scroll */

let wheelLast = -1;
let tapButton = null;           /* qcode of a pending ring-button tap */
let tapMoved = 0;

function wheelPos(e) {
    const r = wheelEl.getBoundingClientRect();
    const dx = e.clientX - (r.left + r.width / 2);
    const dy = e.clientY - (r.top + r.height / 2);
    /* atan2 with screen-y down is clockwise; rotate so 0 = 12 o'clock */
    const ang = Math.atan2(dy, dx) + Math.PI / 2;
    return ((Math.round(ang / (2 * Math.PI) * 96) % 96) + 96) % 96;
}

if (hasWheelApi) {
    wheelEl.addEventListener('pointerdown', (e) => {
        console.debug('[wheel] down', e.target.id || 'ring', wheelPos(e));
        try { wheelEl.setPointerCapture(e.pointerId); } catch (err) {}
        wheelLast = wheelPos(e);
        tapButton = RING_BUTTONS[e.target.id] ?? null;
        tapMoved = 0;
        qemu._qemu_wasm_wheel(wheelLast, 1);
        e.preventDefault();
    });
    wheelEl.addEventListener('pointermove', (e) => {
        if (wheelLast < 0) return;
        const p = wheelPos(e);
        if (p !== wheelLast) {
            /* shortest signed distance around the ring */
            let d = (p - wheelLast + 96) % 96;
            if (d > 48) d -= 96;
            tapMoved += Math.abs(d);
            if (tapMoved > TAP_SLOP_CLICKS) {
                tapButton = null;   /* it's a scroll, not a tap */
            }
            wheelLast = p;
            qemu._qemu_wasm_wheel(p, 1);
        }
    });
    const lift = (e) => {
        if (wheelLast < 0) return;
        qemu._qemu_wasm_wheel(wheelLast, 0);
        wheelLast = -1;
        if (tapButton !== null) {
            /* finger lifted without rotating: deliver the button tap,
             * held long enough for the guest's button tick to see it */
            const q = tapButton;
            tapButton = null;
            key(q, true);
            setTimeout(() => key(q, false), 100);
        }
    };
    wheelEl.addEventListener('pointerup', lift);
    wheelEl.addEventListener('pointercancel', lift);
}

/* ---- audio: stream the I2S tee into Web Audio ----
 * The SDL backend cannot run from QEMU's pthread in the browser, so
 * the shim publishes PCM chunks through MEMFS (like the framebuffer)
 * and we schedule them on an AudioContext, created on the first user
 * gesture to satisfy autoplay policies. */
let actx = null, playhead = 0, lastChunk = 0;

function ensureAudio() {
    try {
        if (!actx) {
            const AC = window.AudioContext || window.webkitAudioContext;
            try { actx = new AC({ sampleRate: 44100 }); }
            catch (e) { actx = new AC(); }
            playhead = 0;
        }
        if (actx.state !== 'running') {
            actx.resume();
            /* canonical iOS unlock: play a short silent buffer from
             * inside the user gesture */
            const b = actx.createBuffer(1, 1, actx.sampleRate);
            const src = actx.createBufferSource();
            src.buffer = b;
            src.connect(actx.destination);
            src.start(0);
        }
    } catch (e) { console.warn('audio unlock failed:', e); }
}
/* iOS Safari only treats some gesture types as activation for audio */
for (const ev of ['touchend', 'click', 'pointerup', 'keydown']) {
    window.addEventListener(ev, ensureAudio, true);
}

function pumpAudio() {
    if (!actx || actx.state !== 'running') return;
    qemu._qemu_wasm_audio_poll();
    try {
        const d = qemu.FS.readFile('/achunk');
        const hdr = new Uint32Array(d.buffer, d.byteOffset, 2);
        if (hdr[0] === lastChunk) return;
        lastChunk = hdr[0];
        const n = hdr[1];                     /* 16-bit samples (L,R,...) */
        const pcm = new Int16Array(d.buffer, d.byteOffset + 8, n);
        const frames = n >> 1;
        if (!frames) return;
        const ab = actx.createBuffer(2, frames, 44100);
        const L = ab.getChannelData(0), R = ab.getChannelData(1);
        for (let i = 0; i < frames; i++) {
            L[i] = pcm[2 * i] / 32768;
            R[i] = pcm[2 * i + 1] / 32768;
        }
        const src = actx.createBufferSource();
        src.buffer = ab;
        src.connect(actx.destination);
        /*
         * Jitter buffer: the producer is bursty (interpreted CPU,
         * polled chunks), so keep a deep cushion ahead of the audio
         * clock and re-establish it fully after an underrun rather
         * than scheduling at the edge.
         */
        if (playhead < actx.currentTime + 0.10) {
            playhead = actx.currentTime + 0.25;
        }
        src.start(playhead);
        playhead += frames / 44100;
    } catch (e) { /* no audio yet */ }
}
setInterval(pumpAudio, 25);

/* debug/automation hook */
window.__ipod = {
    key: (q, d) => qemu._qemu_wasm_key_event(q, d),
    wheel: (p, t) => qemu._qemu_wasm_wheel(p, t),
    fb: () => lastFrame,
    QKEY,
};
console.log('[ipod6g] input wired; wheelApi=', hasWheelApi);

canvas.focus();
