# blat-server

The host side of [blat](../PROTOCOL.md). It finds blat devices over Bluetooth
LE, connects, pairs with the code a device shows, and serves a web page with
a form for each device's settings, generated from what the device declares.
Run it on a Mac, a Raspberry Pi, or anything with a Bluetooth adapter, and
use the page from any browser that can reach it.

```
browser ── HTTP + server-sent events ──► blat-server ── BLE (noble) ──► devices
```

The server is the blat host: it holds the session keys and the keys devices
give it to reconnect without a code. The browser never talks blat.

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
| `src/blat/` | The host side of the protocol, independent of Bluetooth and HTTP: `client.ts` (`BlatClient`), `crypto.ts` (SPAKE2, sealing), `schema.ts`, `wire.ts`, `link.ts` (the transport interface and stream framing) |
| `src/ble.ts` | Scanning, and `NobleLink`: the blat GATT service over noble |
| `src/sim.ts` | The simulated device as a child process |
| `src/devices.ts` | `DeviceManager`: connections, pairing, remembered keys, events |
| `src/http.ts`, `src/cli.ts` | The server |
| `src/store.ts` | Remembered keys and cached schemas on disk |
| `web/` | The page |
| `test/` | `client.test.ts` (the protocol against the simulated device), `server.test.ts` (the API end to end) |

```sh
pnpm test          # needs the simulated device: make -C ../firmware/test sim
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
- Tested against the simulated device only, not yet an X4 over Bluetooth.
