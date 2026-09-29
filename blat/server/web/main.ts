// blat-server's page: the devices the server knows, and a form for the one
// you pick. State lives here; render functions rebuild the parts that change.

import type { DeviceState, DeviceSummary, PairingStarted, ServerEvent, Value } from '../src/api.ts';
import { RequestError, api, listen } from './api.ts';
import { type Form, renderControls } from './controls.ts';
import { clear, h } from './dom.ts';

const STATUS_WRONG_CODE = 16;
const STATUS_LOCKED_OUT = 17;

const state = {
  devices: [] as DeviceSummary[],
  scanning: false,
  canScan: false,
  selected: null as string | null,
  device: null as DeviceState | null,
  form: null as Form | null,
  connecting: false,
  pairing: null as (PairingStarted & { error?: string }) | null,
  simCodes: new Map<string, string | null>(),
  // From a QR code on the device: /#d=<deviceId>&c=<code> (PROTOCOL.md#the-qr-code).
  qr: null as { deviceId: string; code: string } | null,
};

const $devices = document.getElementById('devices')!;
const $device = document.getElementById('device')!;
const $toast = document.getElementById('toast')!;
const $linkState = document.getElementById('link-state')!;

// --- Messages --------------------------------------------------------------------

let toastTimer: number | undefined;
function toast(text: string, error = false) {
  $toast.textContent = text;
  $toast.className = error ? 'error' : '';
  $toast.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => ($toast.hidden = true), 4000);
}

const LEVELS = ['Read only', 'Paired', 'Paired with a code'];

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
    return;
  }
  const list = h('ul', { class: 'device-list' });
  for (const d of state.devices) {
    list.append(
      h('li', {},
        h('button', { type: 'button', class: 'device', 'aria-current': d.id === state.selected ? 'true' : null, onclick: () => select(d.id) },
          h('span', { class: 'name' }, d.name),
          h('span', { class: 'meta' },
            d.connected ? h('span', { class: `badge level-${d.level}` }, LEVELS[d.level] ?? `Level ${d.level}`) : h('span', { class: 'muted' }, 'Not connected'),
            d.rssi !== null ? h('span', { class: 'muted' }, `${d.rssi} dBm`) : null,
          ),
        ),
      ),
    );
  }
  $devices.append(list);
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
  clear($device);
  state.form = null;
  const id = state.selected;
  if (!id) {
    $device.append(h('p', { class: 'muted empty' }, 'Pick a device.'));
    return;
  }
  const summary = state.devices.find((d) => d.id === id);
  const d = state.device;
  if (!d || !d.connected) {
    $device.append(
      h('header', { class: 'device-head' }, h('h2', {}, summary?.name ?? id)),
      state.connecting
        ? h('p', { class: 'muted' }, 'Connecting…')
        : h('p', {}, h('button', { type: 'button', onclick: () => connect(id) }, 'Connect')),
    );
    return;
  }

  const level = d.level;
  $device.append(
    h('header', { class: 'device-head' },
      h('div', {},
        h('h2', {}, d.name),
        h('p', { class: 'muted' }, [d.model, d.firmware && `firmware ${d.firmware}`, d.deviceId && `id ${d.deviceId}`].filter(Boolean).join(' · ')),
      ),
      h('div', { class: 'actions' },
        h('span', { class: `badge level-${level}` }, LEVELS[level] ?? `Level ${level}`),
        level < 2 && !state.pairing ? h('button', { type: 'button', onclick: startPairing }, level === 0 ? 'Pair' : 'Enter a code') : null,
        d.remembered ? h('button', { type: 'button', class: 'quiet', title: 'Stop reconnecting without a code', onclick: forget }, 'Forget') : null,
        h('button', { type: 'button', class: 'quiet', onclick: () => api.disconnect(d.id) }, 'Disconnect'),
      ),
    ),
  );
  if (state.pairing) $device.append(pairingBox(d));

  state.form = renderControls(d.controls, d.values, {
    level,
    set: async (cid: number, value: Value) => {
      await api.set(d.id, { [cid]: value });
    },
    invoke: async (action: number, params: Record<number, Value>) => {
      await api.invoke(d.id, action, params);
    },
    pair: () => void startPairing(),
  });
  $device.append(state.form.el);
}

async function startPairing() {
  const d = state.device;
  if (!d) return;
  try {
    state.pairing = await api.pair(d.id);
  } catch (err) {
    const e = err as RequestError;
    toast(e.status === STATUS_LOCKED_OUT ? `Too many wrong codes: try again in ${e.detail} s` : e.message, true);
    return;
  }
  renderDevice();
  tryQrCode();
}

function pairingBox(d: DeviceState): HTMLElement {
  const p = state.pairing!;
  const field = h('input', {
    type: 'text',
    inputmode: 'numeric',
    autocomplete: 'one-time-code',
    pattern: `\\d{${p.digits}}`,
    maxlength: p.digits + 1,
    placeholder: '0'.repeat(p.digits),
    'aria-label': 'Pairing code',
  });
  const simCode = state.simCodes.get(d.id);
  const box = h('form', { class: 'pairing', onsubmit: (e: Event) => {
    e.preventDefault();
    void submitCode(field.value);
  } },
    h('p', {}, `Enter the ${p.digits}-digit code shown on ${d.name}.`),
    simCode ? h('p', { class: 'muted' }, `The simulated device shows ${simCode}.`) : null,
    h('div', { class: 'inline' }, field, h('button', { type: 'submit' }, 'Pair'), h('button', { type: 'button', class: 'quiet', onclick: () => {
      state.pairing = null;
      renderDevice();
    } }, 'Cancel')),
    p.error ? h('p', { class: 'status error' }, p.error) : h('p', { class: 'muted' }, `${p.attemptsLeft} tries, ${Math.round(p.expiresSeconds / 60)} minutes.`),
  );
  queueMicrotask(() => field.focus());
  return box;
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
    const e = err as RequestError;
    if (e.status === STATUS_WRONG_CODE && e.detail) {
      state.pairing = { ...pairing, attemptsLeft: e.detail, error: `Wrong code: ${e.detail} ${e.detail === 1 ? 'try' : 'tries'} left.` };
    } else {
      state.pairing = null;
      toast(e.status === STATUS_WRONG_CODE ? 'Wrong code, and no tries left. Pair again for a new code.' : e.message, true);
    }
  }
  renderDevice();
}

async function forget() {
  const d = state.device;
  if (!d) return;
  state.device = await api.forget(d.id);
  renderDevice();
}

// A code from a QR code, for the device that's showing it.
function tryQrCode() {
  const qr = state.qr;
  if (!qr || !state.pairing || state.device?.deviceId !== qr.deviceId) return;
  state.qr = null;
  history.replaceState(null, '', location.pathname);
  void submitCode(qr.code);
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
      // A device showing a code for us (e.g. paired from another tab).
      if (now && !now.pairing && state.pairing && now.level === 2) {
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

function readQr() {
  const params = new URLSearchParams(location.hash.slice(1));
  const deviceId = params.get('d');
  const code = params.get('c');
  if (deviceId && code) state.qr = { deviceId: deviceId.toLowerCase(), code };
}

readQr();
renderDevices();
renderDevice();
listen(onEvent, (online) => {
  $linkState.hidden = online;
});
