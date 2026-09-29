// The simulated device from ../firmware/test (sim_device.cpp): the real
// firmware library, run as a child process and spoken to over the stream
// framing. Build it with `make -C blat/firmware/test sim`.

import { type ChildProcess, spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { FrameStream, type Link, type LinkHandlers } from './blat/link.ts';

export const DEFAULT_SIM = fileURLToPath(new URL('../../firmware/test/build/sim_device', import.meta.url));

// sim_device's test-only channels, both ways.
const SIM_CONNECT = 0xf0; // host → sim: u16 max message size
const SIM_DISCONNECT = 0xf1;
const SIM_SET = 0xf2; // host → sim: the firmware sets a value
const SIM_CLOCK = 0xf3; // host → sim: move the clock forward
const SIM_CODE = 0xf0; // sim → host: the code it shows (empty: hidden)
const SIM_INVOKED = 0xf1; // sim → host: an action ran

export interface SimHandlers {
  /** The code the device would show on its screen, or null when hidden. */
  onCode?(code: string | null): void;
  onInvoked?(action: number, params: number): void;
}

/**
 * One simulated device. It keeps running between connections, so saved
 * values and remembered hosts last until close(), as on a real device.
 */
export class SimDevice {
  #proc: ChildProcess;
  #stream: FrameStream;
  #handlers: SimHandlers;
  #current: SimConnection | null = null;
  code: string | null = null;

  constructor(path = DEFAULT_SIM, handlers: SimHandlers = {}) {
    if (!existsSync(path)) {
      throw new Error(`No simulated device at ${path}. Build it: make -C blat/firmware/test sim`);
    }
    this.#handlers = handlers;
    this.#proc = spawn(path, [], { stdio: ['pipe', 'pipe', 'inherit'] });
    this.#stream = new FrameStream(
      this.#proc.stdout!,
      this.#proc.stdin!,
      (channel, data) => this.#frame(channel, data),
      () => this.#current?.closed(),
    );
  }

  #frame(channel: number, data: Uint8Array) {
    if (channel === SIM_CODE) {
      this.code = data.length ? new TextDecoder().decode(data) : null;
      this.#handlers.onCode?.(this.code);
    } else if (channel === SIM_INVOKED && data.length >= 3) {
      this.#handlers.onInvoked?.(data[0] | (data[1] << 8), data[2]);
    } else {
      this.#current?.message(channel, data);
    }
  }

  /** A Link for one connection. `maxMessage` plays the part of the MTU. */
  link(maxMessage = 244): Link {
    return new SimConnection(this, maxMessage);
  }

  /** The firmware changes a value, e.g. a sensor reading. */
  setValue(id: number, value: number): Promise<void> {
    const b = new Uint8Array(6);
    const v = new DataView(b.buffer);
    v.setUint16(0, id, true);
    v.setInt32(2, value, true);
    return this.#stream.write(SIM_SET, b);
  }

  /** Move the device's clock forward (code expiry, lockouts). */
  advanceClock(ms: number): Promise<void> {
    const b = new Uint8Array(4);
    new DataView(b.buffer).setUint32(0, ms, true);
    return this.#stream.write(SIM_CLOCK, b);
  }

  close(): void {
    this.#current?.closed();
    this.#proc.kill();
  }

  // For SimConnection.
  async connect(connection: SimConnection, maxMessage: number): Promise<void> {
    if (this.#current) this.#current.closed(); // one host at a time, like the X4
    this.#current = connection;
    const b = new Uint8Array(2);
    new DataView(b.buffer).setUint16(0, maxMessage, true);
    await this.#stream.write(SIM_CONNECT, b);
  }

  disconnect(connection: SimConnection): void {
    if (this.#current !== connection) return;
    this.#current = null;
    void this.#stream.write(SIM_DISCONNECT, new Uint8Array(0));
  }

  write(channel: number, data: Uint8Array): Promise<void> {
    return this.#stream.write(channel, data);
  }
}

class SimConnection implements Link {
  #sim: SimDevice;
  #max: number;
  #handlers: LinkHandlers | null = null;

  constructor(sim: SimDevice, maxMessage: number) {
    this.#sim = sim;
    this.#max = maxMessage;
  }

  async open(handlers: LinkHandlers): Promise<void> {
    this.#handlers = handlers;
    await this.#sim.connect(this, this.#max);
  }

  send(channel: number, data: Uint8Array): Promise<void> {
    if (!this.#handlers) return Promise.reject(new Error('Not connected'));
    return this.#sim.write(channel, data);
  }

  async readInfo(): Promise<Uint8Array | null> {
    return null; // a stream: Info comes from hello
  }

  maxMessage(): number {
    return this.#max;
  }

  close(): void {
    this.#sim.disconnect(this);
    this.closed();
  }

  message(channel: number, data: Uint8Array) {
    this.#handlers?.onMessage(channel, data);
  }

  closed() {
    const h = this.#handlers;
    this.#handlers = null;
    h?.onClose();
  }
}
