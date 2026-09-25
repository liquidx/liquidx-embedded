// ble-cast: push 1-bit frames to an Xteink X4 running the shell firmware's
// Bluetooth app, over Web Bluetooth (Chrome / Edge, secure context).
//
//   import { BleCast } from './lib/ble-cast.js';
//   const cast = new BleCast();
//   await cast.connect();                      // needs a user gesture
//   await cast.sendImage(imgOrCanvas, { persist: true, name: 'photo' });
//   await cast.sendElement(document.querySelector('#screen'));
//
// Events (EventTarget): 'connected', 'disconnected', 'progress' (detail:
// { sent, total }), 'status' (detail: { event, code, value }).

import {
  CHARACTERISTIC, ERROR_TEXT, EVENT, OP_CANCEL, OP_COMMIT, SERVICE, crc32, encodeBegin, encodeData, parseStatus,
} from './protocol.js';
import { captureElement } from './dom-capture.js';
import { rasterize } from './raster.js';

export { rasterize } from './raster.js';
export { captureElement } from './dom-capture.js';

const STATUS_TIMEOUT_MS = 20000;
const OFFSET_BYTES = 4;
const FALLBACK_CHUNK = 20 - 3 - OFFSET_BYTES; // default ATT MTU
const FALLBACK_WINDOW = 8;

export class CastError extends Error {
  constructor(code, value) {
    super(`Device rejected the frame: ${ERROR_TEXT[code] ?? `error ${code}`}`);
    this.code = code;
    this.value = value;
  }
}

export class BleCast extends EventTarget {
  #device = null;
  #chars = null;
  #waiters = [];
  #backlog = []; // statuses that arrived before anyone waited for them
  #queue = Promise.resolve();
  info = null;

  static get supported() {
    return typeof navigator !== 'undefined' && !!navigator.bluetooth;
  }

  get connected() {
    return !!this.#device?.gatt?.connected && !!this.#chars;
  }

  get deviceName() {
    return this.#device?.name ?? this.info?.name ?? null;
  }

  /** Pick a device (browser chooser) and connect. Call from a user gesture. */
  async connect() {
    const device = await navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE] }] });
    if (this.#device !== device) {
      this.#device?.removeEventListener('gattserverdisconnected', this.#onDisconnected);
      this.#device = device;
      device.addEventListener('gattserverdisconnected', this.#onDisconnected);
    }
    await this.#open();
  }

  /** Reconnect to the device picked earlier, retrying while it's asleep or out of range. */
  async reconnect({ timeoutMs = 30000, retryMs = 1500 } = {}) {
    if (!this.#device) throw new Error('No device: call connect() first');
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

  disconnect() {
    this.#device?.gatt?.disconnect();
  }

  /** Read the device's Info characteristic (frame area, chunk size, ...). */
  async readInfo() {
    const value = await this.#chars.info.readValue();
    this.info = JSON.parse(new TextDecoder().decode(value));
    return this.info;
  }

  /**
   * Rasterize any drawable (img, canvas, video, ImageBitmap) to the device's
   * current frame area (or opts.width × opts.height) and send it. Options are
   * those of rasterize() and sendFrame(). Resolves with sendFrame()'s result.
   */
  async sendImage(source, opts = {}) {
    await this.#ensureConnected(opts);
    const info = await this.readInfo();
    const frame = rasterize(source, { width: opts.width ?? info.w, height: opts.height ?? info.h, ...opts });
    opts.onRaster?.(frame);
    return this.sendFrame(frame.bits, { ...opts, width: frame.width, height: frame.height });
  }

  /**
   * Capture a DOM element in place (see dom-capture.js) and send it. Size the
   * element to the device's frame area (info.w × info.h CSS px) for a
   * pixel-exact frame; otherwise it's fitted (opts.fit, default 'contain').
   * UI-like content usually looks best with opts.dither = 'threshold'.
   */
  async sendElement(element, opts = {}) {
    const canvas = await captureElement(element, { scale: opts.captureScale ?? 1 });
    return this.sendImage(canvas, { fit: 'contain', ...opts });
  }

  /**
   * Send a raw1 frame. Resolves with { sleepSeconds } once the device has shown
   * it; sleepSeconds > 0 means it's about to disconnect and sleep until just
   * before `nextFrameSeconds`. Frames are sent one at a time, in call order.
   */
  sendFrame(bits, opts) {
    const run = () => this.#send(bits, opts);
    const result = this.#queue.then(run, run);
    this.#queue = result.catch(() => {});
    return result;
  }

  async #send(bits, { width, height, persist = false, name = '', nextFrameSeconds = 0, signal, reconnectTimeoutMs } = {}) {
    await this.#ensureConnected({ reconnectTimeoutMs });
    const { control, data } = this.#chars;

    this.#backlog = [];
    await control.writeValueWithResponse(
      encodeBegin({ persist, width, height, byteLength: bits.length, crc: crc32(bits), nextFrameSeconds, name }),
    );
    const ready = await this.#nextStatus(EVENT.READY);
    const chunk = ready.value || this.info?.chunk || FALLBACK_CHUNK;
    // Writes without response have no flow control: the device acks every
    // `window` writes, and we wait for each ack or chunks get dropped.
    const window = this.info?.window || FALLBACK_WINDOW;

    const writes = Math.ceil(bits.length / chunk);
    for (let i = 0; i < writes; i++) {
      if (signal?.aborted) {
        await control.writeValueWithResponse(OP_CANCEL);
        throw signal.reason ?? new DOMException('Aborted', 'AbortError');
      }
      const offset = i * chunk;
      await data.writeValueWithoutResponse(encodeData(offset, bits.subarray(offset, offset + chunk)));
      if ((i + 1) % window === 0 || i === writes - 1) {
        const ack = await this.#nextStatus(EVENT.ACK);
        this.dispatchEvent(new CustomEvent('progress', { detail: { sent: ack.value, total: bits.length } }));
      }
    }

    await control.writeValueWithResponse(OP_COMMIT);
    const done = await this.#nextStatus(EVENT.DONE);
    return { sleepSeconds: done.value };
  }

  async #ensureConnected({ reconnectTimeoutMs = 30000 } = {}) {
    if (this.connected) return;
    await this.reconnect({ timeoutMs: reconnectTimeoutMs });
  }

  async #open() {
    const server = await this.#device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE);
    const chars = {};
    for (const [key, uuid] of Object.entries(CHARACTERISTIC)) chars[key] = await service.getCharacteristic(uuid);
    chars.status.addEventListener('characteristicvaluechanged', this.#onStatus);
    await chars.status.startNotifications();
    this.#chars = chars;
    await this.readInfo();
    this.dispatchEvent(new CustomEvent('connected', { detail: this.info }));
  }

  // Resolves with the next status if it's `expected`; rejects on an error
  // status (or anything else unexpected).
  async #nextStatus(expected) {
    const status = this.#backlog.length ? this.#backlog.shift() : await this.#waitStatus();
    if (status.event === EVENT.ERROR) throw new CastError(status.code, status.value);
    if (status.event !== expected) throw new Error(`Unexpected status ${status.event} (wanted ${expected})`);
    return status;
  }

  #waitStatus() {
    return new Promise((resolve, reject) => {
      const waiter = { resolve, reject };
      waiter.timer = setTimeout(() => {
        this.#waiters = this.#waiters.filter((w) => w !== waiter);
        reject(new Error('Timed out waiting for the device'));
      }, STATUS_TIMEOUT_MS);
      this.#waiters.push(waiter);
    });
  }

  #onStatus = (event) => {
    const status = parseStatus(event.target.value);
    this.dispatchEvent(new CustomEvent('status', { detail: status }));
    const waiter = this.#waiters.shift();
    if (!waiter) return this.#backlog.push(status);
    clearTimeout(waiter.timer);
    waiter.resolve(status);
  };

  #onDisconnected = () => {
    this.#chars = null;
    for (const w of this.#waiters.splice(0)) {
      clearTimeout(w.timer);
      w.reject(new Error('Disconnected'));
    }
    this.dispatchEvent(new Event('disconnected'));
  };
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}
