// End to end: the JS host library (../../js/blit.js) against the firmware
// library, running as build/sim_display over the stream framing. The two are
// written separately from the spec, so if they agree, the spec describes both.
//
//   make -C blit/firmware/test      (builds the sim, then runs this)
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { test } from 'node:test';

import { Blit, BlitError, ENCODING, ERROR, FORMAT, KEY, frameBytes } from '../../js/blit.js';

const SIM = process.env.SIM_DISPLAY ?? new URL('./build/sim_display', import.meta.url).pathname;

// Test-only channels (see sim_display.cpp).
const CH = { CONTROL: 1, DATA: 2, STATUS: 3, EVENT: 4, CONNECT: 0xf0, INFO: 0xf2, KEY: 0xf3, AREA: 0xf4, FORMATS: 0xf5, SHOWN: 0xf3 };

/** A Blit transport over the sim's stdin/stdout, plus hooks to poke the display. */
class SimProcess {
  #proc = spawn(SIM, [], { stdio: ['pipe', 'pipe', 'inherit'] });
  #rx = Buffer.alloc(0);
  #handlers = null;
  #infoWaiters = [];
  #connected = false;
  shown = []; // frames the display showed: { format, width, height, pixels }
  #shownWaiters = [];

  constructor({ maxMessage = 244 } = {}) {
    this.maxMessage = maxMessage;
    this.#proc.stdout.on('data', (chunk) => this.#read(chunk));
  }

  get name() {
    return 'blit-sim';
  }

  get connected() {
    return this.#connected;
  }

  async open(handlers) {
    this.#handlers = handlers;
    this.#send(CH.CONNECT, Uint8Array.of(this.maxMessage & 0xff, this.maxMessage >> 8));
    this.#connected = true;
  }

  readInfo() {
    return new Promise((resolve) => {
      this.#infoWaiters.push(resolve);
      this.#send(CH.INFO, new Uint8Array(0));
    });
  }

  async control(bytes) {
    this.#send(CH.CONTROL, bytes);
  }

  async data(bytes) {
    this.#send(CH.DATA, bytes);
  }

  close() {
    this.#connected = false;
    this.#proc.stdin.end();
    this.#handlers?.onDisconnect();
  }

  pressKey(key) {
    this.#send(CH.KEY, Uint8Array.of(key));
  }

  setArea(width, height) {
    this.#send(CH.AREA, Uint8Array.of(width & 0xff, width >> 8, height & 0xff, height >> 8));
  }

  setFormats(formats) {
    this.#send(CH.FORMATS, Uint8Array.from(formats));
  }

  nextShown() {
    return this.shown.length ? Promise.resolve(this.shown.shift()) : new Promise((r) => this.#shownWaiters.push(r));
  }

  #send(channel, bytes) {
    const out = Buffer.alloc(3 + bytes.length);
    out[0] = channel;
    out.writeUInt16LE(bytes.length, 1);
    out.set(bytes, 3);
    this.#proc.stdin.write(out);
  }

  #read(chunk) {
    this.#rx = Buffer.concat([this.#rx, chunk]);
    while (this.#rx.length >= 3) {
      const length = this.#rx.readUInt16LE(1);
      if (this.#rx.length < 3 + length) return;
      const channel = this.#rx[0];
      const body = new Uint8Array(this.#rx.subarray(3, 3 + length));
      this.#rx = this.#rx.subarray(3 + length);
      const dv = new DataView(body.buffer, body.byteOffset, body.byteLength);
      if (channel === CH.STATUS) this.#handlers?.onStatus(dv);
      else if (channel === CH.EVENT) this.#handlers?.onEvent(dv);
      else if (channel === CH.INFO) this.#infoWaiters.shift()?.(dv);
      else if (channel === CH.SHOWN) {
        const frame = { format: body[0], width: dv.getUint16(1, true), height: dv.getUint16(3, true), pixels: body.subarray(5) };
        const waiter = this.#shownWaiters.shift();
        if (waiter) waiter(frame);
        else this.shown.push(frame);
      }
    }
  }
}

// A host connected to a fresh sim, which goes away when the test ends.
async function connected(t, opts) {
  const sim = new SimProcess(opts);
  const blit = new Blit({ hostName: 'node test' });
  t.after(() => blit.disconnect());
  await blit.connectTransport(sim);
  return { sim, blit };
}

function frame(format, w, h, fill = 0) {
  return new Uint8Array(frameBytes(format, w, h)).fill(fill);
}

test('caps from the firmware parse as the display declared them', async (t) => {
  const { blit } = await connected(t);
  const c = blit.caps;
  assert.equal(c.version, 2);
  assert.equal(c.name, 'blit-sim');
  assert.deepEqual([c.width, c.height], [64, 32]);
  assert.deepEqual(c.formats, [FORMAT.MONO1, FORMAT.GRAY2]);
  assert.deepEqual(c.encodings, [ENCODING.NONE, ENCODING.PACKBITS]);
  assert.equal(c.chunk, 240);
  assert.equal(c.regionAlign, 8);
  assert.deepEqual(c.keys, [KEY.UP, KEY.DOWN, KEY.SELECT]);
  assert.equal(c.features.persist, true);
  assert.equal(c.features.frameSleep, false);
  assert.deepEqual(c.battery, { percent: 87, charging: false, external: false });
});

test('frames arrive intact, plain and PackBits, over small and large MTUs', async (t) => {
  for (const maxMessage of [20, 244, 512]) {
    const { sim, blit } = await connected(t, { maxMessage });
    const px = frame(FORMAT.MONO1, 64, 32);
    for (let i = 0; i < px.length; i += 7) px[i] = i & 0xff; // not much to compress
    const plain = await blit.sendFrame(px, { width: 64, height: 32, encoding: ENCODING.NONE });
    assert.equal(plain.encoding, ENCODING.NONE);
    assert.deepEqual((await sim.nextShown()).pixels, px);

    const sparse = frame(FORMAT.GRAY2, 64, 32);
    sparse.fill(0xff, 40, 80);
    const packed = await blit.sendFrame(sparse, { format: FORMAT.GRAY2, width: 64, height: 32 });
    assert.equal(packed.encoding, ENCODING.PACKBITS);
    const shown = await sim.nextShown();
    assert.equal(shown.format, FORMAT.GRAY2);
    assert.deepEqual(shown.pixels, sparse);
  }
});

test('regions patch the frame on the display, and fall back to full frames', async (t) => {
  const { sim, blit } = await connected(t);
  const a = frame(FORMAT.MONO1, 64, 32);
  await blit.sendFrame(a, { width: 64, height: 32, regions: true });
  await sim.nextShown();

  const b = a.slice();
  b[10 * 8 + 3] = 0x81; // row 10, pixels 24 and 31
  const r = await blit.sendFrame(b, { width: 64, height: 32, regions: true });
  assert.deepEqual(r.region, { x: 24, y: 10, width: 8, height: 1 });
  assert.deepEqual((await sim.nextShown()).pixels, b);

  // The area changes (and back), so the display's base no longer counts:
  // error 9, and the host resends the whole frame.
  const areaBack = new Promise((resolve) => blit.addEventListener('caps', (e) => e.detail.width === 64 && resolve()));
  sim.setArea(56, 32);
  sim.setArea(64, 32);
  await areaBack;
  const c = b.slice();
  c[20 * 8] = 0x80;
  const full = await blit.sendFrame(c, { width: 64, height: 32, regions: true });
  assert.equal(full.region, null);
  assert.deepEqual((await sim.nextShown()).pixels, c);
});

test('errors from the firmware reach the host as BlitErrors', async (t) => {
  const { blit } = await connected(t);
  await assert.rejects(
    blit.sendFrame(frame(FORMAT.GRAY4, 64, 32), { format: FORMAT.GRAY4, width: 64, height: 32 }),
    (err) => err instanceof BlitError && err.code === ERROR.UNSUPPORTED,
  );
  await assert.rejects(
    blit.sendFrame(frame(FORMAT.MONO1, 8, 8), { width: 8, height: 8, region: true, x: 4, y: 0 }),
    (err) => err instanceof BlitError && err.code === ERROR.BAD_REGION,
  );
  await assert.rejects(
    blit.sendFrame(frame(FORMAT.MONO1, 2048, 96), { width: 2048, height: 96, encoding: ENCODING.NONE }),
    /at most/, // the host checks maxBytes itself
  );
});

test('keys and caps changes reach the host', async (t) => {
  const { sim, blit } = await connected(t);
  const keys = [];
  blit.addEventListener('key', (e) => keys.push(e.detail.name));
  const caps = new Promise((resolve) => blit.addEventListener('caps', (e) => e.detail.formats[0] === FORMAT.GRAY2 && resolve(e.detail)));
  sim.pressKey(KEY.DOWN);
  sim.pressKey(KEY.SELECT);
  sim.setFormats([FORMAT.GRAY2, FORMAT.MONO1]);
  await caps;
  assert.deepEqual(keys, ['down', 'select']);
  assert.equal(blit.preferredFormat(), FORMAT.GRAY2);
});
