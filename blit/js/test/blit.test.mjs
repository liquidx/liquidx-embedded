// Run: node --test blit/js/test/*.test.mjs
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { Blit, changedRect, cropPixels } from '../blit.js';
import {
  ENCODING, FORMAT, KEY, KEY_ACTION, crc32, encodeBegin, encodeCaps, frameBytes, packbits, parseBegin, parseCaps,
  parseEvent, encodeKeyEvent, unpackbits,
} from '../protocol.js';
import { packLevels, rasterizeRgba, unpackLevels } from '../raster.js';
import { SimDisplay, SimTransport } from '../sim-display.js';

test('crc32 matches zlib', () => {
  assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
  const bytes = new TextEncoder().encode('hello world');
  assert.equal(crc32(bytes.subarray(5), crc32(bytes.subarray(0, 5))), crc32(bytes));
});

test('packbits round-trips', () => {
  const cases = [
    new Uint8Array(0),
    Uint8Array.of(1),
    new Uint8Array(1000),
    Uint8Array.from({ length: 1000 }, (_, i) => (i * 7919) & 0xff),
    Uint8Array.from({ length: 5000 }, (_, i) => (i % 300 < 200 ? 0 : i & 3)),
  ];
  for (const c of cases) assert.deepEqual(unpackbits(packbits(c)), c);
  assert.ok(packbits(new Uint8Array(1000)).length < 20);
});

test('caps survive a TLV round trip, and unknown tags are skipped', () => {
  const caps = {
    version: 2, name: 'Test', panel: { width: 800, height: 480 }, width: 716, height: 480,
    formats: [FORMAT.GRAY4, FORMAT.MONO1], encodings: [ENCODING.NONE, ENCODING.PACKBITS],
    maxBytes: 200000, chunk: 244, window: 16,
    features: { persist: true, frameSleep: true, fastRefresh: false, pointer: true },
    regionAlign: 8, keys: [KEY.UP, KEY.DOWN], minIntervalMs: 1000, refreshMs: 1500,
    battery: { percent: 50, charging: true, external: true },
  };
  const bytes = encodeCaps(caps);
  assert.deepEqual(parseCaps(bytes), caps);
  const withUnknown = Uint8Array.from([...bytes, 0xee, 3, 1, 2, 3]);
  assert.deepEqual(parseCaps(withUnknown), caps);
});

test('v1 JSON Info parses into the same caps shape', () => {
  const json = '{"v":1,"name":"X4-1A2B","w":716,"h":480,"fullW":800,"fullH":480,"chunk":508,"window":16,"maxBytes":65536,"formats":[1],"frameSleep":true}';
  const caps = parseCaps(new TextEncoder().encode(json));
  assert.equal(caps.version, 1);
  assert.equal(caps.width, 716);
  assert.deepEqual(caps.panel, { width: 800, height: 480 });
  assert.equal(caps.chunk, 508);
  assert.deepEqual(caps.formats, [1]);
  assert.equal(caps.features.frameSleep, true);
  assert.equal(caps.regionAlign, 0);
});

test('begin headers encode and parse (v1 and v2)', () => {
  const v1 = encodeBegin({ version: 1, width: 716, height: 480, byteLength: 43440, crc: 0xdeadbeef, nextFrameSeconds: 60, name: 'clock', persist: true });
  assert.equal(v1.length, 21 + 5);
  assert.deepEqual(
    { ...parseBegin(v1) },
    { version: 1, format: 1, encoding: 0, persist: true, region: false, hold: false, refresh: 0, width: 716, height: 480, x: 0, y: 0, byteLength: 43440, crc: 0xdeadbeef, nextFrameSeconds: 60, name: 'clock' },
  );
  const v2 = encodeBegin({ format: FORMAT.GRAY4, encoding: 1, region: true, hold: true, refresh: 2, width: 64, height: 32, x: 8, y: 16, byteLength: 99, crc: 1, name: '' });
  assert.equal(v2.length, 29);
  const h = parseBegin(v2);
  assert.equal(h.version, 2);
  assert.equal(h.format, FORMAT.GRAY4);
  assert.equal(h.region, true);
  assert.equal(h.hold, true);
  assert.equal(h.refresh, 2);
  assert.equal(h.x, 8);
  assert.equal(h.y, 16);
});

test('events parse', () => {
  assert.deepEqual(parseEvent(encodeKeyEvent(KEY.PAGE_NEXT, KEY_ACTION.LONG, 1234)), { type: 'key', key: 9, name: 'pageNext', action: 'long', timeMs: 1234 });
});

test('pixel packing: levels and rows padded to a byte', () => {
  const values = Uint8Array.from([3, 0, 1, 2, 3]); // 5 px at 2 bpp -> 2 bytes
  const packed = packLevels(values, 5, 1, 2);
  assert.deepEqual([...packed], [0b11000110, 0b11000000]);
  assert.deepEqual(unpackLevels(packed, 5, 1, 2), values);
  assert.equal(frameBytes(FORMAT.MONO1, 716, 480), 90 * 480);
  assert.equal(frameBytes(FORMAT.GRAY4, 3, 2), 4);
  assert.equal(frameBytes(FORMAT.RGB565, 3, 2), 12);
});

test('rasterizeRgba: white is 0, black is full ink, in every grey format', () => {
  const rgba = { width: 2, height: 1, data: Uint8ClampedArray.from([0, 0, 0, 255, 255, 255, 255, 255]) };
  assert.deepEqual([...rasterizeRgba(rgba, { format: FORMAT.MONO1 }).pixels], [0b10000000]);
  assert.deepEqual([...rasterizeRgba(rgba, { format: FORMAT.GRAY2 }).pixels], [0b11000000]);
  assert.deepEqual([...rasterizeRgba(rgba, { format: FORMAT.GRAY4 }).pixels], [0xf0]);
  assert.deepEqual([...rasterizeRgba(rgba, { format: FORMAT.GRAY8 }).pixels], [255, 0]);
  assert.deepEqual([...rasterizeRgba(rgba, { format: FORMAT.RGB565 }).pixels], [0, 0, 0xff, 0xff]);
});

test('changedRect finds the aligned bounding box', () => {
  const w = 64;
  const h = 10;
  const a = new Uint8Array(frameBytes(FORMAT.MONO1, w, h));
  const b = a.slice();
  assert.equal(changedRect(a, b, FORMAT.MONO1, w, h, 8), null);
  b[3 * 8 + 2] = 0x01; // row 3, pixels 16..23
  b[5 * 8 + 4] = 0x80; // row 5, pixels 32..39
  assert.deepEqual(changedRect(a, b, FORMAT.MONO1, w, h, 8), { x: 16, y: 3, width: 24, height: 3 });
  assert.deepEqual(changedRect(a, b, FORMAT.MONO1, w, h, 16), { x: 16, y: 3, width: 32, height: 3 });
  const crop = cropPixels(b, FORMAT.MONO1, w, { x: 16, y: 3, width: 24, height: 3 });
  assert.deepEqual([...crop], [1, 0, 0, 0, 0, 0, 0, 0, 0x80]);
});

async function connected(opts) {
  const display = new SimDisplay({ refreshMs: 5, ...opts });
  const blit = new Blit({ hostName: 'test' });
  await blit.connectTransport(new SimTransport(display));
  return { display, blit };
}

function frame(format, w, h, fill = 0) {
  return new Uint8Array(frameBytes(format, w, h)).fill(fill);
}

test('end to end: v2 frame with PackBits reaches the display', async () => {
  const { display, blit } = await connected({ width: 64, height: 32 });
  assert.equal(blit.caps.version, 2);
  assert.equal(blit.preferredFormat(), FORMAT.MONO1);
  const px = frame(FORMAT.MONO1, 64, 32);
  px.fill(0xff, 0, 8); // first row black
  const result = await blit.sendFrame(px, { width: 64, height: 32 });
  assert.equal(result.encoding, ENCODING.PACKBITS);
  assert.ok(result.bytes < px.length);
  assert.equal(display.frames, 1);
  assert.equal(display.visible[0], 0); // top-left black
  assert.equal(display.visible[64 * 4], 255); // second row white
});

test('end to end: gray4, regions and skipping unchanged frames', async () => {
  const { display, blit } = await connected({ width: 64, height: 32, formats: [FORMAT.GRAY4, FORMAT.MONO1] });
  assert.equal(blit.preferredFormat(), FORMAT.GRAY4);
  const a = frame(FORMAT.GRAY4, 64, 32);
  await blit.sendFrame(a, { format: FORMAT.GRAY4, width: 64, height: 32, regions: true });
  const skipped = await blit.sendFrame(a, { format: FORMAT.GRAY4, width: 64, height: 32, skipUnchanged: true });
  assert.equal(skipped.skipped, true);
  const resent = await blit.sendFrame(a, { format: FORMAT.GRAY4, width: 64, height: 32, regions: true });
  assert.equal(resent.skipped, false); // regions alone never drop a frame
  assert.equal(resent.region, null);
  const b = a.slice();
  b[10 * 32 + 5] = 0xf0; // row 10, pixel 10 -> black
  const r = await blit.sendFrame(b, { format: FORMAT.GRAY4, width: 64, height: 32, regions: true });
  assert.deepEqual(r.region, { x: 8, y: 10, width: 8, height: 1 });
  assert.equal(display.lastHeader.region, true);
  assert.equal(display.visible[(10 * 64 + 10) * 4], 0);
  assert.equal(display.visible[(10 * 64 + 11) * 4], 255);
});

test('end to end: key presses and caps changes reach the host', async () => {
  const { display, blit } = await connected({ width: 64, height: 32 });
  const keys = [];
  blit.addEventListener('key', (e) => keys.push(`${e.detail.name}:${e.detail.action}`));
  const capsSeen = new Promise((resolve) => blit.addEventListener('caps', (e) => e.detail.width === 32 && resolve(e.detail)));
  display.pressKey(KEY.DOWN);
  display.pressKey(KEY.SELECT, KEY_ACTION.LONG);
  display.setCaps({ width: 32, formats: [FORMAT.GRAY2] });
  const caps = await capsSeen;
  assert.deepEqual(keys, ['down:press', 'select:long']);
  assert.equal(caps.width, 32);
  assert.equal(blit.preferredFormat(), FORMAT.GRAY2);
});

test('end to end: a v1 display gets v1 headers and mono1 only', async () => {
  const { display, blit } = await connected({ version: 1, width: 48, height: 16 });
  assert.equal(blit.caps.version, 1);
  await blit.sendFrame(frame(FORMAT.MONO1, 48, 16, 0xff), { width: 48, height: 16, regions: true });
  assert.equal(display.lastHeader.version, 1);
  assert.equal(display.visible[0], 0);
  await assert.rejects(blit.sendFrame(frame(FORMAT.GRAY4, 48, 16), { format: FORMAT.GRAY4, width: 48, height: 16 }));
});

test('end to end: the display rejects bad frames with errors', async () => {
  const { blit } = await connected({ width: 64, height: 32, formats: [FORMAT.MONO1] });
  await assert.rejects(blit.sendFrame(frame(FORMAT.GRAY4, 64, 32), { format: FORMAT.GRAY4, width: 64, height: 32 }), /unsupported/);
  await assert.rejects(blit.sendFrame(frame(FORMAT.MONO1, 16, 8), { width: 16, height: 8, region: true, x: 60, y: 0 }), /region/);
  // Still usable afterwards.
  await blit.sendFrame(frame(FORMAT.MONO1, 64, 32), { width: 64, height: 32 });
});

test('end to end: a region the display has no base for is resent as a full frame', async () => {
  const { display, blit } = await connected({ width: 64, height: 32 });
  const a = frame(FORMAT.MONO1, 64, 32);
  await blit.sendFrame(a, { width: 64, height: 32, regions: true });
  // The display loses the frame the host diffs against (as after a restart).
  display.setCaps({ width: 64 });
  const b = a.slice();
  b[10 * 8 + 1] = 0x80; // row 10, pixel 8 -> black
  const r = await blit.sendFrame(b, { width: 64, height: 32, regions: true });
  assert.equal(r.region, null);
  assert.equal(display.lastHeader.region, false);
  assert.equal(display.visible[(10 * 64 + 8) * 4], 0);
  // The next change goes as a region again.
  const c = b.slice();
  c[20 * 8] = 0x80;
  assert.deepEqual((await blit.sendFrame(c, { width: 64, height: 32, regions: true })).region, { x: 0, y: 20, width: 8, height: 1 });
});

test('disconnect() forgets the display: later sends do not reconnect', async () => {
  const { display, blit } = await connected({ width: 64, height: 32 });
  blit.disconnect();
  assert.equal(display.connected, false);
  assert.equal(blit.caps, null);
  assert.equal(blit.deviceName, null);
  await assert.rejects(blit.sendFrame(frame(FORMAT.MONO1, 64, 32), { width: 64, height: 32 }), /No display/);
  assert.equal(display.connected, false);
});
