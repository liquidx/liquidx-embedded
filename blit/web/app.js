// Demo page for ../js/blit.js: send images, or run a slideshow, to a display
// over Web Bluetooth or to a simulated display in the page.
import { Blit, FORMAT, FORMAT_INFO, KEY, isChooserCancelled, rasterize } from '../js/blit.js';
import { SimDisplay, SimTransport } from '../js/sim-display.js';

const DEFAULT_AREA = { w: 716, h: 480 }; // before a display reports its own

const $ = (id) => document.getElementById(id);
const cast = new Blit({ hostName: 'blit demo' });
let sim = null;

const images = []; // { name, url, img }
let selected = -1;
let slideshow = null; // { timer, index }

// --- helpers ------------------------------------------------------------

function log(message, kind = '') {
  const line = document.createElement('div');
  line.className = kind;
  line.textContent = `${new Date().toLocaleTimeString()}  ${message}`;
  $('log').prepend(line);
}

function frameSize() {
  const caps = cast.caps;
  if (!caps) return DEFAULT_AREA;
  if ($('size').value === 'full') return { w: caps.panel.width, h: caps.panel.height };
  return { w: caps.width, h: caps.height };
}

function frameFormat() {
  const chosen = $('format').value;
  if (chosen === 'auto' || !cast.caps) return cast.caps ? cast.preferredFormat() : FORMAT.MONO1;
  const format = Number(chosen);
  return cast.caps.formats.includes(format) ? format : FORMAT.MONO1;
}

function frameOptions() {
  const { w, h } = frameSize();
  return {
    width: w,
    height: h,
    format: frameFormat(),
    fit: $('fit').value,
    dither: $('dither').value,
    contrast: Number($('contrast').value) || 1,
    threshold: Number($('threshold').value) || 128,
    persist: $('persist').checked,
  };
}

function showPreview(frame) {
  const canvas = $('preview');
  canvas.width = frame.width;
  canvas.height = frame.height;
  canvas.getContext('2d').putImageData(frame.preview, 0, 0);
}

function previewSource(source) {
  if (!source) return;
  showPreview(rasterize(source, frameOptions()));
}

function currentSource() {
  return images[selected]?.img ?? null;
}

async function send(source, extra = {}) {
  if (!cast.connected && !cast.deviceName) {
    log('Connect to a device first', 'err');
    return;
  }
  const opts = { ...frameOptions(), ...extra, onRaster: showPreview };
  // In "frame area" mode let sendImage() size the frame from fresh caps: the
  // area changes when the display's chrome is toggled.
  if ($('size').value === 'device') {
    delete opts.width;
    delete opts.height;
  }
  const started = performance.now();
  try {
    const { sleepSeconds } = await cast.sendImage(source, opts);
    const secs = ((performance.now() - started) / 1000).toFixed(1);
    log(`Sent ${extra.name || 'frame'} in ${secs} s${sleepSeconds ? `; device sleeping ${sleepSeconds} s` : ''}`, 'ok');
  } catch (err) {
    log(err.message, 'err');
    throw err;
  } finally {
    $('progress').value = 0;
  }
}

function updateButtons() {
  const haveDevice = !!cast.deviceName;
  const haveImage = selected >= 0;
  $('sendOne').disabled = !haveDevice || !haveImage;
  $('startShow').disabled = !haveDevice || images.length === 0 || !!slideshow;
  $('stopShow').hidden = !slideshow;
  $('startShow').hidden = !!slideshow;
}

function updateMeta() {
  const caps = cast.caps;
  $('mName').textContent = cast.deviceName ?? '–';
  $('mArea').textContent = caps ? `${caps.width}×${caps.height}` : '–';
  $('mChunk').textContent = caps ? `${caps.chunk} B` : '–';
  $('mFormats').textContent = caps ? caps.formats.map((f) => FORMAT_INFO[f]?.name ?? f).join(', ') : '–';
  $('mVersion').textContent = caps ? `v${caps.version}` : '–';
  $('mSleep').textContent = caps ? (caps.features.frameSleep ? 'on' : 'off') : '–';
  const chip = $('chip');
  chip.textContent = cast.connected ? `Connected to ${cast.deviceName}` : cast.deviceName ? `${cast.deviceName} (disconnected)` : 'Not connected';
  chip.classList.toggle('on', cast.connected);
  $('connect').hidden = cast.connected;
  $('simulate').hidden = cast.connected;
  $('disconnect').hidden = !cast.connected;
}

// --- connection ------------------------------------------------------------

if (!Blit.supported) $('unsupported').hidden = false;

$('connect').addEventListener('click', async () => {
  try {
    await cast.connect();
  } catch (err) {
    if (!isChooserCancelled(err)) log(`${err.name}: ${err.message}`, 'err');
  }
});
$('disconnect').addEventListener('click', () => cast.disconnect());

// --- simulated display -----------------------------------------------------

const SIM_MODES = {
  1: { width: 400, height: 300, formats: [FORMAT.MONO1, FORMAT.GRAY2, FORMAT.GRAY4] },
  3: { width: 400, height: 300, formats: [FORMAT.GRAY4, FORMAT.GRAY2, FORMAT.MONO1] },
  '3-small': { width: 200, height: 150, formats: [FORMAT.GRAY4, FORMAT.GRAY2, FORMAT.MONO1] },
};

function drawSim() {
  const canvas = $('simScreen');
  canvas.width = sim.caps.width;
  canvas.height = sim.caps.height;
  canvas.getContext('2d').putImageData(sim.imageData(), 0, 0);
}

$('simulate').addEventListener('click', async () => {
  if (!sim) {
    sim = new SimDisplay({ name: 'Simulator', ...SIM_MODES[$('simMode').value], refreshMs: 400 });
    sim.addEventListener('frame', drawSim);
    sim.addEventListener('caps', drawSim);
    sim.addEventListener('log', (e) => log(`display: ${e.detail}`));
  }
  $('simSection').hidden = false;
  drawSim();
  await cast.connectTransport(new SimTransport(sim));
});

for (const button of document.querySelectorAll('[data-key]')) {
  button.addEventListener('click', () => sim?.pressKey(Number(button.dataset.key)));
}
$('simScreen').addEventListener('click', (e) => {
  const canvas = e.currentTarget;
  const rect = canvas.getBoundingClientRect();
  sim?.tap(((e.clientX - rect.left) * canvas.width) / rect.width, ((e.clientY - rect.top) * canvas.height) / rect.height);
});
$('simMode').addEventListener('change', () => sim?.setCaps(SIM_MODES[$('simMode').value]));

// --- events from the display -------------------------------------------------

cast.addEventListener('caps', (e) => {
  log(`Display asks for ${e.detail.width}×${e.detail.height}, ${FORMAT_INFO[e.detail.formats[0]]?.name}`);
  updateMeta();
  previewSource(currentSource());
});

// Buttons on the display step through the images.
cast.addEventListener('key', (e) => {
  log(`Key: ${e.detail.name} (${e.detail.action})`);
  if (!images.length || e.detail.action !== 'press') return;
  const step = { [KEY.UP]: -1, [KEY.LEFT]: -1, [KEY.PAGE_PREV]: -1, [KEY.DOWN]: 1, [KEY.RIGHT]: 1, [KEY.PAGE_NEXT]: 1 }[e.detail.key];
  if (step) {
    selected = (Math.max(0, selected) + step + images.length) % images.length;
    renderThumbs();
  }
  if (step || e.detail.key === KEY.SELECT) {
    const item = images[selected];
    send(item.img, { name: stripExt(item.name) }).catch(() => {});
  }
});
cast.addEventListener('pointer', (e) => log(`Pointer: ${e.detail.action} at ${e.detail.x}, ${e.detail.y}`));

cast.addEventListener('connected', (e) => {
  log(`Connected to ${e.detail.name}: ${e.detail.width}×${e.detail.height}, protocol v${e.detail.version}, ${e.detail.chunk} B chunks`, 'ok');
  updateMeta();
  updateButtons();
  previewSource(currentSource());
});
cast.addEventListener('disconnected', () => {
  log('Disconnected');
  updateMeta();
  updateButtons();
});
cast.addEventListener('progress', (e) => {
  $('progress').max = e.detail.total;
  $('progress').value = e.detail.sent;
});

// --- frame options ---------------------------------------------------------

for (const id of ['size', 'format', 'fit', 'dither', 'contrast', 'threshold']) {
  $(id).addEventListener('change', () => {
    updateMeta();
    previewSource(currentSource());
  });
}

// --- images ----------------------------------------------------------------

function renderThumbs() {
  const thumbs = $('thumbs');
  thumbs.replaceChildren(
    ...images.map((item, i) => {
      const fig = document.createElement('figure');
      fig.setAttribute('aria-current', i === selected);
      const img = document.createElement('img');
      img.src = item.url;
      img.alt = item.name;
      const cap = document.createElement('figcaption');
      cap.textContent = item.name;
      const remove = document.createElement('button');
      remove.className = 'remove secondary';
      remove.textContent = '×';
      remove.title = 'Remove';
      remove.addEventListener('click', (e) => {
        e.stopPropagation();
        URL.revokeObjectURL(item.url);
        images.splice(i, 1);
        if (selected >= images.length) selected = images.length - 1;
        renderThumbs();
        previewSource(currentSource());
      });
      fig.append(img, cap, remove);
      fig.addEventListener('click', () => {
        selected = i;
        renderThumbs();
        previewSource(item.img);
      });
      return fig;
    }),
  );
  updateButtons();
}

async function addFiles(files) {
  for (const file of files) {
    if (!file.type.startsWith('image/')) continue;
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.src = url;
    await img.decode();
    images.push({ name: file.name, url, img });
  }
  if (selected < 0 && images.length) selected = 0;
  renderThumbs();
  previewSource(currentSource());
}

$('files').addEventListener('change', (e) => addFiles(e.target.files));
const drop = $('drop');
drop.addEventListener('dragover', (e) => {
  e.preventDefault();
  drop.classList.add('over');
});
drop.addEventListener('dragleave', () => drop.classList.remove('over'));
drop.addEventListener('drop', (e) => {
  e.preventDefault();
  drop.classList.remove('over');
  addFiles(e.dataTransfer.files);
});

$('sendOne').addEventListener('click', () => {
  const item = images[selected];
  if (item) send(item.img, { name: stripExt(item.name) }).catch(() => {});
});

function stripExt(name) {
  return name.replace(/\.[^.]+$/, '');
}

function intervalSeconds(id) {
  return Math.max(5, Number($(id).value) || 60);
}

async function showNext() {
  if (!slideshow) return;
  const item = images[slideshow.index];
  selected = slideshow.index;
  renderThumbs();
  const interval = intervalSeconds('interval');
  const last = slideshow.index === images.length - 1 && !$('loop').checked;
  try {
    await send(item.img, { name: stripExt(item.name), nextFrameSeconds: last ? 0 : interval });
  } catch {
    // Logged; keep going at the next tick.
  }
  if (!slideshow) return;
  if (last) return stopShow();
  slideshow.index = (slideshow.index + 1) % images.length;
  slideshow.timer = setTimeout(showNext, interval * 1000);
}

function stopShow() {
  if (slideshow) clearTimeout(slideshow.timer);
  slideshow = null;
  updateButtons();
}

$('startShow').addEventListener('click', () => {
  slideshow = { index: Math.max(0, selected), timer: null };
  updateButtons();
  log(`Slideshow: ${images.length} images every ${intervalSeconds('interval')} s`);
  showNext();
});
$('stopShow').addEventListener('click', () => {
  stopShow();
  log('Slideshow stopped');
});

updateMeta();
updateButtons();
