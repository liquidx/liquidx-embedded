// End to end: a local page in headless Chromium, sent to the simulated display.
// Run: npm test (from blit/server)
import assert from 'node:assert/strict';
import { createServer, type Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import { after, before, test } from 'node:test';

import { BlitHost, type HostOptions } from '../src/host.ts';
import { Blit, KEY, SimDisplay, SimTransport } from '../src/lib.ts';

const PAGE = `<!doctype html>
<style>body { margin: 0; font: 40px sans-serif } #box { position: absolute; left: 0; top: 0; width: 200px; height: 100px; background: #000 }</style>
<div id="box"></div>
<button id="b" style="position:absolute;left:300px;top:0;width:100px;height:100px" onclick="document.body.style.background='#000'">x</button>
<div id="key" style="position:absolute;left:0;top:200px">-</div>
<script>addEventListener('keydown', (e) => { document.getElementById('key').textContent = e.key; });</script>`;

let server: Server;
let url: string;

before(async () => {
  server = createServer((_, res) => res.end(PAGE)).listen(0);
  await new Promise((r) => server.once('listening', r));
  url = `http://127.0.0.1:${(server.address() as AddressInfo).port}/`;
});
after(() => server.close());

async function setup(page: Partial<HostOptions['page']> = {}, opts: Partial<HostOptions> = {}) {
  const display = new SimDisplay({ width: 400, height: 240, refreshMs: 0 });
  const blit = new Blit({ hostName: 'test' });
  await blit.connectTransport(new SimTransport(display));
  const host = new BlitHost(blit, {
    page: { url, zoom: 1, fit: 'contain', waitUntil: 'load', settleMs: 50, chromeArgs: [], ...page },
    size: 'area', intervalSeconds: 0, reloadSeconds: 0, sleep: false, format: 'auto', dither: 'threshold', threshold: 128, contrast: 1,
    invert: false, persist: false, name: '', refresh: 0, regions: true, buttons: 'keys', taps: true,
    reconnectTimeoutMs: 1000, log: () => {}, verbose: () => {},
    ...opts,
  });
  await host.start();
  return { display, blit, host };
}

/** Is the simulated panel black at (x, y)? */
function black(display: SimDisplay, x: number, y: number) {
  return display.visible[(y * display.caps.width + x) * 4] < 128;
}

function nextFrame(display: SimDisplay) {
  return new Promise((r) => display.addEventListener('frame', r, { once: true }));
}

test('sends the page at the display size, and skips unchanged frames', async () => {
  const { display, host } = await setup();
  try {
    const first = await host.frame();
    assert.equal(first.sent, true);
    assert.equal(display.frames, 1);
    assert.ok(black(display, 10, 10), 'box is black');
    assert.ok(black(display, 199, 99), 'box ends at 200×100');
    assert.ok(!black(display, 201, 50), 'right of the box is white');
    const second = await host.frame();
    assert.equal(second.skipped, true);
    assert.equal(display.frames, 1);
  } finally {
    await host.stop();
  }
});

test('zoom renders at display pixels per CSS pixel', async () => {
  const { display, host } = await setup({ zoom: 2 });
  try {
    await host.frame();
    assert.ok(black(display, 399, 199));
    assert.ok(!black(display, 10, 201));
  } finally {
    await host.stop();
  }
});

test('a selector is fitted to the display', async () => {
  const { display, host } = await setup({ selector: '#box', fit: 'contain' });
  try {
    await host.frame();
    // 200×100 contained in 400×240: scaled ×2, letterboxed 20 px top and bottom.
    assert.ok(!black(display, 200, 10));
    assert.ok(black(display, 200, 30));
    assert.ok(black(display, 399, 219));
    assert.ok(!black(display, 200, 230));
  } finally {
    await host.stop();
  }
});

test('a viewport width alone keeps the display aspect ratio', async () => {
  const { display, host } = await setup({ viewport: { width: 800 } });
  try {
    await host.frame();
    assert.deepEqual(await host.source.page.evaluate(() => [innerWidth, innerHeight]), [800, 480]);
    // Laid out at 800×480, shown at 400×240: the 200×100 box is 100×50.
    assert.ok(black(display, 99, 49));
    assert.ok(!black(display, 101, 25));
    assert.ok(!black(display, 50, 51));
  } finally {
    await host.stop();
  }
});

test('size panel renders for the whole screen, cropped to the frame area', async () => {
  const display = new SimDisplay({ width: 300, height: 240, panelWidth: 400, panelHeight: 240, refreshMs: 0 });
  const blit = new Blit({ hostName: 'test' });
  await blit.connectTransport(new SimTransport(display));
  const host = new BlitHost(blit, {
    page: { url, zoom: 1, fit: 'contain', waitUntil: 'load', settleMs: 0, chromeArgs: [] },
    size: 'panel', intervalSeconds: 0, reloadSeconds: 0, sleep: false, format: 'auto', dither: 'threshold', threshold: 128,
    contrast: 1, invert: false, persist: false, name: '', refresh: 0, regions: true, buttons: 'keys', taps: true,
    reconnectTimeoutMs: 1000, log: () => {}, verbose: () => {},
  });
  await host.start();
  try {
    await host.frame();
    assert.deepEqual(await host.source.page.evaluate(() => [innerWidth, innerHeight]), [400, 240]);
    assert.ok(black(display, 199, 99), 'drawn at native size, pinned top left');
    // The page changes: sent whole, since a region can't patch a cropped frame.
    await host.source.page.evaluate(() => { document.getElementById('key')!.textContent = 'changed'; });
    const second = await host.frame();
    assert.equal(second.sent, true);
    assert.equal(second.region, null);
    // Chrome hidden: the frame area is the panel, and regions work again.
    display.setCaps({ width: 400, height: 240 });
    await host.frame();
    await host.source.page.evaluate(() => { document.getElementById('key')!.textContent = 'again'; });
    const third = await host.frame();
    assert.ok(third.region, `sent as a region: ${JSON.stringify(third)}`);
  } finally {
    await host.stop();
  }
});

test('buttons press keys, and taps click, then a frame follows', async () => {
  const { display, host } = await setup();
  try {
    await host.frame();
    let shown = nextFrame(display);
    assert.ok(display.pressKey(KEY.SELECT));
    await shown;
    assert.equal(await host.source.page.$eval('#key', (el) => el.textContent), 'Enter');

    shown = nextFrame(display);
    assert.ok(display.tap(350, 50));
    await shown;
    assert.ok(black(display, 250, 150), 'the button turned the page black');
  } finally {
    await host.stop();
  }
});

test('a caps change re-renders at the new size', async () => {
  const { display, host } = await setup();
  try {
    await host.frame();
    const shown = nextFrame(display);
    display.setCaps({ width: 160, height: 80 });
    await shown;
    assert.equal(display.caps.width, 160);
    assert.ok(black(display, 150, 70));
  } finally {
    await host.stop();
  }
});
