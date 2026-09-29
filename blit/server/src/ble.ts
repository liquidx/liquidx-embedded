// A blit transport over Bluetooth LE for Node, using noble (the host is the
// GATT central). It has the same shape as WebBluetoothTransport in
// ../../js/blit.js, so the Blit class drives it unchanged.

import type { Characteristic, Noble, Peripheral, Service } from '@stoprocent/noble';

import { CHARACTERISTIC, SERVICE } from '../../js/protocol.js';

export interface TransportHandlers {
  onStatus(data: DataView): void;
  onEvent(data: DataView): void;
  onDisconnect(): void;
}

// noble writes UUIDs as lowercase hex without dashes.
const uuid = (u: string) => u.replace(/-/g, '').toLowerCase();

let nobleInstance: Promise<Noble> | null = null;

/** noble, loaded on first use: loading it opens the Bluetooth adapter. */
function loadNoble(): Promise<Noble> {
  nobleInstance ??= import('@stoprocent/noble').then((m) => m.default);
  return nobleInstance;
}

export interface FindOptions {
  /** Match the advertised name (case-insensitive substring), id or address. */
  device?: string;
  timeoutMs?: number;
  log?: (text: string) => void;
}

/** Scan for a display advertising the blit service. */
export async function findDisplay({ device, timeoutMs = 30000, log = () => {} }: FindOptions = {}): Promise<NobleTransport> {
  const noble = await loadNoble();
  await noble.waitForPoweredOnAsync(10000).catch(() => {
    throw new Error(`Bluetooth isn't available (adapter state: ${noble.state}). On macOS, allow Bluetooth for your terminal in System Settings → Privacy & Security.`);
  });
  const want = device?.toLowerCase();
  // The scan filter isn't enough: macOS also reports nearby Apple devices.
  const isDisplay = (p: Peripheral) => p.advertisement.serviceUuids?.includes(uuid(SERVICE));
  const matches = (p: Peripheral) => !want
    || p.advertisement.localName?.toLowerCase().includes(want)
    || p.id.toLowerCase() === want
    || p.address?.toLowerCase() === want;

  log(`Scanning for ${device ? `"${device}"` : 'a blit display'}…`);
  let timer: NodeJS.Timeout | undefined;
  const timeout = new Promise<never>((_, reject) => {
    timer = setTimeout(() => reject(new Error(`No blit display found in ${Math.round(timeoutMs / 1000)} s`)), timeoutMs);
  });
  const scan = (async () => {
    await noble.startScanningAsync([uuid(SERVICE)], false);
    for await (const p of noble.discoverAsync()) {
      if (!isDisplay(p)) continue;
      if (matches(p)) return p;
      log(`Ignoring ${p.advertisement.localName || p.id}`);
    }
    throw new Error('Scan stopped');
  })();
  try {
    return new NobleTransport(noble, await Promise.race([scan, timeout]));
  } finally {
    clearTimeout(timer);
    await noble.stopScanningAsync().catch(() => {});
  }
}

export class NobleTransport {
  #chars: Record<'info' | 'control' | 'data' | 'status', Characteristic> & { event: Characteristic | null } | null = null;
  #handlers: TransportHandlers | null = null;
  readonly noble: Noble;
  readonly peripheral: Peripheral;

  constructor(noble: Noble, peripheral: Peripheral) {
    this.noble = noble;
    this.peripheral = peripheral;
    peripheral.on('disconnect', () => {
      if (!this.#chars) return;
      this.#chars = null;
      this.#handlers?.onDisconnect();
    });
  }

  get name(): string | null {
    return this.peripheral.advertisement.localName || null;
  }

  get connected(): boolean {
    return this.peripheral.state === 'connected' && !!this.#chars;
  }

  async open(handlers: TransportHandlers): Promise<void> {
    this.#handlers = handlers;
    const p = this.peripheral;
    try {
      if (p.state !== 'connected') await p.connectAsync();
    } catch (err) {
      throw asError(err);
    }
    const characteristics = await this.#discover();
    const byUuid = (u: string) => characteristics.find((c) => c.uuid === uuid(u)) ?? null;
    const need = (key: keyof typeof CHARACTERISTIC) => {
      const c = byUuid(CHARACTERISTIC[key]);
      if (!c) throw new Error(`Display has no ${key} characteristic`);
      return c;
    };
    const chars = {
      info: need('info'), control: need('control'), data: need('data'), status: need('status'), event: need('event'),
    };
    // Listeners live as long as the characteristic objects; reconnecting
    // rediscovers them, so drop any from the last connection first.
    chars.status.removeAllListeners('data').on('data', (d: Buffer) => this.#handlers?.onStatus(asView(d)));
    await chars.status.subscribeAsync();
    chars.event.removeAllListeners('data').on('data', (d: Buffer) => this.#handlers?.onEvent(asView(d)));
    await chars.event.subscribeAsync();
    this.#chars = chars;
  }

  // The blit service's characteristics. Asking for the service by UUID
  // sometimes finds nothing just after connecting (the display's GATT table
  // isn't cached yet), so fall back to discovering every service.
  async #discover(): Promise<Characteristic[]> {
    const p = this.peripheral;
    try {
      const find = (services: Service[]) => services.find((s) => s.uuid === uuid(SERVICE));
      const service = find(await p.discoverServicesAsync([uuid(SERVICE)])) ?? find(await p.discoverServicesAsync());
      if (!service) throw new Error("Display doesn't have the blit service");
      return await service.discoverCharacteristicsAsync();
    } catch (err) {
      p.disconnectAsync().catch(() => {});
      throw asError(err);
    }
  }

  async readInfo(): Promise<DataView> {
    return asView(await this.#need().info.readAsync());
  }

  control(bytes: Uint8Array): Promise<void> {
    return this.#need().control.writeAsync(Buffer.from(bytes), false);
  }

  data(bytes: Uint8Array): Promise<void> {
    return this.#need().data.writeAsync(Buffer.from(bytes), true);
  }

  close(): void {
    this.peripheral.disconnectAsync().catch(() => {});
  }

  #need() {
    if (!this.#chars) throw new Error('Not connected');
    return this.#chars;
  }
}

// noble rejects with strings as well as Errors.
function asError(err: unknown): Error {
  return err instanceof Error ? err : new Error(String(err));
}

function asView(b: Uint8Array): DataView {
  return new DataView(b.buffer, b.byteOffset, b.byteLength);
}
