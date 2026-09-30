// What the server keeps between runs: the keys devices gave it to resume
// with, and schemas it has read (by CRC, so reconnecting skips the transfer).

import { chmodSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import type { HostKey } from './blat/client.ts';

interface Remembered extends HostKey {
  name: string;
  savedAt: string;
}

export class Store {
  #dir: string;
  #hosts: Record<string, Remembered>;

  constructor(dir: string) {
    this.#dir = dir;
    mkdirSync(join(dir, 'schemas'), { recursive: true });
    const file = join(dir, 'hosts.json');
    this.#hosts = existsSync(file) ? JSON.parse(readFileSync(file, 'utf8')) : {};
  }

  /** The key to resume on this device (by its Info deviceId), if we have one. */
  hostKey(deviceId: string): HostKey | null {
    return this.#hosts[deviceId] ?? null;
  }

  saveHostKey(deviceId: string, name: string, key: HostKey): void {
    this.#hosts[deviceId] = { ...key, name, savedAt: new Date().toISOString() };
    this.#saveHosts();
  }

  forgetHostKey(deviceId: string): void {
    delete this.#hosts[deviceId];
    this.#saveHosts();
  }

  #saveHosts() {
    // Anyone with these keys can change the devices' settings.
    const file = join(this.#dir, 'hosts.json');
    writeFileSync(file, JSON.stringify(this.#hosts, null, 2), { mode: 0o600 });
    chmodSync(file, 0o600);
  }

  schema(crc: number): Uint8Array | null {
    const file = join(this.#dir, 'schemas', `${crc.toString(16).padStart(8, '0')}.bin`);
    return existsSync(file) ? new Uint8Array(readFileSync(file)) : null;
  }

  saveSchema(crc: number, blob: Uint8Array): void {
    writeFileSync(join(this.#dir, 'schemas', `${crc.toString(16).padStart(8, '0')}.bin`), blob);
  }
}
