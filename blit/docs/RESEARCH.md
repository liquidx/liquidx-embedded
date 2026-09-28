# blit and other remote-screen protocols

Notes from comparing blit with other protocols that move pixels (or
something that becomes pixels) to a screen somewhere else, to see what their
designs can teach us. The goals for blit are unchanged: simple, low
bandwidth, low power, BLE first, with a faster update rate as an option.

The short version:

- **blit's basic shape is right.** Every protocol here that targets small or
  battery-powered screens ends up where blit already is: the host renders,
  the display declares what it can take, one update is in flight at a time,
  and the latest frame replaces any queued one.
- **For a sleeping display, the connection costs more than the pixels.**
  Once regions and compression are in play, a clock update is a few hundred
  bytes, while reconnecting takes 15 or more round trips. The largest wins are
  in reconnecting less often and doing less per connection, not in better
  compression.
- **The display should say what it already holds.** OpenEPaperLink, RDP's
  persistent cache, TRMNL and Mosh all identify the receiver's current state,
  so the sender can skip a transfer or send a diff against it. This is the
  `baseCrc` open issue in [PROTOCOL.md](../PROTOCOL.md#regions-dont-survive-a-reconnect),
  taken a step further.
- **PackBits stays the only encoding.** Measured on X4-sized frames, it
  already cuts 1-bit UI screens by 80–90 %. A row filter or deflate would
  take more off (30–45 % and 50–65 % of what's left), but that's a few
  Data writes per frame, and not worth a second encoding for now. The
  measurements are kept in [compression](#compression) in case a grey,
  colour or fast display changes that.
- **Three round trips per frame limits the frame rate** far more than
  bandwidth does. A faster mode needs fewer round trips, not a new transport.
- **Keep refusing to draw on the display.** Vector, template and tile-cache
  designs save bytes, but every one of them moves work and state onto the
  display, against principle 1. Things to leave out are listed
  [at the end](#what-not-to-adopt).

## Where the time and energy go

A rough cost model for the X4 over BLE, to decide which ideas matter. Radio
time is energy, so "time the radio is busy" stands in for both.

| Step | Cost (rough) |
| --- | --- |
| One round trip (write with response, or write then wait for a notification) | 1–2 connection intervals: 30–60 ms at 15–30 ms |
| Full X4 frame, `mono1`, 716 × 480 | 43,200 B, 86 Data writes at 508 B |
| Same, `gray2` | 86,400 B |
| Typical UI frame, PackBits | often a tenth to a third of that |
| A clock's changed digits as a region, PackBits | a few hundred bytes, one Data write |
| Sending a frame (begin/ready, acks, commit/done) | 3 round trips + 1 per `window` writes |
| Reconnecting after frame sleep: scan, connect, MTU exchange, data length, service discovery, read Info, two CCCD writes, hello | about 10–15 round trips plus scan time: often 0.5–2 s of radio |

Actual BLE throughput varies a lot by host. An embedded central on the 1M
PHY can approach 100 kB/s, and the 2M PHY 170 kB/s. Browsers and phones
often manage a few tens of kB/s because they send fewer packets per
connection event. So a full `mono1` frame takes about 0.5–2 s and a region
takes one packet.

Two conclusions follow:

1. **E-paper with frame sleep** (the X4's main case): once a region is
   small, nearly all the cost is reconnecting. Regions can't even be used
   there yet, because the region base is forgotten on reconnect.
2. **An LCD at a higher rate** (e.g. the M5StickS3's 135 × 240 `rgb565`,
   64,800 B per frame): three round trips per frame cap the rate at about
   5–10 frames a second even when the frame is tiny. For full frames, the
   limit is bandwidth.

## The protocols

| Protocol | What it moves | Transport | What to take from it |
| --- | --- | --- | --- |
| [RFB / VNC](#rfb--vnc) | pixels, rectangles | TCP | Client-paced updates, many rectangles per update, CopyRect, ContinuousUpdates for fast mode |
| [RDP](#rdp) | drawing orders, cached bitmaps and glyphs | TCP/UDP | A persistent cache the client lists on connect, so the server knows what's already there |
| [DisplayLink (udlfb)](#displaylink) | pixel runs, copies | USB | The host keeps a shadow of the device's framebuffer and sends only changed spans |
| [Stream Deck](#stream-deck) | a JPEG per key | USB HID | Many small independent regions, each a complete image |
| [Flipper Zero screen stream](#flipper-zero) | 1-bit 128 × 64 frames | BLE, USB (protobuf RPC) | For tiny screens, full frames are fine |
| [Pebble AppMessage images](#pebble-and-sony-smartwatch) | chunked bitmaps | BLE / Classic | The same design as blit: size first, chunks, a completion message |
| [Bluetooth ESL](#bluetooth-electronic-shelf-labels) | images uploaded ahead, then short commands | BLE PAwR + GATT (OTS) | Stored image slots, one-packet commands, timed display, required encryption |
| [OpenEPaperLink](#openepaperlink) | compressed images in blocks | 802.15.4 | The tag checks in, the AP says what's new and when to check in next; content versioning; selective resend |
| [TRMNL](#trmnl) | a 1-bit image by URL | Wi-Fi HTTP | The device polls, and the server sets the refresh interval |
| [Espruino / Bangle.js](#retained-and-command-mode-designs) | JavaScript drawing calls | BLE UART | Very low bandwidth for text, at the cost of an interpreter on the device |
| [Microsoft SideShow](#retained-and-command-mode-designs) | XML pages, cached | various | Cache content on the device so the host can sleep |
| [Mosh (SSP)](#mosh) | terminal state diffs | UDP | Diff against the state the receiver acknowledged, and skip intermediate states |
| [BLE itself](#ble-transport-lessons) | | | Peripheral latency, GATT caching, L2CAP credit-based channels |

### RFB / VNC

[RFC 6143](https://www.rfc-editor.org/rfc/rfc6143.html) and the
[community spec](https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst).

- **The client paces the server.** In classic RFB the server sends an update
  only after the client asks for one (`FramebufferUpdateRequest`, with an
  *incremental* flag). A slow client is never flooded. blit gets the same
  effect from one frame in flight and `done`, and it's worth keeping.
- **An update carries many rectangles.** A `FramebufferUpdate` is a count and
  then rectangles, each with its own position, size and encoding. blit has
  one rectangle per begin/commit, and `hold` to combine several, which costs
  three round trips per rectangle.
- **CopyRect** moves a rectangle already on the screen. It costs 4 bytes for
  any amount of scrolling. That's useful for tickers and lists on an LCD. It
  doesn't help e-paper, where the refresh costs more than the transfer.
- **Encodings are per rectangle and tiled** (Hextile 16 × 16, TRLE, ZRLE
  64 × 64 with palettes). The server picks one per tile. That's more choice
  than an MCU should have to decode.
- **Pseudo-encodings** carry feature negotiation and events in the same
  stream: `DesktopSize` (like blit's `caps` event after an area change),
  `LastRect`, `Cursor`.
- **ContinuousUpdates + Fence** (a TigerVNC extension) switch to
  server-paced streaming for speed. The server measures what the link
  handles and sends up to a rate cap; `Fence` marks sync points for changes
  like pixel format. That's the pattern for a faster blit mode: stop
  requesting each update, and keep a flow-control and sync mechanism.

### RDP

- Instead of pixels, RDP sends **drawing orders** and **cached** bitmaps and
  glyphs, which is why it runs over slow links.
- The **persistent bitmap cache** survives across sessions. On connect, the
  client sends the list of keys it holds, so the server never resends what's
  already there. It's the same idea as blit's `baseCrc` proposal: identify
  what the receiver holds by content, not by connection.
- Orders and caches need memory and a renderer on the client, so the rest
  doesn't fit blit.

### DisplayLink

DisplayLink's protocol is proprietary, but the open Linux drivers (`udlfb`,
`udl`) show a small command set: write raw pixels, write run-length pixels,
copy within device memory. The host keeps a **shadow copy** of what the
device holds and compares against it to send only changed spans. The blit JS
host already does this (`#last`, `changedRect`).

What's worth taking: changes are sent as **spans within rows**, not as one
bounding rectangle. Two small changes far apart (a clock's minutes and a
battery icon in the opposite corner) cost two small spans, not a rectangle
covering most of the screen. See [deltas](#deltas-against-the-frame-the-display-holds).

### Stream Deck

Each key is a small LCD that gets a complete JPEG, split into 1 KB HID
reports with a page index and a last-page flag. Updating a key never touches
the others. It fits the "every frame stands alone" principle, but uses lossy
compression, because the MCU has a hardware or ROM decoder. See
[what not to adopt](#what-not-to-adopt).

### Flipper Zero

The Flipper's RPC ([protobuf](https://github.com/flipperdevices/flipperzero_protobuf_py))
has `StartScreenStream` / `StopScreenStream` and `ScreenFrame` messages with
the raw 1-bit 128 × 64 buffer (1,024 bytes), sent in the opposite direction
to blit. Third-party mirrors report 3–5 frames a second over BLE. There are no
regions or compression: at 1 KB they aren't worth it. This supports keeping
blit's minimal profile (`mono1`, no encoding, no regions) fully usable, so
small displays can stop there.

Protobuf needs a code generator and a varint decoder on the device and gains
little over fixed layouts at this scale, so blit's fixed binary layouts are
the better choice.

### Pebble and Sony SmartWatch

Pebble's [pattern for sending images from the phone](https://developer.rebble.io/blog/2014/10/29/Displaying-remote-images/):
the watch says how large a message it accepts, the phone sends the total size
first, then chunks over `AppMessage`, then a completion message. It's the
same shape as blit's begin, Data, commit. Each AppMessage is acknowledged,
which makes it slow; blit's `window` acks are faster.

Both Pebble and Sony's SmartWatch 2 also let the phone send **layouts** once
and then only text or image updates to named elements. That saved a lot of
bandwidth, but meant a layout engine on the watch. See
[retained designs](#retained-and-command-mode-designs).

### Bluetooth Electronic Shelf Labels

The [Bluetooth ESL](https://www.bluetooth.com/blog/bluetooth-esl-the-global-standard-for-the-electronic-shelf-label-market/)
service and profile (Bluetooth 5.4) are the closest standard to blit's
"e-paper that sleeps" case, at store scale: one access point, thousands of
labels, years of battery.

- **Images are uploaded ahead of time** over a normal GATT connection, into
  numbered slots, using the Object Transfer Service. Showing one is a
  **one-packet command**: "display image *i* on display *d*".
- **Commands go out by PAwR** (periodic advertising with responses): the
  label wakes for its scheduled subevent, receives commands without ever
  connecting, and answers in a response slot. There's no connection setup
  at all, which is where blit spends most of its time (see the cost model).
- **Timed commands** ("display image *i* at time *t*") let the AP send ahead,
  and the label acts on its own clock.
- **Security is required**: LE Secure Connections for bonding and encrypted
  advertising data for PAwR.

PAwR needs a central that can run periodic advertising. Phones and browsers
can't, so it would take a hub (an ESP32 or nRF acting as the host's radio).
Stored slots and one-packet commands carry over to blit's connected case
without that.

### OpenEPaperLink

[OpenEPaperLink](https://github.com/OpenEPaperLink/OpenEPaperLink) drives
reflashed shelf labels over 802.15.4. Its [protocol](https://github.com/OpenEPaperLink/OpenEPaperLink/blob/master/oepl-proto.h)
is built round a sleeping tag:

- **The tag checks in** on its own timer, with a tiny packet: battery,
  temperature, radio quality, hardware type, wake reason.
- **The AP answers** with `AvailDataInfo`: a 64-bit **`dataVer`** (a hash of
  the content), size, type, and **`nextCheckIn`** in minutes. If nothing
  changed, the tag goes straight back to sleep. The AP, not the tag,
  controls how often it wakes.
- **Content is identified by hash**, so a tag can tell whether it already has
  it.
- **Transfers are in 4 KB blocks** of 42 parts. After each block the tag
  requests only the parts it missed (a bitmask) and checks a block checksum.
- **Compression** (zlib or G5, a Group 4 fax-style codec for 1-bit images)
  saves airtime.

Selective resend fits a lossy link. BLE's link layer already retransmits, so
blit doesn't need it.

### TRMNL

[TRMNL](https://docs.trmnl.com/go/private-api/screens) is a Wi-Fi e-paper
display that polls `GET /api/display` with its battery voltage, RSSI and
current refresh rate. The reply has the image URL, a `filename`, and
**`refresh_rate`**: how long the device should sleep. The server sets the
device's schedule, as `nextFrameSeconds` does in blit. The filename lets the
device tell whether the image changed. Here the device pulls, where blit's
host pushes, and pulling makes "nothing new, sleep again" nearly free.

### Retained and command-mode designs

Espruino on Bangle.js takes **JavaScript** over the Nordic UART service (the
Gadgetbridge app sends calls like `GB({t:"notify",...})`) and the watch
draws. Microsoft **SideShow** (Windows Vista) sent XML pages and menus that
the device cached, so the user could browse while the PC slept. LCDproc sends
text to character displays. All of them need little bandwidth for text and
menus, but put a renderer, fonts and layout state on the device, which is
what principle 1 rules out. SideShow's caching idea survives without the
renderer, as [stored slots](#6-stored-frames-slots).

### Mosh

Mosh's State Synchronization Protocol sends **a diff from the last state the
receiver acknowledged** to the current state, and skips intermediate states
when the link is slow. Every diff names its base, so a diff against the wrong
base is detected and dropped. That's the model for blit regions: name the
base (`baseCrc`), always diff against what the display confirmed it holds,
and replace a queued frame instead of sending both, as the JS host already
does.

### BLE transport lessons

- **Peripheral latency.** The X4 asks for a 15–30 ms interval with latency 0
  whenever it's connected. With peripheral latency *N*, the display may skip
  up to *N* connection events when it has nothing to send, while the host
  still sees the link as up. Its own sends (key presses) go out at the next
  event. HID devices use this to stay connected on little power. Apple's
  accessory guidelines allow latency up to 30 as long as
  `intervalMax × (latency + 1) ≤ 2 s`. Bluetooth 5.3's connection subrating
  does the same with faster switching, where both ends support it.
- **Reconnects.** GATT caching (the Database Hash and robust caching from
  Bluetooth 5.1, or bonding) lets a host skip service discovery on a known
  display. Bonded devices also keep their CCCD subscriptions.
- **L2CAP connection-oriented channels** have credit-based flow control
  built in and larger SDUs, with more throughput than GATT writes. Android
  (API 29) and iOS (11) support them. Web Bluetooth doesn't, so they can only
  be an option. blit's stream transport framing would run over one
  unchanged, with the credits replacing `window` acks.
- **mcumgr SMP** (Zephyr/MCUboot firmware upload over GATT) answers each
  chunk with the next offset it expects, so an upload resumes after a
  disconnect instead of starting again. blit's error 5 already returns the
  expected offset. That's only worth using for frames that take many
  seconds, like `gray2` on a slow link.

## What blit already gets right

Things the survey confirms, and which shouldn't change:

- Host renders, display declares (every small-display design converges here).
- Fixed binary layouts host → display, TLV for caps (versus protobuf, XML).
- One frame in flight, latest wins (RFB request pacing, Mosh).
- The display sets the rate (`minIntervalMs`), the host sets the schedule
  (`nextFrameSeconds`, like OEPL `nextCheckIn` and TRMNL `refresh_rate`).
- A usable minimum profile (Flipper shows full frames are enough for tiny
  screens).
- Streamable PackBits. It gets fill-rectangles for free: a solid region
  decodes from a couple of bytes per 128.

## Compression

blit v2 hosts already compress: the JS host sends PackBits (encoding 1)
whenever the display lists it and it saves at least 10 %, and the X4
decodes it. So the question is what beats PackBits, and what it costs the
display.

Measured with a one-off script (not kept in the repo) on frames at the
X4's frame area (716 × 480), made by the host's own rasterizer (`js/raster.js`). The inputs are
a clock, the X4's home and settings screens, a dashboard, a page of text and
two photos. Sizes are in bytes. One Data write carries 508.

### 1-bit frames (`mono1`, the X4's default)

| Frame | Raw | PackBits (today) | PackBits-up | LZ4 | deflate | CCITT G4 | heatshrink |
| --- | --- | --- | --- | --- | --- | --- | --- |
| clock | 43,200 | 4,693 | 2,519 | 2,305 | 1,728 | 997 | 6,388 |
| X4 home | 43,200 | 4,814 | 2,803 | 2,544 | 1,844 | 940 | 6,530 |
| X4 settings | 43,200 | 5,304 | 2,902 | 2,731 | 2,055 | 1,434 | 7,030 |
| dashboard | 43,200 | 8,379 | 5,944 | 5,253 | 3,936 | 3,639 | 8,166 |
| page of text | 43,200 | 14,176 | 13,310 | 11,265 | 9,166 | 10,434 | 14,520 |
| photo, no dither | 43,200 | 18,371 | 17,918 | 14,205 | 11,474 | 8,642 | 14,487 |
| photo, Atkinson dither | 43,200 | 37,312 | 37,791 | 36,607 | 33,089 | 72,020 | 38,911 |

All of these are without dithering unless stated. "PackBits-up" is the
row-filter option described below. Deflate is with a 1 KB window, and zstd at level 19 (not a
candidate, just a reference) is only 10–20 % smaller than deflate.

`gray2` frames are 1.4–1.8 times the size of `mono1` after compression,
not twice, and the ratios between codecs are about the same: X4 home is
6,943 PackBits, 4,694 PackBits-up and 2,999 deflate. Colour (`rgb565`) UI at
this size is 23–62 KB with PackBits and 9–25 KB with deflate. A 32 KB window
matters there, because each row is 1.4 KB.

### Deltas against the frame the display holds

An update XORed with the frame the display holds is zero wherever nothing
changed, so it compresses to almost nothing. The measured updates: the
clock going from 12:34 to 12:35, and the dashboard with four values changed
in different places (the time, two readings and a label).

| Update | Full frame, PackBits | Bounding region, PackBits | Region, deflate | XOR, PackBits | XOR, PackBits-up | XOR, deflate |
| --- | --- | --- | --- | --- | --- | --- |
| clock, `mono1` | 4,621 | 649 (72 × 101) | 177 | 1,527 | 555 | 435 |
| dashboard, `mono1` | 8,380 | 1,960 (416 × 111) | 965 | 924 | 308 | 294 |
| dashboard, `gray2` | 12,795 | 3,230 (408 × 111) | 1,664 | 1,731 | 488 | 479 |

When the change is in one place, a tight region is best. When changes are
scattered, the bounding box covers most of them and a delta is 3–6 times
smaller. Either way, the update fits in one or two Data writes, instead of
10–17 for a full frame.

### What the numbers say

- **Heatshrink** (LZSS, popular on MCUs) is *worse* than PackBits on UI.
  Its matches are capped at 16 bytes, so long white runs still cost about a
  bit per byte, where PackBits spends 2 bytes per 128.
- **PackBits with a row filter** ("PackBits-up") is the cheap win: XOR each
  byte with the one above it, so repeated rows and vertical edges become
  zeros. It's 30–45 % smaller than PackBits on UI screens (6 % on a page
  of text) and needs no memory, since
  the row above is already decoded. It does worse on dithered photos, so the
  host picks per frame, as it already does between PackBits and none.
- **Deflate** is another third smaller than PackBits-up, and the best
  general choice. Its window barely matters for 1-bit and 2-bit frames: 512
  bytes (5 rows of `mono1`) is within about 3 % of 32 KB, because the useful
  matches are a row or two up. Hosts get it for free (`zlib` in Node,
  `CompressionStream('deflate-raw')` in browsers). The cost is a real decoder
  on the display, a few KB of code, and decoder state.
- **CCITT G4** (fax, the family OpenEPaperLink's G5 comes from) is the best
  on clean 1-bit UI, down to half the size of deflate. But it's larger than deflate
  on dense text, *larger than raw* on dithered images, `mono1` only, and
  browsers have no encoder for it. It only makes sense later, as a
  `mono1` extra for displays that mostly show dashboards.
- **LZ4** has the simplest real LZ decoder, but it lands between PackBits-up
  and deflate, and hosts would need an encoder. Deflate is the better step up.
- **Dithering costs more than any codec saves.** A dithered photo is almost
  incompressible (31–37 KB of 43 KB whatever the codec). For UI, dithering
  anti-aliased text adds about 10 % with deflate, and up to 60 % with G4. The Chrome extension and the
  server default to Atkinson, which is the right call for photos and the
  wrong one for UI. For photos, an undithered `gray2` frame deflates to 18 KB,
  about half a dithered `mono1` one, if the display's grey refresh is
  acceptable.
- **In time**, the X4's 1-bit screens are already 10–17 Data writes with
  PackBits. Deflate makes them 4–8, saving perhaps 50–300 ms of radio per
  frame, depending on the host. That's worth having, but reconnecting costs
  more. The bigger payoffs are deltas (10–30× smaller than a full frame),
  grey and colour frames (deflate is 2–2.5× smaller than PackBits there),
  and high frame rates, where every byte counts.

### Decision: keep PackBits

PackBits (encoding 1) stays the only encoding. It's already in every host
and in the X4, its decoder is two counters, and it takes most of the size
off. Everything below is recorded as measured options, not planned work.
It's worth revisiting if a display makes the remaining bytes matter: grey
or colour frames, or a fast LCD mode.

### Options considered

If that happens, these are the designs the measurements point to.

| Code | Name | Decoder | Notes |
| --- | --- | --- | --- |
| 0 | none | | |
| 1 | `packbits` | 2 counters | As now |
| 2 | `packbits-up` | 2 counters | PackBits, then each decoded byte is XORed with the decoded byte one row above (row 0 as is). Plus `0x80 u16 n`: `n + 1` zero bytes. `0x80` is a no-op in PackBits, so the code is free. |
| 3 | `deflate` | an inflater that can stop at the end of a Data write and resume with the next (miniz's `tinfl` works this way) | Raw deflate (RFC 1951). Caps say the largest window the display accepts. |

And one header flag, orthogonal to the encoding:

- **`delta`** (flags bit 3): the decoded bytes are XORed onto the frame
  (or region) the display holds instead of replacing it. Needs `baseCrc` to
  match, like regions, and error 9 otherwise. Zero bytes leave pixels
  unchanged, so any encoding with cheap zero runs is a delta encoding.

This replaces the separate skip op considered earlier (under XOR, a skip is just a run
of zeros), and it combines with regions: a delta region of just the digits
that changed.

The decoder stays inside principle 2. The X4 already decodes into a
contiguous receive buffer (`rx_`), so the row above and deflate's whole
window are already in memory. A 32 KB window costs it nothing, and the browser's
built-in deflater can be used as is. A display that decodes straight into a
panel-width frame buffer declares a small window (9–10 bits) and keeps a ring
of that size, and hosts use `zlib` (Node) or pako (browsers), which let
the window be set. Deflate is bit-oriented, so principle 2's "a byte at a
time" becomes "chunk by chunk", which is all the display needs anyway.

Caps: the `encodings` list already says which codes a display takes. Deflate
also needs its window size: a new tag, or one byte after the code in the
list. Hosts try the encodings a display lists, keep the smallest, and fall
back to none as now.

## Recommendations

In order of payoff for the X4 and similar displays. The first two don't
change the wire format.

### 1. Relax the connection when idle

Use two sets of connection parameters: the current short interval while a
frame is transferring, and peripheral latency (e.g. interval 30–45 ms,
latency 20–30) the rest of the time. The display can switch by itself: on
`ready` it asks for the fast set, and a second or so after `done` it asks for
the slow set. Key events still go out at once, since latency only delays the
host's packets. The first host write after idle waits up to
`interval × (latency + 1)`, under a second.

For a display that stays connected (an LCD stick, or e-paper without frame
sleep) this is the biggest power saving available, and it's a firmware
change only.

### 2. Fewer round trips per frame

A frame costs three round trips (begin/ready, last ack, commit/done). Two
of them can go without breaking v2 displays:

- **Don't wait for the last ack.** ATT delivers writes in order on the
  bearer, so a commit written after the last Data write can't overtake it.
  Let hosts send commit straight after the last Data write. The display
  sends the ack as now, and the host ignores acks once it has sent commit.
  Spec change: "hosts may commit without waiting for the final ack".
- **Start sending before `ready`** when the frame fits in the first window:
  use the caps `chunk` (not larger) and ignore `ready`'s value unless it's
  smaller. This needs displays to set up the transfer while handling the
  begin write, before the next write is processed. The X4 already does
  (`CastServer::onControl` handles begin in the write callback). If the begin is
  rejected, the display reports the error and drops the Data that follows,
  as the X4 already does with Data when no transfer is in progress.

Together these make a small frame one round trip, which roughly triples
the frame rate for small updates on an LCD.

### 3. The display says what it holds

Adopt the `baseCrc` proposal from the open issues, and add its other half:
a caps tag with the CRC-32 of the frame the display holds right now.

| Tag | Name | Value |
| --- | --- | --- |
| 0x0C | `frame` | `u32 crc32` of the unencoded pixels shown, `u8 format`, `u16 width, u16 height` |

After a reconnect, the host reads caps and knows before sending anything
whether it can send a region (or delta), or whether the frame is unchanged
and doesn't need sending at all. Without the tag, it finds out from error 9
after sending the payload. It's the same as OEPL's `dataVer`, RDP's persistent
key list and HTTP's `ETag`. A display that reloads a persisted frame after a
restart reports its CRC too, so regions work across restarts.

### 4. Compression: keep PackBits

Keep PackBits as the only encoding (see [compression](#compression)). For
small updates, rely on regions, made to work across reconnects by
`baseCrc` (recommendation 3). The row filter, deflate and the `delta` flag
are documented as options for later.

One change costs nothing on the wire: switch hosts to no dithering for UI content (tab and element capture),
and keep dithering for images, or add an `auto` mode that dithers only when
most pixels are mid-greys. It's smaller and crisper.

### 5. Nothing to send: let the display sleep again

When the host has no new frame at the time it predicted, it has to either
connect and resend the same frame (the full reconnect cost, plus a refresh on
e-paper) or stay away and leave the display advertising until something
times out. Two fixes, both small:

- **A wake window.** A caps entry (or a `pacing` field) says how long the
  display advertises after waking from frame sleep. If no host connects
  in that time, it sleeps for the same interval again. A host with nothing
  new just stays away, which costs it nothing and the display only the
  window. This is OpenEPaperLink's "no data, back to sleep".
- **A `sleep` op** (`0x05`, `u32 seconds`) for a host that did connect (to
  read caps, or check `frame`) but has nothing to show. The display replies
  `done` with the seconds it will sleep and disconnects, as after a frame.

### 6. Stored frames (slots)

For displays with storage (the X4 has an SD card and already persists
frames): let a frame be stored under a slot number, and show a slot with a
one-write command, like ESL image indices or SideShow's cache. Slideshows
and dashboards that switch between a few screens then send each screen
once. Declared in caps as a slot count, so it stays within the display's
declared limits. It's lower priority than 1–5 and only worth doing if a
real use needs it.

### 7. A faster mode for LCDs

For LCDs that refresh in milliseconds, with the changes above:

- Declare **pipelining** in caps (`u8 depth`): the host may begin frame
  *n + 1* before frame *n*'s `done`, up to `depth` frames outstanding. This
  lets the next transfer overlap the panel update, the double buffering
  ContinuousUpdates gets in RFB. E-paper displays leave it at 1.
- Keep `minIntervalMs` as the rate cap. The host adds a latest-wins
  queue: when the link falls behind, frames are dropped, not delayed.
- Use the [stream transport](../PROTOCOL.md#stream-transports) over an L2CAP
  channel where the host can open one (Android, iOS, native desktop, not Web
  Bluetooth), with credits replacing the `window` acks.

### 8. Smaller colour formats the panel takes directly

`rgb565` doubles the size of a UI frame that uses a handful of colours.
Two options that still fit principle 2:

- **`rgb444`** (12 bpp): ST7789 and similar controllers accept it directly
  (`COLMOD` 0x03). It's 25 % smaller with no work on the display.
- **Indexed colour** (`index4`, `index8`) with a palette sent in the header
  or a Control op, as RFB's `SetColourMapEntries` does. The display expands
  each pixel through a 16- or 256-entry table as it writes to the panel,
  which is still a byte-at-a-time stream. For UI content, `index4` is a
  quarter of `rgb565`.

### 9. Optional encryption

Anyone in range can currently send frames to a blit display, or read its
key presses. Shelf labels require LE Secure Connections. blit can offer it as
a display setting: mark Control and Data as requiring encryption, and the
host's OS pairs on the first write (Web Bluetooth and the phone stacks handle
this). It costs nothing on the wire, and the pairing is a one-time step.

## What not to adopt

- **Drawing commands, templates or fonts on the display** (RDP orders,
  Espruino, Pebble/Sony layouts, SideShow). They save bandwidth for text,
  but need a renderer and state on the display. Stored slots and delta
  encoding get most of the savings for pixel content.
- **Tile or glyph caches** (RDP). The display would need to manage memory
  on the host's behalf.
- **Lossy codecs** (Stream Deck's JPEG). Wrong for 1-bit e-paper UI. For
  photos on a colour LCD they help a lot, and a TJpgDec-style decoder runs
  in a few KB streaming by 8- or 16-row blocks. Leave it as a possible
  future encoding code for displays that ask for it, not a core feature.
- **Selective resend** (OpenEPaperLink's part bitmasks). The BLE link layer
  already retransmits. Error 5's expected offset is enough.
- **Protobuf or JSON on the wire** (Flipper, TRMNL, blit v1's Info). Fixed
  layouts are smaller and simpler to parse on an MCU.
- **PAwR, for now.** It's the lowest-power design for many sleeping
  displays, but no phone or browser can be the central. Worth revisiting if
  blit gets a hub (e.g. an ESP32 host relaying frames from the server).

## Next steps

1. Measure before changing the spec: log the time from wake to `done` on
   the X4 with frame sleep, split into scan, connect, discovery, caps and
   transfer, to check the cost model.
2. Peripheral latency in `CastServer` (recommendation 1). No spec change.
3. Spec v2 changes while no v2 display has shipped: `baseCrc` (open issue),
   the `frame` caps tag, committing without the final ack, the `sleep` op
   and the wake window.
4. Default to no dithering for UI capture in the Chrome extension and
   server.
5. The rest when a display needs them: pipelining and `rgb444` / indexed
   for the M5StickS3, slots for slideshows, and the compression options if
   grey, colour or fast frames make the bytes matter.

## Sources

- [RFC 6143: The Remote Framebuffer Protocol](https://www.rfc-editor.org/rfc/rfc6143.html)
- [RFB community specification (rfbproto)](https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst): ContinuousUpdates, Fence
- [Bluetooth ESL: the global standard for the electronic shelf label market](https://www.bluetooth.com/blog/bluetooth-esl-the-global-standard-for-the-electronic-shelf-label-market/)
- [Nordic: Getting started with Bluetooth ESL and PAwR](https://devzone.nordicsemi.com/guides/nrf-connect-sdk-guides/b/getting-started/posts/getting-started-with-bluetooth-electronic-shelf-labels-esl-and-periodic-advertising-with-responses-pawr)
- [Silicon Labs AN1419: BLE Electronic Shelf Label](https://www.silabs.com/documents/public/application-notes/an1419-ble-electronic-shelf-label.pdf)
- [OpenEPaperLink](https://github.com/OpenEPaperLink/OpenEPaperLink) and its [`oepl-proto.h`](https://github.com/OpenEPaperLink/OpenEPaperLink/blob/master/oepl-proto.h)
- [TRMNL display API](https://docs.trmnl.com/go/private-api/screens), [TRMNL firmware](https://github.com/usetrmnl/trmnl-firmware)
- [Flipper Zero protobuf bindings](https://github.com/flipperdevices/flipperzero_protobuf_py)
- [Pebble: displaying remote images](https://developer.rebble.io/blog/2014/10/29/Displaying-remote-images/), [AppMessage](https://developer.rebble.io/docs/c/Foundation/AppMessage/)
- [RFC 1951: DEFLATE](https://www.rfc-editor.org/rfc/rfc1951), [CompressionStream](https://developer.mozilla.org/en-US/docs/Web/API/CompressionStream)
- [TI BLE5-Stack: L2CAP connection-oriented channels](https://software-dl.ti.com/lprf/simplelink_cc2640r2_sdk/1.35.00.33/exports/docs/ble5stack/ble_user_guide/html/ble-stack/l2cap.html)
- From general knowledge rather than fetched for this note: RDP caching,
  DisplayLink's `udlfb` driver, Stream Deck HID reports, Espruino/Gadgetbridge,
  Microsoft SideShow, Mosh SSP, Apple's Accessory Design Guidelines for
  connection parameters, mcumgr SMP. Check these before relying on details.
