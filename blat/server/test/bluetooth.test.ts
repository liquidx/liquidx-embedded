/// <reference lib="dom" />
// The Web Bluetooth page end to end, in headless Chromium. Chromium has no
// Bluetooth here, so the page gets a stand-in navigator.bluetooth whose
// characteristics are wired to the simulated device: everything else (the
// page, the protocol library and its crypto, bundled by Vite) is what a
// browser runs against a real device.
//
// Needs Chromium: set CHROME_PATH, or `npx playwright install chromium`.
// Skipped when there's none.
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import type { Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, before, test } from 'node:test';

import { type Browser, type Page, chromium } from 'playwright-core';

import type { Link } from '../src/blat/link.ts';
import { DeviceManager } from '../src/devices.ts';
import { createApp } from '../src/http.ts';
import { SimDevice } from '../src/sim.ts';
import { Store } from '../src/store.ts';

const chromePath = [process.env.CHROME_PATH, safe(() => chromium.executablePath())].find((p) => p && existsSync(p));

function safe<T>(f: () => T): T | undefined {
  try {
    return f();
  } catch {
    return undefined;
  }
}

let server: Server;
let base: string;
let browser: Browser;
let sim: SimDevice;
let dataDir: string;

before(async () => {
  if (!chromePath) return;
  dataDir = mkdtempSync(join(tmpdir(), 'blat-bt-test-'));
  // The server only serves the page here (Vite, so no build is needed).
  const manager = new DeviceManager({ store: new Store(dataDir), hostName: 'unused' });
  server = (await createApp({ manager, scanner: null, dev: true })).listen(0, '127.0.0.1');
  await new Promise((r) => server.once('listening', r));
  base = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  browser = await chromium.launch({ executablePath: chromePath });
  sim = new SimDevice();
});

after(async () => {
  await browser?.close();
  sim?.close();
  server?.close();
  if (dataDir) rmSync(dataDir, { recursive: true, force: true });
});

// Runs in the page: a navigator.bluetooth with one device, whose GATT calls
// go to Node (window.__sim*) and whose notifications come back through
// window.__notify.
function fakeBluetooth() {
  const SERVICE = 'b1a70000-e295-445e-b079-edd9ea2725cb';
  const channelOf: Record<string, number> = {
    'b1a70002-e295-445e-b079-edd9ea2725cb': 1, // request
    'b1a70003-e295-445e-b079-edd9ea2725cb': 3, // reply
    'b1a70004-e295-445e-b079-edd9ea2725cb': 2, // data
    'b1a70005-e295-445e-b079-edd9ea2725cb': 4, // dataOut
    'b1a70006-e295-445e-b079-edd9ea2725cb': 5, // event
    'b1a70001-e295-445e-b079-edd9ea2725cb': 0, // info
  };
  const w = window as unknown as Record<string, (...a: unknown[]) => Promise<void>>;
  const chars = new Map<number, EventTarget & { value?: DataView }>();
  const device = new EventTarget() as EventTarget & Record<string, unknown>;
  const gatt = {
    connected: false,
    async connect() {
      await w.__simConnect();
      gatt.connected = true;
      return gatt;
    },
    disconnect() {
      gatt.connected = false;
      void w.__simDisconnect();
      device.dispatchEvent(new Event('gattserverdisconnected'));
    },
    async getPrimaryService(uuid: string) {
      if (uuid !== SERVICE) throw new DOMException('No service', 'NotFoundError');
      return {
        async getCharacteristic(cu: string) {
          const channel = channelOf[cu];
          const c = Object.assign(new EventTarget(), {
            async startNotifications() {
              chars.set(channel, c);
              return c;
            },
            async readValue(): Promise<DataView> {
              throw new DOMException('No Info characteristic on the simulator', 'NotSupportedError');
            },
            async writeValueWithResponse(data: Uint8Array) {
              await w.__simSend(channel, Array.from(data));
            },
            async writeValueWithoutResponse(data: Uint8Array) {
              await w.__simSend(channel, Array.from(data));
            },
          });
          return c;
        },
      };
    },
  };
  Object.assign(device, { name: 'Sim-0001', id: 'sim', gatt });
  (window as unknown as Record<string, unknown>).__notify = (channel: number, bytes: number[]) => {
    const c = chars.get(channel);
    if (!c) return;
    c.value = new DataView(Uint8Array.from(bytes).buffer);
    c.dispatchEvent(new Event('characteristicvaluechanged'));
  };
  Object.defineProperty(navigator, 'bluetooth', {
    value: { requestDevice: async () => device },
  });
}

async function openPage(): Promise<Page> {
  const page = await browser.newPage();
  const errors: string[] = [];
  page.on('pageerror', (e) => errors.push(e.message));
  let link: Link | null = null;
  await page.exposeFunction('__simConnect', async () => {
    link = sim.link();
    await link.open({
      onMessage: (channel, data) => void page.evaluate(([c, d]) => (window as any).__notify(c, d), [channel, Array.from(data)] as const),
      onClose: () => {},
    });
  });
  await page.exposeFunction('__simSend', (channel: number, bytes: number[]) => link?.send(channel, Uint8Array.from(bytes)));
  await page.exposeFunction('__simDisconnect', () => link?.close());
  await page.addInitScript(fakeBluetooth);
  await page.goto(`${base}/bluetooth.html`);
  (page as unknown as { errors: string[] }).errors = errors;
  return page;
}

const until = async (check: () => boolean, ms = 3000) => {
  const end = Date.now() + ms;
  while (!check()) {
    if (Date.now() > end) throw new Error('Timed out waiting');
    await new Promise((r) => setTimeout(r, 20));
  }
};

test('pair, change settings, see live values, reconnect without a code', { skip: !chromePath && 'no Chromium (set CHROME_PATH)' }, async () => {
  const page = await openPage();
  await page.getByRole('button', { name: 'Choose a device' }).click();
  await page.getByText('Read only').first().waitFor();
  assert.equal(await page.locator('[data-key="device.name"]').getByText('Hidden until paired').count(), 1);

  await page.getByRole('button', { name: 'Pair', exact: true }).click();
  await until(() => sim.code !== null);
  await page.getByLabel('Pairing code').fill(sim.code!);
  await page.getByRole('button', { name: 'Pair', exact: true }).click();
  await page.getByText('Paired with a code').first().waitFor();
  assert.equal(await page.locator('[data-key="device.name"] input').inputValue(), 'Sim');

  // Settings, checked on the device side through a fresh value read.
  await page.locator('[data-key="display.mode"]').getByLabel('Full').check();
  await page.locator('[data-key="display.mode"] .status', { hasText: 'Saved' }).waitFor();
  await page.locator('[data-key="wifi.password"] input').fill('correct horse');
  await page.locator('[data-key="wifi.password"] button').click();
  await page.locator('[data-key="wifi.password"] .secret-state', { hasText: /^Set$/ }).waitFor();

  // A live value from the device. Widgets are id'd c<control id>.
  const battery = Number((await page.locator('[data-key="power.battery"] output').getAttribute('id'))!.slice(1));
  await sim.setValue(battery, 42);
  await page.locator('[data-key="power.battery"] output', { hasText: '42' }).waitFor();

  // Disconnect and reconnect: the browser resumes with the key it kept.
  await page.getByRole('button', { name: 'Disconnect' }).click();
  await page.getByRole('button', { name: 'Reconnect' }).click();
  await page.getByText('Paired', { exact: true }).first().waitFor();
  assert.equal(await page.locator('[data-key="display.mode"]').getByLabel('Full').isChecked(), true);
  assert.ok(await page.evaluate(() => Object.keys(localStorage).some((k) => k.startsWith('blat.host.'))));

  assert.deepEqual((page as unknown as { errors: string[] }).errors, []);
  await page.close();
});
