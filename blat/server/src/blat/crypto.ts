// The cryptography of blat authentication (PROTOCOL.md#authentication), host
// side: SPAKE2 party A on P-256, HKDF and HMAC for keys, AES-128-CCM for
// sealing. Pure JavaScript (@noble), so the same code runs in Node and in a
// browser: Web Crypto has no AES-CCM and no raw point arithmetic.

import { cbc, ecb } from '@noble/ciphers/aes.js';
import { p256 } from '@noble/curves/nist.js';
import { hkdf as nobleHkdf } from '@noble/hashes/hkdf.js';
import { hmac as nobleHmac } from '@noble/hashes/hmac.js';
import { sha256 as nobleSha256 } from '@noble/hashes/sha2.js';

const Point = p256.Point;
const N = Point.Fn.ORDER;

// RFC 9382 section 6, P-256.
const M = Point.fromHex('02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f');
const NN = Point.fromHex('03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49');

export const TAG_BYTES = 8;
const enc = new TextEncoder();

export function random(n: number): Uint8Array {
  return globalThis.crypto.getRandomValues(new Uint8Array(n));
}

export function sha256(...parts: Uint8Array[]): Uint8Array {
  return nobleSha256(concat(...parts));
}

export function hmac(key: Uint8Array, ...parts: Uint8Array[]): Uint8Array {
  return nobleHmac(nobleSha256, key, concat(...parts));
}

/** HKDF-SHA256. An empty salt means HashLen zero bytes, as RFC 5869 says. */
export function hkdf(salt: Uint8Array, ikm: Uint8Array, info: Uint8Array | string, length: number): Uint8Array {
  const i = typeof info === 'string' ? enc.encode(info) : info;
  return nobleHkdf(nobleSha256, ikm, salt.length ? salt : new Uint8Array(32), i, length);
}

/** Constant-time comparison. */
export function equal(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

const toHexString = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const bigFromBytes = (b: Uint8Array) => BigInt('0x' + (toHexString(b) || '0'));
function bytesFromBig(v: bigint): Uint8Array {
  const hex = v.toString(16).padStart(64, '0');
  return Uint8Array.from(hex.match(/../g)!, (h) => parseInt(h, 16));
}

function lengthPrefixed(b: Uint8Array): Uint8Array {
  const out = new Uint8Array(8 + b.length);
  new DataView(out.buffer).setBigUint64(0, BigInt(b.length), true);
  out.set(b, 8);
  return out;
}

/** SPAKE2's w for a code: SHA-256("blat code" ‖ salt ‖ code) mod n. */
export function codeToW(salt: Uint8Array, code: string): bigint {
  return bigFromBytes(sha256(enc.encode('blat code'), salt, enc.encode(code))) % N;
}

/** SPAKE2 (RFC 9382 construction) as party A: the host. */
export class Spake2 {
  readonly pA: Uint8Array;
  #x: bigint;
  #w: bigint;
  #idA: Uint8Array;
  #idB: Uint8Array;
  #aad: Uint8Array;

  constructor(w: bigint, idA: Uint8Array, idB: Uint8Array, aad: Uint8Array) {
    this.#w = w;
    this.#idA = idA;
    this.#idB = idB;
    this.#aad = aad;
    // 1..n-1; 40 bytes so the reduction's bias is negligible.
    this.#x = (bigFromBytes(random(40)) % (N - 1n)) + 1n;
    this.pA = Point.BASE.multiply(this.#x).add(M.multiply(w)).toBytes(false);
  }

  /** Given B's share, the shared key Ke and both confirmations. Throws if pB
   * isn't a point on the curve. */
  finish(pB: Uint8Array): { ke: Uint8Array; confirmA: Uint8Array; confirmB: Uint8Array } {
    const B = Point.fromBytes(pB);
    B.assertValidity();
    const K = B.subtract(NN.multiply(this.#w)).multiply(this.#x);
    const tt = concat(
      lengthPrefixed(this.#idA),
      lengthPrefixed(this.#idB),
      lengthPrefixed(this.pA),
      lengthPrefixed(pB),
      lengthPrefixed(K.toBytes(false)),
      lengthPrefixed(bytesFromBig(this.#w)),
    );
    const hash = sha256(tt);
    const kc = hkdf(new Uint8Array(0), hash.subarray(16), concat(enc.encode('ConfirmationKeys'), this.#aad), 32);
    return {
      ke: hash.subarray(0, 16),
      confirmA: hmac(kc.subarray(0, 16), tt),
      confirmB: hmac(kc.subarray(16), tt),
    };
  }
}

// --- AES-128-CCM (RFC 3610), 13-byte nonce, 8-byte tag -------------------------------
//
// Built from the public AES modes: CCM's MAC is the last block of CBC
// encryption under a zero IV, and its keystream is ECB over counter blocks.

const NONCE_BYTES = 13;
const L = 15 - NONCE_BYTES; // bytes of message length in B0 and the counters

function padded(b: Uint8Array): Uint8Array {
  const out = new Uint8Array(Math.ceil(b.length / 16) * 16);
  out.set(b);
  return out;
}

function ccmMac(key: Uint8Array, nonce: Uint8Array, aad: Uint8Array, plain: Uint8Array): Uint8Array {
  const b0 = new Uint8Array(16);
  b0[0] = (aad.length > 0 ? 0x40 : 0) | (((TAG_BYTES - 2) / 2) << 3) | (L - 1);
  b0.set(nonce, 1);
  b0[14] = plain.length >> 8;
  b0[15] = plain.length & 0xff;
  // AAD under 2^16 - 2^8 bytes: a 2-byte length, then the data, zero-padded.
  const a = aad.length > 0 ? padded(concat(Uint8Array.of(aad.length >> 8, aad.length & 0xff), aad)) : new Uint8Array(0);
  const blocks = concat(b0, a, padded(plain));
  const macBlocks = cbc(key, new Uint8Array(16), { disablePadding: true }).encrypt(blocks);
  return macBlocks.subarray(macBlocks.length - 16);
}

// S_0, S_1, … S_n: AES of the counter blocks.
function keystream(key: Uint8Array, nonce: Uint8Array, blocks: number): Uint8Array {
  const counters = new Uint8Array(16 * (blocks + 1));
  for (let i = 0; i <= blocks; i++) {
    counters[16 * i] = L - 1;
    counters.set(nonce, 16 * i + 1);
    counters[16 * i + 14] = i >> 8;
    counters[16 * i + 15] = i & 0xff;
  }
  return ecb(key, { disablePadding: true }).encrypt(counters);
}

function checkSizes(key: Uint8Array, nonce: Uint8Array, aad: Uint8Array, length: number) {
  if (key.length !== 16 || nonce.length !== NONCE_BYTES) throw new Error('AES-CCM: bad key or nonce size');
  if (length >= 1 << (8 * L) || aad.length >= 0xff00) throw new Error('AES-CCM: message too long');
}

/** AES-128-CCM; the 8-byte tag is appended. */
export function seal(key: Uint8Array, nonce: Uint8Array, aad: Uint8Array, plain: Uint8Array): Uint8Array {
  checkSizes(key, nonce, aad, plain.length);
  const s = keystream(key, nonce, Math.ceil(plain.length / 16));
  const mac = ccmMac(key, nonce, aad, plain);
  const out = new Uint8Array(plain.length + TAG_BYTES);
  for (let i = 0; i < plain.length; i++) out[i] = plain[i] ^ s[16 + i];
  for (let i = 0; i < TAG_BYTES; i++) out[plain.length + i] = mac[i] ^ s[i];
  return out;
}

/** Opens what seal() made, or returns null if it doesn't authenticate. */
export function open(key: Uint8Array, nonce: Uint8Array, aad: Uint8Array, sealed: Uint8Array): Uint8Array | null {
  if (sealed.length < TAG_BYTES) return null;
  const n = sealed.length - TAG_BYTES;
  checkSizes(key, nonce, aad, n);
  const s = keystream(key, nonce, Math.ceil(n / 16));
  const plain = new Uint8Array(n);
  for (let i = 0; i < n; i++) plain[i] = sealed[i] ^ s[16 + i];
  const mac = ccmMac(key, nonce, aad, plain);
  const tag = new Uint8Array(TAG_BYTES);
  for (let i = 0; i < TAG_BYTES; i++) tag[i] = mac[i] ^ s[i];
  return equal(tag, sealed.subarray(n)) ? plain : null;
}

/** A 13-byte nonce for Request, Reply and Event: channel, message count. */
export function counterNonce(channel: number, count: number): Uint8Array {
  const n = new Uint8Array(13);
  n[0] = channel;
  new DataView(n.buffer).setUint32(1, count, true);
  return n;
}

/** A 13-byte nonce for Data and DataOut: channel, transfer id, offset. */
export function transferNonce(channel: number, transferId: number, offset: number): Uint8Array {
  const n = new Uint8Array(13);
  n[0] = channel;
  n[1] = transferId;
  new DataView(n.buffer).setUint32(2, offset, true);
  return n;
}

export function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}
