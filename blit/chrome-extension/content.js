// Injected into the blitted tab on demand (chrome.scripting.executeScript).
// It picks the region to blit, reports where that region is now, and replays
// the display's buttons and taps on the page. Everything is driven by
// messages from the popup and the session page; it does nothing on its own.
(() => {
  if (window.__blit) return;
  window.__blit = true;

  const Z = 2147483647;

  // --- geometry ------------------------------------------------------------------

  function viewport() {
    return { width: window.innerWidth, height: window.innerHeight, dpr: window.devicePixelRatio };
  }

  function rectOf(selector) {
    const el = selector && document.querySelector(selector);
    if (!el) return { found: false, viewport: viewport() };
    const r = el.getBoundingClientRect();
    return { found: true, x: r.left, y: r.top, width: r.width, height: r.height, viewport: viewport() };
  }

  // A selector that finds `el` again later: its id, or a path of
  // :nth-of-type steps up to the nearest ancestor with an id.
  function selectorFor(el) {
    const parts = [];
    for (let node = el; node && node.nodeType === Node.ELEMENT_NODE; node = node.parentElement) {
      if (node.id && document.querySelectorAll(`#${CSS.escape(node.id)}`).length === 1) {
        parts.unshift(`#${CSS.escape(node.id)}`);
        break;
      }
      if (node === document.body || node === document.documentElement) {
        parts.unshift(node.tagName.toLowerCase());
        break;
      }
      const tag = node.tagName.toLowerCase();
      const same = [...(node.parentElement?.children ?? [])].filter((c) => c.tagName === node.tagName);
      parts.unshift(same.length > 1 ? `${tag}:nth-of-type(${same.indexOf(node) + 1})` : tag);
    }
    return parts.join(' > ');
  }

  function labelFor(el) {
    let label = el.tagName.toLowerCase();
    if (el.id) label += `#${el.id}`;
    else if (el.classList.length) label += `.${[...el.classList].slice(0, 2).join('.')}`;
    return label;
  }

  // --- picking -----------------------------------------------------------------------

  let picker = null;

  function overlay(css) {
    const el = document.createElement('div');
    el.style.cssText = `position:fixed;z-index:${Z};pointer-events:none;box-sizing:border-box;${css}`;
    document.documentElement.append(el);
    return el;
  }

  function toast(text, ms = 2500) {
    const el = overlay(
      'left:50%;top:16px;transform:translateX(-50%);padding:8px 14px;border-radius:999px;background:#111;color:#fff;' +
        'font:13px/1.4 ui-monospace,monospace;box-shadow:0 4px 16px rgba(0,0,0,.25)',
    );
    el.textContent = text;
    if (ms) setTimeout(() => el.remove(), ms);
    return el;
  }

  function stopPicking() {
    if (!picker) return;
    picker.cleanup();
    picker = null;
  }

  async function save(tabId, region) {
    await chrome.storage.session.set({ [`region:${tabId}`]: region });
  }

  function pickElement(tabId) {
    const box = overlay('border:2px solid #e5484d;background:rgba(229,72,77,.12);display:none');
    const tip = toast('Click the part of the page to blit. Esc cancels.', 0);
    let current = null;
    const move = (e) => {
      const el = document.elementFromPoint(e.clientX, e.clientY);
      if (!el || el === current) return;
      current = el;
      const r = el.getBoundingClientRect();
      Object.assign(box.style, { display: 'block', left: `${r.left}px`, top: `${r.top}px`, width: `${r.width}px`, height: `${r.height}px` });
      tip.textContent = `${labelFor(el)} · ${Math.round(r.width)}×${Math.round(r.height)}  (click to pick, Esc cancels)`;
    };
    const click = async (e) => {
      e.preventDefault();
      e.stopPropagation();
      if (!current) return;
      const r = current.getBoundingClientRect();
      const region = { type: 'element', selector: selectorFor(current), label: labelFor(current), width: Math.round(r.width), height: Math.round(r.height) };
      stopPicking();
      await save(tabId, region);
      toast(`Blitting ${region.label} (${region.width}×${region.height})`);
    };
    return listen({ move, click }, () => {
      box.remove();
      tip.remove();
    });
  }

  function pickRect(tabId) {
    const box = overlay('border:2px dashed #e5484d;background:rgba(229,72,77,.12);display:none');
    const shade = overlay('inset:0;background:rgba(0,0,0,.08);cursor:crosshair');
    const tip = toast('Drag out the rectangle to blit. Esc cancels.', 0);
    let start = null;
    const rect = (e) => ({
      x: Math.min(start.x, e.clientX),
      y: Math.min(start.y, e.clientY),
      width: Math.abs(e.clientX - start.x),
      height: Math.abs(e.clientY - start.y),
    });
    const down = (e) => {
      e.preventDefault();
      start = { x: e.clientX, y: e.clientY };
    };
    const move = (e) => {
      if (!start) return;
      const r = rect(e);
      Object.assign(box.style, { display: 'block', left: `${r.x}px`, top: `${r.y}px`, width: `${r.width}px`, height: `${r.height}px` });
      tip.textContent = `${Math.round(r.width)}×${Math.round(r.height)}`;
    };
    const up = async (e) => {
      if (!start) return;
      const r = rect(e);
      start = null;
      if (r.width < 8 || r.height < 8) return;
      stopPicking();
      const region = { type: 'rect', x: Math.round(r.x), y: Math.round(r.y), width: Math.round(r.width), height: Math.round(r.height) };
      await save(tabId, region);
      toast(`Blitting a ${region.width}×${region.height} rectangle`);
    };
    return listen({ down, move, up, click: (e) => e.preventDefault() }, () => {
      box.remove();
      shade.remove();
      tip.remove();
    });
  }

  function listen({ down, move, up, click }, cleanup) {
    const key = (e) => {
      if (e.key === 'Escape') stopPicking();
    };
    const opts = { capture: true };
    if (down) addEventListener('mousedown', down, opts);
    if (move) addEventListener('mousemove', move, opts);
    if (up) addEventListener('mouseup', up, opts);
    if (click) addEventListener('click', click, opts);
    addEventListener('keydown', key, opts);
    return {
      cleanup() {
        removeEventListener('mousedown', down, opts);
        removeEventListener('mousemove', move, opts);
        removeEventListener('mouseup', up, opts);
        removeEventListener('click', click, opts);
        removeEventListener('keydown', key, opts);
        cleanup();
      },
    };
  }

  // --- replaying input -------------------------------------------------------------

  function scrollable(el) {
    for (let node = el; node && node !== document.body && node !== document.documentElement; node = node.parentElement) {
      const style = getComputedStyle(node);
      const canY = /(auto|scroll)/.test(style.overflowY) && node.scrollHeight > node.clientHeight;
      const canX = /(auto|scroll)/.test(style.overflowX) && node.scrollWidth > node.clientWidth;
      if (canX || canY) return node;
    }
    return null;
  }

  // Scroll the region's own scroller if it has one, else the page, by a
  // fraction of what's visible.
  function scroll({ dx = 0, dy = 0, selector }) {
    const el = selector && document.querySelector(selector);
    const target = (el && scrollable(el)) || null;
    const w = target ? target.clientWidth : window.innerWidth;
    const h = target ? target.clientHeight : window.innerHeight;
    const by = { left: dx * w, top: dy * h, behavior: 'instant' };
    if (target) target.scrollBy(by);
    else window.scrollBy(by);
  }

  // Synthetic keys: pages see isTrusted = false. Most keyboard handlers
  // (slides, lists, games) don't check; browser defaults like scrolling
  // don't happen, which is what the scroll mode is for.
  function key({ key }) {
    const target = document.activeElement && document.activeElement !== document.body ? document.activeElement : document;
    const init = { key, code: key.length === 1 ? `Key${key.toUpperCase()}` : key, bubbles: true, cancelable: true, composed: true };
    target.dispatchEvent(new KeyboardEvent('keydown', init));
    target.dispatchEvent(new KeyboardEvent('keyup', init));
  }

  function click({ x, y }) {
    const el = document.elementFromPoint(x, y);
    if (!el) return false;
    const init = { clientX: x, clientY: y, bubbles: true, cancelable: true, composed: true, view: window, button: 0 };
    el.dispatchEvent(new PointerEvent('pointerdown', { ...init, pointerType: 'mouse', isPrimary: true }));
    el.dispatchEvent(new MouseEvent('mousedown', init));
    el.dispatchEvent(new PointerEvent('pointerup', { ...init, pointerType: 'mouse', isPrimary: true }));
    el.dispatchEvent(new MouseEvent('mouseup', init));
    if (typeof el.focus === 'function') el.focus({ preventScroll: true });
    el.dispatchEvent(new MouseEvent('click', init));
    // A ring where the tap landed, so it's visible on the next frame too.
    const ring = overlay(`left:${x - 14}px;top:${y - 14}px;width:28px;height:28px;border:3px solid #111;border-radius:50%`);
    setTimeout(() => ring.remove(), 900);
    return true;
  }

  // --- messages ------------------------------------------------------------------------

  chrome.runtime.onMessage.addListener((msg, _sender, reply) => {
    switch (msg?.blit) {
      case 'ping':
        reply(true);
        break;
      case 'viewport':
        reply(viewport());
        break;
      case 'rect':
        reply(rectOf(msg.selector));
        break;
      case 'pick':
        stopPicking();
        picker = msg.mode === 'rect' ? pickRect(msg.tabId) : pickElement(msg.tabId);
        reply(true);
        break;
      case 'scroll':
        scroll(msg);
        reply(true);
        break;
      case 'key':
        key(msg);
        reply(true);
        break;
      case 'click':
        reply(click(msg));
        break;
      default:
        return false;
    }
    return false;
  });
})();
