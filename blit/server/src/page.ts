// The page being blitted: a headless Chromium tab, via Puppeteer. It lays the
// page out for the display, screenshots it to exactly the display's frame
// area (Chromium does the scaling, so text stays sharp), and replays the
// display's buttons and taps on it.

import puppeteer, { type Browser, type KeyInput, type Page, type PuppeteerLifeCycleEvent } from 'puppeteer';

import { decodePng } from './png.ts';

export type Fit = 'contain' | 'cover' | 'none';
export type Buttons = 'scroll' | 'keys' | 'off';

export interface PageOptions {
  url: string;
  /** Blit one element (CSS selector) instead of the viewport. */
  selector?: string;
  /**
   * Lay the page out at this CSS size and fit it to the display, instead of
   * at the display's size. Without a height, the display's aspect ratio.
   */
  viewport?: { width: number; height?: number };
  /** Display pixels per CSS pixel, when the page is laid out at the display's size (or with fit 'none'). */
  zoom: number;
  fit: Fit;
  waitUntil: PuppeteerLifeCycleEvent;
  /** Extra wait after loading, and after input, before a frame is taken. */
  settleMs: number;
  /** Extra CSS injected into the page. */
  css?: string;
  executablePath?: string;
  chromeArgs: string[];
  userAgent?: string;
  log: (text: string) => void;
}

/** RGBA pixels at the display's frame area, as ImageData has them. */
export interface Rgba {
  data: Uint8ClampedArray;
  width: number;
  height: number;
}

/** How the last frame was made from the page, to map taps back. */
interface Transform {
  // The part of the viewport (CSS px) that was captured,
  x: number;
  y: number;
  // its scale (display px per CSS px),
  scale: number;
  // and where it landed on the display.
  dx: number;
  dy: number;
}

// Element that's not on the page (yet): not an error, the frame is skipped.
export class NotFound extends Error {}

export class PageSource {
  #browser: Browser | null = null;
  #page: Page | null = null;
  #size = { width: 0, height: 0 }; // display frame area the page is laid out for
  #transform: Transform | null = null;
  readonly opts: PageOptions;

  constructor(opts: PageOptions) {
    this.opts = opts;
  }

  get page(): Page {
    if (!this.#page) throw new Error('Page not open');
    return this.#page;
  }

  /** Launch Chromium and load the URL, laid out for a width × height frame area. */
  async open(width: number, height: number): Promise<void> {
    const { opts } = this;
    this.#browser = await puppeteer.launch({
      headless: true,
      executablePath: opts.executablePath,
      args: opts.chromeArgs,
      defaultViewport: null,
      // The server shuts Chromium down itself, after the display.
      handleSIGINT: false,
      handleSIGTERM: false,
      handleSIGHUP: false,
    });
    this.#browser.on('disconnected', () => {
      if (this.#browser) opts.log('Chromium exited');
    });
    const page = (this.#page = await this.#browser.newPage());
    if (opts.userAgent) await page.setUserAgent({ userAgent: opts.userAgent });
    // E-paper is slow and monochrome: ask pages for light, still content.
    await page.emulateMediaFeatures([
      { name: 'prefers-color-scheme', value: 'light' },
      { name: 'prefers-reduced-motion', value: 'reduce' },
    ]);
    page.on('pageerror', (err) => opts.log(`Page error: ${err instanceof Error ? err.message : err}`));
    await this.resize(width, height);
    await this.load();
  }

  /** Load (or reload) the URL. */
  async load(): Promise<void> {
    const { opts } = this;
    const page = this.page;
    const response = await page.goto(opts.url, { waitUntil: opts.waitUntil, timeout: 60000 });
    if (response && !response.ok()) opts.log(`${opts.url}: HTTP ${response.status()}`);
    if (opts.css) await page.addStyleTag({ content: opts.css });
    await this.settle();
  }

  async settle(): Promise<void> {
    if (this.opts.settleMs) await new Promise((r) => setTimeout(r, this.opts.settleMs));
  }

  /** Lay the page out for a new frame area (the display's caps changed). */
  async resize(width: number, height: number): Promise<void> {
    if (width === this.#size.width && height === this.#size.height) return;
    this.#size = { width, height };
    const { viewport, zoom, selector } = this.opts;
    // Laid out at the display's size, the viewport is the frame at `zoom`, so
    // Chromium renders at the display's resolution. Otherwise (a set
    // viewport, or one element) grab() sets the scale to fit.
    const direct = !viewport && !selector;
    await this.#setViewport({
      width: viewport?.width ?? Math.max(1, Math.round(width / zoom)),
      height: viewport ? viewport.height ?? Math.max(1, Math.round((viewport.width * height) / width)) : Math.max(1, Math.round(height / zoom)),
      deviceScaleFactor: direct ? zoom : this.#viewport.deviceScaleFactor || 1,
    });
  }

  #viewport = { width: 0, height: 0, deviceScaleFactor: 0 };

  // Screenshots come out at CSS px × deviceScaleFactor, and Chromium renders
  // at that resolution (the clip's own `scale` is ignored), so scaling is done
  // here. Only changed when needed: it costs a repaint.
  async #setViewport(v: { width: number; height: number; deviceScaleFactor: number }) {
    const c = this.#viewport;
    if (v.width === c.width && v.height === c.height && Math.abs(v.deviceScaleFactor - c.deviceScaleFactor) < 1e-6) return;
    this.#viewport = v;
    await this.page.setViewport(v);
  }

  /** Screenshot the page (or element) fitted to width × height, on white. */
  async grab(width: number, height: number): Promise<Rgba> {
    await this.resize(width, height);
    const page = this.page;
    const { selector, viewport, fit, zoom } = this.opts;

    if (!selector && !viewport) {
      const png = await page.screenshot({ type: 'png', optimizeForSpeed: true, captureBeyondViewport: false });
      this.#transform = { x: 0, y: 0, scale: zoom, dx: 0, dy: 0 };
      return place(decodePng(png), width, height, 0, 0);
    }

    // What to capture, in viewport CSS px (the part that's scrolled into view).
    const view = await page.evaluate(() => ({ w: innerWidth, h: innerHeight, sx: scrollX, sy: scrollY }));
    let src = { x: 0, y: 0, width: view.w, height: view.h };
    if (selector) {
      const rect = await page.$eval(selector, (el) => {
        const r = el.getBoundingClientRect();
        return { x: r.x, y: r.y, width: r.width, height: r.height };
      }).catch(() => null);
      if (!rect || !rect.width || !rect.height) throw new NotFound(`No element matches ${selector}`);
      src = rect;
    }

    // Scale, and the part of `src` that fits (cover crops, none keeps the top left).
    const scale = fit === 'none' ? zoom
      : fit === 'cover' ? Math.max(width / src.width, height / src.height)
      : Math.min(width / src.width, height / src.height);
    const fits = { w: Math.min(src.width, width / scale), h: Math.min(src.height, height / scale) };
    let clip = {
      x: src.x + (fit === 'cover' ? (src.width - fits.w) / 2 : 0),
      y: src.y + (fit === 'cover' ? (src.height - fits.h) / 2 : 0),
      width: fits.w,
      height: fits.h,
    };
    // Placed centred for contain, top left otherwise.
    const dx = fit === 'contain' ? (width - clip.width * scale) / 2 : 0;
    const dy = fit === 'contain' ? (height - clip.height * scale) / 2 : 0;

    // Only what's in the viewport renders; the rest stays white.
    const vis = intersect(clip, { x: 0, y: 0, width: view.w, height: view.h });
    this.#transform = { x: clip.x, y: clip.y, scale, dx, dy };
    if (!vis) throw new NotFound(`${selector} is scrolled out of view`);
    const offset = { x: dx + (vis.x - clip.x) * scale, y: dy + (vis.y - clip.y) * scale };
    clip = vis;

    await this.#setViewport({ ...this.#viewport, deviceScaleFactor: scale });
    const png = await page.screenshot({
      type: 'png',
      optimizeForSpeed: true,
      captureBeyondViewport: false,
      // Puppeteer takes the clip in page coordinates.
      clip: { x: clip.x + view.sx, y: clip.y + view.sy, width: clip.width, height: clip.height },
    });
    return place(decodePng(png), width, height, Math.round(offset.x), Math.round(offset.y));
  }

  /** Display pixel -> viewport CSS pixel, by the last frame's transform. */
  toPage(x: number, y: number): { x: number; y: number } | null {
    const t = this.#transform;
    if (!t) return null;
    return { x: t.x + (x - t.dx) / t.scale, y: t.y + (y - t.dy) / t.scale };
  }

  /** A display button, as the extension handles it: scroll the page, or press a key. */
  async button(name: string, mode: Buttons): Promise<boolean> {
    const page = this.page;
    if (mode === 'scroll' && SCROLL[name]) {
      const [dx, dy] = SCROLL[name];
      await page.evaluate(scrollBy, { dx, dy, selector: this.opts.selector ?? null });
      return true;
    }
    if (mode === 'keys' || (mode === 'scroll' && !SCROLL[name])) {
      const key = DOM_KEY[name] ?? (/^f\d+$/.test(name) ? (name.toUpperCase() as KeyInput) : null);
      if (!key) return false;
      await page.keyboard.press(key);
      return true;
    }
    return false;
  }

  /** A tap on the display: click the page where it lands. */
  async tap(x: number, y: number): Promise<{ x: number; y: number } | null> {
    const at = this.toPage(x, y);
    if (!at) return null;
    const view = await this.page.evaluate(() => ({ w: innerWidth, h: innerHeight }));
    if (at.x < 0 || at.y < 0 || at.x >= view.w || at.y >= view.h) return null;
    await this.page.mouse.click(at.x, at.y);
    return at;
  }

  async close(): Promise<void> {
    const browser = this.#browser;
    this.#browser = null;
    this.#page = null;
    await browser?.close().catch(() => {});
  }
}

// Fractions of the viewport (or the element's scroller) per button, as the extension.
const SCROLL: Record<string, [number, number]> = {
  up: [0, -0.8], down: [0, 0.8], left: [-0.8, 0], right: [0.8, 0], pageNext: [0, 0.95], pagePrev: [0, -0.95],
};
const DOM_KEY: Record<string, KeyInput> = {
  up: 'ArrowUp', down: 'ArrowDown', left: 'ArrowLeft', right: 'ArrowRight', select: 'Enter', back: 'Escape',
  menu: 'ContextMenu', home: 'Home', pageNext: 'PageDown', pagePrev: 'PageUp',
};

// Runs in the page: scroll the element's own scroller if it has one, else the window.
function scrollBy({ dx, dy, selector }: { dx: number; dy: number; selector: string | null }) {
  const scrollable = (el: Element | null) => {
    for (let node = el; node && node !== document.documentElement; node = node.parentElement) {
      const style = getComputedStyle(node);
      const canY = /(auto|scroll)/.test(style.overflowY) && node.scrollHeight > node.clientHeight;
      const canX = /(auto|scroll)/.test(style.overflowX) && node.scrollWidth > node.clientWidth;
      if (canY || canX) return node;
    }
    return null;
  };
  const target = selector ? scrollable(document.querySelector(selector)) : null;
  const w = target ? target.clientWidth : innerWidth;
  const h = target ? target.clientHeight : innerHeight;
  const by = { left: dx * w, top: dy * h, behavior: 'instant' as ScrollBehavior };
  if (target) target.scrollBy(by);
  else window.scrollBy(by);
}

function intersect(a: Box, b: Box): Box | null {
  const x = Math.max(a.x, b.x);
  const y = Math.max(a.y, b.y);
  const r = Math.min(a.x + a.width, b.x + b.width);
  const bottom = Math.min(a.y + a.height, b.y + b.height);
  return r - x >= 1 && bottom - y >= 1 ? { x, y, width: r - x, height: bottom - y } : null;
}

interface Box {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** `img` drawn at (dx, dy) on a white width × height frame, cropped to it. */
export function place(img: Rgba, width: number, height: number, dx: number, dy: number): Rgba {
  if (img.width === width && img.height === height && !dx && !dy) return img;
  const data = new Uint8ClampedArray(width * height * 4).fill(255);
  const x0 = Math.max(0, dx);
  const x1 = Math.min(width, dx + img.width);
  if (x1 > x0) {
    for (let y = Math.max(0, dy); y < Math.min(height, dy + img.height); y++) {
      const from = ((y - dy) * img.width + (x0 - dx)) * 4;
      data.set(img.data.subarray(from, from + (x1 - x0) * 4), (y * width + x0) * 4);
    }
  }
  return { data, width, height };
}
