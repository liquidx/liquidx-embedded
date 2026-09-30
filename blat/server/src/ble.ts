// blat over Bluetooth LE for Node, with noble (the host is the GATT central).

import type { Characteristic, Noble, Peripheral, Service } from '@stoprocent/noble';

import type { Link, LinkHandlers } from './blat/link.ts';
import { CHANNEL, CHARACTERISTIC, SERVICE } from './blat/wire.ts';

// noble writes UUIDs as lowercase hex without dashes.
const uuid = (u: string) => u.replace(/-/g, '').toLowerCase();

let nobleInstance: Promise<Noble> | null = null;

/** noble, loaded on first use: loading it opens the Bluetooth adapter. */
function loadNoble(): Promise<Noble> {
  nobleInstance ??= import('@stoprocent/noble').then((m) => m.default);
  return nobleInstance;
}

export interface Seen {
  id: string;
  name: string;
  rssi: number;
  peripheral: Peripheral;
}

/**
 * Scans for devices with the blat service. Devices may put its UUID in the
 * advertisement or the scan response (the X4 does the latter), so the scan
 * isn't filtered by service: each advertisement is checked once noble has
 * merged the scan response in.
 */
export class Scanner {
  #noble: Noble | null = null;
  #scanning = false;
  #timer: NodeJS.Timeout | undefined;
  onSeen: (seen: Seen) => void = () => {};
  onScanning: (scanning: boolean) => void = () => {};

  get scanning(): boolean {
    return this.#scanning;
  }

  async start(seconds: number): Promise<void> {
    const noble = (this.#noble ??= await loadNoble());
    await noble.waitForPoweredOnAsync(10000).catch(() => {
      throw new Error(
        `Bluetooth isn't available (adapter state: ${noble.state}). On macOS, allow Bluetooth for your terminal in System Settings → Privacy & Security.`,
      );
    });
    clearTimeout(this.#timer);
    this.#timer = setTimeout(() => void this.stop(), seconds * 1000);
    if (this.#scanning) return;
    noble.removeAllListeners('discover');
    noble.on('discover', (p: Peripheral) => {
      if (!p.advertisement.serviceUuids?.includes(uuid(SERVICE))) return;
      this.onSeen({ id: p.id, name: p.advertisement.localName || p.id, rssi: p.rssi, peripheral: p });
    });
    await noble.startScanningAsync([], true);
    this.#scanning = true;
    this.onScanning(true);
  }

  async stop(): Promise<void> {
    clearTimeout(this.#timer);
    if (!this.#scanning || !this.#noble) return;
    this.#scanning = false;
    await this.#noble.stopScanningAsync().catch(() => {});
    this.onScanning(false);
  }
}

type Chars = Record<'info' | 'request' | 'reply' | 'data' | 'dataOut' | 'event', Characteristic>;

/** The blat GATT service on one peripheral. */
export class NobleLink implements Link {
  #peripheral: Peripheral;
  #chars: Chars | null = null;
  #handlers: LinkHandlers | null = null;

  constructor(peripheral: Peripheral) {
    this.#peripheral = peripheral;
  }

  async open(handlers: LinkHandlers): Promise<void> {
    const p = this.#peripheral;
    this.#handlers = handlers;
    if (p.state !== 'connected') await p.connectAsync().catch((e) => Promise.reject(asError(e)));
    p.once('disconnect', () => this.#closed());
    const characteristics = await this.#discover();
    const need = (key: keyof typeof CHARACTERISTIC) => {
      const c = characteristics.find((c) => c.uuid === uuid(CHARACTERISTIC[key]));
      if (!c) throw new Error(`Device has no blat ${key} characteristic`);
      return c;
    };
    const chars: Chars = {
      info: need('info'),
      request: need('request'),
      reply: need('reply'),
      data: need('data'),
      dataOut: need('dataOut'),
      event: need('event'),
    };
    // Listeners live as long as the characteristic objects; reconnecting
    // rediscovers them, so drop any from the last connection first.
    for (const [key, channel] of [['reply', CHANNEL.reply], ['dataOut', CHANNEL.dataOut], ['event', CHANNEL.event]] as const) {
      chars[key].removeAllListeners('data').on('data', (d: Buffer) => this.#handlers?.onMessage(channel, new Uint8Array(d)));
      await chars[key].subscribeAsync();
    }
    this.#chars = chars;
  }

  // Asking for the service by UUID sometimes finds nothing just after
  // connecting (the GATT table isn't cached yet), so fall back to every service.
  async #discover(): Promise<Characteristic[]> {
    const p = this.#peripheral;
    try {
      const find = (services: Service[]) => services.find((s) => s.uuid === uuid(SERVICE));
      const service = find(await p.discoverServicesAsync([uuid(SERVICE)])) ?? find(await p.discoverServicesAsync());
      if (!service) throw new Error("Device doesn't have the blat service");
      return await service.discoverCharacteristicsAsync();
    } catch (err) {
      p.disconnectAsync().catch(() => {});
      throw asError(err);
    }
  }

  #closed() {
    this.#chars = null;
    const h = this.#handlers;
    this.#handlers = null;
    h?.onClose();
  }

  async send(channel: number, data: Uint8Array): Promise<void> {
    const chars = this.#chars;
    if (!chars) throw new Error('Not connected');
    // Request is write-with-response; Data is write-without-response.
    if (channel === CHANNEL.request) await chars.request.writeAsync(Buffer.from(data), false);
    else await chars.data.writeAsync(Buffer.from(data), true);
  }

  async readInfo(): Promise<Uint8Array | null> {
    if (!this.#chars) throw new Error('Not connected');
    return new Uint8Array(await this.#chars.info.readAsync());
  }

  maxMessage(): number {
    const mtu = (this.#peripheral as { mtu?: number | null }).mtu ?? 23;
    return Math.min(mtu - 3, 512);
  }

  close(): void {
    this.#peripheral.disconnectAsync().catch(() => {});
  }
}

// noble rejects with strings as well as Errors.
function asError(err: unknown): Error {
  return err instanceof Error ? err : new Error(String(err));
}
