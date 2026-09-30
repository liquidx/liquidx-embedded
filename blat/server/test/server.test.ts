// The HTTP API end to end, against the simulated device.
//   make -C blat/firmware/test sim   (once)
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, readdirSync, rmSync, statSync } from 'node:fs';
import type { Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, before, test } from 'node:test';

import type { DeviceState, ServerEvent } from '../src/api.ts';
import { DeviceManager } from '../src/devices.ts';
import { createApp } from '../src/http.ts';
import { SimDevice } from '../src/sim.ts';
import { Store } from '../src/store.ts';

let server: Server;
let base: string;
let sim: SimDevice;
let manager: DeviceManager;
let dataDir: string;
const events: ServerEvent[] = [];
let stopEvents: () => void;

before(async () => {
  dataDir = mkdtempSync(join(tmpdir(), 'blat-server-test-'));
  manager = new DeviceManager({ store: new Store(dataDir), hostName: 'test server' });
  sim = new SimDevice(undefined, { onCode: (code) => manager.emit({ type: 'simCode', id: 'sim', code }) });
  manager.add({ id: 'sim', name: 'Sim', rssi: null, link: () => sim.link() });
  server = (await createApp({ manager, scanner: null, dev: false })).listen(0);
  await new Promise((r) => server.once('listening', r));
  base = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  stopEvents = await readEvents();
});

after(() => {
  stopEvents();
  manager.close();
  sim.close();
  server.close();
  rmSync(dataDir, { recursive: true, force: true });
});

// Server-sent events, collected into `events`.
async function readEvents(): Promise<() => void> {
  const abort = new AbortController();
  const res = await fetch(`${base}/api/events`, { signal: abort.signal });
  const reader = res.body!.getReader();
  const decoder = new TextDecoder();
  let buffer = '';
  void (async () => {
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) return;
        buffer += decoder.decode(value, { stream: true });
        let end;
        while ((end = buffer.indexOf('\n\n')) >= 0) {
          const block = buffer.slice(0, end);
          buffer = buffer.slice(end + 2);
          const data = block.split('\n').find((l) => l.startsWith('data: '));
          if (data) events.push(JSON.parse(data.slice(6)));
        }
      }
    } catch {
      // aborted
    }
  })();
  return () => abort.abort();
}

async function call<T = unknown>(method: string, path: string, body?: unknown, headers: Record<string, string> = {}) {
  const res = await fetch(`${base}${path}`, {
    method,
    headers: body === undefined ? headers : { 'content-type': 'application/json', ...headers },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  return { code: res.status, body: (await res.json()) as T & { error?: string; status?: number; detail?: number } };
}

const until = async (check: () => boolean, ms = 2000) => {
  const end = Date.now() + ms;
  while (!check()) {
    if (Date.now() > end) throw new Error('Timed out waiting');
    await new Promise((r) => setTimeout(r, 20));
  }
};

const id = (d: DeviceState, key: string) => d.controls.find((c) => c.key === key)!.id;

test('connect, pair, change values, and events', async () => {
  const list = await call<Array<{ id: string }>>('GET', '/api/devices');
  assert.deepEqual(list.body.map((d) => d.id), ['sim']);
  assert.ok(events.some((e) => e.type === 'devices'));

  let { code, body: d } = await call<DeviceState>('POST', '/api/devices/sim/connect', {});
  assert.equal(code, 200);
  assert.equal(d.level, 0);
  assert.equal(d.deviceId, '5a11000000000001');
  const mode = id(d, 'display.mode');
  assert.equal(d.values[mode], 1);

  // Not paired: the device refuses, and the API says why.
  const refused = await call('PUT', '/api/devices/sim/values', { values: { [mode]: 2 } });
  assert.equal(refused.code, 403);
  assert.equal(refused.body.status, 15);
  assert.equal(refused.body.detail, 1);

  // Pair with the code the simulated device "shows".
  const started = await call<{ digits: number }>('POST', '/api/devices/sim/pair', {});
  assert.equal(started.body.digits, 6);
  await until(() => sim.code !== null);
  assert.ok(events.some((e) => e.type === 'simCode' && e.code === sim.code));
  const wrong = await call('POST', '/api/devices/sim/code', { code: sim.code === '000000' ? '111111' : '000000' });
  assert.equal(wrong.code, 403);
  assert.equal(wrong.body.status, 16);
  assert.equal(wrong.body.detail, 2);
  ({ body: d } = await call<DeviceState>('POST', '/api/devices/sim/code', { code: sim.code }));
  assert.equal(d.level, 2);
  assert.equal(d.remembered, true);
  // The key is saved, readable only by us.
  const hosts = JSON.parse(readFileSync(join(dataDir, 'hosts.json'), 'utf8'));
  assert.ok(hosts['5a11000000000001'].hostKey);
  assert.equal(statSync(join(dataDir, 'hosts.json')).mode & 0o077, 0);

  // Change values; the reply is what the device reports after.
  const set = await call('PUT', '/api/devices/sim/values', {
    values: { [mode]: 2, [id(d, 'wifi.password')]: 'correct horse' },
  });
  assert.equal(set.code, 200);
  assert.deepEqual(set.body, { [mode]: 2, [id(d, 'wifi.password')]: true });
  const invalid = await call('PUT', '/api/devices/sim/values', { values: { [id(d, 'display.brightness')]: 52 } });
  assert.equal(invalid.code, 422);
  assert.equal(invalid.body.error, 'invalid value');

  // A live value changed on the device arrives as an event.
  const battery = id(d, 'power.battery');
  await sim.setValue(battery, 37);
  await until(() => events.some((e) => e.type === 'values' && e.values[battery] === 37));

  // Actions.
  const invoked = await call<{ status: number }>('POST', '/api/devices/sim/invoke', {
    action: id(d, 'display.message'),
    params: { [id(d, 'display.message.text')]: 'Hello' },
  });
  assert.equal(invoked.code, 200);
  assert.equal(invoked.body.status, 0);
});

test('reconnecting resumes without a code, and uses the cached schema', async () => {
  await call('POST', '/api/devices/sim/disconnect', {});
  await until(() => !manager.list()[0].connected);
  // Read on the first connection, cached by CRC for this one.
  assert.equal(readdirSync(join(dataDir, 'schemas')).length, 1);
  const { body: d } = await call<DeviceState>('POST', '/api/devices/sim/connect', {});
  assert.equal(d.level, 1);
  assert.equal(d.values[id(d, 'display.mode')], 2); // kept by the device
  assert.equal(d.values[id(d, 'device.name')], 'Sim'); // level 1 reads more
  // Wi-Fi needs a fresh code.
  const refused = await call('PUT', '/api/devices/sim/values', { values: { [id(d, 'wifi.ssid')]: 'Work' } });
  assert.equal(refused.body.detail, 2);
});

test('refuses cross-site and non-JSON writes', async () => {
  const cross = await call('POST', '/api/devices/sim/pair', {}, { origin: 'https://evil.example' });
  assert.equal(cross.code, 403);
  const form = await fetch(`${base}/api/devices/sim/pair`, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: 'a=1',
  });
  assert.equal(form.status, 415);
  const missing = await call('POST', '/api/devices/nope/connect', {});
  assert.equal(missing.code, 404);
});
