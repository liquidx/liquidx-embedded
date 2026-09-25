// The toolbar popup: settings for blitting this tab, and start / stop. The
// work happens in the session page (session.html), which the popup opens,
// because a popup closes as soon as it loses focus (e.g. to the Bluetooth
// device chooser).
import {
  WHOLE_TAB, describeRegion, ensureContent, findSession, key, loadConfig, loadRegion, saveConfig, saveRegion, sessionUrl,
} from './config.js';

const $ = (id) => document.getElementById(id);
const FIELDS = ['fit', 'zoom', 'format', 'dither', 'contrast', 'invert', 'update', 'seconds', 'capture', 'buttons', 'taps', 'persist'];
const ALL_SITES = { origins: ['<all_urls>'] };

const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
let config = await loadConfig(tab.id);
let region = await loadRegion(tab.id);
let session = await findSession(tab.id);

// --- settings ----------------------------------------------------------------------

function fill() {
  for (const id of FIELDS) {
    const el = $(id);
    if (el.type === 'checkbox') el.checked = !!config[id];
    else el.value = String(config[id]);
  }
  showRegion();
  showDependent();
}

function read() {
  const next = { ...config };
  for (const id of FIELDS) {
    const el = $(id);
    if (el.type === 'checkbox') next[id] = el.checked;
    else if (el.type === 'number') next[id] = Number(el.value) || config[id];
    else next[id] = el.value;
  }
  return next;
}

function showDependent() {
  $('zoomBox').hidden = config.fit !== 'actual';
  $('secondsBox').hidden = config.update === 'manual';
  $('secondsLabel').textContent = config.update === 'interval' ? 'Every (s)' : 'Check every (s)';
  $('updateNote').textContent = {
    live: 'Captures on each check, sends only when something changed.',
    interval: 'Sends every time and tells the display when the next frame is due, so it can sleep in between.',
    manual: 'Sends on "Send now" and after a display button press.',
  }[config.update];
}

for (const id of FIELDS) {
  $(id).addEventListener('change', async () => {
    config = read();
    showDependent();
    await saveConfig(tab.id, config);
  });
}

// --- region ------------------------------------------------------------------------

function showRegion() {
  $('regionType').value = region.type;
  $('pick').hidden = region.type === 'viewport';
  $('pick').textContent = 'Pick again';
  $('regionText').textContent = describeRegion(region);
}

async function pick(mode) {
  try {
    // The picker (content.js) saves its result straight to session storage.
    await chrome.storage.session.setAccessLevel({ accessLevel: 'TRUSTED_AND_UNTRUSTED_CONTEXTS' });
    await ensureContent(tab.id);
    await chrome.tabs.sendMessage(tab.id, { blit: 'pick', mode, tabId: tab.id });
    window.close(); // out of the way of the page
  } catch (err) {
    $('regionText').textContent = `Can't pick on this page: ${err.message}`;
  }
}

$('regionType').addEventListener('change', async () => {
  const type = $('regionType').value;
  if (type === 'viewport') {
    region = WHOLE_TAB;
    await saveRegion(tab.id, region);
    showRegion();
  } else {
    await pick(type);
  }
});
$('pick').addEventListener('click', () => pick(region.type));

// --- session -----------------------------------------------------------------------

function showState(state) {
  const chip = $('state');
  const running = !!session;
  chip.className = `chip${running && state?.connected ? ' on' : running ? ' warn' : ''}`;
  chip.textContent = running ? state?.summary ?? 'Running' : 'Not running';
  $('start').hidden = running;
  $('sendNow').hidden = !running;
  $('show').hidden = !running;
  $('stop').hidden = !running;
}

async function refreshState() {
  const k = key.state(tab.id);
  const { [k]: state } = await chrome.storage.session.get(k);
  showState(state);
}

chrome.storage.onChanged.addListener(async (changes, area) => {
  if (area !== 'session') return;
  if (changes[key.region(tab.id)]) {
    region = changes[key.region(tab.id)].newValue ?? WHOLE_TAB;
    showRegion();
  }
  if (changes[key.state(tab.id)]) {
    session = await findSession(tab.id);
    showState(changes[key.state(tab.id)].newValue);
  }
});

const command = (name) => chrome.runtime.sendMessage({ blit: 'command', tabId: tab.id, command: name });

$('start').addEventListener('click', async () => {
  config = read();
  await saveConfig(tab.id, config);
  session = await findSession(tab.id);
  if (session) {
    await showSession();
  } else {
    // A normal window, not type 'popup': Chrome anchors the Bluetooth chooser
    // to the address bar, and in a window without one requestDevice() fails
    // at once with "User cancelled the requestDevice() chooser".
    await chrome.windows.create({ url: sessionUrl(tab.id), type: 'normal', width: 460, height: 860 });
  }
  window.close();
});
$('sendNow').addEventListener('click', () => command('send'));
$('show').addEventListener('click', async () => {
  if (session) await showSession();
  window.close();
});

// Bring the session to the front: its window, and its tab within it.
async function showSession() {
  await chrome.windows.update(session.windowId, { focused: true });
  if (session.tabId != null) await chrome.tabs.update(session.tabId, { active: true });
}
$('stop').addEventListener('click', async () => {
  await command('stop').catch(() => {});
  session = null;
  showState(null);
});

// Host permission for every site: lets the session re-inject content.js after
// the page navigates (activeTab only covers the page as it was when opened).
$('allSites').checked = await chrome.permissions.contains(ALL_SITES);
$('allSites').addEventListener('change', async (e) => {
  const want = e.target.checked;
  const ok = want ? await chrome.permissions.request(ALL_SITES) : await chrome.permissions.remove(ALL_SITES);
  if (!ok) e.target.checked = !want;
});

// --- start ---------------------------------------------------------------------------

$('tabName').textContent = tab.title || tab.url || `Tab ${tab.id}`;
$('tabName').title = tab.url ?? '';
const capturable = /^(https?|file):/.test(tab.url ?? '');
$('unavailable').hidden = capturable;
$('start').disabled = !capturable;
$('regionType').disabled = !capturable;
fill();
await refreshState();
