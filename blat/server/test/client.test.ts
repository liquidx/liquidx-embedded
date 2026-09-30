// BlatClient against the real firmware library, run as the simulated device
// (../../firmware/test/sim_device.cpp). Build that first:
//   make -C blat/firmware/test sim
import assert from 'node:assert/strict';
import { after, test } from 'node:test';

import { BlatClient, BlatError } from '../src/blat/client.ts';
import type { Link, LinkHandlers } from '../src/blat/link.ts';
import type { Control } from '../src/blat/schema.ts';
import { CHANNEL, INFO, OP, STATUS } from '../src/blat/wire.ts';
import { SimDevice } from '../src/sim.ts';

const sims: SimDevice[] = [];
after(() => sims.forEach((s) => s.close()));

function sim() {
  const s = new SimDevice();
  sims.push(s);
  return s;
}

const byKey = (client: BlatClient, key: string) => {
  const c = client.controls.find((c) => c.key === key);
  assert.ok(c, key);
  return c.id;
};

const tick = (ms = 50) => new Promise((r) => setTimeout(r, ms));

async function connect(s: SimDevice, maxMessage = 244) {
  const client = new BlatClient(s.link(maxMessage), { hostName: 'test', timeoutMs: 3000 });
  const info = await client.connect();
  return { client, info };
}

test('hello, schema and public values', async () => {
  const s = sim();
  const { client, info } = await connect(s);
  assert.equal(info.name, 'Sim-0001');
  assert.equal(info.model, 'blat-sim');
  assert.equal(info.deviceId, '5a11000000000001');
  assert.equal(info.level, 0);
  await client.readSchema();
  const mode = client.controls.find((c) => c.key === 'display.mode')!;
  assert.equal(mode.type, 'enum');
  assert.deepEqual(mode.options.map((o) => o.label), ['Off', 'Eco', 'Full']);
  assert.equal(mode.default, 1);
  assert.ok(mode.live);
  const bright = client.controls.find((c) => c.key === 'display.brightness')!;
  assert.deepEqual([bright.min, bright.max, bright.step, bright.unit], [0, 100, 5, '%']);
  const values = await client.get();
  assert.equal(values.get(mode.id), 1);
  assert.equal(values.has(byKey(client, 'device.name')), false); // needs level 1
  await assert.rejects(client.set([[mode.id, 2]]), (e: BlatError) => e.status === STATUS.authRequired && e.detail === 1);
  client.close();
});

test('pairing with the code, then sealed requests', async () => {
  const s = sim();
  const { client } = await connect(s);
  await client.readSchema();
  const started = await client.beginPairing();
  assert.equal(started.digits, 6);
  await tick();
  assert.match(s.code ?? '', /^\d{6}$/);

  const wrong = s.code === '000000' ? '111111' : '000000';
  await assert.rejects(client.submitCode(wrong), (e: BlatError) => e.status === STATUS.wrongCode && e.detail === 2);
  assert.equal(await client.submitCode(s.code!), 2);
  assert.ok(client.sealed);
  await tick();
  assert.equal(s.code, null); // hidden once used

  const mode = byKey(client, 'display.mode');
  const ssid = byKey(client, 'wifi.ssid');
  const password = byKey(client, 'wifi.password');
  await client.set([[mode, 2], [ssid, 'Home'], [password, 'correct horse']]);
  const values = await client.get([mode, ssid, password]);
  assert.deepEqual([...values], [[mode, 2], [ssid, 'Home'], [password, true]]);
  await assert.rejects(client.set([[byKey(client, 'power.battery'), 3]]), (e: BlatError) => e.status === STATUS.readOnly);

  // The schema again, sealed this time.
  await client.readSchema();
  client.close();
});

test('live changes arrive as events', async () => {
  const s = sim();
  const { client } = await connect(s);
  await client.readSchema();
  const battery = byKey(client, 'power.battery');
  const changed = new Promise<Map<number, unknown>>((resolve) => (client.onChanged = resolve));
  await s.setValue(battery, 42);
  assert.deepEqual([...(await changed)], [[battery, 42]]);
  client.close();
});

test('remember, reconnect and resume at level 1', async () => {
  const s = sim();
  let { client } = await connect(s);
  await client.readSchema();
  await client.beginPairing();
  await tick();
  await client.submitCode(s.code!);
  const key = await client.remember('test host');
  client.close();

  ({ client } = await connect(s));
  await client.readSchema();
  assert.equal(await client.resume(key), 1);
  await client.set([[byKey(client, 'display.mode'), 0]]);
  await assert.rejects(
    client.invoke(byKey(client, 'system.restart')),
    (e: BlatError) => e.status === STATUS.authRequired && e.detail === 2,
  );
  // Level 2 with a fresh code, inside the resumed session.
  await client.beginPairing();
  await tick();
  assert.equal(await client.submitCode(s.code!), 2);
  assert.equal(await client.invoke(byKey(client, 'system.restart')), STATUS.ok);
  client.close();

  ({ client } = await connect(s));
  await assert.rejects(client.resume({ ...key, hostKey: '00'.repeat(32) }), (e: BlatError) => e.status === STATUS.wrongCode);
  client.close();
});

test('actions with params', async () => {
  const s = sim();
  const { client } = await connect(s);
  await client.readSchema();
  await client.beginPairing();
  await tick();
  await client.submitCode(s.code!);
  const message = byKey(client, 'display.message');
  const text = byKey(client, 'display.message.text');
  const seconds = byKey(client, 'display.message.seconds');
  assert.equal(await client.invoke(message, [[text, 'Hi'], [seconds, 10]]), STATUS.ok);
  await assert.rejects(client.invoke(message, [[seconds, 10]]), (e: BlatError) => e.status === STATUS.invalidValue && e.detail === text);
  client.close();
});

test('a small MTU: hello without Info on a stream fails cleanly', async () => {
  // A stream has no Info characteristic, so when Info doesn't fit in hello's
  // reply there's nothing to fall back on.
  await assert.rejects(connect(sim(), 20), /didn't send Info/);
});

test('get follows `more` by asking for the rest by id', async () => {
  // A scripted device: Info in hello, then a get cut off after two values.
  const requests: number[][] = [];
  let handlers: LinkHandlers;
  const reply = (op: number, seq: number, flags: number, body: number[]) =>
    handlers.onMessage(CHANNEL.reply, Uint8Array.from([op, seq, 0, flags, 0, 0, ...body]));
  const info = [1, INFO.schema, 8, 0, 0, 0, 0, 0, 0, 0, 0, INFO.auth, 3, 0, 0, 6];
  const link: Link = {
    async open(h) {
      handlers = h;
    },
    async send(_channel, data) {
      const [op, seq, ...body] = data;
      queueMicrotask(() => {
        if (op === OP.hello) return reply(op, seq, 0, info);
        const ids = [];
        for (let i = 0; i < body.length; i += 2) ids.push(body[i] | (body[i + 1] << 8));
        requests.push(ids);
        // Enum values: id, u16. First reply stops after two with `more`.
        if (ids.length === 0) return reply(op, seq, 1, [1, 0, 7, 0, 2, 0, 8, 0]);
        reply(op, seq, 0, ids.flatMap((id) => [id, 0, id + 6, 0]));
      });
    },
    readInfo: async () => null,
    maxMessage: () => 20,
    close() {},
  };
  const client = new BlatClient(link);
  await client.connect();
  client.controls = [1, 2, 3, 4].map((id) => ({ id, type: 'enum', typeCode: 3, read: 0 }) as Control);
  const values = await client.get();
  assert.deepEqual([...values], [[1, 7], [2, 8], [3, 9], [4, 10]]);
  assert.deepEqual(requests, [[], [3, 4]]);
});
