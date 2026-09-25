// Settings shared by the popup and the session page.
//
// Per-tab state lives in chrome.storage.session (cleared when the browser
// quits): `config:<tabId>` (the popup's settings), `region:<tabId>` (what to
// blit, written by the picker in content.js) and `state:<tabId>` (the
// session's status, for the popup). The last settings used become the
// defaults for the next tab, in chrome.storage.local.

export const DEFAULTS = {
  fit: 'contain', // contain | cover | stretch | actual
  zoom: 1, // 'actual': display pixels per CSS pixel
  format: 'auto', // auto (the display's choice) | 1 | 2 | 3 (protocol format codes)
  dither: 'atkinson', // floyd | atkinson | threshold
  contrast: 1,
  invert: false,
  update: 'live', // live (send when changed) | interval (every N s; the display may sleep) | manual
  seconds: 5,
  capture: 'stream', // stream (tabCapture; works in the background) | screenshot (tab must be visible)
  buttons: 'scroll', // scroll | keys | off
  taps: true, // pointer events from the display click the page
  persist: false,
};

export const WHOLE_TAB = { type: 'viewport' };

export const key = {
  config: (tabId) => `config:${tabId}`,
  region: (tabId) => `region:${tabId}`,
  state: (tabId) => `state:${tabId}`,
};

export async function loadConfig(tabId) {
  const k = key.config(tabId);
  const [{ [k]: config }, { defaults }] = await Promise.all([chrome.storage.session.get(k), chrome.storage.local.get('defaults')]);
  return { ...DEFAULTS, ...defaults, ...config };
}

export async function saveConfig(tabId, config) {
  await Promise.all([
    chrome.storage.session.set({ [key.config(tabId)]: config }),
    chrome.storage.local.set({ defaults: config }),
  ]);
}

export async function loadRegion(tabId) {
  const k = key.region(tabId);
  const { [k]: region } = await chrome.storage.session.get(k);
  return region ?? WHOLE_TAB;
}

export function saveRegion(tabId, region) {
  return chrome.storage.session.set({ [key.region(tabId)]: region });
}

export function describeRegion(region) {
  if (region.type === 'element') return `${region.label} (${region.width}×${region.height} when picked)`;
  if (region.type === 'rect') return `${region.width}×${region.height} at ${region.x}, ${region.y}`;
  return 'The whole visible tab';
}

export function sessionUrl(tabId) {
  return chrome.runtime.getURL(`session.html?tab=${tabId}`);
}

/** The open session page for a tab, if any: { windowId, tabId } (Chrome 116+). */
export async function findSession(tabId) {
  const [context] = await chrome.runtime.getContexts({ contextTypes: ['TAB'], documentUrls: [sessionUrl(tabId)] });
  return context ?? null;
}

/** Inject content.js into the tab unless it's already there. */
export async function ensureContent(tabId) {
  try {
    if (await chrome.tabs.sendMessage(tabId, { blit: 'ping' })) return;
  } catch {
    // Not injected yet (or the page navigated).
  }
  await chrome.scripting.executeScript({ target: { tabId }, files: ['content.js'] });
}
