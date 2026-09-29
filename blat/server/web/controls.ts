// A device's controls as a form, generated from its schema
// (PROTOCOL.md#control-types): groups become sections, each type gets a
// widget, and anything the connection can't change is shown read-only.

import type { Control, Value } from '../src/api.ts';
import { h } from './dom.ts';

export interface ControlContext {
  level: number;
  set(id: number, value: Value): Promise<void>;
  invoke(action: number, params: Record<number, Value>): Promise<void>;
  pair(): void;
}

interface Widget {
  el: HTMLElement;
  /** A new value from the device. */
  update(v: Value | undefined): void;
}

export interface Form {
  el: HTMLElement;
  update(values: Record<number, Value>): void;
}

export function renderControls(controls: Control[], values: Record<number, Value>, ctx: ControlContext): Form {
  const widgets = new Map<number, Widget>();
  const byParent = new Map<number, Control[]>();
  for (const c of controls) {
    if (c.hidden) continue;
    byParent.set(c.parent, [...(byParent.get(c.parent) ?? []), c]);
  }

  const place = (c: Control, into: HTMLElement) => {
    if (c.type === 'group') return; // sections are built separately
    const w = c.type === 'action' ? action(c, byParent.get(c.id) ?? [], ctx) : row(c, values[c.id], ctx);
    widgets.set(c.id, w);
    into.append(w.el);
  };

  const root = h('div', { class: 'controls' });
  const top = byParent.get(0) ?? [];
  const loose = top.filter((c) => c.type !== 'group');
  if (loose.length) {
    const section = h('section', { class: 'group' }, h('h3', {}, 'General'));
    loose.forEach((c) => place(c, section));
    root.append(section);
  }
  for (const g of top.filter((c) => c.type === 'group')) {
    const children = byParent.get(g.id) ?? [];
    const section = h('section', { class: 'group' }, h('h3', {}, g.label), g.help ? h('p', { class: 'help' }, g.help) : null);
    const advanced = h('details', { class: 'advanced' }, h('summary', {}, 'Advanced'));
    for (const c of children) place(c, c.advanced ? advanced : section);
    if (advanced.children.length > 1) section.append(advanced);
    root.append(section);
  }

  return {
    el: root,
    update(changed) {
      for (const [k, v] of Object.entries(changed)) widgets.get(Number(k))?.update(v);
    },
  };
}

// --- Access ------------------------------------------------------------------------

const canRead = (c: Control, level: number) => c.read <= level;
const canWrite = (c: Control, level: number) => (c.type === 'action' || !c.readOnly) && level >= Math.max(1, c.write);

function lockNote(c: Control, ctx: ControlContext): HTMLElement | null {
  if (c.readOnly && c.type !== 'action') return null;
  if (canWrite(c, ctx.level)) return null;
  const need = Math.max(1, c.write);
  const text = need >= 2 ? 'Needs a fresh code from the device' : 'Pair to change';
  return h('button', { type: 'button', class: 'lock', title: `Access level ${need}`, onclick: () => ctx.pair() }, `🔒 ${text}`);
}

// --- Rows ----------------------------------------------------------------------------

function row(c: Control, value: Value | undefined, ctx: ControlContext): Widget {
  const status = h('span', { class: 'status' });
  const commit = async (v: Value) => {
    status.textContent = 'Saving…';
    status.className = 'status';
    try {
      await ctx.set(c.id, v);
      status.textContent = 'Saved';
      setTimeout(() => status.textContent === 'Saved' && (status.textContent = ''), 1500);
    } catch (err) {
      status.textContent = (err as Error).message;
      status.className = 'status error';
      widget.update(lastValue); // back to what the device has
    }
  };
  let lastValue = value;
  const readable = canRead(c, ctx.level);
  const writable = readable && canWrite(c, ctx.level);
  const widget = readable ? input(c, value, writable, commit) : { el: h('span', { class: 'muted' }, 'Hidden until paired'), update() {} };

  const el = h(
    'div',
    { class: `row type-${c.type}`, 'data-key': c.key },
    h('div', { class: 'label' }, h('label', { for: `c${c.id}` }, c.label), c.help ? h('p', { class: 'help' }, c.help) : null),
    h('div', { class: 'value' }, widget.el, status, lockNote(c, ctx), c.restart ? h('span', { class: 'muted' }, 'Applies after a restart') : null),
  );
  return {
    el,
    update(v) {
      lastValue = v;
      widget.update(v);
    },
  };
}

function input(c: Control, value: Value | undefined, writable: boolean, commit: (v: Value) => void): Widget {
  const id = `c${c.id}`;
  switch (c.type) {
    case 'bool': {
      const box = h('input', { id, type: 'checkbox', class: 'switch', checked: !!value, disabled: !writable, onchange: () => commit(box.checked ? 1 : 0) });
      return { el: box, update: (v) => (box.checked = !!v) };
    }
    case 'enum':
      return choice(c, value, writable, commit);
    case 'int':
      return number(c, value, writable, commit);
    case 'text':
      return text(c, value, writable, commit);
    case 'secret':
      return secret(c, value, writable, commit);
    default:
      return { el: h('span', { class: 'muted' }, `(${c.type} controls aren't supported here yet)`), update() {} };
  }
}

function choice(c: Control, value: Value | undefined, writable: boolean, commit: (v: Value) => void): Widget {
  const id = `c${c.id}`;
  if (c.options.length <= 4) {
    // A few options: a segmented control.
    const buttons = c.options.map((o) =>
      h('button', { type: 'button', 'aria-pressed': String(o.value === value), disabled: !writable, onclick: () => commit(o.value) }, o.label),
    );
    const el = h('div', { id, class: 'segmented', role: 'group', 'aria-label': c.label }, ...buttons);
    const update = (v: Value | undefined) => c.options.forEach((o, i) => buttons[i].setAttribute('aria-pressed', String(o.value === v)));
    return { el, update };
  }
  const select = h('select', { id, disabled: !writable, onchange: () => commit(Number(select.value)) },
    ...c.options.map((o) => h('option', { value: String(o.value) }, o.label)));
  select.value = String(value);
  return { el: select, update: (v) => (select.value = String(v)) };
}

function number(c: Control, value: Value | undefined, writable: boolean, commit: (v: Value) => void): Widget {
  const k = 10 ** c.scale;
  const show = (v: Value | undefined) => (typeof v === 'number' ? (v / k).toFixed(c.scale) : '');
  const min = c.min ?? -2147483648;
  const max = c.max ?? 2147483647;
  const step = c.step ?? 1;
  const unit = c.unit ? h('span', { class: 'unit' }, c.unit) : null;
  if (c.readOnly || !writable) {
    const out = h('output', { id: `c${c.id}` }, show(value));
    return { el: h('span', { class: 'readout' }, out, unit), update: (v) => (out.textContent = show(v)) };
  }
  const slider = (max - min) / step <= 200;
  const field: HTMLInputElement = h('input', {
    id: `c${c.id}`,
    type: slider ? 'range' : 'number',
    min: min / k,
    max: max / k,
    step: step / k,
    value: show(value),
    onchange: () => commit(Math.round(Number(field.value) * k)),
    oninput: () => slider && (out.textContent = field.value),
  });
  const out = h('output', {}, show(value));
  const el = h('span', { class: 'number' }, field, slider ? out : null, unit);
  return {
    el,
    update(v) {
      field.value = show(v);
      out.textContent = show(v);
    },
  };
}

function text(c: Control, value: Value | undefined, writable: boolean, commit: (v: Value) => void): Widget {
  if (!writable) {
    const show = (v: Value | undefined) => (v === undefined || v === '' ? '—' : String(v));
    const out = h('output', { id: `c${c.id}` }, show(value));
    return { el: out, update: (v) => (out.textContent = show(v)) };
  }
  let saved = String(value ?? '');
  const multiline = c.hint === 'multiline';
  const field = multiline
    ? h('textarea', { id: `c${c.id}`, maxlength: c.maxLength, rows: 3 })
    : h('input', { id: `c${c.id}`, type: inputType(c.hint), maxlength: c.maxLength, minlength: c.minLength, autocomplete: 'off' });
  field.value = saved;
  const save = h('button', { type: 'submit', disabled: true }, 'Save');
  const dirty = () => (save.disabled = field.value === saved);
  field.addEventListener('input', dirty);
  const form = h('form', { class: 'inline', onsubmit: (e: Event) => {
    e.preventDefault();
    commit(field.value);
  } }, field, save);
  return {
    el: form,
    update(v) {
      saved = String(v ?? '');
      // Don't overwrite what someone is typing.
      if (document.activeElement !== field || field.value === saved) field.value = saved;
      dirty();
    },
  };
}

function secret(c: Control, value: Value | undefined, writable: boolean, commit: (v: Value) => void): Widget {
  const state = h('span', { class: 'secret-state' });
  const show = (v: Value | undefined) => (state.textContent = v ? 'Set' : 'Not set');
  show(value);
  if (!writable) return { el: state, update: show };
  const field = h('input', { id: `c${c.id}`, type: 'password', maxlength: c.maxLength, minlength: c.minLength, autocomplete: 'new-password', placeholder: 'New value' });
  const form = h('form', { class: 'inline', onsubmit: (e: Event) => {
    e.preventDefault();
    commit(field.value);
    field.value = ''; // never kept on the page
  } }, state, field, h('button', { type: 'submit' }, 'Set'));
  return { el: form, update: show };
}

function inputType(hint: string): string {
  return { email: 'email', url: 'url' }[hint] ?? 'text';
}

// --- Actions -----------------------------------------------------------------------

function action(c: Control, params: Control[], ctx: ControlContext): Widget {
  const writable = canWrite(c, ctx.level);
  const status = h('span', { class: 'status' });
  const fields = new Map<number, () => Value>();
  const paramRows = params.map((p) => {
    const read = paramInput(p);
    if (!writable) (read.el as HTMLInputElement).disabled = true;
    fields.set(p.id, read.value);
    return h('label', { class: 'param' }, h('span', {}, p.label + (p.required ? '' : ' (optional)')), read.el);
  });
  const form = h(
    'form',
    {
      class: 'action',
      onsubmit: async (e: Event) => {
        e.preventDefault();
        if (c.confirm && !window.confirm(`${c.label}?`)) return;
        const values: Record<number, Value> = {};
        for (const [id, read] of fields) {
          const v = read();
          if (v !== '') values[id] = v;
        }
        status.textContent = 'Running…';
        status.className = 'status';
        try {
          await ctx.invoke(c.id, values);
          status.textContent = 'Done';
        } catch (err) {
          status.textContent = (err as Error).message;
          status.className = 'status error';
        }
      },
    },
    ...paramRows,
    h('div', { class: 'value' }, h('button', { type: 'submit', disabled: !writable, class: c.confirm ? 'danger' : '' }, c.label), status, lockNote(c, ctx)),
  );
  const el = h('div', { class: 'row type-action', 'data-key': c.key }, h('div', { class: 'label' }, h('span', {}, c.label), c.help ? h('p', { class: 'help' }, c.help) : null), form);
  return { el, update() {} };
}

function paramInput(p: Control): { el: HTMLElement; value: () => Value } {
  switch (p.type) {
    case 'bool': {
      const box = h('input', { type: 'checkbox', checked: !!p.default });
      return { el: box, value: () => (box.checked ? 1 : 0) };
    }
    case 'enum': {
      const select = h('select', {}, ...p.options.map((o) => h('option', { value: String(o.value) }, o.label)));
      if (p.default !== undefined) select.value = String(p.default);
      return { el: select, value: () => Number(select.value) };
    }
    case 'int': {
      const k = 10 ** p.scale;
      const field = h('input', { type: 'number', min: (p.min ?? 0) / k, max: (p.max ?? 0) / k, step: (p.step ?? 1) / k });
      if (typeof p.default === 'number') field.value = String(p.default / k);
      return { el: field, value: () => (field.value === '' ? '' : Math.round(Number(field.value) * k)) };
    }
    default: {
      const field = h('input', { type: p.type === 'secret' ? 'password' : 'text', maxlength: p.maxLength, required: p.required });
      if (typeof p.default === 'string') field.value = p.default;
      return { el: field, value: () => field.value };
    }
  }
}
