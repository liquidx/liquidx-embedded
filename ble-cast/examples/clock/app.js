// Example: embed ble-cast in a webapp and cast one of its elements.
import { BleCast } from '../../lib/ble-cast.js';

const $ = (id) => document.getElementById(id);
const screen = $('screen');
const cast = new BleCast();
let frames = 0;
let timer = null;

// --- the page's own content ---------------------------------------------------

function isoWeek(d) {
  const t = new Date(Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()));
  t.setUTCDate(t.getUTCDate() + 4 - (t.getUTCDay() || 7));
  return Math.ceil(((t - Date.UTC(t.getUTCFullYear(), 0, 1)) / 86400000 + 1) / 7);
}

function render() {
  const now = new Date();
  $('date').textContent = now.toLocaleDateString(undefined, { weekday: 'short', day: 'numeric', month: 'short' });
  $('time').textContent = now.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit', hour12: false });
  const pct = Math.round(((now.getHours() * 60 + now.getMinutes()) / 1440) * 100);
  $('dayPct').textContent = `${pct}%`;
  $('dayBar').style.setProperty('--pct', `${pct}%`);
  $('week').textContent = `W${isoWeek(now)}`;
  $('frames').textContent = String(frames);
}
render();
setInterval(render, 1000);

// --- casting ----------------------------------------------------------------

function status(text, kind = '') {
  $('status').textContent = text;
  $('status').className = kind;
}

function every() {
  return Math.max(10, Number($('every').value) || 60);
}

async function send() {
  render();
  const started = performance.now();
  try {
    const { sleepSeconds } = await cast.sendElement(screen, {
      dither: $('dither').value,
      persist: $('persist').checked,
      name: 'clock',
      nextFrameSeconds: timer ? every() : 0,
      onRaster: (frame) => {
        $('preview').width = frame.width;
        $('preview').height = frame.height;
        $('preview').getContext('2d').putImageData(frame.preview, 0, 0);
      },
    });
    frames += 1;
    const secs = ((performance.now() - started) / 1000).toFixed(1);
    status(`Sent in ${secs} s${sleepSeconds ? `, device sleeping ${sleepSeconds} s` : ''}`, 'ok');
  } catch (err) {
    status(err.message, 'err');
  }
}

function sizeToDevice(info) {
  // Pixel-exact: make the element exactly the device's frame area.
  screen.style.width = `${info.w}px`;
  screen.style.height = `${info.h}px`;
}

$('connect').addEventListener('click', async () => {
  try {
    await cast.connect();
  } catch (err) {
    if (err.name !== 'NotFoundError') status(err.message, 'err');
  }
});

cast.addEventListener('connected', (e) => {
  sizeToDevice(e.detail);
  status(`Connected to ${e.detail.name} (${e.detail.w}×${e.detail.h})`, 'ok');
  $('send').disabled = false;
  $('auto').disabled = false;
  $('connect').textContent = 'Reconnect';
});
cast.addEventListener('disconnected', () => {
  if (!timer) status('Disconnected');
});
cast.addEventListener('progress', (e) => status(`Sending… ${Math.round((e.detail.sent / e.detail.total) * 100)}%`));

$('send').addEventListener('click', send);

$('auto').addEventListener('click', () => {
  if (timer) {
    clearTimeout(timer);
    timer = null;
    $('auto').textContent = 'Auto-send';
    status('Auto-send stopped');
    return;
  }
  $('auto').textContent = 'Stop';
  const tick = async () => {
    await send();
    if (timer) timer = setTimeout(tick, every() * 1000);
  };
  timer = -1; // running; the first send's hint uses the interval
  tick();
});
