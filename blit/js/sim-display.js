// A simulated blit display: the display side of ../PROTOCOL.md in plain
// JavaScript. It's the reference for display implementers, and lets hosts
// (the demo page, the Chrome extension, tests) run without hardware.
//
//   import { SimDisplay, SimTransport } from './sim-display.js';
//   const display = new SimDisplay({ width: 400, height: 300, formats: [FORMAT.GRAY4, FORMAT.MONO1] });
//   await blit.connectTransport(new SimTransport(display));
//   display.addEventListener('frame', () => ctx.putImageData(display.imageData(), 0, 0));
//   display.pressKey(KEY.DOWN);
//
// Events: 'frame' (a frame was shown), 'caps' (caps changed), 'log' (detail: text).

import {
  ENCODING, ERROR, FORMAT, FORMAT_INFO, HELLO_FLAG, KEY, KEY_ACTION, OP, POINTER_ACTION, PackBitsDecoder, STATUS,
  crc32, encodeCaps, encodeCapsEvent, encodeKeyEvent, encodePointerEvent, encodeStatus, frameBytes, parseBegin,
} from './protocol.js';
import { unpackLevels } from './raster.js';

const MIN_SLEEP_GAP_SECONDS = 30;
const WAKE_MARGIN_SECONDS = 10;
const OFFSET_BYTES = 4;

export class SimDisplay extends EventTarget {
  /**
   * Options: name, width, height (frame area), panelWidth, panelHeight,
   * formats, encodings, maxBytes, chunk, window, regionAlign (0 = none), keys,
   * pointer, frameSleep, minIntervalMs, refreshMs (how long "showing" takes),
   * version (1 = behave like the X4 firmware today).
   */
  constructor({
    name = 'blit-sim', width = 400, height = 300, panelWidth, panelHeight,
    formats = [FORMAT.MONO1, FORMAT.GRAY2, FORMAT.GRAY4], encodings = [ENCODING.PACKBITS],
    maxBytes = 512 * 1024, chunk = 244, window = 16, regionAlign = 8,
    keys = [KEY.UP, KEY.DOWN, KEY.LEFT, KEY.RIGHT, KEY.SELECT, KEY.PAGE_NEXT, KEY.PAGE_PREV],
    pointer = true, frameSleep = false, minIntervalMs = 0, refreshMs = 250, version = 2,
  } = {}) {
    super();
    this.version = version;
    this.caps = {
      version,
      name,
      panel: { width: panelWidth ?? width, height: panelHeight ?? height },
      width,
      height,
      formats: version < 2 ? [FORMAT.MONO1] : formats,
      encodings: version < 2 ? [] : encodings,
      maxBytes,
      chunk,
      window,
      features: { persist: true, frameSleep, fastRefresh: version >= 2, pointer: version >= 2 && pointer },
      regionAlign: version < 2 ? 0 : regionAlign,
      keys: version < 2 ? [] : keys,
      minIntervalMs,
      refreshMs,
      battery: version < 2 ? null : { percent: 87, charging: false, external: false },
    };
    this.#resize();
    this.frames = 0;
    this.asleepUntil = 0;
    this.lastHeader = null;
  }

  #link = null; // { status(bytes), event(bytes), disconnect() }
  #rx = null; // transfer in progress
  #showing = false;
  #wants = { keys: false, pointer: false }; // what the host's hello asked for
  #base = null; // the full frame regions patch: { format }, or null

  get connected() {
    return !!this.#link;
  }

  // --- what a host sees ---------------------------------------------------------

  /** Info characteristic value: v2 TLV caps, or v1 JSON. */
  info() {
    const c = this.caps;
    if (this.version < 2) {
      return new TextEncoder().encode(JSON.stringify({
        v: 1, name: c.name, w: c.width, h: c.height, fullW: c.panel.width, fullH: c.panel.height,
        chunk: c.chunk, window: c.window, maxBytes: c.maxBytes, formats: [1], frameSleep: c.features.frameSleep,
      }));
    }
    return encodeCaps(c);
  }

  /** Change caps (e.g. { width, height } or { formats }) and tell the host. */
  setCaps(changes) {
    Object.assign(this.caps, changes);
    if ('width' in changes || 'height' in changes) this.#resize();
    this.#event(encodeCapsEvent(this.caps));
    this.dispatchEvent(new CustomEvent('caps', { detail: this.caps }));
  }

  /** A button press, forwarded to the host if it's in caps.keys and the host's hello asked for keys. */
  pressKey(key, action = KEY_ACTION.PRESS) {
    if (!this.caps.keys.includes(key) || !this.#wants.keys) return false;
    this.#event(encodeKeyEvent(key, action, Math.round(performance.now()) >>> 0));
    return true;
  }

  /** A tap at frame-area pixel (x, y). */
  tap(x, y) {
    if (!this.caps.features.pointer || !this.#wants.pointer) return false;
    this.#event(encodePointerEvent(POINTER_ACTION.TAP, Math.round(x), Math.round(y)));
    return true;
  }

  /** The panel as RGBA ImageData (frame-area sized). */
  imageData() {
    return new ImageData(new Uint8ClampedArray(this.visible), this.caps.width, this.caps.height);
  }

  // --- link (called by SimTransport) ---------------------------------------------

  attach(link) {
    if (this.asleepUntil > Date.now()) throw new Error('Display is asleep');
    this.#link = link;
    this.#wants = { keys: false, pointer: false };
    this.#base = null; // a new host may be diffing against anything
  }

  detach() {
    this.#link = null;
    this.#rx = null;
    this.#base = null;
  }

  control(bytes) {
    if (!bytes.length) return this.#error(ERROR.BAD_HEADER);
    switch (bytes[0]) {
      case OP.BEGIN:
        return this.#begin(bytes);
      case OP.COMMIT:
        return this.#commit();
      case OP.CANCEL:
        this.#rx = null;
        return undefined;
      case OP.HELLO:
        if (this.version < 2) return this.#error(ERROR.BAD_HEADER, bytes[0]);
        this.#wants = { keys: !!(bytes[2] & HELLO_FLAG.KEYS), pointer: !!(bytes[2] & HELLO_FLAG.POINTER) };
        this.#log(`Hello from ${new TextDecoder().decode(bytes.subarray(4, 4 + bytes[3])) || 'a host'} (v${bytes[1]})`);
        return this.#event(encodeCapsEvent(this.caps));
      default:
        return this.#error(ERROR.BAD_HEADER, bytes[0]);
    }
  }

  data(bytes) {
    const rx = this.#rx;
    if (!rx || bytes.length < OFFSET_BYTES) return;
    const offset = new DataView(bytes.buffer, bytes.byteOffset).getUint32(0, true);
    const payload = bytes.subarray(OFFSET_BYTES);
    if (offset !== rx.received) return this.#fail(ERROR.OUT_OF_ORDER, rx.received);
    if (rx.received + payload.length > rx.h.byteLength) return this.#fail(ERROR.TOO_LARGE, rx.h.byteLength);
    rx.crc = crc32(payload, rx.crc);
    rx.received += payload.length;
    rx.write(payload);
    if (++rx.unacked >= this.caps.window || rx.received === rx.h.byteLength) {
      rx.unacked = 0;
      this.#status(STATUS.ACK, 0, rx.received);
    }
  }

  // --- receiving -------------------------------------------------------------------

  #begin(bytes) {
    this.#rx = null;
    const h = parseBegin(bytes);
    if (!h) return this.#error(ERROR.BAD_HEADER);
    const c = this.caps;
    if (h.version < 1 || h.version > this.version) return this.#error(ERROR.UNSUPPORTED, h.version);
    if (!c.formats.includes(h.format) || !FORMAT_INFO[h.format]) return this.#error(ERROR.UNSUPPORTED, h.format);
    if (h.encoding !== ENCODING.NONE && !c.encodings.includes(h.encoding)) return this.#error(ERROR.UNSUPPORTED, h.encoding);
    if (!h.width || !h.height || h.width > 4096 || h.height > 4096) return this.#error(ERROR.BAD_HEADER);
    const expected = frameBytes(h.format, h.width, h.height);
    if (h.byteLength > c.maxBytes || expected > c.maxBytes || (h.encoding === ENCODING.NONE && h.byteLength !== expected)) {
      return this.#error(ERROR.TOO_LARGE, expected);
    }
    if (h.region) {
      const align = c.regionAlign;
      const bpp = FORMAT_INFO[h.format].bpp;
      const fits = h.x + h.width <= c.width && h.y + h.height <= c.height;
      const aligned = align && h.x % align === 0 && (h.width % align === 0 || h.x + h.width === c.width) && (h.x * bpp) % 8 === 0;
      // A region patches the last full frame from this connection, so it
      // needs one, of the area's size and in the same format.
      const based = this.#base?.format === h.format;
      if (!fits || !aligned || !based) return this.#error(ERROR.BAD_REGION);
    }
    if (this.#showing) return this.#error(ERROR.BUSY);

    // Decode straight into the frame buffer as bytes arrive (principle 2).
    const pixels = new Uint8Array(expected);
    const rx = { h, pixels, decoded: 0, received: 0, crc: 0, unacked: 0 };
    const emit = (b) => {
      if (rx.decoded < expected) pixels[rx.decoded] = b;
      rx.decoded++;
    };
    if (h.encoding === ENCODING.PACKBITS) {
      const decoder = new PackBitsDecoder(emit);
      rx.write = (chunk) => decoder.feed(chunk);
    } else {
      rx.write = (chunk) => chunk.forEach(emit);
    }
    this.#rx = rx;
    this.#status(STATUS.READY, 0, c.chunk);
  }

  #commit() {
    const rx = this.#rx;
    this.#rx = null;
    if (!rx) return this.#error(ERROR.INCOMPLETE, 0);
    const { h } = rx;
    if (rx.received !== h.byteLength) return this.#error(ERROR.INCOMPLETE, rx.received);
    if (rx.crc !== h.crc) return this.#error(ERROR.BAD_CRC);
    if (rx.decoded !== rx.pixels.length) return this.#error(ERROR.DECODE, rx.decoded);

    this.#draw(h, rx.pixels);
    if (!h.region) this.#base = h.width === this.caps.width && h.height === this.caps.height ? { format: h.format } : null;
    this.lastHeader = h;
    this.#showing = true;
    this.#log(`${h.region ? 'Region' : 'Frame'} ${h.width}×${h.height} ${FORMAT_INFO[h.format].name}${h.encoding ? ' packbits' : ''}, ${h.byteLength} B${h.hold ? ' (held)' : ''}`);
    setTimeout(() => {
      this.#showing = false;
      if (!h.hold) {
        this.visible.set(this.buffer);
        this.frames++;
        this.dispatchEvent(new CustomEvent('frame', { detail: h }));
      }
      const sleep = this.caps.features.frameSleep && h.nextFrameSeconds >= MIN_SLEEP_GAP_SECONDS
        ? h.nextFrameSeconds - WAKE_MARGIN_SECONDS : 0;
      this.#status(STATUS.DONE, 0, sleep);
      if (sleep) {
        this.#log(`Sleeping ${sleep} s`);
        this.asleepUntil = Date.now() + sleep * 1000;
        setTimeout(() => this.#link?.disconnect(), 50);
      }
    }, h.hold ? 0 : this.caps.refreshMs);
  }

  // Unpack into the RGBA back buffer: regions at x,y; full frames top-left
  // (cropped) when larger than the area, centred when smaller, like the X4.
  #draw(h, pixels) {
    const W = this.caps.width;
    const H = this.caps.height;
    const x0 = h.region ? h.x : h.width >= W ? 0 : Math.floor((W - h.width) / 2);
    const y0 = h.region ? h.y : h.height >= H ? 0 : Math.floor((H - h.height) / 2);
    if (!h.region) this.buffer.fill(255);
    const info = FORMAT_INFO[h.format];
    const stride = Math.ceil((h.width * info.bpp) / 8);
    const levels = info.levels ? unpackLevels(pixels, h.width, h.height, info.bpp) : null;
    for (let y = 0; y < h.height && y0 + y < H; y++) {
      for (let x = 0; x < h.width && x0 + x < W; x++) {
        const p = ((y0 + y) * W + x0 + x) * 4;
        let r;
        let g;
        let b;
        if (levels) {
          r = g = b = Math.round(255 * (1 - levels[y * h.width + x] / (info.levels - 1)));
        } else if (h.format === FORMAT.RGB565) {
          const v = (pixels[y * stride + x * 2] << 8) | pixels[y * stride + x * 2 + 1];
          r = ((v >> 11) & 31) * 255 / 31;
          g = ((v >> 5) & 63) * 255 / 63;
          b = (v & 31) * 255 / 31;
        } else {
          const q = y * stride + x * 3;
          [r, g, b] = [pixels[q], pixels[q + 1], pixels[q + 2]];
        }
        this.buffer[p] = r;
        this.buffer[p + 1] = g;
        this.buffer[p + 2] = b;
        this.buffer[p + 3] = 255;
      }
    }
  }

  #resize() {
    this.#base = null;
    const n = this.caps.width * this.caps.height * 4;
    this.buffer = new Uint8ClampedArray(n).fill(255); // what's been received
    this.visible = new Uint8ClampedArray(n).fill(255); // what's on the panel
  }

  #fail(code, value) {
    this.#rx = null;
    this.#error(code, value);
  }

  #error(code, value = 0) {
    this.#log(`Error ${code}`);
    this.#status(STATUS.ERROR, code, value);
  }

  #status(event, code, value) {
    const link = this.#link;
    const bytes = encodeStatus(event, code, value);
    if (link) setTimeout(() => link.status(bytes), 0); // notifications arrive asynchronously
  }

  #event(bytes) {
    // Like a BLE display: only to a subscribed host, never queued.
    const link = this.#link;
    if (link && this.version >= 2) setTimeout(() => link.event(bytes), 0);
  }

  #log(text) {
    this.dispatchEvent(new CustomEvent('log', { detail: text }));
  }
}

/** Connects a Blit host to a SimDisplay in the same page. */
export class SimTransport {
  #connected = false;
  constructor(display) {
    this.display = display;
  }

  get name() {
    return this.display.caps.name;
  }

  get connected() {
    return this.#connected;
  }

  async open(handlers) {
    const dv = (bytes) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    this.display.attach({
      status: (bytes) => handlers.onStatus(dv(bytes)),
      event: (bytes) => handlers.onEvent(dv(bytes)),
      disconnect: () => this.#drop(handlers),
    });
    this.#connected = true;
    this.handlers = handlers;
    return { hasEvents: this.display.version >= 2 };
  }

  async readInfo() {
    const bytes = this.display.info();
    return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  async control(bytes) {
    this.#check();
    this.display.control(Uint8Array.from(bytes));
  }

  async data(bytes) {
    this.#check();
    this.display.data(Uint8Array.from(bytes));
  }

  close() {
    if (this.#connected) this.#drop(this.handlers);
  }

  #check() {
    if (!this.#connected) throw new Error('Not connected');
  }

  #drop(handlers) {
    this.#connected = false;
    this.display.detach();
    handlers?.onDisconnect();
  }
}
