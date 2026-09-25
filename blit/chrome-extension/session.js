// The session page: one per blitted tab, opened by the popup. It holds the
// display connection (Web Bluetooth needs a page that stays open), captures
// the tab, renders the chosen region for the display and sends it on the
// chosen schedule, and replays the display's buttons and taps on the page.
import { Blit, FORMAT_INFO, WebBluetoothTransport, fitRect, rasterize, sourceSize } from './lib/blit.js';
import { SimDisplay, SimTransport } from './lib/sim-display.js';
import { WHOLE_TAB, describeRegion, ensureContent, key, loadConfig, loadRegion } from './config.js';

const $ = (id) => document.getElementById(id);
const tabId = Number(new URLSearchParams(location.search).get('tab'));

// Not an error: a frame that can't be sent right now (tab hidden, element
// scrolled away). Logged once until something else happens.
class Skip extends Error {}

const blit = new Blit({ hostName: 'Chrome' });
let config = await loadConfig(tabId);
let region = await loadRegion(tabId);
let capture = null; // { requested, kind, grab(), stop() }
let sim = null;
let paused = false;
let busy = false;
let again = null; // a tick that arrived while busy
let nextAt = 0;
let transform = null; // how the last frame was made, to map taps back to the page
let lastMessage = '';
const stats = { sent: 0, skipped: 0, last: 0 };

// --- the tab ------------------------------------------------------------------------

const tab = await chrome.tabs.get(tabId).catch(() => null);
if (!tab) window.close();
const title = tab?.title || tab?.url || `Tab ${tabId}`;
$('target').textContent = title;
document.title = `Blit: ${title}`;
blit.hostName = `Chrome: ${title}`.slice(0, 32);

// --- log and status ---------------------------------------------------------------------

function log(text, kind = '') {
  lastMessage = text;
  const line = document.createElement('div');
  line.className = kind;
  line.textContent = `${new Date().toLocaleTimeString()}  ${text}`;
  $('log').prepend(line);
  while ($('log').childElementCount > 200) $('log').lastChild.remove();
}

function logOnce(text, kind) {
  if (text !== lastMessage) log(text, kind);
}

function ago(ms) {
  if (!ms) return '–';
  const s = Math.round((Date.now() - ms) / 1000);
  return s < 60 ? `${s} s ago` : `${Math.round(s / 60)} min ago`;
}

function updateMeta() {
  const caps = blit.caps;
  $('mArea').textContent = caps ? `${caps.width}×${caps.height}` : '–';
  $('mFormat').textContent = caps ? FORMAT_INFO[chooseFormat()]?.name : '–';
  $('mLast').textContent = ago(stats.last);
  $('mSent').textContent = String(stats.sent);
  $('mSkipped').textContent = String(stats.skipped);
  const chip = $('chip');
  chip.className = `chip${blit.connected ? ' on' : blit.deviceName ? ' warn' : ''}`;
  chip.textContent = blit.connected ? blit.deviceName : blit.deviceName ? `${blit.deviceName} (away)` : 'No display';
  $('connect').hidden = blit.connected;
  $('simulate').hidden = blit.connected;
  $('disconnect').hidden = !blit.connected;
  $('sendNow').disabled = !blit.caps;
  $('pause').disabled = !blit.caps;
  $('pause').textContent = paused ? 'Resume' : 'Pause';
}

// Tell the popup (and the toolbar badge) how it's going.
function publish() {
  const connected = blit.connected;
  const summary = paused ? 'Paused' : connected ? `${blit.deviceName} · ${stats.sent} sent` : blit.deviceName ? `${blit.deviceName} (away)` : 'No display yet';
  chrome.storage.session.set({ [key.state(tabId)]: { connected, summary } });
  chrome.action.setBadgeText({ tabId, text: paused ? '||' : connected ? 'ON' : '…' });
  chrome.action.setBadgeBackgroundColor({ tabId, color: connected && !paused ? '#1f7a3a' : '#777' });
}

// --- capture ------------------------------------------------------------------------------

async function startCapture() {
  if (capture?.requested === config.capture) return;
  capture?.stop();
  capture = config.capture === 'screenshot' ? screenshotCapture() : await streamCapture();
  capture.requested = config.capture;
}

// tabCapture: a live video of the tab, which keeps rendering even when it's
// in the background.
async function streamCapture() {
  try {
    const streamId = await chrome.tabCapture.getMediaStreamId({ targetTabId: tabId });
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: false,
      video: { mandatory: { chromeMediaSource: 'tab', chromeMediaSourceId: streamId, maxWidth: 4096, maxHeight: 4096, maxFrameRate: 10 } },
    });
    const video = $('video');
    video.srcObject = stream;
    await video.play();
    stream.getVideoTracks()[0].addEventListener('ended', () => {
      log('Tab capture ended', 'err');
      capture = null;
    });
    log('Capturing the tab as a stream');
    return {
      kind: 'stream',
      async grab() {
        if (!video.videoWidth) await new Promise((r) => video.addEventListener('loadedmetadata', r, { once: true }));
        return video;
      },
      stop() {
        for (const t of stream.getTracks()) t.stop();
        video.srcObject = null;
      },
    };
  } catch (err) {
    log(`Tab stream unavailable (${err.message}); using screenshots`, 'err');
    return screenshotCapture();
  }
}

// captureVisibleTab: a screenshot of the window's active tab. Only works
// while the blitted tab is the one showing in its window.
function screenshotCapture() {
  return {
    kind: 'screenshot',
    async grab() {
      const t = await chrome.tabs.get(tabId);
      if (!t.active) throw new Skip('The tab is hidden: screenshots need it showing in its window');
      const url = await chrome.tabs.captureVisibleTab(t.windowId, { format: 'png' });
      return createImageBitmap(await (await fetch(url)).blob());
    },
    stop() {},
  };
}

// Where the region is now, in the tab's CSS pixels.
async function pageGeometry() {
  try {
    await ensureContent(tabId);
    if (region.type === 'element') {
      const r = await chrome.tabs.sendMessage(tabId, { blit: 'rect', selector: region.selector });
      if (!r.found) throw new Skip(`Can't find ${region.label} on the page`);
      return { viewport: r.viewport, rect: r };
    }
    const viewport = await chrome.tabs.sendMessage(tabId, { blit: 'viewport' });
    return { viewport, rect: region.type === 'rect' ? region : null };
  } catch (err) {
    if (err instanceof Skip) throw err;
    if (region.type === 'element') throw new Skip(`Can't reach the page to find ${region.label} (did it navigate? Allow all sites in the popup)`);
    // No content script (e.g. after a navigation): the tab's size will do.
    const t = await chrome.tabs.get(tabId);
    return { viewport: { width: t.width, height: t.height }, rect: region.type === 'rect' ? region : null };
  }
}

function intersect(a, b) {
  const x = Math.max(a.x, b.x);
  const y = Math.max(a.y, b.y);
  return { x, y, width: Math.min(a.x + a.width, b.x + b.width) - x, height: Math.min(a.y + a.height, b.y + b.height) - y };
}

function chooseFormat() {
  if (config.format === 'auto') return blit.preferredFormat();
  const wanted = Number(config.format);
  return blit.caps?.formats.includes(wanted) ? wanted : blit.preferredFormat();
}

async function renderFrame() {
  await startCapture();
  const source = await capture.grab();
  const { width: sw } = sourceSize(source);
  const { viewport, rect } = await pageGeometry();
  const scale = sw / viewport.width; // source pixels per CSS pixel

  // The region keeps its full size even when partly scrolled out of view,
  // so the frame's scale doesn't jump; the hidden part is left white.
  const screen = { x: 0, y: 0, width: viewport.width, height: viewport.height };
  const crop = rect ? { x: rect.x, y: rect.y, width: rect.width, height: rect.height } : screen;
  const visible = intersect(crop, screen);
  if (visible.width < 1 || visible.height < 1) throw new Skip('The region is scrolled out of view');

  // Crop at full resolution, or at zoom × CSS pixels for "actual pixels".
  const actual = config.fit === 'actual';
  const k = actual ? config.zoom : scale; // canvas pixels per CSS pixel
  const cw = Math.max(1, Math.round(crop.width * k));
  const ch = Math.max(1, Math.round(crop.height * k));
  const canvas = new OffscreenCanvas(cw, ch);
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, cw, ch);
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(
    source,
    visible.x * scale, visible.y * scale, visible.width * scale, visible.height * scale,
    (visible.x - crop.x) * k, (visible.y - crop.y) * k, visible.width * k, visible.height * k,
  );

  const { width, height } = blit.caps;
  const fit = actual ? 'none' : config.fit;
  const frame = rasterize(canvas, {
    width, height, fit, format: chooseFormat(), dither: config.dither, contrast: config.contrast, invert: config.invert,
  });
  transform = { crop, cw, ch, placed: fitRect(cw, ch, width, height, fit) };
  return frame;
}

function showPreview(frame) {
  const canvas = $('preview');
  canvas.width = frame.width;
  canvas.height = frame.height;
  canvas.getContext('2d').putImageData(frame.preview, 0, 0);
}

// --- sending --------------------------------------------------------------------------------

function periodMs() {
  return Math.max(config.seconds * 1000, blit.caps?.minIntervalMs ?? 0, 1000);
}

async function tick(reason) {
  if (!blit.caps || (paused && reason !== 'manual')) return;
  if (busy) {
    again = again === 'manual' ? again : reason;
    return;
  }
  busy = true;
  try {
    const frame = await renderFrame();
    showPreview(frame);
    const interval = config.update === 'interval';
    const result = await blit.sendFrame(frame.pixels, {
      format: frame.format,
      width: frame.width,
      height: frame.height,
      // A display told when the next frame is due may be asleep until then,
      // so interval mode always sends. Otherwise unchanged frames are skipped.
      skipUnchanged: !interval && reason !== 'manual',
      regions: true,
      persist: config.persist,
      name: 'blit',
      nextFrameSeconds: interval ? Math.round(periodMs() / 1000) : 0,
    });
    if (result.skipped) {
      stats.skipped++;
    } else {
      stats.sent++;
      stats.last = Date.now();
      const what = result.region ? `${result.region.width}×${result.region.height} region` : `${frame.width}×${frame.height} frame`;
      log(`Sent ${what}, ${result.bytes} B${result.sleepSeconds ? `; display sleeping ${result.sleepSeconds} s` : ''}`, 'ok');
    }
  } catch (err) {
    if (err instanceof Skip) logOnce(err.message);
    else log(err.message, 'err');
  } finally {
    busy = false;
    updateMeta();
    publish();
    if (again) {
      const next = again;
      again = null;
      tick(next);
    }
  }
}

const ticker = new Worker('ticker.js');
ticker.onmessage = () => {
  $('mLast').textContent = ago(stats.last);
  if (config.update === 'manual' || paused || !blit.caps || Date.now() < nextAt) return;
  nextAt = Date.now() + periodMs();
  tick('timer');
};

// --- the display's buttons and taps -----------------------------------------------------------

const SCROLL = {
  up: [0, -0.8], down: [0, 0.8], left: [-0.8, 0], right: [0.8, 0], pageNext: [0, 0.95], pagePrev: [0, -0.95],
};
const DOM_KEY = {
  up: 'ArrowUp', down: 'ArrowDown', left: 'ArrowLeft', right: 'ArrowRight', select: 'Enter', back: 'Escape',
  menu: 'ContextMenu', home: 'Home', pageNext: 'PageDown', pagePrev: 'PageUp',
};

// Give the page a moment to react, then show the result.
function refreshSoon() {
  setTimeout(() => tick('input'), 400);
}

blit.addEventListener('key', async (e) => {
  const { name, action } = e.detail;
  log(`Button: ${name} (${action})`);
  if (!['press', 'long', 'repeat'].includes(action)) return;
  try {
    if (config.buttons === 'scroll' && SCROLL[name]) {
      await ensureContent(tabId);
      const [dx, dy] = SCROLL[name];
      await chrome.tabs.sendMessage(tabId, { blit: 'scroll', dx, dy, selector: region.selector });
    } else if (config.buttons === 'keys') {
      const domKey = DOM_KEY[name] ?? (/^f\d+$/.test(name) ? name.toUpperCase() : null);
      if (domKey) {
        await ensureContent(tabId);
        await chrome.tabs.sendMessage(tabId, { blit: 'key', key: domKey });
      }
    }
  } catch (err) {
    log(`Couldn't reach the page: ${err.message}`, 'err');
  }
  refreshSoon();
});

// Display pixel -> page CSS pixel, undoing the crop and fit of the last frame.
function toPage(x, y) {
  if (!transform) return null;
  const { crop, cw, ch, placed } = transform;
  const sx = ((x - placed.dx) * cw) / placed.dw;
  const sy = ((y - placed.dy) * ch) / placed.dh;
  if (sx < 0 || sy < 0 || sx >= cw || sy >= ch) return null;
  return { x: crop.x + (sx * crop.width) / cw, y: crop.y + (sy * crop.height) / ch };
}

blit.addEventListener('pointer', async (e) => {
  if (!config.taps || e.detail.action !== 'tap') return;
  const at = toPage(e.detail.x, e.detail.y);
  if (!at) return;
  log(`Tap at ${Math.round(at.x)}, ${Math.round(at.y)}`);
  try {
    await ensureContent(tabId);
    await chrome.tabs.sendMessage(tabId, { blit: 'click', x: at.x, y: at.y });
  } catch (err) {
    log(`Couldn't reach the page: ${err.message}`, 'err');
  }
  refreshSoon();
});

blit.addEventListener('caps', (e) => {
  const c = e.detail;
  log(`Display asks for ${c.width}×${c.height}, ${FORMAT_INFO[c.formats[0]]?.name ?? c.formats[0]}`);
  updateMeta();
  tick('caps');
});

blit.addEventListener('connected', (e) => {
  const c = e.detail;
  log(`Connected to ${c.name || blit.deviceName}: ${c.width}×${c.height}, protocol v${c.version}`, 'ok');
  nextAt = Date.now() + periodMs();
  updateMeta();
  publish();
  tick('connect');
});

blit.addEventListener('disconnected', () => {
  log('Display disconnected');
  updateMeta();
  publish();
});

blit.addEventListener('power', (e) => {
  if (e.detail.percent != null) log(`Display battery ${e.detail.percent} %${e.detail.charging ? ', charging' : ''}`);
});

// --- connecting ---------------------------------------------------------------------------------

if (!Blit.supported) {
  $('unsupported').hidden = false;
  $('connect').disabled = true;
}

async function connectDevice(transport) {
  await blit.connectTransport(transport);
  await chrome.storage.local.set({ lastDevice: transport.device?.id ?? null });
}

$('connect').addEventListener('click', async () => {
  try {
    await connectDevice(await WebBluetoothTransport.request());
  } catch (err) {
    if (err.name !== 'NotFoundError') log(err.message, 'err'); // NotFoundError: chooser cancelled
  }
});
$('disconnect').addEventListener('click', () => blit.disconnect());
$('sendNow').addEventListener('click', () => tick('manual'));
$('pause').addEventListener('click', () => {
  paused = !paused;
  log(paused ? 'Paused' : 'Resumed');
  updateMeta();
  publish();
  if (!paused) tick('config');
});

// Reconnect to the last display without the chooser, where Chrome allows it
// (navigator.bluetooth.getDevices, behind chrome://flags on some versions).
async function reconnectRemembered() {
  const { lastDevice } = await chrome.storage.local.get('lastDevice');
  if (!lastDevice || !navigator.bluetooth?.getDevices) return;
  const device = (await navigator.bluetooth.getDevices()).find((d) => d.id === lastDevice);
  if (!device) return;
  log(`Reconnecting to ${device.name ?? 'the last display'}…`);
  try {
    await connectDevice(new WebBluetoothTransport(device));
  } catch (err) {
    log(`Couldn't reach ${device.name ?? 'it'} (${err.message}). Press Connect.`);
  }
}

// --- simulator -----------------------------------------------------------------------------------

const SIM_MODES = {
  mono: { width: 400, height: 300, formats: [1, 2, 3] },
  gray: { width: 400, height: 300, formats: [3, 2, 1] },
  x4: { version: 1, width: 716, height: 480, panelWidth: 800, panelHeight: 480, refreshMs: 1200 },
};

function drawSim() {
  const canvas = $('simScreen');
  canvas.width = sim.caps.width;
  canvas.height = sim.caps.height;
  canvas.getContext('2d').putImageData(sim.imageData(), 0, 0);
}

async function startSim() {
  sim = new SimDisplay({ name: 'Simulator', refreshMs: 300, ...SIM_MODES[$('simMode').value] });
  sim.addEventListener('frame', drawSim);
  sim.addEventListener('caps', drawSim);
  $('simSection').hidden = false;
  drawSim();
  await blit.connectTransport(new SimTransport(sim));
}

$('simulate').addEventListener('click', startSim);
$('simMode').addEventListener('change', () => sim && startSim());
for (const button of document.querySelectorAll('[data-key]')) {
  button.addEventListener('click', () => sim?.pressKey(Number(button.dataset.key)));
}
$('simScreen').addEventListener('click', (e) => {
  const canvas = e.currentTarget;
  const r = canvas.getBoundingClientRect();
  sim?.tap(((e.clientX - r.left) * canvas.width) / r.width, ((e.clientY - r.top) * canvas.height) / r.height);
});

// --- settings changes and lifecycle ------------------------------------------------------------

chrome.storage.onChanged.addListener((changes, area) => {
  if (area !== 'session') return;
  const c = changes[key.config(tabId)];
  if (c?.newValue) {
    config = { ...config, ...c.newValue };
    nextAt = Date.now() + periodMs();
    log('Settings changed');
    tick('config');
  }
  const r = changes[key.region(tabId)];
  if (r) {
    region = r.newValue ?? WHOLE_TAB;
    log(`Blitting: ${describeRegion(region)}`);
    tick('config');
  }
});

async function stop() {
  capture?.stop();
  blit.disconnect();
  await Promise.allSettled([
    chrome.storage.session.remove(key.state(tabId)),
    chrome.action.setBadgeText({ tabId, text: '' }),
  ]);
  window.close();
}

chrome.runtime.onMessage.addListener((msg) => {
  if (msg?.blit !== 'command' || msg.tabId !== tabId) return;
  if (msg.command === 'send') tick('manual');
  if (msg.command === 'stop') stop();
});
chrome.tabs.onRemoved.addListener((id) => id === tabId && stop());
addEventListener('pagehide', () => {
  chrome.storage.session.remove(key.state(tabId));
  chrome.action.setBadgeText({ tabId, text: '' });
});

log(`Blitting: ${describeRegion(region)}`);
updateMeta();
publish();
reconnectRemembered();
