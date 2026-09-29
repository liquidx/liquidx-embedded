// Types for the parts of the shared host library (../../js, plain JavaScript)
// that the server uses. The runtime is the library itself.

import * as blit from '../../js/blit.js';
import * as sim from '../../js/sim-display.js';

export { ENCODING, FORMAT, FORMAT_INFO, KEY, REFRESH, rasterizeRgba } from '../../js/blit.js';

export interface Caps {
  version: number;
  name: string;
  panel: { width: number; height: number } | null;
  width: number;
  height: number;
  formats: number[];
  encodings: number[];
  maxBytes: number;
  chunk: number;
  window: number;
  features: { persist: boolean; frameSleep: boolean; fastRefresh: boolean; pointer: boolean };
  regionAlign: number;
  keys: number[];
  minIntervalMs: number;
  refreshMs: number;
  battery: { percent: number | null; charging: boolean; external: boolean } | null;
}

export interface Region {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface SendOptions {
  format?: number;
  width: number;
  height: number;
  persist?: boolean;
  name?: string;
  nextFrameSeconds?: number;
  refresh?: number;
  skipUnchanged?: boolean;
  regions?: boolean;
  encoding?: 'auto' | number;
  reconnectTimeoutMs?: number;
  signal?: AbortSignal;
}

export interface SendResult {
  sleepSeconds: number;
  skipped: boolean;
  region?: Region | null;
  bytes?: number;
  encoding?: number;
}

/** What Blit needs from a link to a display (WebBluetoothTransport, SimTransport, NobleTransport). */
export interface Transport {
  readonly name: string | null;
  readonly connected: boolean;
  open(handlers: { onStatus(data: DataView): void; onEvent(data: DataView): void; onDisconnect(): void }): Promise<void>;
  readInfo(): Promise<DataView>;
  control(bytes: Uint8Array): Promise<void>;
  data(bytes: Uint8Array): Promise<void>;
  close(): void;
}

export interface Blit extends EventTarget {
  hostName: string;
  caps: Caps | null;
  readonly connected: boolean;
  readonly deviceName: string | null;
  connectTransport(transport: Transport): Promise<void>;
  reconnect(opts?: { timeoutMs?: number; retryMs?: number }): Promise<void>;
  disconnect(): void;
  readCaps(): Promise<Caps>;
  preferredFormat(): number;
  sendFrame(pixels: Uint8Array, opts: SendOptions): Promise<SendResult>;
}
export const Blit = blit.Blit as unknown as new (opts?: { hostName?: string }) => Blit;

export interface SimDisplayOptions {
  name?: string;
  width?: number;
  height?: number;
  panelWidth?: number;
  panelHeight?: number;
  formats?: number[];
  encodings?: number[];
  maxBytes?: number;
  chunk?: number;
  window?: number;
  regionAlign?: number;
  keys?: number[];
  pointer?: boolean;
  frameSleep?: boolean;
  minIntervalMs?: number;
  refreshMs?: number;
  version?: number;
}

export interface SimDisplay extends EventTarget {
  caps: Caps;
  frames: number;
  /** RGBA of what's on the panel, frame-area sized. */
  visible: Uint8ClampedArray;
  pressKey(key: number, action?: number): boolean;
  tap(x: number, y: number): boolean;
  setCaps(changes: Partial<Caps>): void;
}
export const SimDisplay = sim.SimDisplay as unknown as new (opts?: SimDisplayOptions) => SimDisplay;
export const SimTransport = sim.SimTransport as unknown as new (display: SimDisplay) => Transport;
