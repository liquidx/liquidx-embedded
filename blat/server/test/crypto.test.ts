// The pure-JS AES-CCM against Node's (OpenSSL's), both ways, at every size
// blat uses, and the hash helpers against node:crypto.
import assert from 'node:assert/strict';
import { createCipheriv, createDecipheriv, createHmac, hkdfSync, randomBytes } from 'node:crypto';
import { test } from 'node:test';

import { hkdf, hmac, open, seal } from '../src/blat/crypto.ts';

function nodeSeal(key: Uint8Array, nonce: Uint8Array, aad: Uint8Array, plain: Uint8Array): Uint8Array {
  const c = createCipheriv('aes-128-ccm', key, nonce, { authTagLength: 8 });
  c.setAAD(aad, { plaintextLength: plain.length });
  return new Uint8Array(Buffer.concat([c.update(plain), c.final(), c.getAuthTag()]));
}

function nodeOpen(key: Uint8Array, nonce: Uint8Array, aad: Uint8Array, sealed: Uint8Array): Uint8Array {
  const body = sealed.subarray(0, sealed.length - 8);
  const d = createDecipheriv('aes-128-ccm', key, nonce, { authTagLength: 8 });
  d.setAuthTag(sealed.subarray(sealed.length - 8));
  d.setAAD(aad, { plaintextLength: body.length });
  const out = d.update(body);
  d.final();
  return new Uint8Array(out);
}

test('AES-CCM matches OpenSSL', () => {
  for (const length of [0, 1, 15, 16, 17, 31, 32, 100, 499, 504]) {
    for (const aadLength of [0, 1, 2, 5, 6, 14, 15, 16]) {
      const key = new Uint8Array(randomBytes(16));
      const nonce = new Uint8Array(randomBytes(13));
      const aad = new Uint8Array(randomBytes(aadLength));
      const plain = new Uint8Array(randomBytes(length));
      const ours = seal(key, nonce, aad, plain);
      assert.deepEqual(ours, nodeSeal(key, nonce, aad, plain), `seal ${length}/${aadLength}`);
      assert.deepEqual(nodeOpen(key, nonce, aad, ours), plain);
      assert.deepEqual(open(key, nonce, aad, ours), plain);
    }
  }
});

test('AES-CCM refuses anything tampered with', () => {
  const key = new Uint8Array(randomBytes(16));
  const nonce = new Uint8Array(randomBytes(13));
  const aad = Uint8Array.of(1, 2);
  const sealed = seal(key, nonce, aad, new TextEncoder().encode('hello'));
  for (let i = 0; i < sealed.length; i++) {
    const bad = sealed.slice();
    bad[i] ^= 1;
    assert.equal(open(key, nonce, aad, bad), null);
  }
  assert.equal(open(key, nonce, Uint8Array.of(1, 3), sealed), null);
  const otherNonce = nonce.slice();
  otherNonce[12] ^= 1;
  assert.equal(open(key, otherNonce, aad, sealed), null);
});

test('HMAC and HKDF match node:crypto', () => {
  const key = new Uint8Array(randomBytes(32));
  const data = new Uint8Array(randomBytes(77));
  assert.deepEqual(hmac(key, data.subarray(0, 10), data.subarray(10)), new Uint8Array(createHmac('sha256', key).update(data).digest()));
  const salt = new Uint8Array(randomBytes(16));
  assert.deepEqual(hkdf(salt, key, 'blat session', 32), new Uint8Array(hkdfSync('sha256', key, salt, 'blat session', 32)));
  // No salt: HashLen zeros.
  assert.deepEqual(hkdf(new Uint8Array(0), key, 'x', 32), new Uint8Array(hkdfSync('sha256', key, new Uint8Array(32), 'x', 32)));
});
