# Blit Chrome extension

Blit a browser tab, or one part of it, to a display that speaks the
[blit protocol](../PROTOCOL.md), such as the Xteink X4's Bluetooth app. It
keeps the display updated as the page changes, and drives the page with the
display's buttons. It uses the library in [`../js`](../js/).

## Install

The extension's sources import the protocol library from [`../js`](../js/),
which the web demo shares, so Chrome loads a built copy rather than this
folder. You need Node.js 20 or later.

```sh
cd blit/chrome-extension
npm install          # esbuild, for the bundled build
npm run dev          # -> build/dev: unbundled, for development
```

Then go to `chrome://extensions`, turn on **Developer mode**, click **Load
unpacked** and pick `blit/chrome-extension/build/dev`.

| Command | Output | Use |
| --- | --- | --- |
| `npm run dev` | `build/dev/` | Unbundled ES modules: the sources as they are, with the library copied into `lib/`, one file each, so DevTools shows them as written. Doesn't need `npm install`. |
| `npm run watch` | `build/dev/` | The same, rebuilt when anything here or in `../js` changes. Then click the extension's reload button in `chrome://extensions`. |
| `npm run build` | `build/dist/` and `build/blit-<version>.zip` | For distribution: esbuild bundles `popup.js` and `session.js` (library included) into one minified file each, and minifies the content script and worker. The zip is what the Chrome Web Store takes; load `build/dist` unpacked to test exactly what ships. |
| `npm test` | | The library's tests (`../js/test`) |

Both builds check that every file the manifest, the HTML pages and the
scripts refer to is in the output, and fail if one isn't. The version comes
from `manifest.json`.

Needs Chrome (or Edge) 116 or later. Web Bluetooth works on desktop Chrome on
macOS, Windows, ChromeOS and Linux (on Linux you may need
`chrome://flags/#enable-experimental-web-platform-features`).

## Use

1. On the tab you want on the display, click the **Blit** toolbar button.
2. Choose **what to blit**, the scaling, pixels and update rate in the popup
   (below). Settings are per tab, and the last ones you used become the
   defaults for the next tab.
3. Press **Start blitting**. A small session window opens: press **Connect
   display** and pick it in Chrome's chooser (on the X4, open the Bluetooth
   app first). Or press **Simulator** to try it without hardware.
4. Leave the session window open, behind other windows or minimised, while
   you want the display updated. Closing it (or **Stop** in the popup, or
   closing the tab) stops blitting. The toolbar badge shows `ON` on tabs being
   blitted.

Reopen the popup at any time to change settings. The session picks up
changes immediately. **Send now** forces a frame.

### What to blit

| Choice | How it's tracked |
| --- | --- |
| **Whole visible tab** | The tab's viewport, whatever is scrolled into view. |
| **One element…** | Click an element on the page (hover highlights it; Esc cancels). The extension stores a CSS selector for it and finds the element again before every frame, so it follows the element as the page lays out, scrolls or re-renders. If part of the element is scrolled out of view, that part is left white. If it disappears, frames are skipped until it's back. |
| **A rectangle…** | Drag out a rectangle. It's fixed to the viewport (like a window onto the tab), not to the content. |

Picking an element is the usual choice for dashboards: size the element to
the display's frame area (716 × 480 for the X4) and choose **Actual pixels**
for a pixel-exact result.

### Scaling and pixels

| Fit | Result |
| --- | --- |
| **Fit (letterbox)** | Scales the region to fit inside the display, white bars on the sides. |
| **Fill (crop edges)** | Scales it to cover the display, cropping the overflow. |
| **Stretch** | Fills the display, ignoring aspect ratio. |
| **Actual pixels** | No scaling: one CSS pixel is `zoom` display pixels, pinned top-left and cropped. Captures at CSS resolution, not the screen's device pixels. |

**Pixels** is the display's choice by default, meaning the first format it
lists in its caps. So a display that switches itself to greyscale gets
greyscale. Or force 1-bit, 4 greys or 16 greys; if the display doesn't take
that format, its choice is used. **Dither** is Atkinson (crisp photos),
Floyd–Steinberg (smooth gradients), or none (crispest text and UI).
**Contrast** and **Invert** apply before dithering.

### Update rates

| Send | Behaviour |
| --- | --- |
| **When the page changes** | Captures every N seconds, and sends only if the rendered frame differs from the last one sent. If the display takes regions, only the changed rectangle is sent. Best for dashboards: an idle page costs nothing on the radio. |
| **On a fixed interval** | Sends every N seconds, changed or not, and tells the display when the next frame is due. A display that supports it (the X4 with Settings → Frame sleep on) deep-sleeps in between and the session reconnects when it wakes. Best for battery. |
| **Only when asked** | Sends on **Send now**, and after a display button press. |

The effective interval is never shorter than the display's own
`minIntervalMs` (e-paper panels ask for a few seconds). Timing runs in a
worker, so it isn't throttled when the session window is in the background.

### Capture

- **Tab stream** (default) uses `chrome.tabCapture`: a live video of the tab
  that keeps rendering when the tab is in the background. Chrome shows its
  "sharing this tab" indicator while it runs.
- **Screenshot** uses `captureVisibleTab`: no indicator, but the tab has to
  be the one showing in its window, and Chrome allows only about 2 per
  second. The session falls back to this if the stream can't start.

### Display buttons

Buttons the display forwards (protocol v2 `key` events) can:

- **Scroll the page**: up/down/left/right scroll by 80 % of a screen, and
  page next/previous by 95 %. With an element region, the element's own
  scroller is used if it has one.
- **Press keys in the page**: arrow keys, Enter (select), Escape (back),
  PageDown/PageUp, Home, F1–F16. These are synthetic `keydown`/`keyup`
  events: most app keyboard handlers (slides, lists, games) accept them, but
  browser defaults such as scrolling don't happen.
- **Ignore** them.

A fresh frame is sent 0.4 s after each press, so the result shows up on the
display. Taps from a touch display (`pointer` events) are mapped back through
the crop and scaling to the spot on the page, and clicked.

The X4 firmware speaks protocol v1 today, which has no button events. Try
this with the **Simulator** in the session window.

### Keep working after the page navigates

The extension runs with `activeTab`: clicking its button grants access to
that page as it is. After the tab navigates to another page, the tab stream
carries on, but element tracking and buttons need the content script
re-injected, and that needs the **Keep working after the page navigates**
option (host permission for all sites, which Chrome asks you to confirm).

## How it's built

```
build.mjs        the build: dev (copy), --watch, --dist (bundle + zip)
manifest.json    MV3; activeTab, tabCapture, scripting, storage; optional <all_urls>
popup.*          settings for this tab; start / stop. Writes chrome.storage.session.
session.*        one window per blitted tab: the Bluetooth link, capture, render, send loop
content.js       injected on demand: region picker, element rects, scroll / key / click replay
config.js        shared settings and storage keys
ticker.js        1 s heartbeat in a worker (not throttled in background windows)
build/           build output (git-ignored); lib/ inside build/dev is ../js
```

Why a session window rather than the popup or a service worker? The popup
closes as soon as it loses focus, and Chrome's Bluetooth chooser takes the
focus. MV3 service workers have no Web Bluetooth and no media capture, and
get stopped when idle. A small extension window can hold the connection and
the capture stream for as long as needed. It's a normal window, with an
address bar: Chrome anchors the Bluetooth chooser there, and in a `popup`
type window `requestDevice()` fails straight away as if cancelled.

Per frame, the session:

1. grabs the tab (a video frame, or a screenshot),
2. asks the content script for the viewport size and the region's current
   rectangle,
3. crops the region at full resolution into a canvas (at `zoom` × CSS
   pixels for **Actual pixels**),
4. rasterizes it with `rasterize()` to the display's frame area and format,
5. sends it with `sendFrame(…, { skipUnchanged, regions: true })`, which does
   the diffing, region cropping and PackBits.

It remembers the crop and fit of the last frame, to map taps back.

## Limitations and next steps

- Synthetic key events are untrusted. Trusted input would need the
  `debugger` permission (and Chrome's "is debugging this browser" banner).
- Screenshot mode can't capture a hidden tab. Tab stream mode can't run
  on `chrome://` pages or the Web Store (Chrome blocks both).
- With several displays, open one session per tab. Blitting one tab to
  two displays isn't supported yet.
- Ideas: rotate for portrait displays; a "fit width, scroll with buttons"
  mode; per-site saved regions; a device-width emulation mode that resizes
  the page to the display.
