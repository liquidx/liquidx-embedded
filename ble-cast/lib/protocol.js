// BLE cast protocol v1, the sender side.
// Spec: ../../xteink-x4-platformio/docs/ble-cast-protocol.md

export const SERVICE = 'b1ec0000-5f3a-4e62-9a47-0c3d8e5f2a10';
export const CHARACTERISTIC = {
  info: 'b1ec0001-5f3a-4e62-9a47-0c3d8e5f2a10',
  control: 'b1ec0002-5f3a-4e62-9a47-0c3d8e5f2a10',
  data: 'b1ec0003-5f3a-4e62-9a47-0c3d8e5f2a10',
  status: 'b1ec0004-5f3a-4e62-9a47-0c3d8e5f2a10',
};

export const VERSION = 1;
export const FORMAT_RAW1 = 1; // 1 bit per pixel, rows top-down, MSB = leftmost, 1 = black
export const FLAG_PERSIST = 0x01;

const OP_BEGIN = 0x01;
export const OP_COMMIT = Uint8Array.of(0x02);
export const OP_CANCEL = Uint8Array.of(0x03);

export const EVENT = { READY: 1, DONE: 2, ERROR: 3, ACK: 4 };

export const ERROR_TEXT = {
  1: 'bad header',
  2: 'unsupported format or version',
  3: 'payload too large or wrong size',
  4: 'device busy showing the previous frame',
  5: 'data out of order',
  6: 'commit before all data arrived',
  7: 'CRC mismatch',
  8: "couldn't save to the SD card (frame still shown)",
};

const HEADER_BYTES = 21;
const MAX_NAME = 64;

/** Control write that starts a frame. */
export function encodeBegin({ format = FORMAT_RAW1, persist = false, width, height, byteLength, crc, nextFrameSeconds = 0, name = '' }) {
  // ASCII only; anything else becomes '_' (the device sanitises further).
  const nameBytes = Uint8Array.from([...name].slice(0, MAX_NAME), (c) => (c.charCodeAt(0) < 0x80 ? c.charCodeAt(0) : 0x5f));
  const out = new Uint8Array(HEADER_BYTES + nameBytes.length);
  const v = new DataView(out.buffer);
  v.setUint8(0, OP_BEGIN);
  v.setUint8(1, VERSION);
  v.setUint8(2, format);
  v.setUint8(3, persist ? FLAG_PERSIST : 0);
  v.setUint16(4, width, true);
  v.setUint16(6, height, true);
  v.setUint32(8, byteLength, true);
  v.setUint32(12, crc, true);
  v.setUint32(16, Math.max(0, Math.round(nextFrameSeconds)), true);
  v.setUint8(20, nameBytes.length);
  out.set(nameBytes, HEADER_BYTES);
  return out;
}

/** Data write: u32 offset + payload bytes. */
export function encodeData(offset, bytes) {
  const out = new Uint8Array(4 + bytes.length);
  new DataView(out.buffer).setUint32(0, offset, true);
  out.set(bytes, 4);
  return out;
}

/** Status notification: { event, code, value }. */
export function parseStatus(view) {
  return { event: view.getUint8(0), code: view.getUint8(1), value: view.getUint32(2, true) };
}

let crcTable;
/** IEEE CRC-32, as zlib's crc32. */
export function crc32(bytes) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}
