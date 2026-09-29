// The host side of blat (PROTOCOL.md): one connection to one device, over any
// Link. Requests go one at a time; replies, transfers and events are opened
// (unsealed) as they arrive.

import { Spake2, codeToW, concat, counterNonce, equal, hkdf, hmac, open, random, seal, transferNonce } from './crypto.ts';
import type { Link } from './link.ts';
import { type Control, type Info, parseInfo, parseSchema } from './schema.ts';
import {
  AUTH,
  CHANNEL,
  EVENT,
  OP,
  REPLY_HEADER_BYTES,
  REPLY_MORE,
  Reader,
  STATUS,
  STATUS_TEXT,
  type Value,
  VERSION,
  Writer,
  crc32,
  decodeValue,
  encodeText,
  encodeValue,
  fromHex,
  toHex,
} from './wire.ts';

/** A reply with a status other than ok. */
export class BlatError extends Error {
  readonly status: number;
  readonly detail: number;
  readonly op: number;
  constructor(op: number, status: number, detail: number) {
    const text = STATUS_TEXT[status] ?? `status ${status}`;
    super(status === STATUS.authRequired ? `${text} (level ${detail})` : text);
    this.op = op;
    this.status = status;
    this.detail = detail;
  }
}

interface Reply {
  status: number;
  flags: number;
  detail: number;
  body: Uint8Array;
}

export interface PairingStarted {
  digits: number;
  attemptsLeft: number;
  expiresSeconds: number;
}

export interface HostKey {
  hostId: string; // hex
  hostKey: string; // hex
}

export interface ClientOptions {
  /** Name the device may show for this host (hello). */
  hostName?: string;
  /** How long to wait for a reply. */
  timeoutMs?: number;
}

const HOST_IDENTITY = encodeText('blat host');
const DEVICE_LABEL = encodeText('blat device');
const SESSION_INFO = 'blat session';

export class BlatClient {
  #link: Link;
  #options: Required<ClientOptions>;
  #queue: Promise<unknown> = Promise.resolve();
  #seq = 0;
  #pending: { op: number; seq: number; resolve: (r: Reply) => void; reject: (e: Error) => void } | null = null;
  #dataOut: Uint8Array[] = [];
  #dataOutWaiter: (() => void) | null = null;
  #open = false;

  // The session (PROTOCOL.md#sealing).
  #keys: { host: Uint8Array; device: Uint8Array } | null = null;
  #txRequests = 0;
  #rxReplies = 0;
  #rxEvents = 0;

  // Pairing in progress.
  #pairing: { salt: Uint8Array } | null = null;

  info: Info | null = null;
  level = 0;
  controls: Control[] = [];

  /** Values the device reported changing (live controls). */
  onChanged: (values: Map<number, Value>) => void = () => {};
  /** The link went away. */
  onClose: () => void = () => {};

  constructor(link: Link, options: ClientOptions = {}) {
    this.#link = link;
    this.#options = { hostName: 'blat host', timeoutMs: 20000, ...options };
  }

  get connected(): boolean {
    return this.#open;
  }

  get deviceId(): Uint8Array {
    return fromHex(this.info?.deviceId ?? '');
  }

  /** Open the link and say hello. Info comes from the hello reply, or the
   * Info characteristic when it doesn't fit. */
  async connect(): Promise<Info> {
    await this.#link.open({
      onMessage: (channel, data) => this.#message(channel, data),
      onClose: () => this.#closed(),
    });
    this.#open = true;
    const name = encodeText(this.#options.hostName).subarray(0, 32);
    const reply = await this.request(OP.hello, new Writer().u8(VERSION).u8(1).u8(name.length).bytes(name).finish());
    let infoBytes: Uint8Array | null = reply.body.length ? reply.body : null;
    if (!infoBytes) infoBytes = await this.#link.readInfo();
    if (!infoBytes) throw new Error("Device didn't send Info");
    this.info = parseInfo(infoBytes);
    this.level = this.info.level;
    return this.info;
  }

  close(): void {
    this.#link.close();
  }

  #closed() {
    if (!this.#open) return;
    this.#open = false;
    this.#keys = null;
    this.level = 0;
    this.#pending?.reject(new Error('Disconnected'));
    this.#pending = null;
    this.#dataOutWaiter?.();
    this.onClose();
  }

  // --- Requests ---------------------------------------------------------------

  /** Send a request and wait for its reply. Throws BlatError unless the
   * status is ok (or one of `accept`). */
  request(op: number, body: Uint8Array = new Uint8Array(0), accept: number[] = []): Promise<Reply> {
    const run = async () => {
      const reply = await this.#exchange(op, body);
      if (reply.status !== STATUS.ok && !accept.includes(reply.status)) {
        throw new BlatError(op, reply.status, reply.detail);
      }
      return reply;
    };
    const result = this.#queue.then(run, run);
    this.#queue = result.catch(() => {});
    return result;
  }

  async #exchange(op: number, body: Uint8Array): Promise<Reply> {
    if (!this.#open) throw new Error('Not connected');
    const seq = (this.#seq = (this.#seq + 1) & 0xff);
    const reply = new Promise<Reply>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#pending = null;
        reject(new Error(`No reply from the device (op 0x${op.toString(16)})`));
      }, this.#options.timeoutMs);
      this.#pending = {
        op,
        seq,
        resolve: (r) => {
          clearTimeout(timer);
          resolve(r);
        },
        reject: (e) => {
          clearTimeout(timer);
          reject(e);
        },
      };
    });
    await this.#sendRequest(op, seq, body);
    return reply;
  }

  // A request with no reply (ack). Goes through the queue like the others.
  #post(op: number, body: Uint8Array): Promise<void> {
    const seq = (this.#seq = (this.#seq + 1) & 0xff);
    return this.#sendRequest(op, seq, body);
  }

  async #sendRequest(op: number, seq: number, body: Uint8Array) {
    const header = Uint8Array.of(op, seq);
    if (this.#keys) {
      body = seal(this.#keys.host, counterNonce(CHANNEL.request, this.#txRequests++), header, body);
    }
    await this.#link.send(CHANNEL.request, concat(header, body));
  }

  // --- Incoming -----------------------------------------------------------------

  #message(channel: number, data: Uint8Array) {
    if (channel === CHANNEL.reply) this.#reply(data);
    else if (channel === CHANNEL.dataOut) this.#chunk(data);
    else if (channel === CHANNEL.event) this.#event(data);
  }

  #reply(data: Uint8Array) {
    if (data.length < REPLY_HEADER_BYTES) return;
    const header = data.subarray(0, REPLY_HEADER_BYTES);
    const r = new Reader(header);
    const op = r.u8();
    const seq = r.u8();
    const status = r.u8();
    const flags = r.u8();
    const detail = r.u16();
    let body: Uint8Array = data.subarray(REPLY_HEADER_BYTES);
    if (this.#keys) {
      const opened = open(this.#keys.device, counterNonce(CHANNEL.reply, this.#rxReplies), header, body);
      if (opened) {
        this.#rxReplies++;
        body = opened;
      } else if (status === STATUS.authRequired && body.length === 0) {
        // The device couldn't open our request and ended the session; this
        // reply is in the clear (PROTOCOL.md#sealing).
        this.#keys = null;
        this.level = 0;
      } else {
        return; // not from our session: ignore
      }
    }
    const p = this.#pending;
    if (!p || p.op !== op || p.seq !== seq) return; // stale
    this.#pending = null;
    p.resolve({ status, flags, detail, body });
  }

  #chunk(data: Uint8Array) {
    this.#dataOut.push(data);
    this.#dataOutWaiter?.();
  }

  #nextChunk(): Promise<Uint8Array> {
    const take = () => this.#dataOut.shift();
    const now = take();
    if (now) return Promise.resolve(now);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#dataOutWaiter = null;
        reject(new Error('Transfer stalled'));
      }, this.#options.timeoutMs);
      this.#dataOutWaiter = () => {
        const chunk = take();
        if (!chunk && this.#open) return;
        clearTimeout(timer);
        this.#dataOutWaiter = null;
        if (chunk) resolve(chunk);
        else reject(new Error('Disconnected'));
      };
    });
  }

  #event(data: Uint8Array) {
    if (data.length < 1) return;
    const header = data.subarray(0, 1);
    let body: Uint8Array | null = data.subarray(1);
    if (this.#keys) {
      body = open(this.#keys.device, counterNonce(CHANNEL.event, this.#rxEvents), header, body);
      if (!body) return;
      this.#rxEvents++;
    }
    if (header[0] === EVENT.changed) {
      try {
        this.onChanged(this.#decodeValues(body));
      } catch {
        // an event for controls we don't know yet: ignore
      }
    }
  }

  #decodeValues(body: Uint8Array): Map<number, Value> {
    const out = new Map<number, Value>();
    const r = new Reader(body);
    while (r.remaining > 0) {
      const id = r.u16();
      const c = this.#control(id);
      out.set(id, decodeValue(c.typeCode, r));
    }
    return out;
  }

  #control(id: number): Control {
    const c = this.controls[id - 1];
    if (!c || c.id !== id) throw new Error(`Unknown control ${id}`);
    return c;
  }

  // --- Schema and values -----------------------------------------------------------

  /** Read the schema with a transfer and parse it. */
  async readSchema(): Promise<Uint8Array> {
    const reply = await this.request(OP.readOpen, Uint8Array.of(0, 0, 0, 0));
    const r = new Reader(reply.body);
    const transferId = r.u8();
    const size = r.u32();
    const crc = r.u32();
    const chunkSize = r.u16();
    const window = Math.max(1, this.info?.window ?? 1);
    const blob = new Uint8Array(size);
    let received = 0;
    let chunks = 0;
    this.#dataOut = this.#dataOut.filter((c) => c[0] === transferId);
    while (received < size) {
      const msg = await this.#nextChunk();
      const header = msg.subarray(0, 5);
      const h = new Reader(header);
      const id = h.u8();
      const offset = h.u32();
      let data: Uint8Array | null = msg.subarray(5);
      if (this.#keys) data = open(this.#keys.device, transferNonce(CHANNEL.dataOut, id, offset), header, data);
      if (!data) throw new Error("Couldn't open a schema chunk");
      if (id !== transferId) continue;
      if (offset !== received || data.length > chunkSize || received + data.length > size) {
        throw new Error('Schema chunks out of order');
      }
      blob.set(data, received);
      received += data.length;
      if (++chunks % window === 0 || received === size) {
        await this.#post(OP.ack, new Writer().u8(transferId).u32(received).finish());
      }
    }
    if (crc32(blob) !== crc) throw new Error('Schema CRC mismatch');
    this.controls = parseSchema(blob);
    return blob;
  }

  /** Use a schema read earlier (cached by its CRC). */
  useSchema(blob: Uint8Array): void {
    if (crc32(blob) !== this.info?.schemaCrc) throw new Error("Cached schema doesn't match the device's");
    this.controls = parseSchema(blob);
  }

  /** Values of these controls, or of every value this connection may read. */
  async get(ids?: number[]): Promise<Map<number, Value>> {
    const out = new Map<number, Value>();
    let want = ids ?? null;
    for (;;) {
      const body = new Writer();
      for (const id of want ?? []) body.u16(id);
      const reply = await this.request(OP.get, body.finish());
      for (const [id, v] of this.#decodeValues(reply.body)) out.set(id, v);
      if (!(reply.flags & REPLY_MORE)) return out;
      // Cut off: ask for the rest by id.
      const all = want ?? this.controls.filter((c) => this.#hasValue(c) && c.read <= this.level).map((c) => c.id);
      const rest = all.filter((id) => !out.has(id));
      if (rest.length === 0 || rest.length === all.length) return out; // nothing more, or no progress
      want = rest;
    }
  }

  #hasValue(c: Control) {
    return c.type !== 'group' && c.type !== 'action';
  }

  /** Change values, all or none. */
  async set(values: Map<number, Value> | Array<[number, Value]>): Promise<void> {
    const w = new Writer();
    for (const [id, v] of values) w.u16(id).bytes(encodeValue(this.#control(id).typeCode, v));
    await this.request(OP.set, w.finish());
  }

  /** Run an action. Resolves with its status: ok, or running (it finishes
   * later). */
  async invoke(action: number, params: Map<number, Value> | Array<[number, Value]> = []): Promise<number> {
    const w = new Writer().u16(action);
    for (const [id, v] of params) w.u16(id).bytes(encodeValue(this.#control(id).typeCode, v));
    const reply = await this.request(OP.invoke, w.finish(), [STATUS.running]);
    return reply.status;
  }

  /** An enum's options, as the device lists them now. */
  async options(id: number): Promise<Array<{ value: number; label: string }>> {
    const reply = await this.request(OP.options, new Writer().u16(id).finish());
    const r = new Reader(reply.body);
    const count = r.u8();
    const out = [];
    for (let i = 0; i < count; i++) {
      const value = r.u16();
      out.push({ value, label: new TextDecoder().decode(r.take(r.u8())) });
    }
    return out;
  }

  // --- Authentication (PROTOCOL.md#authentication) --------------------------------

  /** Ask the device to show a pairing code. */
  async beginPairing(): Promise<PairingStarted> {
    const reply = await this.request(OP.authBegin, Uint8Array.of(AUTH.methodCode));
    const r = new Reader(reply.body);
    const started = { digits: r.u8(), attemptsLeft: r.u8(), expiresSeconds: r.u16() };
    this.#pairing = { salt: r.take(16).slice() };
    return started;
  }

  /** Try the code the user read off the device. Resolves with the new level;
   * a wrong code throws BlatError with status wrongCode and the attempts
   * left in `detail`. */
  async submitCode(code: string): Promise<number> {
    const pairing = this.#pairing;
    if (!pairing) throw new Error('Call beginPairing() first');
    const spake = new Spake2(codeToW(pairing.salt, code), HOST_IDENTITY, this.deviceId, pairing.salt);
    const shares = await this.request(OP.authCode, spake.pA);
    const pB = shares.body.subarray(0, 65);
    const { ke, confirmA, confirmB } = spake.finish(pB);
    const codeRight = equal(shares.body.subarray(65, 97), confirmB);
    // Send confirmA whatever happened: that's how the device counts the try.
    const reply = await this.request(OP.authConfirm, confirmA, [STATUS.wrongCode]);
    if (reply.status === STATUS.wrongCode) {
      if (reply.detail === 0) this.#pairing = null;
      throw new BlatError(OP.authConfirm, reply.status, reply.detail);
    }
    if (!codeRight) {
      // The device accepted our proof but its own is wrong: not the device
      // we think it is.
      this.close();
      throw new Error("The device's key confirmation didn't match");
    }
    this.#pairing = null;
    this.#startSession(hkdf(pairing.salt, ke, SESSION_INFO, 32), reply.body[0]);
    return this.level;
  }

  /** Ask to be remembered, so later connections can resume without a code.
   * Keep what it returns secret. */
  async remember(name: string): Promise<HostKey> {
    const n = encodeText(name).subarray(0, 23);
    const reply = await this.request(OP.remember, new Writer().u8(n.length).bytes(n).finish());
    const r = new Reader(reply.body);
    return { hostId: toHex(r.take(8)), hostKey: toHex(r.take(32)) };
  }

  /** Resume as a remembered host. Resolves with the level; throws BlatError
   * (wrongCode) if the device doesn't know this host any more. */
  async resume(key: HostKey): Promise<number> {
    const hostKey = fromHex(key.hostKey);
    const begun = await this.request(OP.authBegin, concat(Uint8Array.of(AUTH.methodResume), fromHex(key.hostId)));
    const challenge = begun.body.subarray(0, 16).slice();
    const nonce = random(16);
    const mac = hmac(hostKey, HOST_IDENTITY, challenge, nonce, this.deviceId);
    const reply = await this.request(OP.authResume, concat(nonce, mac));
    const expect = hmac(hostKey, DEVICE_LABEL, challenge, nonce, this.deviceId);
    if (!equal(reply.body.subarray(0, 32), expect)) {
      this.close();
      throw new Error("The device's resume MAC didn't match");
    }
    this.#startSession(hkdf(concat(challenge, nonce), hostKey, SESSION_INFO, 32), reply.body[32]);
    return this.level;
  }

  // New keys apply from the next message (the reply that brought them was
  // under the old session).
  #startSession(keys: Uint8Array, level: number) {
    this.#keys = { host: keys.slice(0, 16), device: keys.slice(16, 32) };
    this.#txRequests = this.#rxReplies = this.#rxEvents = 0;
    this.level = level;
    if (this.info) this.info.level = level;
  }

  get sealed(): boolean {
    return this.#keys !== null;
  }
}

