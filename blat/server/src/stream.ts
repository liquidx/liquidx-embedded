// The stream framing (PROTOCOL.md#stream-transports) over Node streams, as
// the simulated device speaks it on stdin and stdout.

import type { Readable, Writable } from 'node:stream';

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
