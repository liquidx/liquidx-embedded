// The blit wire format: UUIDs, message encoding and parsing, CRC-32, PackBits.
// Spec: ../PROTOCOL.md. Shared by hosts (blit.js) and the simulated display
// (sim-display.js), so it has no DOM or Bluetooth dependencies.

export const SERVICE = 'b1ec0000-5f3a-4e62-9a47-0c3d8e5f2a10';
export const CHARACTERISTIC = {
  info: 'b1ec0001-5f3a-4e62-9a47-0c3d8e5f2a10',
  control: 'b1ec0002-5f3a-4e62-9a47-0c3d8e5f2a10',
  data: 'b1ec0003-5f3a-4e62-9a47-0c3d8e5f2a10',
  status: 'b1ec0004-5f3a-4e62-9a47-0c3d8e5f2a10',
  event: 'b1ec0005-5f3a-4e62-9a47-0c3d8e5f2a10', // v2
};

export const VERSION = 2;

export const FORMAT = { MONO1: 1, GRAY2: 2, GRAY4: 3, GRAY8: 4, RGB565: 16, RGB888: 24 };
export const FORMAT_INFO = {
  1: { name: 'mono1', bpp: 1, levels: 2 },
  2: { name: 'gray2', bpp: 2, levels: 4 },
  3: { name: 'gray4', bpp: 4, levels: 16 },
  4: { name: 'gray8', bpp: 8, levels: 256 },
  16: { name: 'rgb565', bpp: 16 },
  24: { name: 'rgb888', bpp: 24 },
};

export const ENCODING = { NONE: 0, PACKBITS: 1 };

export const FLAG = { PERSIST: 0x01, REGION: 0x02, HOLD: 0x04 };
export const REFRESH = { AUTO: 0, FAST: 1, FULL: 2 };
export const OP = { BEGIN: 0x01, COMMIT: 0x02, CANCEL: 0x03, HELLO: 0x04 };
export const HELLO_FLAG = { KEYS: 0x01, POINTER: 0x02 };

// Status characteristic.
export const STATUS = { READY: 1, DONE: 2, ERROR: 3, ACK: 4 };
export const ERROR = {
  BAD_HEADER: 1, UNSUPPORTED: 2, TOO_LARGE: 3, BUSY: 4, OUT_OF_ORDER: 5,
  INCOMPLETE: 6, BAD_CRC: 7, SAVE_FAILED: 8, BAD_REGION: 9, DECODE: 10,
};
export const ERROR_TEXT = {
  1: 'bad header',
  2: 'unsupported version, format or encoding',
  3: 'payload too large or wrong size',
  4: 'display busy showing the previous frame',
  5: 'data out of order',
  6: 'commit before all data arrived',
  7: 'CRC mismatch',
  8: "couldn't save the frame (it's still shown)",
  9: 'region outside the frame area or misaligned',
  10: "payload didn't decode to the frame size",
};

// Event characteristic (v2).
export const EVENT = { CAPS: 0x01, KEY: 0x02, POINTER: 0x03, POWER: 0x04 };
export const KEY = {
  UP: 1, DOWN: 2, LEFT: 3, RIGHT: 4, SELECT: 5, BACK: 6, MENU: 7, HOME: 8, PAGE_NEXT: 9, PAGE_PREV: 10,
  F1: 16, // F1..F16 = 16..31
};
export const KEY_NAME = {
  1: 'up', 2: 'down', 3: 'left', 4: 'right', 5: 'select', 6: 'back', 7: 'menu', 8: 'home', 9: 'pageNext', 10: 'pagePrev',
};
for (let i = 0; i < 16; i++) KEY_NAME[16 + i] = `f${i + 1}`;
export const KEY_ACTION = { PRESS: 1, LONG: 2, DOWN: 3, UP: 4, REPEAT: 5 };
export const KEY_ACTION_NAME = { 1: 'press', 2: 'long', 3: 'down', 4: 'up', 5: 'repeat' };
export const POINTER_ACTION = { DOWN: 1, MOVE: 2, UP: 3, TAP: 4 };
export const POINTER_ACTION_NAME = { 1: 'down', 2: 'move', 3: 'up', 4: 'tap' };

// Caps TLV tags and feature bits.
export const TAG = {
  NAME: 0x01, PANEL: 0x02, AREA: 0x03, FORMATS: 0x04, ENCODINGS: 0x05, LIMITS: 0x06,
  FEATURES: 0x07, REGIONS: 0x08, KEYS: 0x09, PACING: 0x0a, POWER: 0x0b,
};
export const FEATURE = { PERSIST: 1 << 0, FRAME_SLEEP: 1 << 1, FAST_REFRESH: 1 << 2, POINTER: 1 << 3 };

const V1_HEADER_BYTES = 21;
const V2_HEADER_BYTES = 29;
const MAX_NAME = 64;
const MAX_HOST_NAME = 32;

// --- pixel layout ------------------------------------------------------------

/** Bytes per row: rows are padded to a whole byte. */
export function rowBytes(format, width) {
  const info = FORMAT_INFO[format];
  if (!info) throw new Error(`Unknown format ${format}`);
  return Math.ceil((width * info.bpp) / 8);
}

/** Unencoded payload bytes for a frame of this size and format. */
export function frameBytes(format, width, height) {
  return rowBytes(format, width) * height;
}

// --- host -> display -----------------------------------------------------------

function asciiBytes(text, max) {
  // ASCII only; anything else becomes '_' (the display sanitises further).
  return Uint8Array.from([...text].slice(0, max), (c) => (c.charCodeAt(0) < 0x80 ? c.charCodeAt(0) : 0x5f));
}

/**
 * Control write that starts a frame. `version` 1 writes the 21-byte v1 header
 * (mono1, full frames, no encoding); 2 writes the v2 header.
 */
export function encodeBegin({
  version = VERSION, format = FORMAT.MONO1, encoding = ENCODING.NONE, persist = false, region = false, hold = false,
  refresh = REFRESH.AUTO, width, height, x = 0, y = 0, byteLength, crc, nextFrameSeconds = 0, name = '',
}) {
  const nameBytes = asciiBytes(name, MAX_NAME);
  const next = Math.max(0, Math.round(nextFrameSeconds));
  if (version === 1) {
    const out = new Uint8Array(V1_HEADER_BYTES + nameBytes.length);
    const v = new DataView(out.buffer);
    v.setUint8(0, OP.BEGIN);
    v.setUint8(1, 1);
    v.setUint8(2, format);
    v.setUint8(3, persist ? FLAG.PERSIST : 0);
    v.setUint16(4, width, true);
    v.setUint16(6, height, true);
    v.setUint32(8, byteLength, true);
    v.setUint32(12, crc, true);
    v.setUint32(16, next, true);
    v.setUint8(20, nameBytes.length);
    out.set(nameBytes, V1_HEADER_BYTES);
    return out;
  }
  const out = new Uint8Array(V2_HEADER_BYTES + nameBytes.length);
  const v = new DataView(out.buffer);
  v.setUint8(0, OP.BEGIN);
  v.setUint8(1, 2);
  v.setUint8(2, format);
  v.setUint8(3, encoding);
  v.setUint8(4, (persist ? FLAG.PERSIST : 0) | (region ? FLAG.REGION : 0) | (hold ? FLAG.HOLD : 0));
  v.setUint8(5, refresh);
  v.setUint16(6, width, true);
  v.setUint16(8, height, true);
  v.setUint16(10, x, true);
  v.setUint16(12, y, true);
  v.setUint32(16, byteLength, true);
  v.setUint32(20, crc, true);
  v.setUint32(24, next, true);
  v.setUint8(28, nameBytes.length);
  out.set(nameBytes, V2_HEADER_BYTES);
  return out;
}

/** Parse a begin header (either version). Used by displays; null if malformed. */
export function parseBegin(bytes) {
  if (bytes.length < 2 || bytes[0] !== OP.BEGIN) return null;
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const text = (from, n) => String.fromCharCode(...bytes.subarray(from, from + n));
  if (bytes[1] === 1) {
    if (bytes.length < V1_HEADER_BYTES || bytes.length < V1_HEADER_BYTES + bytes[20]) return null;
    return {
      version: 1, format: bytes[2], encoding: 0, persist: !!(bytes[3] & FLAG.PERSIST), region: false, hold: false,
      refresh: 0, width: v.getUint16(4, true), height: v.getUint16(6, true), x: 0, y: 0,
      byteLength: v.getUint32(8, true), crc: v.getUint32(12, true), nextFrameSeconds: v.getUint32(16, true),
      name: text(V1_HEADER_BYTES, Math.min(bytes[20], MAX_NAME)),
    };
  }
  if (bytes.length < V2_HEADER_BYTES || bytes.length < V2_HEADER_BYTES + bytes[28]) return null;
  return {
    version: bytes[1], format: bytes[2], encoding: bytes[3], persist: !!(bytes[4] & FLAG.PERSIST),
    region: !!(bytes[4] & FLAG.REGION), hold: !!(bytes[4] & FLAG.HOLD), refresh: bytes[5],
    width: v.getUint16(6, true), height: v.getUint16(8, true), x: v.getUint16(10, true), y: v.getUint16(12, true),
    byteLength: v.getUint32(16, true), crc: v.getUint32(20, true), nextFrameSeconds: v.getUint32(24, true),
    name: text(V2_HEADER_BYTES, Math.min(bytes[28], MAX_NAME)),
  };
}

export const COMMIT = Uint8Array.of(OP.COMMIT);
export const CANCEL = Uint8Array.of(OP.CANCEL);

/** Control write introducing the host (v2). */
export function encodeHello({ version = VERSION, keys = true, pointer = true, name = '' } = {}) {
  let nameBytes = new TextEncoder().encode(name);
  if (nameBytes.length > MAX_HOST_NAME) nameBytes = nameBytes.subarray(0, MAX_HOST_NAME);
  const out = new Uint8Array(4 + nameBytes.length);
  out[0] = OP.HELLO;
  out[1] = version;
  out[2] = (keys ? HELLO_FLAG.KEYS : 0) | (pointer ? HELLO_FLAG.POINTER : 0);
  out[3] = nameBytes.length;
  out.set(nameBytes, 4);
  return out;
}

/** Data write: u32 offset + payload bytes. */
export function encodeData(offset, bytes) {
  const out = new Uint8Array(4 + bytes.length);
  new DataView(out.buffer).setUint32(0, offset, true);
  out.set(bytes, 4);
  return out;
}

// --- display -> host -----------------------------------------------------------

function view(bytes) {
  if (bytes instanceof DataView) return bytes;
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

function bytesOf(dv) {
  return new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
}

export function encodeStatus(event, code = 0, value = 0) {
  const out = new Uint8Array(6);
  const v = new DataView(out.buffer);
  v.setUint8(0, event);
  v.setUint8(1, code);
  v.setUint32(2, value >>> 0, true);
  return out;
}

/** Status notification -> { event, code, value }. */
export function parseStatus(data) {
  const v = view(data);
  return { event: v.getUint8(0), code: v.getUint8(1), value: v.getUint32(2, true) };
}

/** Caps with every default filled in. */
export function defaultCaps() {
  return {
    version: 1,
    name: '',
    panel: null,
    width: 0,
    height: 0,
    formats: [FORMAT.MONO1],
    encodings: [ENCODING.NONE],
    maxBytes: 0,
    chunk: 0,
    window: 0,
    features: { persist: false, frameSleep: false, fastRefresh: false, pointer: false },
    regionAlign: 0, // 0 = no region updates
    keys: [],
    minIntervalMs: 0,
    refreshMs: 0,
    battery: null, // { percent, charging, external }
  };
}

/**
 * Parse Info (or a caps event body) into one normalised structure, whichever
 * version the display speaks: v1 JSON or v2 TLV.
 */
export function parseCaps(data) {
  const bytes = bytesOf(view(data));
  const caps = defaultCaps();
  if (bytes[0] === 0x7b) {
    const j = JSON.parse(new TextDecoder().decode(bytes));
    caps.version = j.v ?? 1;
    caps.name = j.name ?? '';
    caps.width = j.w;
    caps.height = j.h;
    caps.panel = { width: j.fullW ?? j.w, height: j.fullH ?? j.h };
    caps.formats = j.formats?.length ? j.formats : [FORMAT.MONO1];
    caps.maxBytes = j.maxBytes ?? 0;
    caps.chunk = j.chunk ?? 0;
    caps.window = j.window ?? 0;
    caps.features.persist = true;
    caps.features.frameSleep = !!j.frameSleep;
    return caps;
  }

  caps.version = bytes[0];
  const v = view(bytes);
  for (let i = 1; i + 2 <= bytes.length; ) {
    const tag = bytes[i];
    const len = bytes[i + 1];
    const at = i + 2;
    i = at + len;
    if (i > bytes.length) break; // truncated record: ignore it
    const val = bytes.subarray(at, at + len);
    switch (tag) {
      case TAG.NAME:
        caps.name = new TextDecoder().decode(val);
        break;
      case TAG.PANEL:
        if (len >= 4) caps.panel = { width: v.getUint16(at, true), height: v.getUint16(at + 2, true) };
        break;
      case TAG.AREA:
        if (len >= 4) {
          caps.width = v.getUint16(at, true);
          caps.height = v.getUint16(at + 2, true);
        }
        break;
      case TAG.FORMATS:
        if (len) caps.formats = [...val];
        break;
      case TAG.ENCODINGS:
        caps.encodings = [ENCODING.NONE, ...[...val].filter((e) => e !== ENCODING.NONE)];
        break;
      case TAG.LIMITS:
        if (len >= 8) {
          caps.maxBytes = v.getUint32(at, true);
          caps.chunk = v.getUint16(at + 4, true);
          caps.window = v.getUint16(at + 6, true);
        }
        break;
      case TAG.FEATURES:
        if (len >= 4) {
          const f = v.getUint32(at, true);
          caps.features = {
            persist: !!(f & FEATURE.PERSIST),
            frameSleep: !!(f & FEATURE.FRAME_SLEEP),
            fastRefresh: !!(f & FEATURE.FAST_REFRESH),
            pointer: !!(f & FEATURE.POINTER),
          };
        }
        break;
      case TAG.REGIONS:
        if (len >= 1) caps.regionAlign = Math.max(1, val[0]);
        break;
      case TAG.KEYS:
        caps.keys = [...val];
        break;
      case TAG.PACING:
        if (len >= 8) {
          caps.minIntervalMs = v.getUint32(at, true);
          caps.refreshMs = v.getUint32(at + 4, true);
        }
        break;
      case TAG.POWER:
        if (len >= 2) caps.battery = parsePower(val[0], val[1]);
        break;
      default:
        break; // unknown tag: skip
    }
  }
  if (!caps.panel) caps.panel = { width: caps.width, height: caps.height };
  return caps;
}

function parsePower(battery, flags) {
  return { percent: battery === 255 ? null : battery, charging: !!(flags & 1), external: !!(flags & 2) };
}

/** Caps -> v2 Info bytes (for displays and simulators). Takes parseCaps()'s shape. */
export function encodeCaps(caps) {
  const out = [2];
  const rec = (tag, bytes) => {
    if (bytes.length > 255) throw new Error(`Caps tag ${tag} too long`);
    out.push(tag, bytes.length, ...bytes);
  };
  const u16 = (n) => [n & 0xff, (n >> 8) & 0xff];
  const u32 = (n) => [n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, (n >>> 24) & 0xff];
  if (caps.name) rec(TAG.NAME, [...new TextEncoder().encode(caps.name)].slice(0, MAX_HOST_NAME));
  if (caps.panel) rec(TAG.PANEL, [...u16(caps.panel.width), ...u16(caps.panel.height)]);
  rec(TAG.AREA, [...u16(caps.width), ...u16(caps.height)]);
  rec(TAG.FORMATS, caps.formats ?? [FORMAT.MONO1]);
  const enc = (caps.encodings ?? []).filter((e) => e !== ENCODING.NONE);
  if (enc.length) rec(TAG.ENCODINGS, enc);
  rec(TAG.LIMITS, [...u32(caps.maxBytes), ...u16(caps.chunk), ...u16(caps.window)]);
  const f = caps.features ?? {};
  rec(TAG.FEATURES, u32(
    (f.persist ? FEATURE.PERSIST : 0) | (f.frameSleep ? FEATURE.FRAME_SLEEP : 0)
      | (f.fastRefresh ? FEATURE.FAST_REFRESH : 0) | (f.pointer ? FEATURE.POINTER : 0),
  ));
  if (caps.regionAlign) rec(TAG.REGIONS, [caps.regionAlign]);
  if (caps.keys?.length) rec(TAG.KEYS, caps.keys);
  if (caps.minIntervalMs || caps.refreshMs) rec(TAG.PACING, [...u32(caps.minIntervalMs ?? 0), ...u32(caps.refreshMs ?? 0)]);
  if (caps.battery) {
    const b = caps.battery;
    rec(TAG.POWER, [b.percent ?? 255, (b.charging ? 1 : 0) | (b.external ? 2 : 0)]);
  }
  return Uint8Array.from(out);
}

/**
 * Event notification -> one of
 *   { type: 'caps', caps | null }   (null: re-read Info)
 *   { type: 'key', key, name, action, timeMs }
 *   { type: 'pointer', action, x, y }
 *   { type: 'power', percent, charging, external }
 *   { type: 'unknown', code }
 */
export function parseEvent(data) {
  const v = view(data);
  const bytes = bytesOf(v);
  const type = bytes[0];
  switch (type) {
    case EVENT.CAPS:
      return { type: 'caps', caps: bytes.length > 1 ? parseCaps(bytes.subarray(1)) : null };
    case EVENT.KEY:
      return {
        type: 'key',
        key: bytes[1],
        name: KEY_NAME[bytes[1]] ?? `key${bytes[1]}`,
        action: KEY_ACTION_NAME[bytes[2]] ?? `action${bytes[2]}`,
        timeMs: bytes.length >= 8 ? v.getUint32(4, true) : 0,
      };
    case EVENT.POINTER:
      return {
        type: 'pointer',
        action: POINTER_ACTION_NAME[bytes[1]] ?? `action${bytes[1]}`,
        x: v.getUint16(2, true),
        y: v.getUint16(4, true),
      };
    case EVENT.POWER:
      return { type: 'power', ...parsePower(bytes[1], bytes[2]) };
    default:
      return { type: 'unknown', code: type };
  }
}

export function encodeKeyEvent(key, action = KEY_ACTION.PRESS, timeMs = 0) {
  const out = new Uint8Array(8);
  const v = new DataView(out.buffer);
  out[0] = EVENT.KEY;
  out[1] = key;
  out[2] = action;
  v.setUint32(4, timeMs >>> 0, true);
  return out;
}

export function encodePointerEvent(action, x, y) {
  const out = new Uint8Array(6);
  const v = new DataView(out.buffer);
  out[0] = EVENT.POINTER;
  out[1] = action;
  v.setUint16(2, x, true);
  v.setUint16(4, y, true);
  return out;
}

export function encodeCapsEvent(caps) {
  if (!caps) return Uint8Array.of(EVENT.CAPS);
  const body = encodeCaps(caps);
  const out = new Uint8Array(1 + body.length);
  out[0] = EVENT.CAPS;
  out.set(body, 1);
  return out;
}

// --- checksums and encodings ---------------------------------------------------

let crcTable;
/** IEEE CRC-32, as zlib's crc32. Pass the previous result as `crc` to continue. */
export function crc32(bytes, crc = 0) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  crc = ~crc;
  for (let i = 0; i < bytes.length; i++) crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  return ~crc >>> 0;
}

/** PackBits-encode `bytes` (runs of 3+ become repeats, the rest literals). */
export function packbits(bytes) {
  const out = new Uint8Array(bytes.length + Math.ceil(bytes.length / 128) + 1);
  let o = 0;
  let i = 0;
  const n = bytes.length;
  while (i < n) {
    // Run of identical bytes starting at i?
    let run = 1;
    while (i + run < n && run < 128 && bytes[i + run] === bytes[i]) run++;
    if (run >= 3) {
      out[o++] = 257 - run;
      out[o++] = bytes[i];
      i += run;
      continue;
    }
    // Literal: up to the next run of 3 (or 128 bytes).
    const start = i;
    while (i < n && i - start < 128) {
      if (i + 2 < n && bytes[i] === bytes[i + 1] && bytes[i] === bytes[i + 2]) break;
      i++;
    }
    out[o++] = i - start - 1;
    out.set(bytes.subarray(start, i), o);
    o += i - start;
  }
  return out.subarray(0, o);
}

/**
 * Streaming PackBits decoder, as a display would run it: feed() chunks as
 * they arrive; each decoded byte goes to `emit(byte)`.
 */
export class PackBitsDecoder {
  #literal = 0; // literal bytes still to copy
  #repeat = 0; // >0: next byte is repeated this many times
  constructor(emit) {
    this.emit = emit;
  }
  feed(bytes) {
    for (const b of bytes) {
      if (this.#literal > 0) {
        this.emit(b);
        this.#literal--;
      } else if (this.#repeat > 0) {
        for (let k = 0; k < this.#repeat; k++) this.emit(b);
        this.#repeat = 0;
      } else if (b < 0x80) {
        this.#literal = b + 1;
      } else if (b > 0x80) {
        this.#repeat = 257 - b;
      } // 0x80: no-op
    }
  }
  get idle() {
    return this.#literal === 0 && this.#repeat === 0;
  }
}

/** Decode a whole PackBits buffer (for tests and tools). */
export function unpackbits(bytes) {
  const out = [];
  new PackBitsDecoder((b) => out.push(b)).feed(bytes);
  return Uint8Array.from(out);
}
