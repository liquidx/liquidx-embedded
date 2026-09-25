# blit

A small protocol for sending finished pixels from a host (a browser, a
phone, a server) to a low-power display over Bluetooth LE, and for sending the
display's button presses back. The host does all the rendering, and the
display just copies bytes to its panel.

- **[PROTOCOL.md](PROTOCOL.md)**: the spec, with its design principles.
- **[js/](js/)**: the host library (Web Bluetooth), and a simulated display.
- **[web/](web/)**: demo page: send images and slideshows, and a webapp that
  blits one of its own elements.
- **[chrome-extension/](chrome-extension/)**: blit any browser tab, or part
  of one, and drive the page with the display's buttons.

Displays: the [Xteink X4 firmware](../xteink-x4-platformio/) (Bluetooth app)
speaks v2. The library speaks v1 and v2. The simulated display speaks both,
so everything here can be tried without hardware.

blit was called "ble-cast" until it grew button events and caps
negotiation. The GATT UUIDs haven't changed, so existing displays still
work.

## Run the web demo

```sh
cd blit
npx vite
```

[Vite](https://vite.dev/) serves `blit/` on <http://localhost:8000/> (see
`vite.config.mjs`), opens <http://localhost:8000/web/>, and reloads the page
when you edit anything under `web/` or `js/`. The first run asks npx to
download Vite; nothing is installed in the repo. Use **Chrome or Edge**
(desktop or Android).
Web Bluetooth needs a secure context, which `localhost` counts as; anywhere
else, serve it over HTTPS. Safari and Firefox have no Web Bluetooth. On macOS,
Chrome also needs Bluetooth permission in System Settings → Privacy &
Security. Serve from `blit/` rather than `blit/web/` because the pages import
`../js/`.

On the X4, open **Bluetooth** from the home screen (it only advertises while
that app is open), then press **Connect** and pick `X4-XXXX`. Or press
**Simulator** to get a display in the page, with buttons.

- **Frame**: size (the display's frame area, or the whole panel), pixel
  format (the display's choice, 1-bit, 4 or 16 greys), fit, dithering,
  contrast, and whether the display keeps each frame (the X4 saves to
  `/images` on its SD card).
- **Images**: drop in images and send one, or run a slideshow at an interval.
  The interval goes to the display as the next-frame hint: the X4 with
  Settings → Frame sleep on deep-sleeps between images (gaps of 30 s or more)
  and reconnects for the next.
- Buttons on the display step through the images.
- **Preview** shows exactly the pixels that will be sent.

### Example: blit part of a webapp

[`web/examples/clock/`](web/examples/clock/) is a small webapp (a clock and day
progress) whose dashboard element is ordinary HTML/CSS. It connects, sizes the
element to the display's frame area, and blits it on demand, every N seconds,
or when a button on the display is pressed, with `sendElement()`. Open
<http://localhost:8000/web/examples/clock/>.

`captureElement()` renders an element in place, with no screen-sharing
prompt: it clones the element with its computed styles inlined, embeds its web
fonts and images as data URLs, and draws it into a canvas through an SVG
`<foreignObject>` (browsers have no direct DOM-to-bitmap API). Limits:

- Images, fonts and stylesheets must be same-origin or CORS-enabled (Google
  Fonts is fine; add `crossorigin="anonymous"` to its `<link>`). Anything else
  renders blank or in a fallback font.
- `<iframe>` contents aren't captured. Canvas, `<video>` and form fields are
  captured as they are at that moment.
- Design the element for the display: black on white, sized to the frame area
  (716 × 480 on the X4 with its chrome, 800 × 480 without). Text looks
  crispest with `dither: 'threshold'`.

The [Chrome extension](chrome-extension/) does the same for pages you don't
control, by capturing the tab instead. It has a build step (`npm run dev` for an
unbundled copy to load while developing, `npm run build` for a bundled zip to
distribute), described in its README.

## The library

Plain ES modules, no build step, no dependencies.

```js
import { Blit, FORMAT } from './js/blit.js';

const blit = new Blit({ hostName: 'My dashboard' });
await blit.connect(); // from a click: opens the device chooser

// Anything drawable: <img>, <canvas>, <video>, ImageBitmap. Rendered at the
// display's frame area in the pixel format it prefers.
await blit.sendImage(canvas, { fit: 'cover', dither: 'atkinson' });

// Blit part of the page, re-rendered each time.
const screen = document.querySelector('#screen');
setInterval(() => blit.sendElement(screen, { dither: 'threshold', skipUnchanged: true, regions: true }), 10_000);

// The display's buttons, and changes to what it asks for.
blit.addEventListener('key', (e) => console.log(e.detail.name, e.detail.action)); // 'down', 'press'
blit.addEventListener('caps', (e) => console.log('now wants', e.detail.width, e.detail.height));
```

| API | |
| --- | --- |
| `new Blit({ hostName })` | Events: `connected` (detail: caps), `disconnected`, `caps`, `key` (`{ key, name, action }`), `pointer` (`{ action, x, y }`), `power`, `progress` (`{ sent, total }`), `status` |
| `connect()` | Device chooser + connect. Needs a user gesture. |
| `connectTransport(t)` | Connect over another transport: `new SimTransport(simDisplay)`, or `new WebBluetoothTransport(device)` for a device you already have (e.g. from `navigator.bluetooth.getDevices()`) |
| `reconnect({ timeoutMs })` | Reconnect to the chosen display, retrying (e.g. while it sleeps) |
| `caps` / `readCaps()` | The display's caps, normalised from v1 JSON or v2 TLV: `{ version, name, width, height, panel, formats, encodings, maxBytes, chunk, window, features, regionAlign, keys, minIntervalMs, refreshMs, battery }` |
| `preferredFormat()` | The first of `caps.formats` the library can render |
| `sendImage(source, opts)` | Rasterize to the frame area (or `opts.width` × `opts.height`) and send |
| `sendElement(element, opts)` | Capture a DOM element in place and send it (fit `contain` by default) |
| `sendFrame(pixels, opts)` | Send pixels you've packed yourself |
| `rasterize(source, opts)` | → `{ pixels, width, height, format, preview }` without sending |
| `captureElement(element, { scale, background })` | → a `<canvas>` of the element, without sending |

Send options: `persist`, `name` (file name when persisted), `nextFrameSeconds`
(when you'll send the next frame; lets the display sleep), `skipUnchanged`
(don't send a frame identical to the last), `regions` (send only the changed
rectangle, when the display takes regions), `encoding` (`'auto'` uses PackBits
when the display takes it and it helps), `refresh` (0 auto, 1 fast, 2 full),
`signal` (`AbortSignal`). Raster options: `width`, `height`, `format`, `fit`
(`cover`, `contain`, `none`, `stretch`), `dither` (`floyd`, `atkinson`,
`threshold`), `threshold`, `contrast`, `invert`. `sendElement` also takes
`captureScale`.

Frames are queued and sent one at a time. The send methods resolve with
`{ sleepSeconds, skipped, region, bytes }` once the display has shown the
frame. If `sleepSeconds` is above 0, the display is about to disconnect and
sleep, and the next send reconnects. A v1 display gets v1 headers and 1-bit
full frames. Region, encoding and format options are dropped or refused as
needed.

### Simulated display

```js
import { SimDisplay, SimTransport } from './js/sim-display.js';

const display = new SimDisplay({ width: 400, height: 300, formats: [FORMAT.GRAY4, FORMAT.MONO1] });
display.addEventListener('frame', () => ctx.putImageData(display.imageData(), 0, 0));
await blit.connectTransport(new SimTransport(display));
display.pressKey(KEY.DOWN);                       // -> 'key' event on the host
display.setCaps({ width: 200, height: 150 });     // -> 'caps' event; next frame follows
```

`SimDisplay` implements the display side of v2 (and v1 with `version: 1`):
validation and error codes, flow control, streaming PackBits, regions with
`hold`, sleeping between frames. It's the reference for display implementers
as much as a test tool.

## Tests

```sh
node --test blit/js/test/*.test.mjs
```

These cover the codec (headers, caps TLV and v1 JSON, events, PackBits,
CRC), pixel packing for every format, and a host talking to a simulated
display end to end: v2 with PackBits, greyscale, regions, skipped frames,
keys, caps changes, a v1 display, and error handling.

## Layout

```
PROTOCOL.md             the spec
js/blit.js              Blit (host): connection, sending, events; transports
js/protocol.js          UUIDs, message encoding and parsing, CRC-32, PackBits
js/raster.js            fit, greyscale, dither, pack to any format
js/dom-capture.js       captureElement: DOM element -> canvas, in place
js/sim-display.js       SimDisplay (a v1/v2 display in JS) and SimTransport
js/test/                node --test
web/                    demo page, and examples/clock/
chrome-extension/       blit a browser tab (npm run dev / build)
```
