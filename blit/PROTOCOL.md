# The blit protocol

blit sends finished pixels from a **host** (a browser, a phone, a server, a
Chrome extension) to a small, low-power **display** (an e-paper reader, a
badge, a stick with an LCD), and sends the display's button presses back.

This document is the spec. Version **2** is described here, with version
**1** (what the X4 firmware first shipped, formerly "ble-cast") as a subset
that every host still has to speak. [Compatibility with v1](#compatibility-with-v1)
lists the differences.

| Term | Meaning |
| --- | --- |
| **Display** | The device that shows frames. The BLE peripheral and GATT server. |
| **Host** | The device that renders and sends frames. The BLE central and GATT client. |
| **Frame area** | The rectangle of the panel the display is showing frames in right now. It may be smaller than the panel (e.g. the X4 leaves room for its own chrome). |
| **Frame** | One transfer of pixels: a full frame area, or a region of it. |
| **Caps** | The display's capabilities: sizes, formats, limits, buttons. |

All integers are **little-endian**. Sizes are in bytes unless stated.

## Principles

These decide every trade-off in the protocol. New features have to fit
them.

1. **The host does the work.** Hosts have CPUs, memory and graphics stacks;
   displays don't. The host scales, crops, dithers and converts to exactly the
   size and pixel format the display asked for. A display never resizes,
   decodes images, parses text, or allocates memory in proportion to anything
   except the limits it declared itself.
2. **Pixels arrive ready to blit.** Every pixel format is a plain memory layout
   a panel driver can take directly, and every encoding can be decoded as a
   stream, a byte at a time. A display can write each chunk into its frame
   buffer as it arrives and never has to hold two copies of a frame.
3. **The display declares, the host chooses.** The display publishes its caps,
   with formats in order of preference, and updates them when they change. The
   host picks what to send, frame by frame. No negotiation round-trips, and the
   display never has to remember anything about a host.
4. **Limits are declared and never exceeded.** Largest frame, chunk size and
   flow-control window come from the display. Every message fits in one BLE
   write or notification once the MTU is negotiated. Anything unexpected gets
   an error status. A display must never crash on bad input.
5. **Binary, fixed layouts, extensible by TLV.** Host-to-display messages have
   fixed binary layouts, so displays parse them by offset. Display-to-host
   data that grows over time (caps) is a list of tag-length-value records, and
   hosts skip tags they don't know. Reserved bits and bytes are sent as 0 and
   ignored when read.
6. **Every frame stands alone.** A frame carries everything needed to show it.
   Losing the link loses at most the frame in flight. The host can always just
   send again, and the display keeps showing the last good frame.
7. **Idle is free.** No keepalives, no polling. Input is sent as events when
   it happens. The host says when its next frame is due, so the display can
   power down in between; the host reconnects when it has something to send.
8. **Send less.** Hosts skip frames that haven't changed, send regions when
   only part of the frame did, and use compression when the display supports
   it. Radio time is the display's biggest energy cost after the panel.

## Transport: Bluetooth LE GATT

The display advertises one primary service and includes its UUID in the
advertisement. **Hosts discover displays by service UUID, not by name.** The
advertised name is for people (the X4 uses `X4-XXXX`, from the last two bytes
of its address).

Service `b1ec0000-5f3a-4e62-9a47-0c3d8e5f2a10`. The UUIDs are unchanged from
ble-cast, so existing displays keep working.

| Characteristic | UUID | Properties | Direction | Purpose |
| --- | --- | --- | --- | --- |
| Info | `b1ec0001-5f3a-4e62-9a47-0c3d8e5f2a10` | read | display → host | [Caps](#caps). Always read fresh: it tracks the display's current state. |
| Control | `b1ec0002-5f3a-4e62-9a47-0c3d8e5f2a10` | write | host → display | [Control ops](#control-ops): begin, commit, cancel, hello |
| Data | `b1ec0003-5f3a-4e62-9a47-0c3d8e5f2a10` | write without response | host → display | Frame payload chunks |
| Status | `b1ec0004-5f3a-4e62-9a47-0c3d8e5f2a10` | notify | display → host | [Replies](#status) to control ops and data |
| Event | `b1ec0005-5f3a-4e62-9a47-0c3d8e5f2a10` | notify | display → host | [Events](#events): caps changes, key presses, taps, battery (v2) |

Status and Event are kept apart on purpose. Status is strictly
request/reply for the frame in flight, so a host can wait for "the next
status" without having to filter. Events arrive whenever the user does
something.

### Link setup

- Displays should ask for an ATT MTU of at least 185 (the X4 asks for 517),
  and for Data Length Extension. A bigger MTU means bigger chunks and fewer
  packets.
- Control messages are at most 96 bytes. If the MTU is smaller, the GATT stack
  uses a Long Write for them. Displays must accept those, as NimBLE and
  Bluedroid do.
- Displays should ask for a short connection interval while connected
  (15–30 ms; Apple hosts need max ≥ min + 15 ms). They must not require the
  2M PHY, because some hosts drop the link with it.

## Session

1. **Connect** to a display advertising the service.
2. **Read Info** and parse it as [caps](#caps). If the first byte is `{`
   (0x7B), the display speaks v1: see
   [Compatibility with v1](#compatibility-with-v1).
3. **Subscribe** to Status, and to Event if the display has it.
4. **Hello** (v2): write a [hello](#hello-0x04) to Control. The display
   answers with a `caps` event. It's optional over BLE unless the host wants
   key or pointer events: a display sends those only after a hello that asks
   for them, so it can keep handling its buttons itself until then.
5. **Send frames**, one at a time, as described below. Re-read caps (or use the
   `caps` event) before rendering each frame, because the frame area can
   change at any time.
6. **Disconnect** whenever you like. Nothing needs to be closed down.

## Sending a frame

```
host                                      display
 |-- Control: begin (header) -------------->|
 |<------------------------ Status: ready --|  value = chunk size
 |-- Data: offset 0, chunk ----------------->|
 |-- Data: offset n, chunk ----------------->|   ... `window` writes ...
 |<-------------------------- Status: ack --|  value = bytes received
 |-- Data ... (repeat until all sent) ----->|
 |<-------------------------- Status: ack --|  (always after the last write)
 |-- Control: commit ---------------------->|
 |<------------------------- Status: done --|  value = seconds it will sleep
```

1. **Begin**: write a [frame header](#frame-header-begin-0x01) to Control.
   Wait for `ready`. Its `value` is the chunk size to use for this frame.
2. **Data**: write the payload to Data in order. Each write is a `u32 offset`
   followed by up to `chunk` payload bytes. The display sends an `ack` after
   every `window` writes and after the last one. Once a group of `window`
   writes is sent, wait for its `ack` before sending more. Writes without
   response have no flow control of their own, so a host that runs ahead loses
   chunks and gets an out-of-order error.
3. **Commit**: write `0x02` to Control and wait for `done` or `error`. The
   display checks the length and CRC, shows the frame, then replies. With an
   e-paper panel, `done` can take a second or two.

Rules:

- **One frame in flight.** Begin again only after `done` or `error`. A new
  Begin discards any partial frame. `cancel` (`0x03`) drops it without
  starting another.
- **Timeouts.** A host gives up on a status after 20 s. A display drops a
  transfer silently if nothing arrives for 10 s.
- **Busy.** If the display is still showing the previous frame, Begin gets
  error 4. Retry after caps `refreshMs`, or after 500 ms if that isn't
  given.
- **Pacing.** Don't begin frames closer together than caps `minIntervalMs`.
- **Errors.** After any `error` the transfer is over. Start again with Begin.

### Frame header (begin, 0x01)

v2 header: 29 bytes plus the name.

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 1 | `op` | `0x01` |
| 1 | 1 | `version` | `2`. Send `1` with the [v1 header](#v1-frame-header) to a v1 display. |
| 2 | 1 | `format` | [Pixel format](#pixel-formats). Must be one of caps `formats`. |
| 3 | 1 | `encoding` | [Encoding](#encodings) of the payload. `0` = none. |
| 4 | 1 | `flags` | bit 0 `persist`: save the frame (e.g. to SD).<br>bit 1 `region`: this is a region update at `x`,`y`; the rest of the frame area keeps its pixels.<br>bit 2 `hold`: don't refresh the panel yet, more regions follow. The next frame without `hold` refreshes everything.<br>Other bits reserved. |
| 5 | 1 | `refresh` | Refresh hint: `0` let the display decide, `1` fast (partial, may ghost), `2` full (clean, slow). Displays without the `fastRefresh` feature ignore it. |
| 6 | 2 | `width` | Pixels |
| 8 | 2 | `height` | Pixels |
| 10 | 2 | `x` | Region origin in the frame area. 0 unless `region`. |
| 12 | 2 | `y` | |
| 14 | 2 | reserved | 0 |
| 16 | 4 | `byteLength` | Payload bytes as sent, after encoding. |
| 20 | 4 | `crc32` | CRC-32 (IEEE, zlib's `crc32`) of the payload as sent. |
| 24 | 4 | `nextFrameSeconds` | When the host expects to send its next frame. 0 = unknown. See [Sleeping between frames](#sleeping-between-frames). |
| 28 | 1 | `nameLength` | 0–64 |
| 29 | n | `name` | ASCII file name for a persisted frame. Optional. Displays sanitise it. |

**Placement.** A full frame (no `region` flag) should be exactly the caps area.
If it isn't, the display draws it pinned top-left and cropped when it's
larger, and centred when it's smaller. It never scales. A region must fit
inside the area. Its `x` must be a multiple of caps `regionAlign`, and so
must its `width`, unless the region reaches the right edge of the area.
Otherwise the display returns error 9.

**What a region patches.** A region changes pixels of the **last full frame
received on the current connection**, and nothing else. That frame must be
exactly the current area and in the same format as the region. The display
returns error 9 when it has no such frame: after a reconnect (including
waking from [frame sleep](#sleeping-between-frames)), a restart, an area
change, or when the last full frame was another size or format. A display
can't check that the host is diffing against the same pixels it holds, so it
only trusts a frame it got from this host on this link.

On error 9 for a region it computed itself, a host sends the same content
again as a full frame (the JS host does this automatically). Hosts should also
stop diffing against their last frame when the area changes.

### Pixel formats

Every format stores rows top to bottom. Each row starts on a byte boundary
(it's padded to a whole byte), and within a byte the leftmost pixel sits in
the most significant bits. So a row is `ceil(width × bpp / 8)` bytes and a
frame is that × `height`.

| Code | Name | bpp | Pixel value |
| --- | --- | --- | --- |
| 1 | `mono1` | 1 | 1 = black (ink), 0 = white |
| 2 | `gray2` | 2 | 0 = white … 3 = black |
| 3 | `gray4` | 4 | 0 = white … 15 = black |
| 4 | `gray8` | 8 | 0 = white … 255 = black |
| 16 | `rgb565` | 16 | R5 G6 B5, **high byte first**, the order SPI panels take |
| 24 | `rgb888` | 24 | R, G, B bytes |

Grey formats count **ink**, so 0 is always white, as in `mono1`. That way
a frame that's all zero bytes is blank, whatever the format.

A display lists the formats it takes in caps `formats`, **most preferred
first**. Hosts use the first one they can produce. That's how a display asks
for greyscale instead of 1-bit, or colour instead of grey: it reorders the
list and sends a `caps` event.

### Encodings

| Code | Name | Notes |
| --- | --- | --- |
| 0 | none | The payload is the pixels. |
| 1 | `packbits` | PackBits run-length encoding of the whole payload, padding included. Byte-oriented and streamable. Big wins on UI content (mostly white). |
| 2–255 | reserved | e.g. deflate with a small window, delta against the last frame |

PackBits, decoded one byte at a time: read a control byte `n`.

- `0x00`–`0x7F`: copy the next `n + 1` bytes literally.
- `0x81`–`0xFF`: repeat the next byte `257 − n` times.
- `0x80`: no-op.

Runs may span Data writes, so the decoder keeps two counters between chunks.
If the decoded length isn't exactly the frame size, the frame fails with
error 10. Hosts should use an encoding only if it makes the payload smaller.
`byteLength` and `crc32` are always of the bytes as sent.

### Commit (0x02), cancel (0x03)

Single-byte control writes. Commit ends the transfer and asks the display to
check and show the frame. Cancel drops the transfer in progress; there's no
reply.

### Hello (0x04)

v2. Optional over BLE, unless the host wants key or pointer events. Required
on stream transports, which have no Info read.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `op` = `0x04` |
| 1 | 1 | `version`: the highest protocol version the host speaks |
| 2 | 1 | `flags`: bit 0 wants key events, bit 1 wants pointer events. Other bits reserved. A display sends key and pointer events only while the latest hello on the connection asks for them. |
| 3 | 1 | `nameLength` (0–32) |
| 4 | n | `name`: UTF-8 host name, e.g. `Chrome: Grafana`. The display may show it. |

The display replies with a `caps` [event](#events), not a status. A v1 display
replies with status error 1, which hosts can ignore.

### Status

Status notifications are six bytes: `u8 event`, `u8 code`, `u32 value`.

| Event | Meaning | `code` | `value` |
| --- | --- | --- | --- |
| 1 `ready` | Begin accepted, send Data | 0 | chunk size for this frame |
| 2 `done` | Frame received, checked and shown (for a `hold` frame: received and stored; nothing is shown yet) | 0 | seconds the display will sleep before listening again; 0 = staying connected |
| 3 `error` | Frame rejected. Start again with Begin. | error code | detail |
| 4 `ack` | Data received so far | 0 | bytes received |

| Error | Meaning | `value` |
| --- | --- | --- |
| 1 | Bad header or unknown op | the op |
| 2 | Unsupported version, format or encoding | the unsupported code |
| 3 | Payload too large, or doesn't match `width × height` | expected bytes |
| 4 | Busy showing the previous frame; retry shortly | |
| 5 | Data out of order | expected offset |
| 6 | Commit before all bytes arrived | bytes received |
| 7 | CRC mismatch | |
| 8 | Couldn't persist (the frame is still shown) | |
| 9 | Region outside the frame area, misaligned, or with no full frame to patch (v2, see [what a region patches](#frame-header-begin-0x01)) | |
| 10 | Payload didn't decode to the frame size (v2) | decoded bytes |

## Caps

v2 displays return caps from Info and in `caps` events. The layout is a
version byte followed by TLV records until the end of the value:

```
u8 version (= 2)
repeat: u8 tag, u8 length, length bytes of value
```

Hosts skip unknown tags. Displays send each tag at most once, and may leave
out any tag, in which case the default applies. Info values are limited to
512 bytes, so there's plenty of room.

| Tag | Name | Value | Default if missing |
| --- | --- | --- | --- |
| 0x01 | `name` | UTF-8, ≤ 32 bytes | none |
| 0x02 | `panel` | `u16 width, u16 height`: the whole panel | = `area` |
| 0x03 | `area` | `u16 width, u16 height`: the frame area right now. **Render full frames at this size.** | required |
| 0x04 | `formats` | `u8[]` [format](#pixel-formats) codes, most preferred first | `[1]` |
| 0x05 | `encodings` | `u8[]` [encodings](#encodings) accepted besides 0 | `[]` |
| 0x06 | `limits` | `u32 maxBytes, u16 chunk, u16 window` | required |
| 0x07 | `features` | `u32` bits: 0 `persist`, 1 `frameSleep` (the display sleeps when given `nextFrameSeconds`), 2 `fastRefresh` (honours the refresh hint), 3 `pointer` (sends pointer events) | 0 |
| 0x08 | `regions` | `u8 align`: region `x` and `width` must be multiples of this (a region may end at the area's right edge). With sub-byte formats, `x × bpp` must also be a multiple of 8. Presence means region updates are accepted. | no regions |
| 0x09 | `keys` | `u8[]` [key codes](#key-codes) the display forwards to the host | none |
| 0x0A | `pacing` | `u32 minIntervalMs` (don't begin frames closer than this), `u32 refreshMs` (typical time to show a frame) | 0, 0 |
| 0x0B | `power` | `u8 battery` (percent, 255 = unknown), `u8 flags` (bit 0 charging, bit 1 on external power) | unknown |

`maxBytes` limits both `byteLength`, the payload as sent, and the frame once
decoded (`rowBytes × height`). Without the second limit a few bytes of
PackBits could ask for megabytes of frame buffer, against principle 1. A
display rejects either with error 3, and a host should render at `area` in a
format whose plain frame size fits. `chunk` is the largest
payload per Data write, after the 4-byte offset. Over BLE that's
`min(MTU − 3, 512) − 4`. The chunk size in `ready` wins over it, because the
MTU can change after Info is read.

### What the caps negotiate

The display asks and the host follows. Some examples:

- **Colour vs 1-bit.** A display with a greyscale mode puts `gray4` first in
  `formats` while the mode is on, and `mono1` first while it's off (for speed,
  or for less ghosting). Each time it switches, it sends a `caps` event.
- **Resolution.** `area` is the resolution the display wants. A display that
  hides its chrome, rotates, or wants a smaller picture (to save power or
  bandwidth) changes `area` and sends a `caps` event. The host renders the next
  frame at the new size. Displays don't scale (principle 1), so there's no
  separate "requested resolution".
- **Rate.** `pacing.minIntervalMs` caps how often frames come. E-paper can use
  it to protect the panel, and a battery display can raise it when the battery
  is low.
- **Bandwidth.** `encodings` and `regions` say what the display can decode.
  The host uses them when they help.

## Events

v2. Notifications on the Event characteristic, sent when something happens.
The first byte is the event type. Events are sent only while a host is
connected and subscribed, and `key` and `pointer` events only after a
[hello](#hello-0x04) that asks for them. They are **never queued across a
disconnect**, because stale key presses do more harm than lost ones.

| Type | Name | Layout | Notes |
| --- | --- | --- | --- |
| 0x01 | `caps` | `u8 type`, then caps bytes (as Info), or nothing | Caps changed. With no body, re-read Info (for when caps don't fit a notification). Also the reply to hello. |
| 0x02 | `key` | `u8 type, u8 key, u8 action, u8 reserved, u32 timeMs` | A forwarded button. `timeMs` = display uptime, or 0. |
| 0x03 | `pointer` | `u8 type, u8 action, u16 x, u16 y` | Touch or stylus, in frame-area pixels (the same space as the frame). |
| 0x04 | `power` | `u8 type, u8 battery, u8 flags` | As the caps `power` tag. Send when it changes by 5 % or more, or when charging starts or stops. |

### Key codes

The display decides which physical buttons it forwards (listed in caps
`keys`) and which it keeps for itself. The X4 keeps Back for leaving the app,
for example. Codes are semantic, so hosts can map them without knowing the
device.

| Code | Key | Code | Key |
| --- | --- | --- | --- |
| 1 | up | 7 | menu |
| 2 | down | 8 | home |
| 3 | left | 9 | page next |
| 4 | right | 10 | page previous |
| 5 | select / OK | 16–31 | function keys 1–16 (unlabelled buttons A, B, C…) |
| 6 | back | | |

| Action | Meaning |
| --- | --- |
| 1 `press` | Short press, released |
| 2 `long` | Long press (sent once, while still held or on release) |
| 3 `down` | Pressed |
| 4 `up` | Released |
| 5 `repeat` | Auto-repeat while held |

A display may send `press`/`long` only (the usual case, cheapest for both
sides), or `down`/`up`/`repeat` as well. Hosts should act on `press` and
`long` and ignore actions they don't use.

Pointer actions: 1 `down`, 2 `move` (at most 10 per second), 3 `up`,
4 `tap`. Map a pointer back to the host's content with the inverse of the
transform the host used to render the last frame.

## Sleeping between frames

When the display has the `frameSleep` feature enabled and a frame arrives with
`nextFrameSeconds` above its own threshold (30 s on the X4), it:

1. shows the frame,
2. replies `done` with `value` = the seconds it will sleep,
3. disconnects, and sleeps for `nextFrameSeconds` minus a wake margin (10 s on
   the X4),
4. wakes into the same mode and advertises again.

A host that sees `done.value > 0` treats the disconnect as expected. It
reconnects when it has the next frame, retrying for about 30 s. Hosts that
can't keep time well should leave `nextFrameSeconds` at 0.

## Stream transports

Draft, for UART, USB CDC, TCP or WebSocket links. It uses the same messages as
BLE, framed with a channel byte:

```
u8 channel, u16 length, length bytes of message
```

| Channel | Direction | BLE equivalent |
| --- | --- | --- |
| 1 | host → display | Control write |
| 2 | host → display | Data write |
| 3 | display → host | Status notification |
| 4 | display → host | Event notification |

There's no Info read, so the host starts with hello and the display answers
with a `caps` event. `chunk` and `window` still apply, so a UART display with
a small RX buffer is protected the same way. On WebSocket each message is one
binary frame, and the `u16 length` is left out.

## Compatibility with v1

v1 is what `xteink-x4-platformio` first shipped. It is v2 without events,
hello, regions, encodings or formats other than `mono1`, and with a JSON Info
and a shorter header. Every host must support v1 displays. A v2 display may
also accept v1 headers.

**Detecting v1.** Info starts with `{` and is UTF-8 JSON:

```json
{"v":1,"name":"X4-1A2B","w":716,"h":480,"fullW":800,"fullH":480,
 "chunk":508,"window":16,"maxBytes":65536,"formats":[1],"frameSleep":false}
```

| JSON | Caps equivalent |
| --- | --- |
| `v` | version |
| `name` | `name` |
| `w`, `h` | `area` |
| `fullW`, `fullH` | `panel` |
| `chunk`, `window`, `maxBytes` | `limits` |
| `formats` | `formats` |
| `frameSleep` | `features.frameSleep` (`persist` is always supported) |

A v1 display has no Event characteristic, so there are no key events and no
caps events. Hosts re-read Info before each frame instead.

### v1 frame header

21 bytes plus the name. Always `mono1`, no encoding, full frames only.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `op` = `0x01` |
| 1 | 1 | `version` = 1 |
| 2 | 1 | `format` = 1 |
| 3 | 1 | `flags`: bit 0 `persist` |
| 4 | 2 | `width` |
| 6 | 2 | `height` |
| 8 | 4 | `byteLength` |
| 12 | 4 | `crc32` |
| 16 | 4 | `nextFrameSeconds` |
| 20 | 1 | `nameLength` (0–64) |
| 21 | n | `name` |

Status, Data, commit, cancel, errors 1–8 and sleeping are the same as v2.

## Implementations

| Where | Role | Speaks |
| --- | --- | --- |
| [`xteink-x4-platformio`](../xteink-x4-platformio/) (`src/ble/CastServer.*`, `src/apps/BleApp.*`) | display (Xteink X4, e-paper): `mono1` and `gray2`, PackBits, regions, keys up / down / select | v2 (and v1 headers) |
| [`js/`](js/) | host library, plus a simulated display | v1, v2 |
| [`web/`](web/) | demo page: images, slideshows, live element capture | via `js/` |
| [`chrome-extension/`](chrome-extension/) | blits a browser tab | via `js/` |

### Notes for display implementers

- Start from the bottom of the feature list: `mono1`, no encodings, no regions,
  no events. Each extra is a caps entry, and hosts adapt to whatever you
  declare.
- Receive into the frame buffer directly when you can. Validate the header up
  front (size, format, `maxBytes`), so you reject a frame before its bytes
  arrive rather than after.
- Do the BLE work on the BLE task and hand finished frames to the main loop.
  Send `done` only once the frame is actually on the panel. Hosts use it as
  the "you can send again" signal.
- CRC the payload incrementally as it arrives, so commit doesn't need a second
  pass.
- [`js/sim-display.js`](js/sim-display.js) is a complete v2 display in under
  400 lines of JavaScript. Read it next to this spec.

### Notes for host implementers

- Parse caps from both v1 JSON and v2 TLV into one structure (see `parseCaps`
  in [`js/protocol.js`](js/protocol.js)).
- Render at `area` exactly. Pick the first entry of `formats` you can produce.
- Hash each rendered frame and skip it if it hasn't changed. With regions,
  compare against the last frame you sent and send the changed rectangle,
  aligned out to `regionAlign`.
- Keep a queue of one: if a new frame is rendered while one is in flight,
  replace the waiting frame rather than queueing both.
- On error 9 for a region you computed, resend it as a full frame. The display
  forgets its region base on every reconnect.

## Open issues

Found implementing v2 on the X4. The first two need a change to the wire
format, so they're written up here rather than in the spec above. v2 isn't on
any shipped hardware yet, so now is the cheap time to make them.

### Regions don't survive a reconnect

Regions save the most where the radio is costly: a dashboard or clock that
changes a few digits a minute, with [frame sleep](#sleeping-between-frames)
between frames. But every wake is a new connection, and a display can only
trust a region base it received on the current one (see [what a region
patches](#frame-header-begin-0x01)). So with frame sleep, every frame is a
full frame and regions never help.

**Proposal:** name the base in the header. Add `u32 baseCrc` to the v2
header: the CRC-32 of the full frame (unencoded pixels, `rowBytes × height`)
the region applies to, 0 for a full frame. The display keeps the CRC of the
frame it holds, updating it after each region (it has to touch every patched
row anyway, or CRC the whole frame, 48 KB on the X4, in a few ms). It accepts
a region whose `baseCrc` matches, whatever connection it arrives on, and
returns error 9 otherwise. A display that reloads its last frame from storage
after a restart (the X4 does, when the frame was persisted) can then accept
regions against it too. The two reserved bytes at offset 14 are too small, so
the header grows to 33 bytes: `baseCrc` at 29, `nameLength` at 33, `name` at
34. Hosts send `version` 2 either way; a display that sees a 29-byte layout
(v2 without the field) can't be told apart, so this should land before any
v2 display ships, or come in as version 3.

### One refresh time for every format

`pacing.refreshMs` is one number, but on e-paper a greyscale refresh is a
different and much slower waveform than a black-and-white one (the X4 offers
`gray2` behind `mono1` for this reason). A host pacing a slideshow or
retrying after busy waits the wrong time for one of them.

**Proposal:** let the `pacing` tag carry an optional list after the two
`u32`s: `u8 format, u32 refreshMs` per format that differs from the default.
Old hosts read the first 8 bytes and ignore the rest.

### Greys are fragile on e-paper

Not a wire-format issue, but worth knowing when choosing formats: on the X4 a
4-level image is a separate refresh pass, and any later black-and-white
refresh (a status change, a key highlight) turns it back into black and
white. The X4 handles this by showing no key feedback over a grey frame,
redrawing status changes with another grey pass (except the brief
"Receiving"), and advertising `mono1` first so hosts only send greys when a
user asks for them. A future `features` bit could tell hosts "greys cost more
than the refresh time suggests", if hosts need to know.
