// blit host: send frames to a blit display and hear its buttons.
// Spec: ../PROTOCOL.md. Speaks v2, and v1 to displays that only know v1 (the
// X4 firmware today).
//
//   import { Blit } from './blit.js';
//   const blit = new Blit();
//   await blit.connect();                          // Web Bluetooth; needs a user gesture
//   await blit.sendImage(imgOrCanvas, { dither: 'atkinson' });
//   await blit.sendElement(document.querySelector('#screen'));
//   blit.addEventListener('key', (e) => console.log(e.detail.name, e.detail.action));
//
// Events (EventTarget): 'connected' (detail: caps), 'disconnected', 'reconnecting', 'caps'
// (detail: caps), 'key' ({ key, name, action }), 'pointer' ({ action, x, y }),
// 'power' ({ percent, charging, external }), 'progress' ({ sent, total }),
// 'status' ({ event, code, value }).

import {
  CANCEL, CHARACTERISTIC, COMMIT, ENCODING, ERROR, ERROR_TEXT, FORMAT, FORMAT_INFO, SERVICE, STATUS,
  crc32, encodeBegin, encodeData, encodeHello, frameBytes, packbits, parseCaps, parseEvent, parseStatus, rowBytes,
} from './protocol.js';
import { captureElement } from './dom-capture.js';
import { rasterize } from './raster.js';

export * from './protocol.js';
export { rasterize, rasterizeRgba, fitRect, sourceSize } from './raster.js';
export { captureElement } from './dom-capture.js';

const STATUS_TIMEOUT_MS = 20000;
const OFFSET_BYTES = 4;
const FALLBACK_CHUNK = 20 - 3 - OFFSET_BYTES; // default ATT MTU
const FALLBACK_WINDOW = 8;
const BUSY_RETRIES = 5;

export class BlitError extends Error {
  constructor(code, value) {
    super(`Display rejected the frame: ${ERROR_TEXT[code] ?? `error ${code}`}`);
    this.code = code;
    this.value = value;
  }
}

/**
 * True if `err` from requesting a device only means the user closed the
 * chooser. Chrome also uses NotFoundError for real failures (no adapter,
 * Bluetooth blocked for the site), and those should be shown.
 */
export function isChooserCancelled(err) {
  return err?.name === 'NotFoundError' && /cancel/i.test(err.message);
}

/** Transport over Web Bluetooth GATT (Chrome / Edge, secure contexts). */
export class WebBluetoothTransport {
  #chars = null;
  #handlers = null;

  constructor(device) {
    this.device = device;
    device.addEventListener('gattserverdisconnected', () => {
      this.#chars = null;
      this.#handlers?.onDisconnect();
    });
  }

  /** Show the browser's device chooser (needs a user gesture). */
  static async request() {
    const device = await navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE] }] });
    return new WebBluetoothTransport(device);
  }

  get name() {
    return this.device.name ?? null;
  }

  get connected() {
    return !!this.device.gatt?.connected && !!this.#chars;
  }

  async open(handlers) {
    this.#handlers = handlers;
    const server = await this.device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE);
    const chars = {};
    for (const key of ['info', 'control', 'data', 'status']) chars[key] = await service.getCharacteristic(CHARACTERISTIC[key]);
    try {
      chars.event = await service.getCharacteristic(CHARACTERISTIC.event);
    } catch {
      chars.event = null; // v1 display
    }
    chars.status.addEventListener('characteristicvaluechanged', this.#onStatus);
    await chars.status.startNotifications();
    if (chars.event) {
      chars.event.addEventListener('characteristicvaluechanged', this.#onEvent);
      await chars.event.startNotifications();
    }
    this.#chars = chars;
    return { hasEvents: !!chars.event };
  }

  readInfo() {
    return this.#chars.info.readValue();
  }

  control(bytes) {
    return this.#chars.control.writeValueWithResponse(bytes);
  }

  data(bytes) {
    return this.#chars.data.writeValueWithoutResponse(bytes);
  }

  close() {
    this.device.gatt?.disconnect();
  }

  #onStatus = (e) => this.#handlers?.onStatus(e.target.value);
  #onEvent = (e) => this.#handlers?.onEvent(e.target.value);
}

export class Blit extends EventTarget {
  #transport = null;
  #hasEvents = false;
  #waiters = [];
  #backlog = []; // statuses that arrived before anyone waited for them
  #queue = Promise.resolve();
  #last = null; // last frame sent: { format, width, height, pixels }
  caps = null;

  /** Options: hostName (shown on displays that care, via hello). */
  constructor({ hostName = 'blit.js' } = {}) {
    super();
    this.hostName = hostName;
  }

  static get supported() {
    return typeof navigator !== 'undefined' && !!navigator.bluetooth;
  }

  get connected() {
    return !!this.#transport?.connected;
  }

  get deviceName() {
    return this.#transport?.name ?? this.caps?.name ?? null;
  }

  /** v1 name for caps. */
  get info() {
    return this.caps;
  }

  /** Pick a display (browser chooser) and connect. Call from a user gesture. */
  async connect() {
    await this.connectTransport(await WebBluetoothTransport.request());
  }

  /** Connect over any transport (WebBluetoothTransport, SimTransport, ...). */
  async connectTransport(transport) {
    if (this.#transport && this.#transport !== transport) this.#transport.close();
    this.#transport = transport;
    this.#last = null;
    await this.#open();
  }

  /** Reconnect to the display picked earlier, retrying while it's asleep or out of range. */
  async reconnect({ timeoutMs = 30000, retryMs = 1500 } = {}) {
    if (!this.#transport) throw new Error('No display: call connect() first');
    this.dispatchEvent(new Event('reconnecting'));
    const deadline = performance.now() + timeoutMs;
    for (;;) {
      try {
        await this.#open();
        return;
      } catch (err) {
        if (performance.now() + retryMs > deadline) throw err;
        await sleep(retryMs);
      }
    }
  }

  /**
   * Disconnect and forget the display: nothing reconnects until connect() or
   * connectTransport() is called again. (A link the display drops on its own,
   * e.g. to sleep between frames, keeps the display, and the next send
   * reconnects.)
   */
  disconnect() {
    const transport = this.#transport;
    this.#transport = null;
    this.caps = null;
    this.#last = null;
    transport?.close();
  }

  /** Read caps fresh from Info. */
  async readCaps() {
    this.caps = parseCaps(await this.#transport.readInfo());
    return this.caps;
  }

  /** The first format in the display's preference list that this library can render. */
  preferredFormat() {
    return this.caps?.formats.find((f) => FORMAT_INFO[f]) ?? FORMAT.MONO1;
  }

  /**
   * Rasterize any drawable (img, canvas, video, ImageBitmap) to the display's
   * frame area (or opts.width × opts.height) in its preferred format (or
   * opts.format) and send it. Options are those of rasterize() and
   * sendFrame(). opts.onRaster(frame) sees the rasterized frame first.
   */
  async sendImage(source, opts = {}) {
    await this.#ensureConnected(opts);
    const caps = this.caps.version < 2 ? await this.readCaps() : this.caps; // v1 has no caps events
    const format = opts.format ?? this.preferredFormat();
    const frame = rasterize(source, { width: caps.width, height: caps.height, ...opts, format });
    opts.onRaster?.(frame);
    return this.sendFrame(frame.pixels, { ...opts, format, width: frame.width, height: frame.height });
  }

  /**
   * Capture a DOM element in place (see dom-capture.js) and send it. Size the
   * element to the display's frame area (caps.width × caps.height CSS px) for
   * a pixel-exact frame; otherwise it's fitted (opts.fit, default 'contain').
   * UI-like content usually looks best with opts.dither = 'threshold'.
   */
  async sendElement(element, opts = {}) {
    const canvas = await captureElement(element, { scale: opts.captureScale ?? 1 });
    return this.sendImage(canvas, { fit: 'contain', ...opts });
  }

  /**
   * Send packed pixels. Resolves with { sleepSeconds, skipped, region, bytes }
   * once the display has shown them; sleepSeconds > 0 means it's about to
   * disconnect and sleep until just before `nextFrameSeconds`.
   *
   * Options: format (default mono1), width, height, persist, name,
   * nextFrameSeconds, refresh, signal (AbortSignal), and:
   *   skipUnchanged  don't send a frame identical to the last one sent
   *   regions        send only the changed rectangle when the display takes regions
   *   encoding       'auto' (default: PackBits when supported and smaller), or a code
   *   x, y, region, hold   send an explicit region (v2)
   *
   * Frames are sent one at a time, in call order.
   */
  sendFrame(pixels, opts) {
    const run = () => this.#send(pixels, opts);
    const result = this.#queue.then(run, run);
    this.#queue = result.catch(() => {});
    return result;
  }

  async #send(pixels, opts = {}) {
    const { format = FORMAT.MONO1, width, height, skipUnchanged = false, regions = false } = opts;
    await this.#ensureConnected(opts);
    const caps = this.caps;
    const v1 = caps.version < 2;
    if (v1 && format !== FORMAT.MONO1) throw new Error('This display only takes mono1 (protocol v1)');
    const expected = frameBytes(format, width, height);
    if (pixels.length !== expected) throw new Error(`Expected ${expected} bytes for that frame, got ${pixels.length}`);

    // Full frame, unless the caller sent a region or asked for automatic ones.
    let region = opts.region ? { x: opts.x ?? 0, y: opts.y ?? 0, width, height } : null;
    let payload = pixels;
    const last = this.#last;
    const sameShape = !opts.region && last && last.format === format && last.width === width && last.height === height;
    if (sameShape && (skipUnchanged || regions)) {
      const changed = changedRect(last.pixels, pixels, format, width, height, caps.regionAlign || 8);
      if (!changed && skipUnchanged) return { sleepSeconds: 0, skipped: true };
      // A region only pays off when it's clearly smaller than the frame.
      if (changed && regions && !v1 && caps.regionAlign && changed.width * changed.height < width * height * 0.6) {
        region = changed;
        payload = cropPixels(pixels, format, width, changed);
      }
    }
    if (!opts.region) this.#last = { format, width, height, pixels: pixels.slice() };

    let encoding = ENCODING.NONE;
    let wire = payload;
    const wantEncoding = opts.encoding ?? 'auto';
    if (!v1 && wantEncoding !== ENCODING.NONE && caps.encodings.includes(ENCODING.PACKBITS)) {
      const packed = packbits(payload);
      if (wantEncoding === ENCODING.PACKBITS || packed.length < payload.length * 0.9) {
        encoding = ENCODING.PACKBITS;
        wire = packed;
      }
    }
    if (caps.maxBytes && wire.length > caps.maxBytes) {
      this.#last = null;
      throw new Error(`Frame is ${wire.length} bytes; the display takes at most ${caps.maxBytes}`);
    }

    const header = encodeBegin({
      version: v1 ? 1 : 2,
      format,
      encoding,
      persist: !!opts.persist,
      region: !!region,
      hold: !!opts.hold,
      refresh: opts.refresh ?? 0,
      width: region?.width ?? width,
      height: region?.height ?? height,
      x: region?.x ?? 0,
      y: region?.y ?? 0,
      byteLength: wire.length,
      crc: crc32(wire),
      nextFrameSeconds: opts.nextFrameSeconds ?? 0,
      name: opts.name ?? '',
    });

    for (let attempt = 0; ; attempt++) {
      try {
        const sleepSeconds = await this.#transfer(header, wire, opts.signal);
        return { sleepSeconds, skipped: false, region, bytes: wire.length, encoding };
      } catch (err) {
        if (err instanceof BlitError && err.code === ERROR.BAD_REGION && region && !opts.region) {
          // The display doesn't hold the frame we diffed against (it restarted,
          // reconnected, or its area changed): send the whole frame instead.
          this.#last = null;
          return this.#send(pixels, opts);
        }
        if (!(err instanceof BlitError) || err.code !== ERROR.BUSY || attempt >= BUSY_RETRIES) {
          this.#last = null; // the display may not have it: don't diff against it
          throw err;
        }
        await sleep(caps.refreshMs || 500);
      }
    }
  }

  async #transfer(header, wire, signal) {
    const t = this.#transport;
    this.#backlog = [];
    await t.control(header);
    const ready = await this.#nextStatus(STATUS.READY);
    const chunk = ready.value || this.caps.chunk || FALLBACK_CHUNK;
    // Writes without response have no flow control: the display acks every
    // `window` writes, and we wait for each ack or chunks get dropped.
    const window = this.caps.window || FALLBACK_WINDOW;

    const writes = Math.ceil(wire.length / chunk);
    for (let i = 0; i < writes; i++) {
      if (signal?.aborted) {
        await t.control(CANCEL);
        throw signal.reason ?? new DOMException('Aborted', 'AbortError');
      }
      const offset = i * chunk;
      await t.data(encodeData(offset, wire.subarray(offset, offset + chunk)));
      if ((i + 1) % window === 0 || i === writes - 1) {
        const ack = await this.#nextStatus(STATUS.ACK);
        this.dispatchEvent(new CustomEvent('progress', { detail: { sent: ack.value, total: wire.length } }));
      }
    }

    await t.control(COMMIT);
    const done = await this.#nextStatus(STATUS.DONE);
    return done.value;
  }

  async #ensureConnected({ reconnectTimeoutMs = 30000 } = {}) {
    if (this.connected) return;
    await this.reconnect({ timeoutMs: reconnectTimeoutMs });
  }

  async #open() {
    const { hasEvents } = await this.#transport.open({
      onStatus: this.#onStatus,
      onEvent: this.#onEvent,
      onDisconnect: this.#onDisconnected,
    });
    this.#hasEvents = hasEvents;
    await this.readCaps();
    if (this.caps.version >= 2 && hasEvents) {
      await this.#transport.control(encodeHello({ name: this.hostName }));
    }
    this.dispatchEvent(new CustomEvent('connected', { detail: this.caps }));
  }

  // Resolves with the next status if it's `expected`; rejects on an error
  // status (or anything else unexpected).
  async #nextStatus(expected) {
    const status = this.#backlog.length ? this.#backlog.shift() : await this.#waitStatus();
    if (status.event === STATUS.ERROR) throw new BlitError(status.code, status.value);
    if (status.event !== expected) throw new Error(`Unexpected status ${status.event} (wanted ${expected})`);
    return status;
  }

  #waitStatus() {
    return new Promise((resolve, reject) => {
      const waiter = { resolve, reject };
      waiter.timer = setTimeout(() => {
        this.#waiters = this.#waiters.filter((w) => w !== waiter);
        reject(new Error('Timed out waiting for the display'));
      }, STATUS_TIMEOUT_MS);
      this.#waiters.push(waiter);
    });
  }

  #onStatus = (data) => {
    const status = parseStatus(data);
    this.dispatchEvent(new CustomEvent('status', { detail: status }));
    const waiter = this.#waiters.shift();
    if (!waiter) return this.#backlog.push(status);
    clearTimeout(waiter.timer);
    waiter.resolve(status);
  };

  #onEvent = async (data) => {
    const event = parseEvent(data);
    if (event.type === 'caps') {
      try {
        this.caps = event.caps ?? (await this.readCaps());
      } catch {
        return; // disconnected mid-read
      }
      this.dispatchEvent(new CustomEvent('caps', { detail: this.caps }));
    } else if (event.type === 'key' || event.type === 'pointer' || event.type === 'power') {
      if (event.type === 'power' && this.caps) this.caps.battery = { percent: event.percent, charging: event.charging, external: event.external };
      this.dispatchEvent(new CustomEvent(event.type, { detail: event }));
    }
  };

  #onDisconnected = () => {
    for (const w of this.#waiters.splice(0)) {
      clearTimeout(w.timer);
      w.reject(new Error('Disconnected'));
    }
    this.dispatchEvent(new Event('disconnected'));
  };
}

/**
 * Bounding rectangle of the pixels that differ between two frames of the same
 * shape, widened so x and width are multiples of `align` (and whole bytes).
 * null if nothing changed.
 */
export function changedRect(before, after, format, width, height, align = 8) {
  const stride = rowBytes(format, width);
  const bpp = FORMAT_INFO[format].bpp;
  let top = -1;
  let bottom = -1;
  let left = stride;
  let right = -1;
  for (let y = 0; y < height; y++) {
    const row = y * stride;
    let rowLeft = -1;
    let rowRight = -1;
    for (let b = 0; b < stride; b++) {
      if (before[row + b] !== after[row + b]) {
        if (rowLeft < 0) rowLeft = b;
        rowRight = b;
      }
    }
    if (rowLeft < 0) continue;
    if (top < 0) top = y;
    bottom = y;
    left = Math.min(left, rowLeft);
    right = Math.max(right, rowRight);
  }
  if (top < 0) return null;
  // Byte columns -> pixels, then out to the alignment (which must also keep
  // x on a byte boundary for sub-byte formats).
  let a = Math.max(1, align);
  while ((a * bpp) % 8) a *= 2;
  const x0 = Math.floor(Math.floor((left * 8) / bpp) / a) * a;
  const x1 = Math.min(width, Math.ceil(Math.ceil(((right + 1) * 8) / bpp) / a) * a);
  return { x: x0, y: top, width: x1 - x0, height: bottom - top + 1 };
}

/** The packed pixels of `rect` (x on a byte boundary) out of a full frame. */
export function cropPixels(pixels, format, width, rect) {
  const stride = rowBytes(format, width);
  const bpp = FORMAT_INFO[format].bpp;
  const outStride = rowBytes(format, rect.width);
  const from = (rect.x * bpp) / 8;
  const out = new Uint8Array(outStride * rect.height);
  for (let y = 0; y < rect.height; y++) {
    const src = (rect.y + y) * stride + from;
    out.set(pixels.subarray(src, src + outStride), y * outStride);
  }
  return out;
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}
