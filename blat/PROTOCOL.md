# The blat protocol

**blat** (**BL**E **A**ttributes **T**ransfer) is a control
plane for small devices over BLE.

**Status: design draft.** Nothing here is implemented yet.

blat lets a **host** (a browser, a phone, a server) see and change what a
small **device** exposes: its settings, its Wi-Fi credentials, actions like
"reboot" or "scan for networks", forms for data input, and files. The device
describes its own controls, so one generic host UI works for every device,
and a device gets a settings page without anyone writing one for it.

**What's exposed is decided in the firmware.** The device's schema is its
exposure policy: a control that isn't in it doesn't exist over the air, and
each one that is carries its own access level. Hosts, including the
[server](#server-and-web-interface), know nothing about a device beyond what
it declares, and add no policy of their own.

It's a sister protocol to [blit](../blit/PROTOCOL.md), and deliberately a
separate one, so blit stays small. blit moves pixels; blat moves
configuration. They share the transport style, the flow control and the
principles, so the code on both sides can be shared, and a device can speak
both. Neither needs the other.

| Term | Meaning |
| --- | --- |
| **Device** | The embedded thing being controlled. BLE peripheral, GATT server. |
| **Host** | What controls it. BLE central, GATT client. |
| **Control** | One thing the device exposes: a setting, a read-only value, an action, a file slot. |
| **Schema** | The device's list of controls, with types, limits and labels. |
| **Access level** | How trusted a connection must be to read or write a control. Declared per control by the device. See [access levels](#access-levels). |
| **Bridge** | A host that relays blat to other clients (over WebSocket). See [Server and web interface](#server-and-web-interface). |

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

**Recommendation:** write blat, but small, and steal:

- the **entity model** from ESPHome and HomeKit (typed controls with ranges
  and units),
- the **transfer and flow control** from blit (already implemented on the X4,
  in JS, and in the Node server in this repo),
- the **security** from ESP-IDF `security2` / Matter: a PAKE (password
  authenticated key exchange) keyed by a code the user reads off the device,
  here [SPAKE2](https://www.rfc-editor.org/rfc/rfc9382) with the code shown
  on the device's screen and in a QR code,
- and **defer firmware updates** to SMP/MCUboot or ESP-IDF OTA rather than
  inventing a bootloader story. A blat `file` control can carry an image, but
  verifying and swapping images is a separate problem those already solve.

## Principles

blit's principles carry over. The ones that change the design:

1. **The device declares, the host renders.** The device publishes a schema:
   every control's type, range, unit, label and help text, and who may read
   and write it. A host that knows nothing about the device can still show a
   complete, validated settings page. Hosts may special-case
   [well-known keys](#well-known-keys) (a proper Wi-Fi picker) but never
   *need* to.
2. **The firmware is the policy.** Which controls exist over the air, and at
   what access level, is compiled into the firmware (see
   [declaring controls](#declaring-controls-in-firmware)). There is no
   host-side list of what to expose, so there's nothing to keep in step with
   the firmware, and every host sees the same device.
3. **The device enforces.** Ranges, read-only flags, secrets and access
   levels are checked on the device on every request. Hosts hide what a
   connection can't use as a courtesy, not as security: anyone in radio
   range can write GATT directly.
4. **No writes without the code.** Any host may connect and read what the
   device makes public, but every write needs a host that has
   [authenticated](#authentication) with a code the device showed, or that
   the device remembers from an earlier code. The code never goes over the
   air.
5. **Binary, fixed headers, TLV for anything that grows.** Same as blit.
   Unknown TLV tags are skipped, reserved bytes are 0.
6. **Every message fits one ATT write or notification.** Anything bigger (the
   schema, a file, a directory listing) goes through a
   [transfer](#transfers) with blit's chunk and window flow control.
7. **Idle is free.** No keepalives. Values the host cares about are pushed as
   events when they change. The device only listens when it's in
   [listening mode](#listening-mode).
8. **Secrets go in, never out.** A secret can be written but never read back,
   logged, or included in an event. Hosts learn only whether it's set.
9. **Keys are stable, ids are cheap.** Every control has a stable string key
   (`wifi.ssid`) that hosts use, and a small numeric id used on
   the wire. Ids only need to be stable for one schema version; keys forever.

## Transport: Bluetooth LE GATT

One primary service, with the same shape as blit's:

Service `b1a70000-e295-445e-b079-edd9ea2725cb`

| Characteristic | UUID | Properties | Direction | Purpose |
| --- | --- | --- | --- | --- |
| Info | `b1a70001-…` | read | device → host | [Info](#info): identity, limits, schema CRC |
| Request | `b1a70002-…` | write | host → device | [Requests](#requests) |
| Reply | `b1a70003-…` | notify | device → host | Replies to requests, and transfer acks |
| Data | `b1a70004-…` | write without response | host → device | Transfer chunks, host to device (uploads) |
| DataOut | `b1a70005-…` | notify | device → host | Transfer chunks, device to host (schema, downloads) |
| Event | `b1a70006-…` | notify | device → host | [Events](#events): value changes, action progress |

(The `…` is the rest of the service UUID.)

Link setup as blit: ask for MTU ≥ 185 (517 where possible) and Data Length
Extension, a 15–30 ms connection interval while a host is active, don't
require 2M PHY. Requests are at most 244 bytes, and devices accept Long
Writes for them.

**Advertising.** A 128-bit service UUID takes 18 of the 31 advertising bytes,
so the advertisement carries flags and the blat UUID, and the scan response
carries the name and a service data record with the device state:

| Byte | Field |
| --- | --- |
| 0 | `state`: bit 0 unprovisioned (no Wi-Fi yet), bit 1 `acceptingHosts` (will show a code), bit 2 has remembered hosts, bit 3 also speaks blit |
| 1 | blat version |

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
 |   (authenticate: a code, or resume; see      |
 |    Authentication. Sealed from here on.)     |
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
3. **Hello.** Says the host's version and whether it wants events.
4. **Authenticate**, if the host will write anything, or read anything
   above level 0. A remembered host [resumes](#resuming-a-remembered-host)
   silently. A new one asks the device to [show a code](#pairing-with-a-code).
   A host may also skip this until a request gets status `authRequired`.
5. **Schema.** If the host has a cached schema with Info's `schemaCrc`, use
   it. Otherwise read it with a [transfer](#transfers). Hosts cache by CRC,
   so a reconnect costs one Info read.
6. **Get** current values. Secrets come back as set/unset.
7. **Set, invoke, transfer files** as the user asks.
8. **Disconnect** whenever. The device drops back to advertising, or leaves
   listening mode.

One request in flight at a time. Replies echo the request's `seq`, so a host
can drop a stale reply after a timeout. Timeouts as blit: the host gives up
after 20 s, the device abandons a transfer after 10 s of silence.

## Info

A version byte, then TLV records (`u8 tag, u8 length, value`), as blit caps.

| Tag | Name | Value | Default |
| --- | --- | --- | --- |
| 0x01 | `name` | UTF-8, ≤ 32 bytes. The user's name for this device. | none |
| 0x02 | `model` | UTF-8, ≤ 32. e.g. `xteink-x4`. Hosts may group devices by it. | none |
| 0x03 | `firmware` | UTF-8, ≤ 32. e.g. `1.4.0+g3c1f2e` | none |
| 0x04 | `deviceId` | 8 bytes, stable across renames and reflashes (e.g. from the MAC or eFuse) | none |
| 0x05 | `schema` | `u32 crc, u32 size`: CRC-32 of the schema blob, and its size | required |
| 0x06 | `limits` | `u16 chunk, u16 window, u32 maxFile` | required |
| 0x07 | `features` | `u32` bits: 0 events, 1 files, 2 dirs | 0 |
| 0x08 | `auth` | `u8 level` (this connection's [access level](#access-levels) right now), `u8 methods` (bit 0 shows a code on a screen, bit 1 has a fixed code printed on it, bit 2 remembers hosts), `u8 digits` (code length, 6–10) | level 0, no methods: a read-only device |

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
| 0x02 | `auth-begin` | `u8 method` (1 code, 2 resume). For resume, also `u8[8] hostId`. | Code: `u8 digits, u8 attemptsLeft, u16 expiresSeconds, u8[16] salt`. Resume: `u8[16] challenge`. See [Authentication](#authentication). |
| 0x03 | `auth-code` | `u8[65] pA`: the host's SPAKE2 share | `u8[65] pB, u8[32] confirmB` |
| 0x04 | `auth-confirm` | `u8[32] confirmA` | `u8 level`. Everything after this is [sealed](#sealing). |
| 0x05 | `auth-resume` | `u8[16] hostNonce, u8[32] hostMac` | `u8[32] deviceMac, u8 level`. Sealed after this. |
| 0x06 | `remember` | Sealed. `u8 nameLength, name`: how the device should list this host | `u8[8] hostId, u8[32] hostKey` |
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
| 15 | `authRequired`: authenticate first, or again with a fresh code | level needed |
| 16 | wrong code, or resume failed | attempts left on this code |
| 17 | locked out after too many wrong codes | seconds until `auth-begin` works again |

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
[`Settings.h`](../xteink-x4-platformio/src/Settings.h)) is already a blat
`enum`:

| `Choice` field | blat |
| --- | --- |
| `key` (`"refresh"`) | `key` (`display.refresh`) |
| `title` / `heading` | `label` |
| `description` | `help` |
| `options[].value`, `options[].list` | `option` records |
| `defaultIndex` | `default` (the option's value) |

So the X4 could serve its whole settings page over blat by walking `kAll`,
with no new UI code. `live` on each would keep a host in step when someone
changes a setting on the device itself.

### Well-known keys

Hosts may give these a better UI. A device uses the ones that apply, with these types.

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
| `auth.hosts` | `enum` `readOnly` `dynamic` | Remembered hosts; option labels are their names |
| `auth.forget` | `action` | Param `host` (an `auth.hosts` value), or all if left out. Level 2. |

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

Once the session is [sealed](#sealing), each chunk also carries an 8-byte
tag. The `chunk` a device returns from `read-open` and `write-open` already
leaves room for it.

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
| **Open** | yes, `acceptingHosts` | any host; a new host may ask for a code | Entered by a deliberate user action: a menu item (the X4 already does this for blit's Bluetooth app), a long button press, or first boot while unprovisioned |
| **Remembered only** | yes, slowly (1–2 s interval) | any host may read level 0, but only remembered hosts can [resume](#resuming-a-remembered-host); asking for a code gets `authRequired` | Mains-powered devices that should stay manageable |
| **Boot window** | yes, for the first 2 minutes after power-on | per the two above | Headless devices with no button: power-cycle to manage |

- Open mode times out (2–5 minutes without a connection) back to the
  device's normal mode. An open connection that's idle for 5 minutes is
  dropped.
- Advertise fast (100 ms) for the first 30 s of a mode, then slow down.
- The device shows it's listening if it can: a status icon, an LED pattern.
- A device that must never miss its main job (a display mid-frame, a sensor
  sampling) can refuse `set` and `invoke` with `busy` rather than stop.

## Authentication

What we're protecting: Wi-Fi passwords going over the air, and settings or
files being changed by someone walking past. The answer is the one a TV or
a games console uses: **the device shows a code, and the person types it
(or scans it) on the host.** Only someone who can see the device can
control it.

The code itself is never sent. From a plain hash or challenge-response,
anyone listening could brute-force a 6-digit code offline in well under a
second. Instead both sides feed it into
[SPAKE2](https://www.rfc-editor.org/rfc/rfc9382), a PAKE (password
authenticated key exchange), as ESP-IDF provisioning and Matter do. That:

- proves the host knows the code,
- gives an eavesdropper nothing to guess against offline (each guess needs
  a new exchange with the device, which limits attempts),
- stops a man in the middle,
- and leaves both sides with a fresh session key that [seals](#sealing)
  everything afterwards, so the Wi-Fi password is encrypted.

It needs nothing from BLE pairing, so it works the same on every host,
including Web Bluetooth (which can't drive pairing) and through a
[bridge](#server-and-web-interface).

### Pairing with a code

```
host                                        device
 |-- auth-begin (code) ------------------------>|  makes a fresh random code,
 |<---- Reply: digits, attempts, expiry, salt --|  shows it and a QR code
 |   user types 482 913, or scans the QR        |
 |-- auth-code: pA ---------------------------->|
 |<----------------- Reply: pB, confirmB -------|
 |   host checks confirmB                       |
 |-- auth-confirm: confirmA ------------------->|  device checks confirmA,
 |<------------------------ Reply: ok, level ---|  hides the code
 |== everything sealed from here ===============|
 |-- remember ("Alastair's laptop") ----------->|  optional
 |<------------------- Reply: hostId, hostKey --|
```

1. **auth-begin** with method 1. The device makes a random code of `digits`
   digits (6 by default), shows it, and replies with a random 16-byte
   `salt`. A code lasts `expiresSeconds` (120) and allows `attemptsLeft`
   (3) tries; after that the host has to ask for a new one. The device may
   require a button press before it shows a code, and refuses while it
   isn't [accepting new hosts](#listening-mode).
2. The user enters the code, or scans the [QR code](#the-qr-code).
3. **SPAKE2** (RFC 9382) on P-256, with SHA-256, HKDF and HMAC:
   - `w = SHA-256("blat code" ‖ salt ‖ code as ASCII digits) mod n`.
   - The host is party A, with identity `"blat host"`. The device is party
     B, with its 8-byte `deviceId` as identity. The additional data is the
     salt.
   - `auth-code` carries `pA`; its reply carries `pB` and the device's key
     confirmation `confirmB`; `auth-confirm` carries the host's `confirmA`.
     Points are uncompressed SEC1 (65 bytes).
   - A wrong code shows up as a bad `confirmA`. The device replies status 16
     with the attempts left. A host that gets a bad `confirmB` stops: it's
     talking to something that doesn't know the code.
4. On success the connection is at **level 2** (see
   [access levels](#access-levels)), the device hides the code, and both
   sides derive the session keys from SPAKE2's shared key `Ke`:
   `HKDF-SHA256(Ke, salt, "blat session")` → 32 bytes, split into
   `hostKey` (host to device) and `deviceKey` (device to host), AES-128
   each.

Six digits and three tries per code is a 1 in 333,333 chance per code, and
every code needs someone in range to ask for it. After three codes in a row
fail, the device refuses new codes for a minute, doubling each time
(status 17).

On the device, SPAKE2 is a few point multiplications with mbedTLS
(hardware-accelerated on ESP32, well under a second). In the browser, Web
Crypto can't do raw point arithmetic, so the JS library uses a small
pure-JS curve library such as `@noble/curves`.

### The QR code

The QR code carries the same code plus which device it is, so a phone can
go straight from its camera to a connected, authenticated page:

```
https://example.com/blat/#d=1a2b3c4d5e6f7081&c=482913
```

- The base URL is set in the firmware: a public web page, or a
  [server](#server-and-web-interface)'s own address. A device with no base
  URL shows only the digits.
- `d` is the `deviceId` in hex, so the page can check it connected to the
  right device (it's in Info). `c` is the code.
- Both are in the URL fragment (after `#`), which browsers never send to the
  server, so the code doesn't end up in web server logs.
- The page still needs one tap on "Connect", because Web Bluetooth only
  opens its device chooser from a user gesture.
- The code is single-use and expires, so a photo of the screen is useless a
  couple of minutes later.

On the X4 the code screen is the QR code with the digits large beneath it.
Generating the QR code is a few KB of C (e.g. Nayuki's `qrcodegen`).

**Devices without a screen** have a fixed code printed on a label, with its
QR code (`methods` bit 1). SPAKE2 still keeps it off the air, but anyone who
reads the label can control the device, like the Wi-Fi password on a
router's sticker. Such devices should only accept a code during their
[boot window](#listening-mode) or after a button press.

### Resuming a remembered host

Typing a code on every connection would be tedious, so after pairing a host
can ask to be **remembered**. The device gives it a random `hostId` and a
32-byte `hostKey` (inside the sealed session), and stores them with the
name the host gave. It keeps up to 8 hosts, dropping the least recently
used.

On later connections the device **challenges** the host:

1. `auth-begin` with method 2 and the `hostId`. The device replies with a
   random 16-byte `challenge`, or status 16 if it doesn't know the host.
2. `auth-resume`: the host sends a random `hostNonce` and
   `hostMac = HMAC-SHA256(hostKey, "blat host" ‖ challenge ‖ hostNonce ‖ deviceId)`.
3. The device checks it and replies with
   `deviceMac = HMAC-SHA256(hostKey, "blat device" ‖ challenge ‖ hostNonce ‖ deviceId)`.
   The host checks that, so it knows it reached the same device.
4. Session keys: `HKDF-SHA256(hostKey, challenge ‖ hostNonce, "blat session")`,
   split as above.

A fresh challenge each time means a recorded exchange can't be replayed.
Because `hostKey` is random rather than a 6-digit code, a plain HMAC is safe
here and no PAKE is needed.

A resumed host gets **level 1**, not 2. It can change everyday settings,
but the things that should need someone standing at the device (Wi-Fi,
factory reset, firmware, forgetting hosts) take a fresh code. The firmware
decides which controls those are, by giving them write level 2.

Hosts store `hostId` and `hostKey` per `deviceId`: a Web Bluetooth page in
IndexedDB, a server in its data directory. A device lists and forgets hosts
with the [well-known controls](#well-known-keys) `auth.hosts` and
`auth.forget`, and `system.factoryReset` forgets them all.

### Sealing

Once authenticated, every message on Request, Reply, Data, DataOut and Event
is sealed with AES-128-CCM and an 8-byte tag: with `hostKey` for what the
host sends, `deviceKey` for what the device sends.

- Fixed headers stay in the clear and are authenticated as associated data:
  `op, seq` on requests, the 6-byte header on replies, the type byte on
  events, `transferId, offset` on transfer chunks. The body is encrypted and
  the tag appended.
- Nonces are never sent. Each is 13 bytes: `u8 channel` (1–5, numbered as in
  [stream transports](#stream-transports)), then
  - on Request, Reply and Event, a `u32` count of messages on that channel
    since authenticating (these arrive reliably and in order);
  - on Data and DataOut, `u8 transferId, u32 offset`. The device never
    reuses a `transferId` within a session; after 255 transfers the host
    authenticates again;

  then zeros.
- A message that fails to open ends the session: the device replies status
  15 and drops the connection back to level 0. Keys and counters belong to
  one connection, so a disconnect ends the session too.

Sealing costs 8 bytes per message and little time (the ESP32 has AES in
hardware). What stays in the clear is the protocol headers, Info, and any
level-0 reads made before authenticating.

**BLE pairing** (LE Secure Connections) isn't needed on top, and devices
shouldn't require it: it depends on the OS, is fragile across platforms,
and Web Bluetooth can't drive it. A device may still use it.

### Access levels

Each control has a read and a write level (the `access` byte). The device
checks them against the connection's level:

| Level | Who | Typical controls |
| --- | --- | --- |
| 0 | any connected host | name, battery, firmware version. **Read only: no control has write level 0.** |
| 1 | a host that authenticated on this connection, with a code or by resuming | everyday settings, files |
| 2 | a host that entered a fresh code on this connection: someone at the device now | Wi-Fi, factory reset, firmware, forgetting hosts |

A request above the connection's level gets status 15 with the level
needed, and the host asks the user for a code. A device with nothing at
level 2 needs no distinction between 1 and 2.

Also:

- Secrets are write-only and never logged (principle 8).
- Hosts don't store secrets. A bridge stores nothing but its own
  `hostId`s and `hostKey`s.

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

## Declaring controls in firmware

The schema is generated from one table in the firmware. That table is the
device's whole exposure policy: settings, values and actions the firmware
has but doesn't list are unreachable over BLE. A sketch for the X4, reusing
its existing `settings::Choice` definitions:

```cpp
// src/ble/BlocControls.cpp: everything the X4 exposes over blat.
using namespace blat;

constexpr Control kControls[] = {
    group("display", "Display"),
    choice("display.refresh", settings::kRefresh, Access{0, 1}, kLive),
    choice("display.frameSleep", settings::kFrameSleep, Access{0, 1}, kLive),

    group("power", "Power"),
    choice("power.sleep", settings::kSleep, Access{0, 1}, kLive),
    readout("power.battery", "Battery", Unit::Percent, Access{0}, kLive),

    group("files", "Images"),
    dir("files.images", "/images", ".bmp,.png", Access{1, 2}),

    action("system.restart", "Restart", Access{2}, kConfirm),
};
// settings::kClock isn't listed, so hosts can't see or change it.
```

- `Access{read, write}` gives the [access levels](#access-levels); a single
  level is read-only. So the exposure of each control is one of: absent
  (not in the table), read-only, or read-write, each at a level.
- The schema blob and its CRC are built from the table at compile time, or
  once at boot, and live in flash. No per-connection work.
- Well-known keys get their types checked at compile time where the
  language allows it, so a firmware can't publish `wifi.password` as plain
  `text`.

**Exposure that changes at runtime.** A device may expose different controls
in different states: `system.firmware` only while on external power, or
everything read-only while a "lock" setting is on. It does that by building
from a different table (or masking entries), which changes the schema CRC
and sends a `schema` event. Hosts just re-read. This should be rare; for
"sometimes you can't do this" a `busy` or `not permitted` status is usually
simpler.

## Server and web interface

The server is a **dumb bridge and renderer**. It knows how to speak blat and
how to draw each [control type](#control-types). It doesn't know what any
device can do until the device tells it, and it keeps no list of what to
expose: that came from the firmware.

Two ways to drive a device, sharing one JS library (a `protocol.js` and a
simulated device, like blit's `js/`):

```
 (a) one person, one device, nothing to run
     browser ── Web Bluetooth ──► device

 (b) a server near the devices
     browser ── HTTPS / WebSocket ──► blat server ── BLE (noble) ──► devices
                                       │
                                       ├─ device list (seen, remembered)
                                       ├─ schema cache (by CRC)
                                       └─ audit log
```

(a) is the "settings page for my gadget" case: open a page, pick the device,
get a form. It needs Chrome or Edge (desktop or Android); Safari and iOS
have no Web Bluetooth.

(b) puts a server where [`blit/server`](../blit/server/) runs (a Pi or Mac
mini with a Bluetooth adapter). It connects to devices on demand, and
relays blat to browsers over WebSocket using the
[stream framing](#stream-transports). The browser runs the same page as in
(a), with a WebSocket instead of Web Bluetooth underneath, so there's one UI
to build.

**Authentication through a bridge.** The server is the host the device
authenticates, and it holds the session keys; browser to server is ordinary
HTTPS. The first time, someone enters the device's code in the server's
page (or scans the QR code, if the device's base URL is the server), and
the server asks to be remembered. After that it resumes at level 1 on every
connection. When a browser user changes a level-2 control, the device
answers `authRequired`, and the server's page asks that user for a fresh
code, exactly as a Web Bluetooth page would. So even through a server,
Wi-Fi changes and resets need someone at the device.

By default every signed-in user of the server can do what the server's
connection can. The server can cap a user at level 0 (look, don't touch);
it then hides and rejects controls using the levels **the device
declared**. That's the only setting the server has, and it's about people,
not controls: the server never decides what a control's level is.

The server stores nothing about devices that it can't rebuild by
reconnecting: its schema cache is keyed by CRC, and a firmware update
that changes what's exposed simply shows up as a new schema.

### Pages

- **Devices:** everything seen, with name, model, firmware, last seen,
  signal, whether it's listening, whether it remembers the server. Scan
  button, and a code prompt for pairing.
- **Device:** the generated form. Groups become sections; each type gets its
  widget (see [control types](#control-types)); `live` values update in
  place; actions show progress; `file` and `dir` controls get drop zones and
  lists. Well-known keys get special UI (a Wi-Fi network picker driven by
  `wifi.scan`). Controls above the user's level are left out.
- **Audit:** who changed what, when, from which client. Secrets recorded as
  "changed", never their value.

## Plan

In order; each step is usable by itself.

1. **JS library and simulated device**: request/reply, schema parser, value
   codecs, transfers (reusing blit's), authentication and sealing, a
   `sim-device.js` with a sample schema that prints its code. Tests like
   `blit/js/test`, including the RFC 9382 test vectors.
2. **Web Bluetooth page** with the generic form renderer. Works against the
   simulated device first.
3. **X4 firmware**: the [controls table](#declaring-controls-in-firmware),
   exposing `settings::Choice` as `enum` controls with `live`, and the code
   screen with its QR code. A second GATT service next to `CastServer`.
4. **Server**: BLE bridge (from `blit/server`), WebSocket relay, the pages
   above.
5. **Files**: `dir` control for the X4's SD images.
6. **Wi-Fi**: on an ESP32 that uses it (the M5StickS3), with `wifi.scan` and
   `wifi.connect`.
7. **Label codes** for headless devices, if one needs blat.

## Open questions

- **Our own composition of standard crypto.** SPAKE2, HKDF, HMAC and
  AES-CCM are standard, but the way they're put together here isn't. It
  should get a careful review, and test vectors, before anything important
  relies on it.
- **Longer codes in the QR code.** The QR code could carry a longer secret
  than the digits on screen, but then typed and scanned codes would need to
  differ. Six digits with SPAKE2 and attempt limits is enough, so they stay
  the same for now.
- **Changing exposure without reflashing.** The table is compiled in, so
  exposing one more setting means a firmware update. If that gets tedious,
  a level-2 control could narrow the table at runtime (hide
  or make read-only), but never widen it past what the firmware declares.
- **SMP compatibility.** Adopting SMP's 8-byte header (and adding a
  "schema" group) would let blat devices talk to existing MCUmgr tools for
  firmware and files. It costs CBOR on the device.
- **Multiple hosts at once.** BLE peripherals can take several connections.
  Events already keep hosts in step; `set` needs no locking because it's
  atomic. Transfers are one at a time per device, so a second host gets
  `busy`.
- **Data input beyond forms.** Actions with params cover forms. Streaming
  input (a text box that types into the device as you type) would be a
  `text` control with `live` plus a debounce on the host; worth trying before
  adding anything.
