# knob: a control plane for small devices over BLE

**Status: design draft.** Nothing here is implemented yet. "knob" is a
working name.

knob lets a **host** (a browser, a phone, a server) see and change what a
small **device** exposes: its settings, its Wi-Fi credentials, actions like
"reboot" or "scan for networks", forms for data input, and files. The device
describes its own controls, so one generic host UI works for every device,
and a device gets a settings page without anyone writing one for it.

It's the sibling of [blit](../blit/PROTOCOL.md). blit moves pixels; knob
moves configuration. They share the transport style, the flow control and
the principles, so the code on both sides can be shared too. A device can
speak both.

| Term | Meaning |
| --- | --- |
| **Device** | The embedded thing being controlled. BLE peripheral, GATT server. |
| **Host** | What controls it. BLE central, GATT client. |
| **Control** | One thing the device exposes: a setting, a read-only value, an action, a file slot. |
| **Schema** | The device's list of controls, with types, limits and labels. |
| **Bridge** | A host that relays knob to other clients (over WebSocket), applying a policy on the way. See [Server and web interface](#server-and-web-interface). |

All integers are little-endian.

## Does this already exist?

Mostly in pieces. Nothing found does the whole job: a self-describing schema
of typed controls, plus files, plus a transport a browser can use through
Web Bluetooth.

| Prior art | What it does | Why not just use it |
| --- | --- | --- |
| **Improv Wi-Fi** (ESPHome) | Tiny open GATT protocol to send SSID and password, get back a URL. Works from Web Bluetooth. | Wi-Fi only, no settings, no files, no security beyond "be nearby". A good model for how small the Wi-Fi part can be. |
| **ESP-IDF provisioning** (`wifi_prov_mgr`, protocomm) | Protobuf over GATT, with custom endpoints and real security (`security2` is SRP6a + AES-GCM with a proof-of-possession code). | Espressif-only, protobuf on both sides, and the custom endpoints are opaque bytes: no schema, so no generic UI. The security design is worth copying. |
| **SMP / MCUmgr** (Zephyr, Nordic, MCUboot) | Request/response over GATT with an 8-byte header and CBOR bodies. Groups for firmware images, a filesystem, **settings**, stats, logs, shell. Has Web Bluetooth clients. | The closest match. But settings are untyped key/value blobs: the host must already know what exists and what it means. CBOR and Zephyr's settings subsystem are heavy outside Zephyr. Worth borrowing its header and its firmware upload group rather than reinventing them. |
| **Plain GATT**: a characteristic per setting, with Characteristic Presentation Format and User Description descriptors | The "Bluetooth-native" way. Generic BLE apps (nRF Connect) can show them. | The GATT table is fixed at boot, each handle costs RAM, discovery is slow, there's no atomic multi-setting write, no enums or ranges, and no files. |
| **Object Transfer Service** (Bluetooth SIG) | Standard file-like objects over BLE. | Needs L2CAP connection-oriented channels, which Web Bluetooth doesn't have. |
| **Matter** commissioning (BTP over GATT) | Hands a device network credentials, very securely. | Huge. Only commissioning; everything else happens over IP. |
| **HomeKit (HAP over BLE)** | Typed characteristics with metadata (min, max, step, unit). | Closed, certification-bound, Apple-only hosts. The metadata model is good. |
| **Shelly Gen2 / Mongoose OS BLE RPC** | JSON-RPC over three GATT characteristics. Shelly uses it for setup. | JSON on the device, and still no schema: the host must know the methods. |
| **ESPHome entities** (native API is TCP, not BLE) | A device declares entities: switch, number, select, text, button, sensor. Home Assistant renders them. | Not over BLE. But it's the proof that a small set of typed entities covers almost everything, and the [control types](#control-types) below follow it. |
| **Nordic UART Service** | A serial pipe over GATT. | A pipe, not a protocol. Fine as a debug fallback. |

**Recommendation:** write knob, but small, and steal:

- the **entity model** from ESPHome and HomeKit (typed controls with ranges
  and units),
- the **transfer and flow control** from blit (already implemented on the X4,
  in JS, and in the Node server in this repo),
- the **security** from ESP-IDF `security2` / Matter (a PAKE keyed by a code
  the user reads off the device), as a later step,
- and **defer firmware updates** to SMP/MCUboot or ESP-IDF OTA rather than
  inventing a bootloader story. A knob `file` control can carry an image, but
  verifying and swapping images is a separate problem those already solve.

## Principles

blit's principles carry over. The ones that change the design:

1. **The device declares, the host renders.** The device publishes a schema:
   every control's type, range, unit, label and help text. A host that knows
   nothing about the device can still show a complete, validated settings
   page. Hosts may special-case [well-known keys](#well-known-keys) (a
   proper Wi-Fi picker) but never *need* to.
2. **The device enforces.** Ranges, read-only flags, secrets and access
   levels are checked on the device on every write. What a host or bridge
   hides is a convenience, not security: anyone in radio range can write
   GATT directly.
3. **Binary, fixed headers, TLV for anything that grows.** Same as blit.
   Unknown TLV tags are skipped, reserved bytes are 0.
4. **Every message fits one ATT write or notification.** Anything bigger (the
   schema, a file, a directory listing) goes through a
   [transfer](#transfers) with blit's chunk and window flow control.
5. **Idle is free.** No keepalives. Values the host cares about are pushed as
   events when they change. The device only listens when it's in
   [listening mode](#listening-mode).
6. **Secrets go in, never out.** A secret can be written but never read back,
   logged, or included in an event. Hosts learn only whether it's set.
7. **Keys are stable, ids are cheap.** Every control has a stable string key
   (`wifi.ssid`) that hosts and policies use, and a small numeric id used on
   the wire. Ids only need to be stable for one schema version; keys forever.

## Transport: Bluetooth LE GATT

One primary service, with the same shape as blit's:

Service `c7a10000-e295-445e-b079-edd9ea2725cb`

| Characteristic | UUID | Properties | Direction | Purpose |
| --- | --- | --- | --- | --- |
| Info | `c7a10001-…` | read | device → host | [Info](#info): identity, limits, schema CRC |
| Request | `c7a10002-…` | write | host → device | [Requests](#requests) |
| Reply | `c7a10003-…` | notify | device → host | Replies to requests, and transfer acks |
| Data | `c7a10004-…` | write without response | host → device | Transfer chunks, host to device (uploads) |
| DataOut | `c7a10005-…` | notify | device → host | Transfer chunks, device to host (schema, downloads) |
| Event | `c7a10006-…` | notify | device → host | [Events](#events): value changes, action progress |

(The `…` is the rest of the service UUID.)

Link setup as blit: ask for MTU ≥ 185 (517 where possible) and Data Length
Extension, a 15–30 ms connection interval while a host is active, don't
require 2M PHY. Requests are at most 244 bytes, and devices accept Long
Writes for them.

**Advertising.** A 128-bit service UUID takes 18 of the 31 advertising bytes,
so the advertisement carries flags and the knob UUID, and the scan response
carries the name and a service data record with the device state:

| Byte | Field |
| --- | --- |
| 0 | `state`: bit 0 unprovisioned (no Wi-Fi yet), bit 1 pairing open (accepting new hosts), bit 2 has bonds, bit 3 also speaks blit |
| 1 | knob version |

A device that also speaks blit advertises one of the two UUIDs (whichever
is its main job) and sets bit 3. Hosts find the other service by GATT
discovery after connecting.

## Session

```
host                                        device
 |-- connect ---------------------------------->|
 |-- read Info -------------------------------->|  schemaCrc = 0x1234abcd
 |-- subscribe Reply, DataOut, Event ---------->|
 |-- Request: hello (want events) ------------->|
 |<------------------------------ Reply: ok ----|
 |   (schema not cached for 0x1234abcd:)        |
 |-- Request: read-open schema ---------------->|
 |<------------ Reply: ok, size, crc, chunk ----|
 |<------------------- DataOut: chunk ... ------|
 |-- Request: ack ----------------------------->|  every `window` chunks
 |<------------------- DataOut: last chunk -----|
 |-- Request: get [all ids] ------------------->|
 |<------------------------ Reply: values ------|
 |   ... user edits ...                         |
 |-- Request: set [id=value, ...] ------------->|
 |<------------------------------ Reply: ok ----|
 |<------------------ Event: changed (live) ----|
```

1. **Connect** and **read Info**.
2. **Subscribe** to Reply, DataOut and Event.
3. **Hello.** Says the host's version and whether it wants events. Also where
   [app-layer auth](#security) would go.
4. **Schema.** If the host has a cached schema with Info's `schemaCrc`, use
   it. Otherwise read it with a [transfer](#transfers). Hosts cache by CRC,
   so a reconnect costs one Info read.
5. **Get** current values. Secrets come back as set/unset.
6. **Set, invoke, transfer files** as the user asks.
7. **Disconnect** whenever. The device drops back to advertising, or leaves
   listening mode.

One request in flight at a time. Replies echo the request's `seq`, so a host
can drop a stale reply after a timeout. Timeouts as blit: the host gives up
after 20 s, the device abandons a transfer after 10 s of silence.

## Info

A version byte, then TLV records (`u8 tag, u8 length, value`), as blit caps.

| Tag | Name | Value | Default |
| --- | --- | --- | --- |
| 0x01 | `name` | UTF-8, ≤ 32 bytes. The user's name for this device. | none |
| 0x02 | `model` | UTF-8, ≤ 32. e.g. `xteink-x4`. Bridges key policies on it. | none |
| 0x03 | `firmware` | UTF-8, ≤ 32. e.g. `1.4.0+g3c1f2e` | none |
| 0x04 | `deviceId` | 8 bytes, stable across renames and reflashes (e.g. from the MAC or eFuse) | none |
| 0x05 | `schema` | `u32 crc, u32 size`: CRC-32 of the schema blob, and its size | required |
| 0x06 | `limits` | `u16 chunk, u16 window, u32 maxFile` | required |
| 0x07 | `features` | `u32` bits: 0 events, 1 files, 2 dirs, 3 app-layer auth, 4 roles | 0 |
| 0x08 | `security` | `u8` level the device is at right now: 0 open, 1 encrypted link, 2 authenticated (bonded or PAKE) | 0 |

## Messages

### Requests

Written to Request. Every request starts with the same two bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `op` |
| 1 | 1 | `seq`: host-chosen, echoed in the reply |
| 2 | n | body, per op |

| Op | Name | Body | Reply body |
| --- | --- | --- | --- |
| 0x01 | `hello` | `u8 version, u8 flags` (bit 0 wants events), `u8 nameLength, name` | Info TLVs (as the Info read) |
| 0x02 | `auth` | Reserved for app-layer auth (see [Security](#security)) | |
| 0x10 | `get` | `u16 id` × n. None = every readable control. | `(u16 id, value)` × n. If they don't fit one notification, the reply sets `more` and the host asks again from the next id. |
| 0x11 | `set` | `(u16 id, value)` × n | none. On error, `detail` is the first id that failed. |
| 0x12 | `invoke` | `u16 actionId`, then `(u16 paramId, value)` × n | `(u16 id, value)` × n results, or status `running` and the outcome comes later as an `actionDone` event |
| 0x13 | `options` | `u16 id` of a `dynamic` enum | `u8 count`, then `(u16 value, u8 len, label)` × count |
| 0x20 | `read-open` | `u8 kind` (0 schema, 1 file control, 2 file in a dir control), `u16 id`, `u8 nameLength, name` | `u8 transferId, u32 size, u32 crc32, u16 chunk` |
| 0x21 | `write-open` | `u8 kind, u16 id, u32 size, u32 crc32, u8 flags` (bit 0 overwrite), `u8 nameLength, name` | `u8 transferId, u16 chunk` |
| 0x22 | `ack` | `u8 transferId, u32 bytesReceived` (host acking DataOut) | no reply |
| 0x23 | `commit` | `u8 transferId` | none, once the device has checked the CRC and stored the file |
| 0x24 | `cancel` | `u8 transferId` | none |
| 0x25 | `list` | `u16 dirId, u16 cursor` | `u16 nextCursor` (0 = end), then `(u32 size, u8 flags, u8 len, name)` × n |
| 0x26 | `delete` | `u16 dirId, u8 len, name` | none |

**`set` is atomic.** The device validates every pair first, and applies none
of them if any is invalid. That's what lets a host send SSID and password
together, or a start and end time that must be in order.

### Replies

Notified on Reply:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `op`, echoed. `0x80` for a transfer ack (below). |
| 1 | 1 | `seq`, echoed |
| 2 | 1 | `status` |
| 3 | 1 | `flags`: bit 0 `more` (the body was cut off; ask again) |
| 4 | 2 | `detail`: which id failed, or an expected value, per status |
| 6 | n | body, per op |

A **transfer ack** (device acking host uploads, like blit's `ack`) is
`op = 0x80, seq = transferId, status = 0, flags = 0, detail = 0`, then
`u32 bytesReceived`.

| Status | Meaning | `detail` |
| --- | --- | --- |
| 0 | ok | |
| 1 | `running`: the action started and will finish with an `actionDone` event | |
| 2 | bad request (malformed, unknown op) | op |
| 3 | unsupported version | device version |
| 4 | unknown id | id |
| 5 | invalid value (type, range, length, pattern) | id |
| 6 | read-only | id |
| 7 | not permitted: needs a higher [access level](#access-levels) | id |
| 8 | busy (another transfer or action is running) | |
| 9 | too large (bigger than `maxFile`, or the slot's `maxSize`) | |
| 10 | out of order (transfer data) | expected offset, low 16 bits; exact value in body as `u32` |
| 11 | CRC mismatch | |
| 12 | storage error, or no space | |
| 13 | not found | |
| 14 | action failed | action id; body may carry a UTF-8 message |

### Values

A value's encoding comes from its control's type in the schema, so values on
the wire carry no type byte.

| Type | Encoding |
| --- | --- |
| `bool` | `u8` 0 or 1 |
| `int` | `i32`. Fixed point: the schema's `scale` says how many decimal places (a temperature of 21.5 with `scale` 1 is sent as 215). No floats on the wire. |
| `enum` | `u16` option value |
| `text` | `u8 length`, UTF-8 |
| `secret` | Written as `text`. Read as `u8`: 0 unset, 1 set. |
| `bytes` | `u8 length`, raw bytes (keys, MAC addresses, small binary settings) |
| `file`, `dir` | Read (by `get`) as `u32 size, u32 crc32`, or for a dir `u16 count`. Written only by transfer. |
| `action`, `group` | No value |

## Schema

The schema is a blob read with a transfer. Its CRC-32 is in Info, and any
change to it (a new firmware, a control appearing because a module was
plugged in) changes the CRC and sends a `schema` event.

```
u8 version (= 1)
u16 count
repeat count times:
  u16 recordLength       (bytes after this field)
  u16 id                 (1..0xFFFE; 0 = none)
  u8  type
  u8  access             (low nibble: read level, high nibble: write level)
  u16 parent             (id of a group or action, 0 = top level)
  u16 flags
  TLV attributes until recordLength is used up: u8 tag, u8 length, value
```

Records are in display order. A group's children follow it, and an action's
params follow it (their `parent` is the action).

### Control types

| Code | Type | What hosts show | Notes |
| --- | --- | --- | --- |
| 1 | `bool` | toggle | |
| 2 | `int` | number field, or a slider when `min`, `max` and `step` make one sensible | Fixed point, see `scale` |
| 3 | `enum` | radio list or dropdown | `options` in the schema, or `dynamic` (fetched with `options`) |
| 4 | `text` | text field | `minLength`, `maxLength`, `hint` |
| 5 | `secret` | password field showing "set" / "not set" | Write-only |
| 6 | `bytes` | hex field | `hint` can say `mac`, `color` |
| 7 | `action` | button, with a form of its params | Params are ordinary controls whose `parent` is the action |
| 8 | `file` | upload and download, with the current size | One file slot: a wallpaper, a certificate, a config file |
| 9 | `dir` | file list with upload, download, delete | A folder: images, fonts, recordings |
| 10 | `group` | a section heading | No value; for layout |

Read-only values (a sensor, the battery, the IP address) are any type with
the `readOnly` flag. With `live` they're also pushed as events.

### Flags

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `readOnly` | Can't be `set` |
| 1 | `live` | The device sends a `changed` event when the value changes, however it changed |
| 2 | `dynamic` | `enum` options change at runtime; fetch them with `options` |
| 3 | `restart` | Takes effect after a restart; hosts should say so |
| 4 | `confirm` | Hosts should ask "are you sure?" first (factory reset, format) |
| 5 | `advanced` | Hosts may fold it away by default |
| 6 | `required` | For action params: must be supplied |
| 7 | `hidden` | Exists for the protocol (e.g. an action param with a default) but hosts don't show it |

### Attributes

| Tag | Name | Value | For |
| --- | --- | --- | --- |
| 0x01 | `key` | ASCII, dotted, ≤ 32: `wifi.ssid`, `display.refresh` | all; **required** |
| 0x02 | `label` | UTF-8, ≤ 32: "Full refresh every" | all |
| 0x03 | `help` | UTF-8, ≤ 160: one or two sentences | all |
| 0x04 | `unit` | UTF-8, ≤ 8: `min`, `°C`, `%` | `int` |
| 0x05 | `range` | `i32 min, i32 max, i32 step` | `int` |
| 0x06 | `scale` | `u8` decimal places | `int` |
| 0x07 | `option` | `u16 value`, then UTF-8 label. Repeated, one per option, in display order. | `enum` |
| 0x08 | `length` | `u8 min, u8 max` | `text`, `secret`, `bytes` |
| 0x09 | `hint` | `u8`: 1 multiline, 2 email, 3 url, 4 hostname, 5 ip, 6 mac, 7 color (RGB888 in an `int`), 8 time of day (minutes in an `int`), 9 date-time (Unix seconds) | `text`, `int`, `bytes` |
| 0x0A | `default` | value, encoded as the type | settable types |
| 0x0B | `accept` | ASCII, ≤ 32: `image/png,.bmp` | `file`, `dir` |
| 0x0C | `maxSize` | `u32` bytes | `file`, `dir` |
| 0x0D | `pattern` | ASCII regex, ≤ 64. Hosts validate with it; devices check what they can (length, charset). | `text` |

The schema's size is bounded by what the device can hold in flash as a
constant: it's generated once, at build time or at boot, never per request.

### Mapping the X4's settings

The X4's `settings::Choice` (in
[`Settings.h`](../xteink-x4-platformio/src/Settings.h)) is already a knob
`enum`:

| `Choice` field | knob |
| --- | --- |
| `key` (`"refresh"`) | `key` (`display.refresh`) |
| `title` / `heading` | `label` |
| `description` | `help` |
| `options[].value`, `options[].list` | `option` records |
| `defaultIndex` | `default` (the option's value) |

So the X4 could serve its whole settings page over knob by walking `kAll`,
with no new UI code. `live` on each would keep a host in step when someone
changes a setting on the device itself.

### Well-known keys

Hosts may give these a better UI, and bridges may give them default
policies. A device uses the ones that apply, with these types.

| Key | Type | Notes |
| --- | --- | --- |
| `device.name` | `text` | The name in Info and the advertisement |
| `wifi.ssid` | `enum` `dynamic` or `text` | A `dynamic` enum filled by `wifi.scan`, or plain text for hidden networks |
| `wifi.password` | `secret` | |
| `wifi.scan` | `action` | Updates `wifi.ssid`'s options, then `optionsChanged` |
| `wifi.connect` | `action` | Params `ssid`, `password`; returns `running`, then `actionDone` with `wifi.state` |
| `wifi.state` | `enum` `readOnly` `live` | 0 off, 1 connecting, 2 connected, 3 wrong password, 4 not found, 5 failed |
| `wifi.ip` | `text` `readOnly` `live` | |
| `power.battery` | `int` `readOnly` `live` | `unit` `%` |
| `time.now` | `int` `hint` date-time | Hosts can offer "set to my clock" |
| `system.restart` | `action` `confirm` | |
| `system.factoryReset` | `action` `confirm` | |
| `system.firmware` | `file` | An OTA image. See [firmware](#firmware-updates). |

Anything else is namespaced by the device: `x4.frameSleep`,
`weather.city`.

## Transfers

The same mechanism as blit frames, in both directions, one at a time.

**Upload** (host → device): `write-open`, reply with `chunk`; write Data
chunks (`u8 transferId, u32 offset, bytes`); the device sends a transfer
ack every `window` chunks and after the last; `commit`; the device checks
the CRC, stores the file, replies.

**Download** (device → host): `read-open`, reply with size, CRC and `chunk`;
the device notifies DataOut chunks (`u8 transferId, u32 offset, bytes`); the
host sends `ack` every `window` chunks; after the last chunk the host checks
the CRC. No commit.

A device writes uploads to a temporary name and renames on commit, so a
dropped link never leaves half a file where a whole one was. Its `window`
and `chunk` in Info are sized for its RAM, as in blit.

## Events

Notified on Event when a host has asked for them in hello. Like blit, they
are never queued across a disconnect.

| Type | Name | Layout |
| --- | --- | --- |
| 0x01 | `changed` | `u8 type`, then `(u16 id, value)` × n |
| 0x02 | `schema` | `u8 type, u32 newCrc`. Re-read the schema. |
| 0x03 | `optionsChanged` | `u8 type, u16 id` |
| 0x04 | `progress` | `u8 type, u16 actionId, u8 percent` (255 = unknown), UTF-8 message |
| 0x05 | `actionDone` | `u8 type, u16 actionId, u8 status`, then `(u16 id, value)` × n results, or a UTF-8 message on failure |
| 0x06 | `log` | `u8 type, u8 level`, UTF-8. Optional; for a debug console. |

## Listening mode

The device decides when it's controllable. Radios cost power, and an always
connectable device is an always attackable one. A device picks the modes
that suit it:

| Mode | Advertises | Accepts | Good for |
| --- | --- | --- | --- |
| **Off** | nothing | nothing | Normal running on battery devices |
| **Open** | yes, `pairingOpen` | any host (bonding a new host allowed) | Entered by a deliberate user action: a menu item (the X4 already does this for blit's Bluetooth app), a long button press, or first boot while unprovisioned |
| **Bonded only** | yes, slowly (1–2 s interval) | only hosts already bonded (filter accept list) | Mains-powered devices that should stay manageable |
| **Boot window** | yes, for the first 2 minutes after power-on | per the two above | Headless devices with no button: power-cycle to manage |

- Open mode times out (2–5 minutes without a connection) back to the
  device's normal mode. An open connection that's idle for 5 minutes is
  dropped.
- Advertise fast (100 ms) for the first 30 s of a mode, then slow down.
- The device shows it's listening if it can: a status icon, an LED pattern.
- A device that must never miss its main job (a display mid-frame, a sensor
  sampling) can refuse `set` and `invoke` with `busy` rather than stop.

## Security

What we're protecting: Wi-Fi passwords going over the air, and settings or
files being changed by someone walking past.

**Level 1: link encryption (LE Secure Connections).** Mark the Request, Data
and DataOut characteristics as needing an encrypted (and, for `access` above
0, authenticated) link. The OS pairs when the host first touches them,
including under Web Bluetooth.

- A device with a screen uses **passkey entry**: it shows six digits, the
  user types them on the host. That's MITM-safe. The X4 can do this.
- A headless device can only do "Just Works" pairing, which encrypts but
  doesn't authenticate. Compensate with listening mode: bonding a new host
  only while the user has pressed the button.
- Bonds persist, so the second connection is instant and silent.
- Downside: pairing UX differs by OS and is fragile (Chrome on Linux and
  macOS each have quirks), and it protects only the BLE hop, not a bridge.

**Level 2: app-layer PAKE (later).** As ESP-IDF `security2` and Matter do:
an SRP6a or SPAKE2+ exchange over the `auth` op, keyed by a setup code
printed on the device or shown on its screen, then AES-CCM on every request
and reply body. It works the same on every host and needs nothing from the
OS. It costs code and a few hundred ms of CPU on the device, so it's
behind the `appAuth` feature bit and not needed for a first version.

Either way:

- Secrets are write-only and never logged (principle 6).
- Hosts don't persist secrets; a bridge stores none.
- `factoryReset` wipes bonds.

### Access levels

Each control has a read and write level (the `access` byte). The device
checks it against the connection's level:

| Level | Who | Typical controls |
| --- | --- | --- |
| 0 | any connected host | name, battery, firmware version |
| 1 | an encrypted link | everyday settings |
| 2 | an authenticated host (passkey-bonded or PAKE) | Wi-Fi, files, actions, factory reset |

A device without the `roles` feature treats 1 and 2 the same.

## Firmware updates

Out of scope for v1, on purpose. A `file` control called `system.firmware`
can deliver an image to an OTA partition with the same transfer code, and on
ESP32 that's most of an update. But confirming the image boots, rolling
back and signing are what MCUboot + SMP and ESP-IDF's OTA already do. Use
those first, and fold them in only if a device has neither.

## Stream transports

As blit: the same messages over UART, USB CDC, TCP or WebSocket, framed as
`u8 channel, u16 length, message` (length left out on WebSocket, one message
per binary frame). There's no Info read, so the host starts with `hello`,
whose reply is the Info TLVs.

| Channel | Direction | BLE equivalent |
| --- | --- | --- |
| 1 | host → device | Request |
| 2 | host → device | Data |
| 3 | device → host | Reply |
| 4 | device → host | DataOut |
| 5 | device → host | Event |

This is what makes a bridge simple: it speaks BLE to the device and this
framing, over WebSocket, to browsers.

## Server and web interface

Two ways to drive a device, sharing one JS library (a `protocol.js` and a
simulated device, like blit's `js/`):

```
 (a) one person, one device, nothing to run
     browser ── Web Bluetooth ──► device

 (b) a server near the devices, many users, a policy
     browser ── HTTPS / WebSocket ──► knob server ── BLE (noble) ──► devices
                                       │
                                       ├─ device registry
                                       ├─ schema cache (by CRC)
                                       ├─ exposure policy
                                       └─ audit log
```

(a) is the "settings page for my gadget" case: open a page, pick the device,
get a form. It needs Chrome or Edge (desktop or Android); Safari and iOS
have no Web Bluetooth.

(b) is the case the policy question is about. The server sits where
[`blit/server`](../blit/server/) does (a Pi or Mac mini with a Bluetooth
adapter), keeps a connection or reconnects on demand, and serves the web UI.

### Controlling what's exposed

The device publishes everything it has. The server decides which of it each
user sees. The policy is keyed by **control key**, not id, so it survives
firmware updates, and set at two levels:

1. **Per model** (`xteink-x4`): the default for every device of that kind.
2. **Per device** (`deviceId`): overrides.

For each key the admin picks:

| Setting | Values |
| --- | --- |
| Exposure | hidden · read-only · read-write |
| Who | roles or users allowed (e.g. `admin`, `family`) |
| Confirm | force a confirmation, beyond the device's own `confirm` flag |
| Label | override the device's label, e.g. "Kitchen display refresh" |

New keys the server hasn't seen before (a firmware update added them) are
**hidden** until an admin exposes them, except `readOnly` controls at
access level 0, which default to read-only. Secrets and actions never
default to exposed.

The server enforces the policy, not just the UI:

- It **filters the schema** it gives each client: hidden controls are
  removed, read-only ones get the `readOnly` flag added.
- It **rejects requests** (`set`, `invoke`, transfers) on controls the
  client can't write, with status 7, before they reach the device.
- It **filters events**, so hidden values don't leak through `changed`.

That's the bridge in the table at the top: to a browser it speaks knob over
WebSocket, a narrower device than the real one. The same web UI works in (a)
and (b), because the filtered schema is still a schema.

(The device-side access levels still apply. The server is one host among
several; someone with a phone next to the device isn't bound by its policy.
So anything sensitive also needs a device access level of 2.)

### Pages

- **Devices:** everything seen, with name, model, firmware, last seen,
  signal, whether it's listening or bonded. Scan button.
- **Device:** the generated form. Groups become sections; each type gets its
  widget (see [control types](#control-types)); `live` values update in
  place; actions show progress; `file` and `dir` controls get drop zones and
  lists. Well-known keys get special UI (a Wi-Fi network picker driven by
  `wifi.scan`).
- **Exposure (admin):** a table of every key for a model, with the device's
  label, type and access level beside the policy columns above. Per-device
  overrides on the same table. A preview of what a given role sees.
- **Audit:** who changed what, when, from which client. Secrets recorded as
  "changed", never their value.

## Plan

In order; each step is usable by itself.

1. **JS library and simulated device**: request/reply, schema parser, value
   codecs, transfers (reusing blit's), a `sim-device.js` with a sample
   schema. Tests like `blit/js/test`.
2. **Web Bluetooth page** with the generic form renderer. Works against the
   simulated device first.
3. **X4 firmware**: expose `settings::Choice` as `enum` controls, with `live`.
   A second GATT service next to `CastServer`.
4. **Server**: BLE bridge (from `blit/server`), policy store (a JSON or
   SQLite file), the pages above.
5. **Files**: `dir` control for the X4's SD images.
6. **Wi-Fi**: on an ESP32 that uses it (the M5StickS3), with `wifi.scan` and
   `wifi.connect`.
7. **Security**: passkey pairing on the X4; PAKE if headless devices need it.

## Open questions

- **Where does the policy live?** The design above keeps it on the server.
  The alternative is to write it to the device (an admin-only `knob.exposed`
  control listing keys), so every host sees the same subset, but then it's
  the device enforcing someone's UI preferences with its flash.
- **One service or two?** knob and blit could merge into one service with
  blit's messages as another op range. Separate is simpler to ship; merged
  saves a UUID in the advertisement.
- **SMP compatibility.** Adopting SMP's 8-byte header (and adding a
  "schema" group) would let knob devices talk to existing MCUmgr tools for
  firmware and files. It costs CBOR on the device.
- **Multiple hosts at once.** BLE peripherals can take several connections.
  Events already keep hosts in step; `set` needs no locking because it's
  atomic. Transfers are one at a time per device, so a second host gets
  `busy`.
- **Data input beyond forms.** Actions with params cover forms. Streaming
  input (a text box that types into the device as you type) would be a
  `text` control with `live` plus a debounce on the host; worth trying before
  adding anything.
