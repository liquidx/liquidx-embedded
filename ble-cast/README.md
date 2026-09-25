# ble-cast

Push images to the Xteink X4 over Bluetooth LE from a web page. Talks to the
**Bluetooth** app in [`xteink-x4-platformio`](../xteink-x4-platformio/), using
the [BLE cast protocol](../xteink-x4-platformio/docs/ble-cast-protocol.md).

Plain ES modules, no build step, no dependencies.

## Run

```sh
cd ble-cast
python3 -m http.server 8000
```

Open <http://localhost:8000> in **Chrome or Edge** (desktop or Android). Web
Bluetooth needs a secure context, which `localhost` counts as; anywhere else,
serve it over HTTPS. Safari and Firefox have no Web Bluetooth. On macOS, Chrome
also needs Bluetooth permission in System Settings → Privacy & Security.

On the X4, open **Bluetooth** from the home screen (it only advertises while
that app is open), then press **Connect** and pick `X4-XXXX`.

## The demo page

The demo page sends images; to cast part of a web page, see the
[example](#example-cast-part-of-a-webapp).

- **Frame**: size (the device's current frame area, 716 × 480 with chrome, or
  the full 800 × 480), fit, dithering, contrast, and whether the device saves
  frames to its SD card (`/images`, so they show up in the Images app).
- **Images**: drop in images and send one, or run a slideshow at an interval.
  The interval goes to the device as the next-frame hint: with Settings →
  Frame sleep on, it deep-sleeps between images (gaps of 30 s or more) and
  reconnects for the next.
- **Preview** shows exactly the 1-bit pixels that will be sent.

## Example: cast part of a webapp

[`examples/clock/`](examples/clock/) is a small webapp (a clock and day
progress) whose dashboard element is ordinary HTML/CSS. It connects, sizes the
element to the device's frame area, and casts it on demand or every N seconds
with `sendElement()`. Open <http://localhost:8000/examples/clock/>.

`captureElement()` renders an element in place, with no screen-sharing prompt:
it clones the element with its computed styles inlined, embeds its web fonts
and images as data URLs, and draws it into a canvas through an SVG
`<foreignObject>` (browsers have no direct DOM-to-bitmap API). Limits:

- Images, fonts and stylesheets must be same-origin or CORS-enabled (Google
  Fonts is fine; add `crossorigin="anonymous"` to its `<link>`). Anything else
  renders blank or in a fallback font.
- `<iframe>` contents aren't captured. Canvas, `<video>` and form fields are
  captured as they are at that moment.
- Design the element for 1 bit: black on white, sized to the frame area
  (716 × 480 with the device's chrome, 800 × 480 without). Text looks crispest
  with `dither: 'threshold'`.

## As a library

```js
import { BleCast, captureElement, rasterize } from './lib/ble-cast.js';

const cast = new BleCast();
await cast.connect(); // from a click: opens the device chooser

// Anything drawable: <img>, <canvas>, <video>, ImageBitmap.
await cast.sendImage(canvas, { fit: 'cover', dither: 'atkinson', persist: false });

// Cast part of the page, re-rendered each time.
const screen = document.querySelector('#screen');
setInterval(() => cast.sendElement(screen, { dither: 'threshold', nextFrameSeconds: 60 }), 60_000);
```

| API | |
| --- | --- |
| `new BleCast()` | Events: `connected` (detail: device Info), `disconnected`, `progress` (`{ sent, total }`), `status` |
| `connect()` | Device chooser + connect. Needs a user gesture. |
| `reconnect({ timeoutMs })` | Reconnect to the chosen device, retrying (e.g. while it sleeps) |
| `info` / `readInfo()` | Device Info: `{ w, h, chunk, frameSleep, ... }` |
| `sendImage(source, opts)` | Rasterize to the device's frame area (or `opts.width` × `opts.height`) and send |
| `sendElement(element, opts)` | Capture a DOM element in place and send it (fit `contain` by default) |
| `sendFrame(bits, opts)` | Send a raw1 frame you've packed yourself |
| `rasterize(source, opts)` | → `{ bits, width, height, preview }` without sending |
| `captureElement(element, { scale, background })` | → a `<canvas>` of the element, without sending |

Send options: `persist`, `name` (file name when persisted), `nextFrameSeconds`
(when you'll send the next frame; lets the device sleep), `signal`
(`AbortSignal`). Raster options: `width`, `height`, `fit` (`cover`, `contain`,
`none`, `stretch`), `dither` (`floyd`, `atkinson`, `threshold`), `threshold`,
`contrast`, `invert`. `sendElement` also takes `captureScale`.

Frames are queued and sent one at a time. `sendImage` / `sendFrame` resolve
with `{ sleepSeconds }` once the device has shown the frame; if that's above 0,
the device is about to disconnect and sleep, and the next send reconnects.

## Layout

```
index.html, app.js   demo page
lib/ble-cast.js      BleCast: connection and sending
lib/protocol.js      UUIDs, message encoding, CRC-32
lib/raster.js        fit, greyscale, dither, pack to 1 bit
lib/dom-capture.js   captureElement: DOM element -> canvas, in place
examples/clock/      a webapp that casts one of its elements
```
