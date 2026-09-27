// The server's main loop: keep a page in headless Chromium, and send it to a
// blit display on a schedule, when the display's caps change, and after
// its buttons or taps change the page.

import { FORMAT_INFO, rasterizeRgba, type Blit, type Caps, type Region } from './lib.ts';
import { PageSource, NotFound, type Buttons, type PageOptions } from './page.ts';
import { writeFramePng } from './png.ts';

export interface HostOptions {
  page: Omit<PageOptions, 'log'>;
  /** Seconds between frames (a frame is only sent if it changed). 0 = only on events. */
  /**
   * Frame size: 'panel' renders for the whole screen, as if the display
   * showed no chrome of its own (it crops the frame to its frame area while
   * it does); 'area' renders for the frame area it asks for.
   */
  size: 'panel' | 'area';
  intervalSeconds: number;
  /** Seconds between page reloads. 0 = never. */
  reloadSeconds: number;
  /** Tell the display when the next frame is due, so it can sleep. */
  sleep: boolean;
  /** Protocol format code, or 'auto' for the display's choice. */
  format: number | 'auto';
  dither: 'floyd' | 'atkinson' | 'threshold';
  threshold: number;
  contrast: number;
  invert: boolean;
  persist: boolean;
  name: string;
  refresh: number;
  regions: boolean;
  buttons: Buttons;
  taps: boolean;
  /** Write each frame sent as a PNG here. */
  preview?: string;
  reconnectTimeoutMs: number;
  log: (text: string) => void;
  verbose: (text: string) => void;
}

export interface FrameResult {
  sent: boolean;
  skipped?: boolean;
  bytes?: number;
  region?: Region | null;
  sleepSeconds?: number;
}

export class BlitHost {
  readonly blit: Blit;
  readonly source: PageSource;
  readonly opts: HostOptions;
  #busy: Promise<FrameResult> | null = null;
  #again = false;
  #timer: NodeJS.Timeout | null = null;
  #reloadTimer: NodeJS.Timeout | null = null;
  #lastSentAt = 0;
  #lastError = '';
  #sleeping = false;
  #started = false;
  #sendWhole = false; // the display's caps changed: it needs a whole frame to patch
  #stopped = false;
  stats = { sent: 0, skipped: 0 };

  constructor(blit: Blit, opts: HostOptions) {
    this.blit = blit;
    this.opts = opts;
    this.source = new PageSource({ ...opts.page, log: opts.log });
    blit.addEventListener('caps', this.#onCaps);
    blit.addEventListener('key', this.#onKey);
    blit.addEventListener('pointer', this.#onPointer);
    blit.addEventListener('power', this.#onPower);
    blit.addEventListener('disconnected', this.#onDisconnected);
    // A display forgets its frame on every new connection: send it whole.
    blit.addEventListener('connected', () => (this.#sendWhole = true));
  }

  /** Open the page at the display's size. Call once connected. */
  async start(): Promise<void> {
    const caps = this.blit.caps;
    if (!caps) throw new Error('Not connected to a display');
    const { width, height } = this.#frameSize(caps);
    this.opts.log(`Loading ${this.opts.page.url} at ${width}×${height}`);
    await this.source.open(width, height);
    this.#started = true;
  }

  /** Send frames on the schedule until stop(). */
  run(): void {
    this.#schedule(0);
    const reload = this.opts.reloadSeconds * 1000;
    if (reload > 0) {
      this.#reloadTimer = setInterval(async () => {
        try {
          await this.source.load();
          this.opts.verbose('Reloaded the page');
          this.frame('reload');
        } catch (err) {
          this.#fail(`Couldn't reload: ${(err as Error).message}`);
        }
      }, reload);
    }
  }

  async stop(): Promise<void> {
    this.#stopped = true;
    if (this.#timer) clearTimeout(this.#timer);
    if (this.#reloadTimer) clearInterval(this.#reloadTimer);
    await this.#busy?.catch(() => {});
    await this.source.close();
  }

  /**
   * Render and send a frame now. A frame asked for while one is in flight is
   * taken once that one's done (a queue of one).
   */
  frame(reason = 'manual'): Promise<FrameResult> {
    if (this.#busy) {
      this.#again = true;
      return this.#busy;
    }
    const run = async (): Promise<FrameResult> => {
      let result: FrameResult = { sent: false };
      let error: unknown = null;
      do {
        this.#again = false;
        try {
          result = await this.#frame(reason);
          error = null;
        } catch (err) {
          error = err;
        }
        reason = 'queued';
      } while (this.#again && !this.#stopped);
      if (error) throw error;
      return result;
    };
    this.#busy = run().finally(() => (this.#busy = null));
    return this.#busy;
  }

  async #frame(reason: string): Promise<FrameResult> {
    const { opts, blit } = this;
    try {
      if (!blit.connected) {
        opts.log(`Reconnecting to ${blit.deviceName ?? 'the display'}…`);
        await blit.reconnect({ timeoutMs: opts.reconnectTimeoutMs });
      } else if (blit.caps!.version < 2) {
        await blit.readCaps(); // v1 has no caps events: re-read before every frame
      }
      const caps = blit.caps!;
      const wait = this.#lastSentAt + caps.minIntervalMs - Date.now();
      if (wait > 0) await new Promise((r) => setTimeout(r, wait));

      const format = this.#chooseFormat();
      const size = this.#frameSize(caps);
      const rgba = await this.source.grab(size.width, size.height);
      const frame = rasterizeRgba(rgba, {
        format, dither: opts.dither, threshold: opts.threshold, contrast: opts.contrast, invert: opts.invert,
      });
      const nextFrameSeconds = opts.sleep ? opts.intervalSeconds : 0;
      const result = await blit.sendFrame(frame.pixels, {
        format,
        width: frame.width,
        height: frame.height,
        skipUnchanged: !this.#sendWhole,
        // Regions patch a frame of exactly the frame area; a bigger one is cropped.
        regions: opts.regions && !this.#sendWhole && size.width === caps.width && size.height === caps.height,
        nextFrameSeconds,
        persist: opts.persist,
        name: opts.name,
        refresh: opts.refresh,
        reconnectTimeoutMs: opts.reconnectTimeoutMs,
      });
      this.#lastError = '';
      this.#sendWhole = false;
      if (result.skipped) {
        this.stats.skipped++;
        opts.verbose(`Unchanged (${reason}), not sent`);
        return { sent: false, skipped: true };
      }
      this.#lastSentAt = Date.now();
      this.stats.sent++;
      const what = result.region ? `region ${result.region.width}×${result.region.height} at ${result.region.x},${result.region.y}` : `${frame.width}×${frame.height}`;
      const fmt = (FORMAT_INFO as Record<number, { name: string }>)[format]?.name ?? format;
      opts.log(`Sent ${what} ${fmt}, ${result.bytes} bytes${result.encoding ? ' packbits' : ''} (${reason})`);
      if (opts.preview) await writeFramePng(opts.preview, frame.pixels, format, frame.width, frame.height);
      if (result.sleepSeconds > 0) {
        this.#sleeping = true;
        opts.log(`Display sleeps for ${result.sleepSeconds} s`);
      }
      return { sent: true, bytes: result.bytes, region: result.region, sleepSeconds: result.sleepSeconds };
    } catch (err) {
      if (err instanceof NotFound) this.#fail(`${err.message}; frame skipped`);
      else this.#fail(`Frame failed: ${(err as Error).message}`);
      throw err;
    }
  }

  #frameSize(caps: Caps): { width: number; height: number } {
    if (this.opts.size === 'panel' && caps.panel?.width && caps.panel.height) return caps.panel;
    return { width: caps.width, height: caps.height };
  }

  #chooseFormat(): number {
    const { format } = this.opts;
    if (format === 'auto') return this.blit.preferredFormat();
    if (this.blit.caps!.formats.includes(format)) return format;
    const fallback = this.blit.preferredFormat();
    this.#fail(`The display doesn't take ${FORMAT_INFO[format as keyof typeof FORMAT_INFO]?.name ?? format}; sending ${FORMAT_INFO[fallback as keyof typeof FORMAT_INFO]?.name}`);
    return fallback;
  }

  // Log a problem once, until something else happens.
  #fail(text: string) {
    if (text !== this.#lastError) this.opts.log(text);
    this.#lastError = text;
  }

  #schedule(ms: number) {
    if (this.#stopped) return;
    if (this.#timer) clearTimeout(this.#timer);
    this.#timer = setTimeout(async () => {
      const started = Date.now();
      await this.frame(ms ? 'interval' : 'start').catch(() => {});
      const interval = this.opts.intervalSeconds * 1000;
      if (interval > 0) this.#schedule(Math.max(0, started + interval - Date.now()));
    }, ms);
  }

  // Give the page a moment to react to input, then show the result.
  async #afterInput() {
    await this.source.settle();
    this.frame('input').catch(() => {});
  }

  #onCaps = (e: Event) => {
    const c = (e as CustomEvent).detail;
    if (!this.#started) return; // the reply to hello, before there's a page to render
    this.opts.log(`Display asks for ${c.width}×${c.height}, ${(FORMAT_INFO as Record<number, { name: string }>)[c.formats[0]]?.name ?? c.formats[0]}`);
    this.#sendWhole = true;
    this.frame('caps').catch(() => {});
  };

  #onKey = async (e: Event) => {
    const { name, action } = (e as CustomEvent).detail;
    this.opts.verbose(`Button: ${name} (${action})`);
    if (this.opts.buttons === 'off' || !['press', 'long', 'repeat'].includes(action)) return;
    try {
      if (await this.source.button(name, this.opts.buttons)) await this.#afterInput();
    } catch (err) {
      this.opts.log(`Couldn't send ${name} to the page: ${(err as Error).message}`);
    }
  };

  #onPointer = async (e: Event) => {
    const { action, x, y } = (e as CustomEvent).detail;
    if (!this.opts.taps || action !== 'tap') return;
    try {
      const at = await this.source.tap(x, y);
      if (!at) return;
      this.opts.verbose(`Tap at ${Math.round(at.x)}, ${Math.round(at.y)}`);
      await this.#afterInput();
    } catch (err) {
      this.opts.log(`Couldn't click the page: ${(err as Error).message}`);
    }
  };

  #onPower = (e: Event) => {
    const { percent, charging } = (e as CustomEvent).detail;
    this.opts.verbose(`Display battery ${percent ?? '?'}%${charging ? ', charging' : ''}`);
  };

  #onDisconnected = () => {
    if (this.#stopped) return;
    if (this.#sleeping) this.opts.verbose('Display disconnected to sleep');
    else this.opts.log('Display disconnected; reconnecting for the next frame');
    this.#sleeping = false;
  };
}
