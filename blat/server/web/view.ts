// One device, drawn the same way whether the page reaches it through
// blat-server (main.ts) or Web Bluetooth (bluetooth.ts): a header with its
// access level, the pairing box, and the form generated from its schema.

import type { Control, PairingStarted, Value } from '../src/api.ts';
import { type Form, renderControls } from './controls.ts';
import { h } from './dom.ts';

export const LEVELS = ['Read only', 'Paired', 'Paired with a code'];
export const levelName = (level: number) => LEVELS[level] ?? `Level ${level}`;

const STATUS_WRONG_CODE = 16;
const STATUS_LOCKED_OUT = 17;

export type Pairing = PairingStarted & { error?: string };

export interface DeviceView {
  name: string;
  details: Array<string | null | undefined>; // model, firmware, id…
  level: number;
  remembered: boolean;
  controls: Control[];
  values: Record<number, Value>;
  pairing: Pairing | null;
  /** Shown in the pairing box, e.g. the simulated device's code. */
  pairingHint?: string | null;
}

export interface DeviceActions {
  pair(): void;
  submitCode(code: string): void;
  cancelPairing(): void;
  forget(): void;
  disconnect(): void;
  set(id: number, value: Value): Promise<void>;
  invoke(action: number, params: Record<number, Value>): Promise<void>;
}

/** Draw a device into `el` (cleared first). Returns the form, for live updates. */
export function renderDeviceView(el: HTMLElement, d: DeviceView, actions: DeviceActions): Form {
  el.replaceChildren(
    h('header', { class: 'device-head' },
      h('div', {}, h('h2', {}, d.name), h('p', { class: 'muted' }, d.details.filter(Boolean).join(' · '))),
      h('div', { class: 'actions' },
        h('span', { class: `level-${d.level}` }, levelName(d.level)),
        d.level < 2 && !d.pairing ? h('button', { type: 'button', onclick: () => actions.pair() }, d.level === 0 ? 'Pair' : 'Enter a code') : null,
        d.remembered ? h('button', { type: 'button', class: 'quiet', title: 'Stop reconnecting without a code', onclick: () => actions.forget() }, 'Forget') : null,
        h('button', { type: 'button', class: 'quiet', onclick: () => actions.disconnect() }, 'Disconnect'),
      ),
    ),
  );
  if (d.pairing) el.append(pairingBox(d, d.pairing, actions));
  const form = renderControls(d.controls, d.values, {
    level: d.level,
    set: (id, v) => actions.set(id, v),
    invoke: (a, p) => actions.invoke(a, p),
    pair: () => actions.pair(),
  });
  el.append(form.el);
  return form;
}

function pairingBox(d: DeviceView, p: Pairing, actions: DeviceActions): HTMLElement {
  const field = h('input', {
    type: 'text',
    inputmode: 'numeric',
    autocomplete: 'one-time-code',
    pattern: `\\d{${p.digits}}`,
    maxlength: p.digits + 1,
    placeholder: '0'.repeat(p.digits),
    'aria-label': 'Pairing code',
  });
  const box = h(
    'form',
    {
      class: 'pairing',
      onsubmit: (e: Event) => {
        e.preventDefault();
        actions.submitCode(field.value.replace(/\D/g, ''));
      },
    },
    h('p', {}, `Enter the ${p.digits}-digit code shown on ${d.name}.`),
    d.pairingHint ? h('p', { class: 'muted' }, d.pairingHint) : null,
    h('div', { class: 'inline' },
      field,
      h('button', { type: 'submit' }, 'Pair'),
      h('button', { type: 'button', class: 'quiet', onclick: () => actions.cancelPairing() }, 'Cancel'),
    ),
    p.error
      ? h('p', { class: 'status error' }, p.error)
      : h('p', { class: 'muted' }, `${p.attemptsLeft} tries, ${Math.round(p.expiresSeconds / 60)} minutes.`),
  );
  queueMicrotask(() => field.focus());
  return box;
}

/** What a failed code means for the pairing box: try again (with the tries
 * left), or start over. `err` carries the blat status and detail. */
export function afterWrongCode(err: { status?: number; detail?: number; message: string }, pairing: Pairing): { pairing: Pairing | null; message?: string } {
  if (err.status === STATUS_WRONG_CODE && err.detail) {
    const n = err.detail;
    return { pairing: { ...pairing, attemptsLeft: n, error: `Wrong code: ${n} ${n === 1 ? 'try' : 'tries'} left.` } };
  }
  if (err.status === STATUS_WRONG_CODE) return { pairing: null, message: 'Wrong code, and no tries left. Pair again for a new code.' };
  return { pairing: null, message: err.message };
}

/** Why a device won't show a code. */
export function pairingRefused(err: { status?: number; detail?: number; message: string }): string {
  return err.status === STATUS_LOCKED_OUT ? `Too many wrong codes: try again in ${err.detail} s` : err.message;
}

// --- Messages --------------------------------------------------------------------

let toastTimer: number | undefined;
export function toast(text: string, error = false): void {
  const el = document.getElementById('toast');
  if (!el) return;
  el.textContent = text;
  el.className = error ? 'error' : '';
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => (el.hidden = true), 4000);
}

/** A code from a device's QR code: /#d=<deviceId>&c=<code> (PROTOCOL.md#the-qr-code). */
export function readQrCode(): { deviceId: string; code: string } | null {
  const params = new URLSearchParams(location.hash.slice(1));
  const deviceId = params.get('d');
  const code = params.get('c');
  return deviceId && code ? { deviceId: deviceId.toLowerCase(), code } : null;
}

export function clearQrCode(): void {
  history.replaceState(null, '', location.pathname + location.search);
}
