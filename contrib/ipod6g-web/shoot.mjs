/*
 * Browser proof harness: drive the wasm iPod emulator in headless
 * Chromium, inject keys, and save canvas screenshots.
 *
 * Usage: node shoot.mjs <url> <outdir> [script]
 *   script = comma-separated: wait:<secs>, key:<DomCode>, shot:<name>
 *   default: boot, screenshot, navigate, screenshot.
 *
 * Run from a directory with puppeteer installed (npm i puppeteer).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
import puppeteer from 'puppeteer';
import { mkdirSync } from 'node:fs';

const url = process.argv[2] ?? 'http://127.0.0.1:8080/';
const outdir = process.argv[3] ?? 'shots';
const script = (process.argv[4] ??
    'wait:90,shot:browser_menu,key:Enter,key:Enter,wait:3,shot:browser_files,'
    + 'key:Enter,wait:4,shot:browser_music,key:Enter,wait:8,shot:browser_playing'
).split(',');

mkdirSync(outdir, { recursive: true });

const browser = await puppeteer.launch({
    headless: 'new',
    args: ['--no-sandbox', '--enable-unsafe-webgpu'],
});
const page = await browser.newPage();
page.on('console', (m) => console.log('[page]', m.text().slice(0, 120)));
page.on('pageerror', (e) => console.log('[pageerror]', e.message));

await page.goto(url, { waitUntil: 'networkidle2', timeout: 120000 });
await page.focus('#screen');

for (const step of script) {
    const [op, arg] = step.split(':');
    if (op === 'wait') {
        await new Promise((r) => setTimeout(r, Number(arg) * 1000));
    } else if (op === 'key') {
        await page.keyboard.down(arg);
        await new Promise((r) => setTimeout(r, 120));
        await page.keyboard.up(arg);
        await new Promise((r) => setTimeout(r, 600));
    } else if (op === 'shot') {
        const canvas = await page.$('#screen');
        await canvas.screenshot({ path: `${outdir}/${arg}.png` });
        console.log('saved', `${outdir}/${arg}.png`);
    }
}
await browser.close();
