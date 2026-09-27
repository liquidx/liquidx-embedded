#!/usr/bin/env node
// blit-server: render a URL in headless Chromium and send it to a blit
// display over Bluetooth LE. Run with --help for the options.

import { readFile } from 'node:fs/promises';
import { hostname } from 'node:os';
import { parseArgs } from 'node:util';

import { findDisplay } from './ble.ts';
import { BlitHost } from './host.ts';
import { Blit, ENCODING, FORMAT, REFRESH, SimDisplay, SimTransport, type Transport } from './lib.ts';
import type { Buttons, Fit } from './page.ts';
import { writeRgbaPng } from './png.ts';

const USAGE = `Usage: blit-server <url> [options]

Renders <url> in headless Chromium and sends it to a blit display over
Bluetooth LE, then keeps the display up to date. The display's buttons
scroll the page, and taps click it.

Display
  --device <name|id>      Display to use: part of its advertised name (e.g.
                          X4-1A2B), or its id or address. Default: the first
                          display found advertising the blit service.
  --scan-timeout <s>      How long to scan for it (default 30)
  --sim                   Send to a simulated display instead. No Bluetooth needed.
  --sim-size <WxH>        The simulated display's frame area (default 716x480,
                          like the X4)
  --sim-out <file.png>    With --sim: write what the simulated panel shows after
                          every frame
  --host-name <text>      Name the display may show (default "blit-server: <host>")

Page
  --selector <css>        Blit one element instead of the whole viewport
  --size <panel|area>     Render for the whole screen (panel, default), as if
                          the display showed no chrome of its own (the X4 can
                          hide it; while it doesn't, it crops the frame), or
                          for the frame area it asks for (area)
  --viewport <W[xH]>      Lay the page out at this CSS size and fit it to the
                          display (default: the frame size / zoom).
                          With just a width, the height follows the display's
                          aspect ratio, so the page fills it exactly.
  --zoom <n>              Display pixels per CSS pixel (default 1)
  --fit <mode>            With --selector or --viewport: contain (default),
                          cover, or none (actual size at --zoom, top left)
  --wait-until <event>    When the page counts as loaded: load,
                          domcontentloaded, networkidle0, networkidle2 (default)
  --settle <ms>           Wait after loading and after input (default 500)
  --css <css|@file>       Extra CSS for the page
  --user-agent <text>
  --chrome <path>         Chrome or Chromium to run (default: Puppeteer's own)
  --no-sandbox            Run Chromium without its sandbox (root, containers)

Frames
  --interval <s>          Take a frame every <s> seconds; it's sent only if it
                          changed (default 10; 0 = only on start and input)
  --reload <s>            Reload the page every <s> seconds (default: never)
  --once                  Send one frame and exit
  --sleep                 Tell the display when the next frame is due (every
                          --interval), so displays with frame sleep can sleep
  --format <name>         auto (the display's choice, default), mono1, gray2,
                          gray4, gray8, rgb565, rgb888
  --dither <mode>         atkinson (default), floyd, threshold (none; crisp text)
  --threshold <0-255>     Black/white cut for mono1 (default 128)
  --contrast <n>          Contrast multiplier (default 1)
  --invert                White on black
  --refresh <mode>        Refresh hint: auto (default), fast, full
  --no-regions            Always send whole frames, not just the changed part
  --persist               Ask the display to save each frame (the X4: to SD)
  --name <text>           File name for saved frames
  --preview <file.png>    Write each frame sent, as the display will show it

Input
  --buttons <mode>        scroll (default: arrows and page keys scroll the page,
                          other buttons press keys), keys (press keys), off
  --no-taps               Ignore taps from the display

  -v, --verbose           Log more
  -h, --help              Show this help

Examples
  blit-server https://example.com
  blit-server https://example.com --device X4 --interval 60 --sleep
  blit-server http://localhost:3000/dash --selector '#panel' --dither threshold
  blit-server https://en.wikipedia.org --sim --sim-out panel.png --once`;

const { values: args, positionals } = parseArgs({
  allowPositionals: true,
  allowNegative: true,
  options: {
    device: { type: 'string' },
    'scan-timeout': { type: 'string', default: '30' },
    sim: { type: 'boolean', default: false },
    'sim-size': { type: 'string', default: '716x480' },
    'sim-out': { type: 'string' },
    'host-name': { type: 'string' },
    selector: { type: 'string' },
    size: { type: 'string', default: 'panel' },
    viewport: { type: 'string' },
    zoom: { type: 'string', default: '1' },
    fit: { type: 'string', default: 'contain' },
    'wait-until': { type: 'string', default: 'networkidle2' },
    settle: { type: 'string', default: '500' },
    css: { type: 'string' },
    'user-agent': { type: 'string' },
    chrome: { type: 'string' },
    sandbox: { type: 'boolean', default: true },
    interval: { type: 'string', default: '10' },
    reload: { type: 'string', default: '0' },
    once: { type: 'boolean', default: false },
    sleep: { type: 'boolean', default: false },
    format: { type: 'string', default: 'auto' },
    dither: { type: 'string', default: 'atkinson' },
    threshold: { type: 'string', default: '128' },
    contrast: { type: 'string', default: '1' },
    invert: { type: 'boolean', default: false },
    refresh: { type: 'string', default: 'auto' },
    regions: { type: 'boolean', default: true },
    persist: { type: 'boolean', default: false },
    name: { type: 'string', default: '' },
    preview: { type: 'string' },
    buttons: { type: 'string', default: 'scroll' },
    taps: { type: 'boolean', default: true },
    verbose: { type: 'boolean', short: 'v', default: false },
    help: { type: 'boolean', short: 'h', default: false },
  },
});

function usage(error?: string): never {
  if (error) {
    console.error(`blit-server: ${error}\n`);
    console.error(USAGE);
    process.exit(2);
  }
  console.log(USAGE);
  process.exit(0);
}

function number(name: string, value: string, min = 0): number {
  const n = Number(value);
  if (!Number.isFinite(n) || n < min) usage(`--${name} must be a number ≥ ${min}`);
  return n;
}

function size(name: string, value: string): { width: number; height: number } {
  const m = /^(\d+)x(\d+)$/i.exec(value);
  if (!m || !Number(m[1]) || !Number(m[2])) usage(`--${name} must look like 800x480`);
  return { width: Number(m[1]), height: Number(m[2]) };
}

function viewportSize(value: string): { width: number; height?: number } {
  if (/^\d+$/.test(value) && Number(value)) return { width: Number(value) };
  const m = /^(\d+)x(\d+)$/i.exec(value);
  if (!m || !Number(m[1]) || !Number(m[2])) usage('--viewport must look like 1280x800, or a width like 1280');
  return { width: Number(m[1]), height: Number(m[2]) };
}

function oneOf<T extends string>(name: string, value: string, choices: readonly T[]): T {
  if (!choices.includes(value as T)) usage(`--${name} must be one of: ${choices.join(', ')}`);
  return value as T;
}

if (args.help) usage();
if (positionals.length !== 1) usage(positionals.length ? 'give one URL' : 'give a URL to blit');

let url = positionals[0];
if (!/^[a-z][a-z0-9+.-]*:/i.test(url)) url = `https://${url}`;

const FORMATS: Record<string, number | 'auto'> = {
  auto: 'auto', mono1: FORMAT.MONO1, gray2: FORMAT.GRAY2, gray4: FORMAT.GRAY4, gray8: FORMAT.GRAY8, rgb565: FORMAT.RGB565, rgb888: FORMAT.RGB888,
};
const REFRESHES: Record<string, number> = { auto: REFRESH.AUTO, fast: REFRESH.FAST, full: REFRESH.FULL };

const verbose = args.verbose;
const log = (text: string) => console.log(`${new Date().toLocaleTimeString()}  ${text}`);
const debug = (text: string) => verbose && log(text);

let css = args.css;
if (css?.startsWith('@')) css = await readFile(css.slice(1), 'utf8');

const interval = number('interval', args.interval);
const scanTimeoutMs = number('scan-timeout', args['scan-timeout'], 1) * 1000;
const hostName = args['host-name'] ?? `blit-server: ${hostname().replace(/\.local$/, '')}`;

// --- the page (built first, so bad options fail before scanning) ------------------------------------------------------------------------

const blit = new Blit({ hostName });
const host = new BlitHost(blit, {
  page: {
    url,
    selector: args.selector,
    viewport: args.viewport ? viewportSize(args.viewport) : undefined,
    zoom: number('zoom', args.zoom, 0.1),
    fit: oneOf<Fit>('fit', args.fit, ['contain', 'cover', 'none']),
    waitUntil: oneOf('wait-until', args['wait-until'], ['load', 'domcontentloaded', 'networkidle0', 'networkidle2'] as const),
    settleMs: number('settle', args.settle),
    css,
    executablePath: args.chrome,
    chromeArgs: args.sandbox ? [] : ['--no-sandbox', '--disable-setuid-sandbox'],
    userAgent: args['user-agent'],
  },
  size: oneOf('size', args.size, ['panel', 'area'] as const),
  intervalSeconds: interval,
  reloadSeconds: number('reload', args.reload),
  sleep: args.sleep,
  format: FORMATS[oneOf('format', args.format, Object.keys(FORMATS))],
  dither: oneOf('dither', args.dither, ['atkinson', 'floyd', 'threshold'] as const),
  threshold: number('threshold', args.threshold),
  contrast: number('contrast', args.contrast),
  invert: args.invert,
  persist: args.persist,
  name: args.name,
  refresh: REFRESHES[oneOf('refresh', args.refresh, Object.keys(REFRESHES))],
  regions: args.regions,
  buttons: oneOf<Buttons>('buttons', args.buttons, ['scroll', 'keys', 'off']),
  taps: args.taps,
  preview: args.preview,
  reconnectTimeoutMs: 30000,
  log,
  verbose: debug,
});

// --- the display ---------------------------------------------------------------------

let sim: SimDisplay | null = null;
let transport: Transport;
if (args.sim) {
  const simSize = size('sim-size', args['sim-size']);
  sim = new SimDisplay({
    name: 'blit-sim', ...simSize, panelWidth: Math.max(simSize.width, 800), panelHeight: Math.max(simSize.height, 480),
    encodings: [ENCODING.PACKBITS], chunk: 508, window: 16, refreshMs: 100,
  });
  const out = args['sim-out'];
  if (out) {
    sim.addEventListener('frame', () => {
      writeRgbaPng(out, sim!.visible, sim!.caps.width, sim!.caps.height).catch((err) => log(`Couldn't write ${out}: ${err.message}`));
    });
  }
  transport = new SimTransport(sim);
} else {
  try {
    transport = await findDisplay({ device: args.device, timeoutMs: scanTimeoutMs, log: debug });
  } catch (err) {
    console.error(`blit-server: ${(err as Error).message}`);
    process.exit(1);
  }
}

blit.addEventListener('connected', (e) => {
  const c = (e as CustomEvent).detail;
  log(`Connected to ${c.name || blit.deviceName}: ${c.width}×${c.height}, protocol v${c.version}`);
});

try {
  // Retry for a while: a display that just woke can refuse the first attempt.
  await blit.connectTransport(transport).catch(() => blit.reconnect({ timeoutMs: 15000 }));
} catch (err) {
  console.error(`blit-server: couldn't connect to ${transport.name ?? 'the display'}: ${(err as Error).message}`);
  process.exit(1);
}

let stopping = false;
async function shutdown(code: number) {
  if (stopping) return;
  stopping = true;
  await host.stop();
  blit.disconnect();
  process.exit(code);
}
process.on('SIGINT', () => shutdown(0));
process.on('SIGTERM', () => shutdown(0));
process.on('SIGHUP', () => shutdown(0));

try {
  await host.start();
} catch (err) {
  console.error(`blit-server: couldn't load ${url}: ${(err as Error).message}`);
  await shutdown(1);
}

if (args.once) {
  const ok = await host.frame('once').then(() => true, () => false);
  // Let the simulator finish showing (and writing) the frame.
  if (sim) await new Promise((r) => setTimeout(r, sim!.caps.refreshMs + 100));
  await shutdown(ok ? 0 : 1);
} else {
  host.run();
}
