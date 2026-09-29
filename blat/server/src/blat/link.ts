// Links carry blat messages between a host and one device: GATT over BLE
// (../ble.ts), or a byte stream with the stream framing
// (PROTOCOL.md#stream-transports).

import type { Readable, Writable } from 'node:stream';

export interface LinkHandlers {
  /** A message from the device on Reply, DataOut or Event. */
  onMessage(channel: number, data: Uint8Array): void;
  onClose(): void;
}

export interface Link {
  open(handlers: LinkHandlers): Promise<void>;
  /** A message to the device on Request or Data. */
  send(channel: number, data: Uint8Array): Promise<void>;
  /** The Info characteristic, or null on transports that have none (the host
   * gets Info from hello instead). */
  readInfo(): Promise<Uint8Array | null>;
  /** The largest message the link carries now. */
  maxMessage(): number;
  /** Disconnect. onClose follows. */
  close(): void;
}

/** The stream framing over a pair of streams: `u8 channel, u16 length
 * (little-endian), message`. */
export class FrameStream {
  #output: Writable;
  #buffer = new Uint8Array(0);

  constructor(input: Readable, output: Writable, onFrame: (channel: number, data: Uint8Array) => void, onEnd: () => void) {
    this.#output = output;
    input.on('data', (chunk: Buffer) => {
      const merged = new Uint8Array(this.#buffer.length + chunk.length);
      merged.set(this.#buffer);
      merged.set(chunk, this.#buffer.length);
      let at = 0;
      while (merged.length - at >= 3) {
        const length = merged[at + 1] | (merged[at + 2] << 8);
        if (merged.length - at < 3 + length) break;
        const channel = merged[at];
        const data = merged.slice(at + 3, at + 3 + length);
        at += 3 + length;
        onFrame(channel, data);
      }
      this.#buffer = merged.slice(at);
    });
    input.on('close', onEnd);
    input.on('error', onEnd);
  }

  write(channel: number, data: Uint8Array): Promise<void> {
    const frame = new Uint8Array(3 + data.length);
    frame[0] = channel;
    frame[1] = data.length & 0xff;
    frame[2] = data.length >> 8;
    frame.set(data, 3);
    return new Promise((resolve, reject) => this.#output.write(frame, (err) => (err ? reject(err) : resolve())));
  }

  end(): void {
    this.#output.end();
  }
}

/** A Link over a byte stream (a socket, a serial port). Frames on channels
 * 0xF0 and up aren't blat; they go to `onOther`. */
export class StreamLink implements Link {
  #input: Readable;
  #output: Writable;
  #stream: FrameStream | null = null;
  #handlers: LinkHandlers | null = null;
  #max: number;
  onOther: (channel: number, data: Uint8Array) => void = () => {};

  constructor(input: Readable, output: Writable, maxMessage = 512) {
    this.#input = input;
    this.#output = output;
    this.#max = maxMessage;
  }

  async open(handlers: LinkHandlers): Promise<void> {
    this.#handlers = handlers;
    this.#stream = new FrameStream(
      this.#input,
      this.#output,
      (channel, data) => (channel >= 0xf0 ? this.onOther(channel, data) : this.#handlers?.onMessage(channel, data)),
      () => this.#closed(),
    );
  }

  #closed() {
    const h = this.#handlers;
    this.#handlers = null;
    h?.onClose();
  }

  send(channel: number, data: Uint8Array): Promise<void> {
    if (!this.#handlers || !this.#stream) return Promise.reject(new Error('Not connected'));
    return this.#stream.write(channel, data);
  }

  async readInfo(): Promise<Uint8Array | null> {
    return null;
  }

  maxMessage(): number {
    return this.#max;
  }

  close(): void {
    this.#stream?.end();
    this.#closed();
  }
}
