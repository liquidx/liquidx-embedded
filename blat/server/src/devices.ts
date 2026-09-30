// The devices the server knows about, and its connection to each.

import type { DeviceState, DeviceSummary, PairingStarted, ServerEvent, Value } from './api.ts';
import { BlatClient, BlatError } from './blat/client.ts';
import type { Link } from './blat/link.ts';
import { STATUS, crc32 } from './blat/wire.ts';
import type { Store } from './store.ts';

/** Where a device came from: a BLE scan, or the simulator. */
export interface Source {
  id: string;
  name: string;
  rssi: number | null;
  link(): Link;
}

interface Entry {
  source: Source;
  client: BlatClient | null;
  connecting: Promise<DeviceState> | null;
  values: Map<number, Value>;
  pairing: boolean;
}

export interface ManagerOptions {
  store: Store;
  /** How the server names itself to devices (hello, and when remembered). */
  hostName: string;
  /** There's a Bluetooth scanner (not just the simulator). */
  canScan?: boolean;
  log?: (text: string) => void;
}

export class DeviceManager {
  #entries = new Map<string, Entry>();
  #listeners = new Set<(e: ServerEvent) => void>();
  #options: Required<ManagerOptions>;
  scanning = false;

  constructor(options: ManagerOptions) {
    this.#options = { log: () => {}, canScan: false, ...options };
  }

  // --- Events ---------------------------------------------------------------

  subscribe(listener: (e: ServerEvent) => void): () => void {
    this.#listeners.add(listener);
    return () => this.#listeners.delete(listener);
  }

  emit(event: ServerEvent): void {
    for (const l of this.#listeners) l(event);
  }

  #emitDevices() {
    this.emit(this.devicesEvent());
  }

  #emitDevice(entry: Entry) {
    this.emit({ type: 'device', device: this.#state(entry) });
    this.#emitDevices();
  }

  // --- Devices ----------------------------------------------------------------

  /** A device was found (or seen again). */
  add(source: Source): void {
    const entry = this.#entries.get(source.id);
    if (entry) {
      entry.source = source; // newer name, RSSI
    } else {
      this.#entries.set(source.id, { source, client: null, connecting: null, values: new Map(), pairing: false });
    }
    this.#emitDevices();
  }

  setScanning(scanning: boolean): void {
    this.scanning = scanning;
    this.#emitDevices();
  }

  devicesEvent(): ServerEvent {
    return { type: 'devices', devices: this.list(), scanning: this.scanning, canScan: this.#options.canScan };
  }

  list(): DeviceSummary[] {
    return [...this.#entries.values()].map((e) => this.#summary(e));
  }

  #entry(id: string): Entry {
    const entry = this.#entries.get(id);
    if (!entry) throw new NotFound(`No device ${id}`);
    return entry;
  }

  #summary(entry: Entry): DeviceSummary {
    const c = entry.client?.connected ? entry.client : null;
    const info = c?.info ?? null;
    return {
      id: entry.source.id,
      name: info?.name || entry.source.name,
      rssi: entry.source.rssi,
      connected: !!c,
      deviceId: info?.deviceId ?? null,
      model: info?.model ?? null,
      firmware: info?.firmware ?? null,
      level: c?.level ?? 0,
      remembered: !!(info && this.#options.store.hostKey(info.deviceId)),
      pairing: entry.pairing,
    };
  }

  #state(entry: Entry): DeviceState {
    return {
      ...this.#summary(entry),
      controls: entry.client?.connected ? entry.client.controls : [],
      values: Object.fromEntries(entry.values),
    };
  }

  state(id: string): DeviceState {
    return this.#state(this.#entry(id));
  }

  #client(id: string): { entry: Entry; client: BlatClient } {
    const entry = this.#entry(id);
    if (!entry.client?.connected) throw new NotConnected(`${entry.source.name} isn't connected`);
    return { entry, client: entry.client };
  }

  // --- Connecting ---------------------------------------------------------------

  /** Connect: hello, resume if we're remembered, the schema (cached by CRC),
   * then every value we may read. */
  connect(id: string): Promise<DeviceState> {
    const entry = this.#entry(id);
    if (entry.client?.connected) return Promise.resolve(this.#state(entry));
    entry.connecting ??= this.#connect(entry).finally(() => (entry.connecting = null));
    return entry.connecting;
  }

  async #connect(entry: Entry): Promise<DeviceState> {
    const { store, hostName, log } = this.#options;
    const client = new BlatClient(entry.source.link(), { hostName });
    client.onChanged = (values) => {
      for (const [k, v] of values) entry.values.set(k, v);
      this.emit({ type: 'values', id: entry.source.id, values: Object.fromEntries(values) });
    };
    client.onClose = () => {
      if (entry.client !== client) return;
      entry.pairing = false;
      log(`${entry.source.name}: disconnected`);
      this.#emitDevice(entry);
    };
    try {
      const info = await client.connect();
      entry.client = client;
      log(`${info.name}: connected (${info.model} ${info.firmware})`);

      const key = store.hostKey(info.deviceId);
      if (key) {
        try {
          await client.resume(key);
          log(`${info.name}: resumed at level ${client.level}`);
        } catch (err) {
          if (!(err instanceof BlatError) || err.status !== STATUS.wrongCode) throw err;
          // The device forgot us (a reset, or it remembered too many hosts).
          store.forgetHostKey(info.deviceId);
          log(`${info.name}: no longer remembers this server; pair again`);
        }
      }

      const cached = store.schema(info.schemaCrc);
      if (cached && crc32(cached) === info.schemaCrc) {
        client.useSchema(cached);
      } else {
        store.saveSchema(info.schemaCrc, await client.readSchema());
      }
      entry.values = await client.get();
      this.#emitDevice(entry);
      return this.#state(entry);
    } catch (err) {
      client.close();
      if (entry.client === client) entry.client = null;
      throw err;
    }
  }

  disconnect(id: string): void {
    const entry = this.#entry(id);
    entry.client?.close();
  }

  // --- Pairing --------------------------------------------------------------------

  /** Ask the device to show a code. */
  async pair(id: string): Promise<PairingStarted> {
    const { entry, client } = this.#client(id);
    const started = await client.beginPairing();
    entry.pairing = true;
    this.#emitDevices();
    return started;
  }

  /** The code the user read off the device. On success the server asks to be
   * remembered, and reads values again: a higher level may see more. */
  async code(id: string, code: string): Promise<DeviceState> {
    const { entry, client } = this.#client(id);
    try {
      await client.submitCode(code);
    } catch (err) {
      if (err instanceof BlatError && err.detail === 0) entry.pairing = false;
      this.#emitDevices();
      throw err;
    }
    entry.pairing = false;
    const info = client.info!;
    this.#options.log(`${info.name}: paired, level ${client.level}`);
    if (info.methods & 0x04) {
      this.#options.store.saveHostKey(info.deviceId, info.name, await client.remember(this.#options.hostName));
    }
    entry.values = await client.get();
    this.#emitDevice(entry);
    return this.#state(entry);
  }

  /** Stop resuming this device without a code. */
  forget(id: string): DeviceState {
    const { entry, client } = this.#client(id);
    this.#options.store.forgetHostKey(client.info!.deviceId);
    this.#emitDevice(entry);
    return this.#state(entry);
  }

  // --- Values and actions -----------------------------------------------------------

  /** Change values (all or none). Returns what the device reports after. */
  async set(id: string, values: Record<string, Value>): Promise<Record<number, Value>> {
    const { entry, client } = this.#client(id);
    const pairs = Object.entries(values).map(([k, v]) => [Number(k), v] as [number, Value]);
    await client.set(pairs);
    // Read back: secrets as set / not set, and what the device made of it.
    const after = await client.get(pairs.map(([k]) => k));
    for (const [k, v] of after) entry.values.set(k, v);
    const out = Object.fromEntries(after);
    this.emit({ type: 'values', id, values: out });
    return out;
  }

  async invoke(id: string, action: number, params: Record<string, Value> = {}): Promise<number> {
    const { client } = this.#client(id);
    return client.invoke(action, Object.entries(params).map(([k, v]) => [Number(k), v] as [number, Value]));
  }

  close(): void {
    for (const e of this.#entries.values()) e.client?.close();
  }
}

export class NotFound extends Error {}
export class NotConnected extends Error {}
