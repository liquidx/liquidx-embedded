// Demo page for lib/ble-cast.js: send images, or run a slideshow.
import { BleCast, rasterize } from './lib/ble-cast.js';

const FULL = { w: 800, h: 480 };
const DEFAULT_AREA = { w: 716, h: 480 }; // before a device reports its own

const $ = (id) => document.getElementById(id);
const cast = new BleCast();

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
  if ($('size').value === 'full') return FULL;
  const info = cast.info;
  return info ? { w: info.w, h: info.h } : DEFAULT_AREA;
}

function frameOptions() {
  const { w, h } = frameSize();
  return {
    width: w,
    height: h,
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
  // In "device area" mode let sendImage() size the frame from fresh Info: the
  // area changes when the device's chrome is toggled.
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
  const info = cast.info;
  $('mName').textContent = cast.deviceName ?? '–';
  $('mArea').textContent = info ? `${info.w}×${info.h}` : '–';
  $('mChunk').textContent = info ? `${info.chunk} B` : '–';
  $('mSleep').textContent = info ? (info.frameSleep ? 'on' : 'off') : '–';
  const chip = $('chip');
  chip.textContent = cast.connected ? `Connected to ${cast.deviceName}` : cast.deviceName ? `${cast.deviceName} (disconnected)` : 'Not connected';
  chip.classList.toggle('on', cast.connected);
  $('connect').hidden = cast.connected;
  $('disconnect').hidden = !cast.connected;
}

// --- connection ------------------------------------------------------------

if (!BleCast.supported) $('unsupported').hidden = false;

$('connect').addEventListener('click', async () => {
  try {
    await cast.connect();
  } catch (err) {
    if (err.name !== 'NotFoundError') log(err.message, 'err'); // NotFoundError: chooser cancelled
  }
});
$('disconnect').addEventListener('click', () => cast.disconnect());

cast.addEventListener('connected', (e) => {
  log(`Connected to ${e.detail.name}: ${e.detail.w}×${e.detail.h}, ${e.detail.chunk} B chunks`, 'ok');
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

for (const id of ['size', 'fit', 'dither', 'contrast', 'threshold']) {
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
