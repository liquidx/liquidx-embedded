// The cryptography of blat authentication (PROTOCOL.md#authentication), host
// side: SPAKE2 party A on P-256, HKDF and HMAC for keys, AES-128-CCM for
// sealing. Point arithmetic is @noble/curves; the rest is node:crypto.

import { p256 } from '@noble/curves/nist.js';
import { createCipheriv, createDecipheriv, createHash, createHmac, hkdfSync, randomBytes, timingSafeEqual } from 'node:crypto';

const Point = p256.Point;
const N = Point.Fn.ORDER;

// RFC 9382 section 6, P-256.
const M = Point.fromHex('02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f');
const NN = Point.fromHex('03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49');

export const TAG_BYTES = 8;
const enc = new TextEncoder();

export const random = (n: number): Uint8Array => new Uint8Array(randomBytes(n));

export function sha256(...parts: Uint8Array[]): Uint8Array {
  const h = createHash('sha256');
  for (const p of parts) h.update(p);
  return new Uint8Array(h.digest());
}

export function hmac(key: Uint8Array, ...parts: Uint8Array[]): Uint8Array {
  const h = createHmac('sha256', key);
  for (const p of parts) h.update(p);
  return new Uint8Array(h.digest());
}

/** HKDF-SHA256. An empty salt means HashLen zero bytes, as RFC 5869 says. */
export function hkdf(salt: Uint8Array, ikm: Uint8Array, info: Uint8Array | string, length: number): Uint8Array {
  const i = typeof info === 'string' ? enc.encode(info) : info;
  return new Uint8Array(hkdfSync('sha256', ikm, salt.length ? salt : new Uint8Array(32), i, length));
}

export function equal(a: Uint8Array, b: Uint8Array): boolean {
  return a.length === b.length && timingSafeEqual(a, b);
}

const bigFromBytes = (b: Uint8Array) => BigInt('0x' + (Buffer.from(b).toString('hex') || '0'));
const bytesFromBig = (v: bigint) => Uint8Array.from(Buffer.from(v.toString(16).padStart(64, '0'), 'hex'));

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

/** AES-128-CCM with a 13-byte nonce; the 8-byte tag is appended. */
export function seal(key: Uint8Array, nonce: Uint8Array, aad: Uint8Array, plain: Uint8Array): Uint8Array {
  const c = createCipheriv('aes-128-ccm', key, nonce, { authTagLength: TAG_BYTES });
  c.setAAD(aad, { plaintextLength: plain.length });
  const body = Buffer.concat([c.update(plain), c.final()]);
  return concat(body, c.getAuthTag());
}

/** Opens what seal() made, or returns null if it doesn't authenticate. */
export function open(key: Uint8Array, nonce: Uint8Array, aad: Uint8Array, sealed: Uint8Array): Uint8Array | null {
  if (sealed.length < TAG_BYTES) return null;
  const body = sealed.subarray(0, sealed.length - TAG_BYTES);
  try {
    const d = createDecipheriv('aes-128-ccm', key, nonce, { authTagLength: TAG_BYTES });
    d.setAuthTag(sealed.subarray(sealed.length - TAG_BYTES));
    d.setAAD(aad, { plaintextLength: body.length });
    const out = d.update(body);
    d.final();
    return new Uint8Array(out);
  } catch {
    return null;
  }
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
