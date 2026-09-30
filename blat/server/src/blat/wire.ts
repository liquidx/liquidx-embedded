// Numbers and encodings from the wire format (../../PROTOCOL.md).

export const VERSION = 1;

// PROTOCOL.md#transport-bluetooth-le-gatt
export const SERVICE = 'b1a70000-e295-445e-b079-edd9ea2725cb';
export const CHARACTERISTIC = {
  info: 'b1a70001-e295-445e-b079-edd9ea2725cb',
  request: 'b1a70002-e295-445e-b079-edd9ea2725cb',
  reply: 'b1a70003-e295-445e-b079-edd9ea2725cb',
  data: 'b1a70004-e295-445e-b079-edd9ea2725cb',
  dataOut: 'b1a70005-e295-445e-b079-edd9ea2725cb',
  event: 'b1a70006-e295-445e-b079-edd9ea2725cb',
} as const;

// Channels, as numbered for stream transports and sealing nonces.
export const CHANNEL = { request: 1, data: 2, reply: 3, dataOut: 4, event: 5 } as const;

// PROTOCOL.md#requests
export const OP = {
  hello: 0x01,
  authBegin: 0x02,
  authCode: 0x03,
  authConfirm: 0x04,
  authResume: 0x05,
  remember: 0x06,
  get: 0x10,
  set: 0x11,
  invoke: 0x12,
  options: 0x13,
  readOpen: 0x20,
  writeOpen: 0x21,
  ack: 0x22,
  commit: 0x23,
  cancel: 0x24,
  list: 0x25,
  delete: 0x26,
} as const;

// PROTOCOL.md#replies
export const STATUS = {
  ok: 0,
  running: 1,
  badRequest: 2,
  unsupportedVersion: 3,
  unknownId: 4,
  invalidValue: 5,
  readOnly: 6,
  notPermitted: 7,
  busy: 8,
  tooLarge: 9,
  outOfOrder: 10,
  badCrc: 11,
  storageError: 12,
  notFound: 13,
  actionFailed: 14,
  authRequired: 15,
  wrongCode: 16,
  lockedOut: 17,
} as const;

export const STATUS_TEXT: Record<number, string> = {
  0: 'ok',
  1: 'running',
  2: 'bad request',
  3: 'unsupported version',
  4: 'unknown control',
  5: 'invalid value',
  6: 'read-only',
  7: 'not permitted',
  8: 'busy',
  9: 'too large',
  10: 'out of order',
  11: 'CRC mismatch',
  12: 'storage error',
  13: 'not found',
  14: 'action failed',
  15: 'authentication required',
  16: 'wrong code',
  17: 'locked out',
};

export const REPLY_MORE = 0x01;
export const REPLY_HEADER_BYTES = 6;

// PROTOCOL.md#events
export const EVENT = { changed: 0x01, schema: 0x02, optionsChanged: 0x03, progress: 0x04, actionDone: 0x05, log: 0x06 } as const;

// PROTOCOL.md#info
export const INFO = { name: 0x01, model: 0x02, firmware: 0x03, deviceId: 0x04, schema: 0x05, limits: 0x06, features: 0x07, auth: 0x08 } as const;

// PROTOCOL.md#control-types
export const TYPE = { bool: 1, int: 2, enum: 3, text: 4, secret: 5, bytes: 6, action: 7, file: 8, dir: 9, group: 10 } as const;
export type TypeName = keyof typeof TYPE;
export const TYPE_NAME: Record<number, TypeName> = Object.fromEntries(
  Object.entries(TYPE).map(([name, code]) => [code, name as TypeName]),
);

// PROTOCOL.md#flags
export const FLAG = {
  readOnly: 1 << 0,
  live: 1 << 1,
  dynamic: 1 << 2,
  restart: 1 << 3,
  confirm: 1 << 4,
  advanced: 1 << 5,
  required: 1 << 6,
  hidden: 1 << 7,
} as const;

// PROTOCOL.md#attributes
export const ATTR = {
  key: 0x01,
  label: 0x02,
  help: 0x03,
  unit: 0x04,
  range: 0x05,
  scale: 0x06,
  option: 0x07,
  length: 0x08,
  hint: 0x09,
  default: 0x0a,
} as const;

export const HINT_NAME: Record<number, string> = {
  1: 'multiline',
  2: 'email',
  3: 'url',
  4: 'hostname',
  5: 'ip',
  6: 'mac',
  7: 'color',
  8: 'timeOfDay',
  9: 'dateTime',
};

export const AUTH = { methodCode: 1, methodResume: 2 } as const;
export const LEVEL = { anyone: 0, paired: 1, present: 2 } as const;

/** A value as the host sees it: numbers for bool, int and enum; strings for
 * text; for a secret, whether it's set. */
export type Value = number | string | boolean;

/** Little-endian reads and writes over a byte array. */
export class Reader {
  readonly bytes: Uint8Array;
  #view: DataView;
  offset = 0;
  constructor(bytes: Uint8Array) {
    this.bytes = bytes;
    this.#view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }
  get remaining(): number {
    return this.bytes.length - this.offset;
  }
  #need(n: number) {
    if (this.remaining < n) throw new Error('Message too short');
  }
  u8(): number {
    this.#need(1);
    return this.bytes[this.offset++];
  }
  u16(): number {
    this.#need(2);
    const v = this.#view.getUint16(this.offset, true);
    this.offset += 2;
    return v;
  }
  u32(): number {
    this.#need(4);
    const v = this.#view.getUint32(this.offset, true);
    this.offset += 4;
    return v;
  }
  i32(): number {
    this.#need(4);
    const v = this.#view.getInt32(this.offset, true);
    this.offset += 4;
    return v;
  }
  take(n: number): Uint8Array {
    this.#need(n);
    const out = this.bytes.subarray(this.offset, this.offset + n);
    this.offset += n;
    return out;
  }
  rest(): Uint8Array {
    return this.take(this.remaining);
  }
}

export class Writer {
  #bytes: number[] = [];
  u8(v: number): this {
    this.#bytes.push(v & 0xff);
    return this;
  }
  u16(v: number): this {
    return this.u8(v).u8(v >> 8);
  }
  u32(v: number): this {
    return this.u8(v).u8(v >> 8).u8(v >> 16).u8(v >>> 24);
  }
  bytes(b: ArrayLike<number>): this {
    for (let i = 0; i < b.length; i++) this.#bytes.push(b[i]);
    return this;
  }
  finish(): Uint8Array {
    return Uint8Array.from(this.#bytes);
  }
}

const utf8 = new TextEncoder();
const fromUtf8 = new TextDecoder();
export const encodeText = (s: string) => utf8.encode(s);
export const decodeText = (b: Uint8Array) => fromUtf8.decode(b);

/** TLV records (`u8 tag, u8 length, value`), in order. */
export function readTlvs(bytes: Uint8Array): Array<[number, Uint8Array]> {
  const out: Array<[number, Uint8Array]> = [];
  const r = new Reader(bytes);
  while (r.remaining >= 2) {
    const tag = r.u8();
    const n = r.u8();
    if (r.remaining < n) break;
    out.push([tag, r.take(n)]);
  }
  return out;
}

/** Encode one value for `set` or `invoke` (PROTOCOL.md#values). */
export function encodeValue(type: number, value: Value): Uint8Array {
  const w = new Writer();
  switch (type) {
    case TYPE.bool:
      return w.u8(value ? 1 : 0).finish();
    case TYPE.int:
      if (typeof value !== 'number' || !Number.isInteger(value)) throw new Error('Expected a whole number');
      return w.u32(value | 0).finish();
    case TYPE.enum:
      if (typeof value !== 'number') throw new Error('Expected an option value');
      return w.u16(value).finish();
    case TYPE.text:
    case TYPE.secret: {
      if (typeof value !== 'string') throw new Error('Expected text');
      const b = encodeText(value);
      if (b.length > 255) throw new Error('Text too long');
      return w.u8(b.length).bytes(b).finish();
    }
    default:
      throw new Error(`Can't encode a value of type ${type}`);
  }
}

/** Decode one value of `type` from `r`. */
export function decodeValue(type: number, r: Reader): Value {
  switch (type) {
    case TYPE.bool:
      return r.u8();
    case TYPE.int:
      return r.i32();
    case TYPE.enum:
      return r.u16();
    case TYPE.secret:
      return r.u8() === 1;
    case TYPE.text:
      return decodeText(r.take(r.u8()));
    default:
      throw new Error(`Can't decode a value of type ${type}`);
  }
}

// IEEE CRC-32 (zlib's).
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes: Uint8Array): number {
  let c = 0xffffffff;
  for (const b of bytes) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

export const toHex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
export const fromHex = (s: string) => Uint8Array.from(s.match(/../g) ?? [], (h) => parseInt(h, 16));
