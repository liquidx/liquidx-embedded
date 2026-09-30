// blat-server's page: the devices the server knows, and a form for the one
// you pick. The server is the blat host; this page talks JSON to it.

import type { DeviceState, DeviceSummary, ServerEvent } from '../src/api.ts';
import { type RequestError, api, listen } from './api.ts';
import type { Form } from './controls.ts';
import { clear, h } from './dom.ts';
import { type Pairing, afterWrongCode, clearQrCode, levelName, pairingRefused, readQrCode, renderDeviceView, toast } from './view.ts';

const state = {
  devices: [] as DeviceSummary[],
  scanning: false,
  canScan: false,
  selected: null as string | null,
  device: null as DeviceState | null,
  form: null as Form | null,
  connecting: false,
  pairing: null as Pairing | null,
  simCodes: new Map<string, string | null>(),
  qr: readQrCode(),
};

const $devices = document.getElementById('devices')!;
const $device = document.getElementById('device')!;
const $linkState = document.getElementById('link-state')!;

// --- Devices ---------------------------------------------------------------------

function renderDevices() {
  clear($devices);
  $devices.append(
    h('div', { class: 'list-head' },
      h('h2', {}, 'Devices'),
      state.canScan
        ? h('button', { type: 'button', disabled: state.scanning, onclick: scan }, state.scanning ? 'Scanning…' : 'Scan')
        : null,
    ),
  );
  if (!state.devices.length) {
    $devices.append(
      h('p', { class: 'muted' }, state.scanning ? 'Looking for devices…' : 'No devices yet. On an X4, open the Bluetooth app so it advertises, then scan.'),
    );
  } else {
    const list = h('ul', { class: 'device-list' });
    for (const d of state.devices) {
      list.append(
        h('li', {},
          h('button', { type: 'button', class: 'device', 'aria-current': d.id === state.selected ? 'true' : null, onclick: () => select(d.id) },
            h('span', { class: 'name' }, d.name),
            h('span', { class: 'meta' },
              d.connected ? h('span', { class: `badge level-${d.level}` }, levelName(d.level)) : h('span', { class: 'muted' }, 'Not connected'),
              d.rssi !== null ? h('span', { class: 'muted' }, `${d.rssi} dBm`) : null,
            ),
          ),
        ),
      );
    }
    $devices.append(list);
  }
  $devices.append(h('p', { class: 'muted small' }, 'Or connect from this browser: ', h('a', { href: './bluetooth.html' }, 'Web Bluetooth page'), '.'));
}

async function scan() {
  try {
    await api.scan();
  } catch (err) {
    toast((err as Error).message, true);
  }
}

async function select(id: string) {
  if (state.selected !== id) {
    state.selected = id;
    state.device = null;
    state.pairing = null;
  }
  renderDevices();
  const summary = state.devices.find((d) => d.id === id);
  if (summary?.connected) {
    state.device = await api.state(id).catch(() => null);
    return renderDevice();
  }
  await connect(id);
}

async function connect(id: string) {
  state.connecting = true;
  renderDevice();
  try {
    state.device = await api.connect(id);
  } catch (err) {
    toast(`Couldn't connect: ${(err as Error).message}`, true);
  } finally {
    state.connecting = false;
    renderDevice();
  }
}

// --- The selected device ---------------------------------------------------------

function renderDevice() {
  state.form = null;
  const id = state.selected;
  if (!id) {
    $device.replaceChildren(h('p', { class: 'muted empty' }, 'Pick a device.'));
    return;
  }
  const d = state.device;
  if (!d || !d.connected) {
    const summary = state.devices.find((x) => x.id === id);
    $device.replaceChildren(
      h('header', { class: 'device-head' }, h('h2', {}, summary?.name ?? id)),
      state.connecting ? h('p', { class: 'muted' }, 'Connecting…') : h('p', {}, h('button', { type: 'button', onclick: () => connect(id) }, 'Connect')),
    );
    return;
  }
  const simCode = state.simCodes.get(d.id);
  state.form = renderDeviceView(
    $device,
    {
      name: d.name,
      details: [d.model, d.firmware && `firmware ${d.firmware}`, d.deviceId && `id ${d.deviceId}`],
      level: d.level,
      remembered: d.remembered,
      controls: d.controls,
      values: d.values,
      pairing: state.pairing,
      pairingHint: simCode ? `The simulated device shows ${simCode}.` : null,
    },
    {
      pair: startPairing,
      submitCode,
      cancelPairing: () => {
        state.pairing = null;
        renderDevice();
      },
      forget: async () => {
        state.device = await api.forget(d.id);
        renderDevice();
      },
      disconnect: () => void api.disconnect(d.id),
      set: async (cid, value) => {
        await api.set(d.id, { [cid]: value });
      },
      invoke: async (action, params) => {
        await api.invoke(d.id, action, params);
      },
    },
  );
}

async function startPairing() {
  const d = state.device;
  if (!d) return;
  try {
    state.pairing = await api.pair(d.id);
  } catch (err) {
    toast(pairingRefused(err as RequestError), true);
    return;
  }
  renderDevice();
  // A code from a QR code, for the device that's showing it.
  if (state.qr && state.qr.deviceId === d.deviceId) {
    const { code } = state.qr;
    state.qr = null;
    clearQrCode();
    await submitCode(code);
  }
}

async function submitCode(code: string) {
  const d = state.device;
  const pairing = state.pairing;
  if (!d || !pairing) return;
  try {
    state.device = await api.code(d.id, code);
    state.pairing = null;
    toast(state.device.remembered ? 'Paired. This server will reconnect without a code.' : 'Paired.');
  } catch (err) {
    const next = afterWrongCode(err as RequestError, pairing);
    state.pairing = next.pairing;
    if (next.message) toast(next.message, true);
  }
  renderDevice();
}

// --- Server events ----------------------------------------------------------------

function onEvent(e: ServerEvent) {
  switch (e.type) {
    case 'devices': {
      const before = state.devices.find((d) => d.id === state.selected);
      state.devices = e.devices;
      state.scanning = e.scanning;
      state.canScan = e.canScan;
      renderDevices();
      const now = state.devices.find((d) => d.id === state.selected);
      if (before && now && before.connected && !now.connected) {
        state.device = null;
        state.pairing = null;
        renderDevice();
      }
      break;
    }
    case 'device':
      if (e.device.id === state.selected) {
        const levelChanged = state.device?.level !== e.device.level;
        state.device = e.device;
        if (levelChanged || !state.form) renderDevice();
        else state.form.update(e.device.values);
      }
      break;
    case 'values':
      if (e.id === state.selected && state.device) {
        Object.assign(state.device.values, e.values);
        state.form?.update(e.values);
      }
      break;
    case 'simCode':
      state.simCodes.set(e.id, e.code);
      if (e.id === state.selected && state.pairing) renderDevice();
      break;
  }
}

renderDevices();
renderDevice();
listen(onEvent, (online) => {
  $linkState.hidden = online;
});
