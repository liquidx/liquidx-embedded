// What the server's HTTP API sends and takes, shared with the web page.

import type { Control } from './blat/schema.ts';
import type { Value } from './blat/wire.ts';

export type { Control, Value };

export interface DeviceSummary {
  /** The server's id for it: the BLE peripheral id, or "sim". */
  id: string;
  name: string;
  rssi: number | null;
  connected: boolean;
  /** From Info, once connected. */
  deviceId: string | null;
  model: string | null;
  firmware: string | null;
  /** The connection's access level: 0 anyone, 1 paired, 2 a fresh code. */
  level: number;
  /** The server has a key to resume with, without a code. */
  remembered: boolean;
  /** The device is showing a pairing code for this server. */
  pairing: boolean;
}

export interface DeviceState extends DeviceSummary {
  controls: Control[];
  /** Keyed by control id. Secrets are true when set. */
  values: Record<number, Value>;
}

export interface PairingStarted {
  digits: number;
  attemptsLeft: number;
  expiresSeconds: number;
}

/** Server-sent events, on /api/events. */
export type ServerEvent =
  | { type: 'devices'; devices: DeviceSummary[]; scanning: boolean; canScan: boolean }
  | { type: 'device'; device: DeviceState }
  | { type: 'values'; id: string; values: Record<number, Value> }
  /** A simulated device's "screen": the code it shows, or null. */
  | { type: 'simCode'; id: string; code: string | null };

export interface ApiError {
  error: string;
  /** The blat status, when the device refused. */
  status?: number;
  /** Its detail: e.g. the level needed, or the attempts left. */
  detail?: number;
}
