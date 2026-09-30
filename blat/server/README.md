# blat-server

The host side of [blat](../PROTOCOL.md), two ways:

```
 server   browser ── HTTP + server-sent events ──► blat-server ── BLE (noble) ──► devices
 browser  browser ── Web Bluetooth ──► device
```

- **blat-server** finds blat devices over Bluetooth LE, connects, pairs with
  the code a device shows, and serves a page with a form for each device's
  settings, generated from what the device declares. Run it on a Mac, a
  Raspberry Pi, or anything with a Bluetooth adapter, and use the page from
  any browser that can reach it. The server is the blat host: it holds the
  session keys and the keys devices give it to reconnect without a code, and
  its page never talks blat.
- **The [Web Bluetooth page](#the-web-bluetooth-page)** (`bluetooth.html`)
  does the same from the browser alone, with no server: the browser is the
  host.

Both use the same protocol library (`src/blat/`, pure TypeScript, for Node
and browsers) and the same form.

## Install

Needs Node.js 22.18 or later (it runs the TypeScript sources directly).

```sh
cd blat/server
pnpm install        # or npm install
pnpm build          # the web page, into dist/
```

Bluetooth, as for [blit-server](../../blit/server/README.md#install):

- **macOS**: the first run asks for Bluetooth permission for your terminal.
- **Linux**: needs BlueZ and permission for raw HCI sockets, e.g.
  `sudo setcap cap_net_raw+eip $(eval readlink -f $(which node))`.

## Run

```sh
node src/cli.ts
```

Then open http://127.0.0.1:8080/. It scans for 15 seconds at start, and
again when you press **Scan**. On an X4, open the **Bluetooth** app first:
it only advertises while that's open.

Pick a device to connect. Anyone can read what the device makes public; to
change anything, press **Pair**, and type the six-digit code the device shows.
The server then asks the device to remember it, so later connections resume
without a code. Some controls (the X4's Restart) always need a fresh code:
someone at the device.

Without hardware, use the simulated device, the real firmware library built
for Linux:

```sh
make -C ../firmware/test sim          # once; needs libmbedtls-dev
node src/cli.ts --sim
```

Its pairing code shows in the log and on the page, where a real device's
would be on its screen.

| Option | Default | |
| --- | --- | --- |
| `--port <n>` | 8080 | |
| `--host <address>` | 127.0.0.1 | Listen on this address. The default is this machine only: anyone who can reach the page can change the settings of every device the server is paired with. |
| `--data <dir>` | `~/.blat-server` | Remembered devices (`hosts.json`, mode 600: its keys let the holder change settings) and cached schemas |
| `--name <text>` | `blat-server on <hostname>` | How the server names itself to devices |
| `--scan <s>` | 15 | Seconds per scan |
| `--sim`, `--sim-path <path>` | | The simulated device instead of Bluetooth |
| `--dev` | | Serve the page through Vite, with live reload |

## The page

Plain TypeScript and DOM, built with Vite; no framework. It's generated
entirely from the device's schema:

| Control | Shown as |
| --- | --- |
| `group` | A section. `advanced` controls fold away inside it. |
| `bool` | A switch |
| `enum` | Buttons for up to four options, a menu for more |
| `int` | A slider when the range is short, else a number field; `scale` and `unit` applied |
| `text` | A field with Save; a text area for the `multiline` hint |
| `secret` | "Set" or "Not set", and a field to replace it. The value is never shown or kept. |
| `action` | A button, with a field per param; `confirm` asks first |
| `readOnly` | The value |

A control the connection can't change says why ("Pair to change", "Needs a
fresh code from the device") and offers to pair. `live` values update in
place as the device reports them.

A URL like `/#d=<deviceId>&c=<code>` (what a device's pairing
[QR code](../PROTOCOL.md#the-qr-code) will carry) fills in the code for that
device when it's pairing.

## The Web Bluetooth page

`bluetooth.html`: **Choose a device** opens the browser's Bluetooth chooser,
listing devices that advertise blat. Then it's the same form as the server's
page, and pairing works the same way. After pairing, the page asks the
device to remember this browser, and keeps the key in the browser's local
storage, so **Reconnect** (or a later visit) resumes without a code. Schemas
are cached there too, by CRC. **Forget** drops the key.

- Needs Chrome or Edge, on a computer or Android. Safari (including every iOS
  browser) and Firefox have no Web Bluetooth; the page says so.
- Needs a secure context: HTTPS, or `localhost`. blat-server serves it at
  `/bluetooth.html`, and `pnpm dev` does too; `pnpm build` puts a static copy
  in `dist/` that works from any HTTPS host (e.g. GitHub Pages), no server
  needed.
- The stored key lets anyone with access to this browser profile change the
  device's everyday settings. Level-2 controls (the X4's Restart) still need
  a fresh code.
- The page is about 25 KB gzipped, mostly the P-256 and AES code: Web Crypto
  has neither raw curve arithmetic nor AES-CCM, so both are pure JavaScript
  (`@noble/curves`, `@noble/ciphers`), checked against OpenSSL in the tests.

## API

JSON over HTTP. Requests that change anything must be JSON, and from the
page's own origin when the browser sends one, so other sites open in the same
browser can't use the server.

| Request | |
| --- | --- |
| `GET /api/devices` | Devices found |
| `POST /api/scan` | Scan again |
| `GET /api/devices/:id` | A device: summary, controls, values |
| `POST /api/devices/:id/connect` | Connect (and resume, if remembered) |
| `POST /api/devices/:id/disconnect` | |
| `POST /api/devices/:id/pair` | Ask the device to show a code |
| `POST /api/devices/:id/code` `{"code"}` | The code; on success the server is remembered |
| `POST /api/devices/:id/forget` | Stop resuming without a code |
| `PUT /api/devices/:id/values` `{"values": {id: value}}` | Change values, all or none |
| `POST /api/devices/:id/invoke` `{"action", "params"}` | Run an action |
| `GET /api/events` | Server-sent events: device list, values as they change, device state |

Errors are `{"error", "status", "detail"}`, with the blat status and detail
when the device refused (e.g. status 15 with the level needed, 16 with the
tries left). Types are in [`src/api.ts`](src/api.ts).

## Files

| File | |
| --- | --- |
| `src/blat/` | The host side of the protocol, for Node and browsers: `client.ts` (`BlatClient`), `crypto.ts` (SPAKE2, AES-CCM), `schema.ts`, `wire.ts`, `link.ts` (the transport interface) |
| `src/ble.ts` | Scanning, and `NobleLink`: the blat GATT service over noble |
| `src/sim.ts`, `src/stream.ts` | The simulated device as a child process, over the stream framing |
| `src/devices.ts` | `DeviceManager`: connections, pairing, remembered keys, events |
| `src/http.ts`, `src/cli.ts` | The server |
| `src/store.ts` | Remembered keys and cached schemas on disk |
| `web/index.html`, `main.ts`, `api.ts` | The server's page |
| `web/bluetooth.html`, `bluetooth.ts`, `webble.ts` | The Web Bluetooth page, and `WebBluetoothLink` |
| `web/view.ts`, `controls.ts`, `dom.ts` | A device and its form, shared by both pages |
| `test/` | `client.test.ts` (the protocol against the simulated device), `crypto.test.ts` (against OpenSSL), `server.test.ts` (the API end to end), `bluetooth.test.ts` (the Web Bluetooth page in Chromium, with `navigator.bluetooth` wired to the simulated device) |

```sh
pnpm test          # needs the simulated device: make -C ../firmware/test sim
                   # bluetooth.test.ts also needs Chromium: CHROME_PATH, or
                   # `npx playwright install chromium`; it's skipped without
pnpm typecheck
pnpm dev           # the server with --dev
```

## Not yet

- Per-user access (a login, and capping some users at read-only), which the
  spec allows for a bridge. For now, everyone who can reach the page has the
  server's access.
- `file` and `dir` controls, and dynamic enum options, which the firmware
  doesn't support yet either.
- More than one connection per device, and more than one device on a
  single-connection adapter at a time.
- Tested against the simulated device only, not yet an X4 over Bluetooth
  (through noble or a browser).
