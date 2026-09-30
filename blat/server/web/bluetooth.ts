// The Web Bluetooth page: this browser is the blat host, with no server in
// between. It pairs with the code the device shows, and keeps the key the
// device gives it (to reconnect without a code) in this browser's storage.

import { BlatClient, type HostKey } from '../src/blat/client.ts';
import { crc32 } from '../src/blat/wire.ts';
import type { Value } from '../src/api.ts';
import type { Form } from './controls.ts';
import { h } from './dom.ts';
import { type Pairing, afterWrongCode, clearQrCode, pairingRefused, readQrCode, renderDeviceView, toast } from './view.ts';
import { WebBluetoothLink, chooseDevice, webBluetoothAvailable } from './webble.ts';

const HOST_NAME = 'Web Bluetooth page';
const METHOD_REMEMBER = 0x04;

const state = {
  device: null as BluetoothDevice | null,
  client: null as BlatClient | null,
  values: {} as Record<number, Value>,
  form: null as Form | null,
  busy: '' as string, // what we're doing, while connecting
  pairing: null as Pairing | null,
  qr: readQrCode(),
};

const $device = document.getElementById('device')!;

// --- What this browser remembers, per device (by its Info deviceId) ---------------

const storage = {
  hostKey(deviceId: string): HostKey | null {
    try {
      return JSON.parse(localStorage.getItem(`blat.host.${deviceId}`) ?? 'null');
    } catch {
      return null;
    }
  },
  saveHostKey(deviceId: string, key: HostKey) {
    localStorage.setItem(`blat.host.${deviceId}`, JSON.stringify(key));
  },
  forgetHostKey(deviceId: string) {
    localStorage.removeItem(`blat.host.${deviceId}`);
  },
  schema(crc: number): Uint8Array | null {
    const b64 = localStorage.getItem(`blat.schema.${crc.toString(16)}`);
    if (!b64) return null;
    const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
    return crc32(bytes) === crc ? bytes : null;
  },
  saveSchema(crc: number, blob: Uint8Array) {
    try {
      localStorage.setItem(`blat.schema.${crc.toString(16)}`, btoa(String.fromCharCode(...blob)));
    } catch {
      // storage full or off: read it again next time
    }
  },
};

// --- Connecting ------------------------------------------------------------------------

async function choose() {
  try {
    state.device = await chooseDevice();
  } catch (err) {
    if ((err as DOMException).name !== 'NotFoundError') toast((err as Error).message, true); // NotFound: chooser cancelled
    return;
  }
  await connect();
}

async function connect() {
  const device = state.device;
  if (!device) return;
  const client = new BlatClient(new WebBluetoothLink(device), { hostName: HOST_NAME });
  client.onChanged = (values) => {
    for (const [k, v] of values) state.values[k] = v;
    state.form?.update(Object.fromEntries(values));
  };
  client.onClose = () => {
    if (state.client !== client) return;
    state.client = null;
    state.pairing = null;
    render();
  };
  try {
    state.busy = `Connecting to ${device.name ?? 'the device'}…`;
    render();
    const info = await client.connect();
    state.client = client;

    const key = storage.hostKey(info.deviceId);
    if (key) {
      state.busy = 'Resuming…';
      render();
      try {
        await client.resume(key);
      } catch (err) {
        if ((err as { status?: number }).status !== 16) throw err;
        storage.forgetHostKey(info.deviceId); // the device forgot us
        toast(`${info.name} no longer remembers this browser. Pair again.`);
      }
    }

    const cached = storage.schema(info.schemaCrc);
    if (cached) {
      client.useSchema(cached);
    } else {
      state.busy = 'Reading its controls…';
      render();
      storage.saveSchema(info.schemaCrc, await client.readSchema());
    }
    state.values = Object.fromEntries(await client.get());
  } catch (err) {
    client.close();
    state.client = null;
    toast(`Couldn't connect: ${(err as Error).message}`, true);
  } finally {
    state.busy = '';
    render();
  }
}

// --- Pairing ----------------------------------------------------------------------------

async function startPairing() {
  const client = state.client;
  if (!client) return;
  try {
    state.pairing = await client.beginPairing();
  } catch (err) {
    toast(pairingRefused(err as Error & { status?: number; detail?: number }), true);
    return;
  }
  render();
  // A code from a QR code, for the device that's showing it.
  if (state.qr && state.qr.deviceId === client.info?.deviceId) {
    const { code } = state.qr;
    state.qr = null;
    clearQrCode();
    await submitCode(code);
  }
}

async function submitCode(code: string) {
  const client = state.client;
  const pairing = state.pairing;
  if (!client || !pairing) return;
  try {
    await client.submitCode(code);
    state.pairing = null;
    const info = client.info!;
    if (info.methods & METHOD_REMEMBER) {
      storage.saveHostKey(info.deviceId, await client.remember(HOST_NAME));
      toast('Paired. This browser will reconnect without a code.');
    } else {
      toast('Paired.');
    }
    state.values = Object.fromEntries(await client.get()); // a higher level may read more
  } catch (err) {
    const next = afterWrongCode(err as Error & { status?: number; detail?: number }, pairing);
    state.pairing = next.pairing;
    if (next.message) toast(next.message, true);
  }
  render();
}

// --- Drawing --------------------------------------------------------------------------

function render() {
  state.form = null;
  const client = state.client;
  if (state.busy) {
    $device.replaceChildren(h('p', { class: 'muted empty' }, state.busy));
    return;
  }
  if (!client?.connected || !client.info) {
    $device.replaceChildren(start());
    return;
  }
  const info = client.info;
  state.form = renderDeviceView(
    $device,
    {
      name: info.name,
      details: [info.model, info.firmware && `firmware ${info.firmware}`, info.deviceId && `id ${info.deviceId}`],
      level: client.level,
      remembered: !!storage.hostKey(info.deviceId),
      controls: client.controls,
      values: state.values,
      pairing: state.pairing,
    },
    {
      pair: startPairing,
      submitCode,
      cancelPairing: () => {
        state.pairing = null;
        render();
      },
      forget: () => {
        storage.forgetHostKey(info.deviceId);
        render();
      },
      disconnect: () => client.close(),
      set: async (id, value) => {
        await client.set([[id, value]]);
        // What the device made of it (secrets come back as set / not set).
        const after = Object.fromEntries(await client.get([id]));
        Object.assign(state.values, after);
        state.form?.update(after);
      },
      invoke: async (action, params) => {
        await client.invoke(action, Object.entries(params).map(([k, v]) => [Number(k), v] as [number, Value]));
      },
    },
  );
}

function start(): HTMLElement {
  if (!webBluetoothAvailable()) {
    return h('div', { class: 'start' },
      h('h2', {}, 'This browser has no Web Bluetooth'),
      h('p', { class: 'muted' }, 'Use Chrome or Edge on a computer or Android phone, over HTTPS or on localhost. Or run ', h('a', { href: './' }, 'blat-server'), ' on a machine with Bluetooth.'),
    );
  }
  return h('div', { class: 'start' },
    h('h2', {}, state.device ? `${state.device.name ?? 'The device'} disconnected` : 'Connect to a blat device'),
    h('p', { class: 'muted' }, 'On an X4, open the Bluetooth app first so it advertises.'),
    h('div', { class: 'inline' },
      state.device ? h('button', { type: 'button', onclick: connect }, 'Reconnect') : null,
      h('button', { type: 'button', class: state.device ? 'quiet' : '', onclick: choose }, state.device ? 'Choose another' : 'Choose a device'),
    ),
  );
}

render();
