// Talking to blat-server: JSON requests, and server-sent events.

import type { ApiError, DeviceState, PairingStarted, ServerEvent, Value } from '../src/api.ts';

export class RequestError extends Error {
  status?: number; // blat status, when the device refused
  detail?: number;
  constructor(body: ApiError) {
    super(body.error);
    this.status = body.status;
    this.detail = body.detail;
  }
}

async function call<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(path, {
    method,
    headers: body === undefined ? {} : { 'content-type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const json = await res.json().catch(() => ({ error: `${res.status} ${res.statusText}` }));
  if (!res.ok) throw new RequestError(json);
  return json as T;
}

const device = (id: string) => `/api/devices/${encodeURIComponent(id)}`;

export const api = {
  scan: () => call<{ scanning: boolean }>('POST', '/api/scan', {}),
  state: (id: string) => call<DeviceState>('GET', device(id)),
  connect: (id: string) => call<DeviceState>('POST', `${device(id)}/connect`, {}),
  disconnect: (id: string) => call<object>('POST', `${device(id)}/disconnect`, {}),
  pair: (id: string) => call<PairingStarted>('POST', `${device(id)}/pair`, {}),
  code: (id: string, code: string) => call<DeviceState>('POST', `${device(id)}/code`, { code }),
  forget: (id: string) => call<DeviceState>('POST', `${device(id)}/forget`, {}),
  set: (id: string, values: Record<number, Value>) =>
    call<Record<number, Value>>('PUT', `${device(id)}/values`, { values }),
  invoke: (id: string, action: number, params: Record<number, Value>) =>
    call<{ status: number }>('POST', `${device(id)}/invoke`, { action, params }),
};

/** Server-sent events, reconnecting on their own (EventSource does). */
export function listen(onEvent: (e: ServerEvent) => void, onOnline: (online: boolean) => void): void {
  const source = new EventSource('/api/events');
  source.onopen = () => onOnline(true);
  source.onerror = () => onOnline(false);
  source.onmessage = (m) => onEvent(JSON.parse(m.data));
}
