# blit-server

A headless blit host. It opens a URL in headless Chromium, renders it at the
display's frame area, and sends it to a display that speaks the
[blit protocol](../PROTOCOL.md) over Bluetooth LE. Then it keeps the display up to
date. The display's buttons scroll the page and its taps click it, as with
the [Chrome extension](../chrome-extension/), but with no browser window and
nobody at the keyboard: run it on a Mac mini, a Raspberry Pi, or a server with
a Bluetooth adapter.

It uses the library in [`../js`](../js/) for the protocol, rasterizing and
dithering, and the simulated display. [Puppeteer](https://pptr.dev/) drives
Chromium, and [noble](https://github.com/stoprocent/noble) does Bluetooth.

## Install

Needs Node.js 23.6 or later. It runs the TypeScript sources directly, so
there's no build step.

```sh
cd blit/server
pnpm install    # or npm install; downloads Puppeteer's Chromium
```

Bluetooth:

- **macOS**: the first run asks for Bluetooth permission for your terminal
  app (or allow it in System Settings → Privacy & Security → Bluetooth).
- **Linux**: needs BlueZ, and permission to use raw HCI sockets, e.g.
  `sudo setcap cap_net_raw+eip $(eval readlink -f $(which node))`. See
  noble's README for more.

## Run

```sh
node src/cli.ts https://example.com
```

It scans for the first display advertising the blit service, connects, loads
the page at the size of the display's whole screen, and sends a frame every 10 seconds when the
page has changed. Only the changed rectangle is sent when the display takes
regions. Stop it with Ctrl-C.

On the X4, open **Bluetooth** from the home screen first: it only advertises
while that app is open.

Try it without hardware on the simulated display. `--sim-out` writes what its
panel shows after each frame:

```sh
node src/cli.ts https://en.wikipedia.org/wiki/Electronic_paper --sim --sim-out panel.png --once
```

More examples:

```sh
# A dashboard once a minute, letting the display sleep between frames.
node src/cli.ts http://localhost:3000/ --device X4 --interval 60 --sleep

# One element of a page, crisp black and white.
node src/cli.ts https://example.com --selector main --dither threshold

# A desktop-width layout shrunk to fit (1280 wide, height to match the display), in 4 greys.
node src/cli.ts https://news.ycombinator.com --viewport 1280 --format gray4

# Twice the size: the page is laid out at half the display's size.
node src/cli.ts https://example.com --zoom 2
```

## Options

`node src/cli.ts --help` lists them all.

| Option | Default | |
| --- | --- | --- |
| `--device <name\|id>` | first found | Part of the display's advertised name (`X4-1A2B`), or its id or address |
| `--scan-timeout <s>` | 30 | How long to scan |
| `--sim`, `--sim-size <WxH>`, `--sim-out <png>` | | Use the simulated display instead of Bluetooth: a 716 × 480 frame area on an 800 × 480 panel, like the X4 with its chrome showing |
| `--host-name <text>` | `blit-server: <host>` | Name the display may show |
| `--size panel\|area` | panel | `panel`: render for the whole screen (800 × 480 on the X4), as if the display showed no chrome of its own. The X4 can hide its chrome; while it's showing, the X4 crops the frame to its frame area (716 × 480), dropping the right edge. `area`: render for the frame area the display asks for |
| `--selector <css>` | | Blit one element. Frames are skipped while it's missing or scrolled away |
| `--viewport <W[xH]>` | frame size ÷ zoom | Lay the page out at this CSS size and fit it to the display. With only a width, the height follows the display's aspect ratio, so the page fills it with no letterbox or crop |
| `--zoom <n>` | 1 | Display pixels per CSS pixel |
| `--fit contain\|cover\|none` | contain | How `--selector` and `--viewport` fit the display. `none` is actual size at `--zoom`, top left |
| `--wait-until <event>` | networkidle2 | When a page load counts as done |
| `--settle <ms>` | 500 | Wait after loading, and after input, before taking a frame |
| `--css <css\|@file>` | | Extra CSS for the page, e.g. to hide banners |
| `--user-agent`, `--chrome <path>`, `--no-sandbox` | | Chromium options. `--no-sandbox` for root or containers |
| `--interval <s>` | 10 | Take a frame this often; it's sent only if it changed. 0 = only at start and after input |
| `--reload <s>` | never | Reload the page this often |
| `--once` | | Send one frame and exit (status 1 if it failed) |
| `--sleep` | | Send `nextFrameSeconds` = `--interval`, so a display with frame sleep sleeps between frames |
| `--format <name>` | auto | `auto` (the display's first choice), `mono1`, `gray2`, `gray4`, `gray8`, `rgb565`, `rgb888`. Falls back to auto if the display doesn't take it |
| `--dither`, `--threshold`, `--contrast`, `--invert` | atkinson, 128, 1 | As in the library's `rasterize()`. `threshold` (no dithering) suits text and UI |
| `--refresh auto\|fast\|full` | auto | Refresh hint for displays that honour it |
| `--no-regions` | | Always send whole frames |
| `--persist`, `--name <text>` | | Ask the display to save each frame (the X4: to `/images` on SD) |
| `--preview <png>` | | Write each frame sent, as the display will show it |
| `--buttons scroll\|keys\|off` | scroll | `scroll`: up/down/left/right and page keys scroll the page (or the `--selector` element's scroller), other buttons press keys. `keys`: every button presses a key (arrows, Enter, Escape, PageDown…). |
| `--no-taps` | | Ignore taps. Otherwise a tap clicks the page where it lands |
| `-v`, `--verbose` | | Log buttons, skipped frames, reconnects |

## How it works

- **Size.** The page is laid out at the frame size ÷ `--zoom` CSS pixels: the
  whole panel (800 × 480 on the X4) or, with `--size area`, the frame area
  the display asks for (716 × 480 on the X4 with its chrome showing), with a device scale factor of `--zoom`, so Chromium renders straight
  at the display's resolution: no resampling, sharp text. With `--viewport` or
  `--selector`, the device scale factor is set so the captured part fits.
  Pages are asked for `prefers-color-scheme: light` and
  `prefers-reduced-motion: reduce`.
- **Frames.** Each frame is rasterized in the display's preferred format (or
  `--format`) and sent with the library's `sendFrame()`: skipped if unchanged,
  a region if only part changed (only when the frame is exactly the frame
  area: a larger one is cropped, so it goes whole), PackBits when it's smaller. One frame is in
  flight at a time; one asked for meanwhile is taken right after.
- **Caps.** When the display changes its frame area or formats (a `caps`
  event), the page is laid out again and a whole frame sent.
- **Sleep and reconnects.** With `--sleep`, the display may disconnect to
  sleep after a frame. The next frame reconnects, retrying for 30 s. So does a
  frame after an unexpected disconnect.

## Files

| File | |
| --- | --- |
| `src/cli.ts` | Command line |
| `src/host.ts` | `BlitHost`: the loop, scheduling, and the display's input |
| `src/page.ts` | `PageSource`: headless Chromium, capture and fit, buttons and taps |
| `src/ble.ts` | `NobleTransport`: the blit GATT client over noble, shaped like the library's `WebBluetoothTransport` |
| `src/lib.ts` | Types for the parts of `../js` the server uses |
| `src/png.ts` | PNG in (screenshots) and out (previews) |
| `test/host.test.ts` | End to end against the simulated display |

```sh
pnpm test         # end-to-end tests (headless Chromium + simulated display)
pnpm typecheck    # tsc --noEmit
```
