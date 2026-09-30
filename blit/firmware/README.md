# blit firmware library

The display side of [blit](../PROTOCOL.md), for ESP32 with Arduino and
NimBLE. It does the protocol: caps, header checks, flow control, CRC,
decoding PackBits straight into a frame buffer as chunks arrive, regions, key
and power events. The firmware does the display: it takes finished frames,
puts them on its panel and says when they're there.

[`xteink-x4-platformio`](../../xteink-x4-platformio/) is the example:
[`src/ble/Cast.cpp`](../../xteink-x4-platformio/src/ble/Cast.cpp) declares
the display and [`src/apps/BleApp.cpp`](../../xteink-x4-platformio/src/apps/BleApp.cpp)
shows its frames.

## Adding blit to a device

Declare what the display is, once, at boot:

```cpp
#include <blit.h>

constexpr blit::Key kKeys[] = {blit::Key::Up, blit::Key::Down, blit::Key::Select};

blit::Receiver receiver;
blit::NimBleLink link(receiver);

void setupBlit() {
  blit::Config config;
  config.name = "Badge-1A2B";         // caps name, ≤ 32 bytes
  config.panelWidth = 296;
  config.panelHeight = 128;
  config.maxBytes = 16 * 1024;        // largest payload, and largest decoded frame
  config.features = blit::kFeaturePersist | blit::kFeatureFastRefresh;
  config.regionAlign = 8;             // 0 for no regions
  config.keys = kKeys;
  config.keyCount = 3;
  config.refreshMs = 1500;
  config.now = [] { return static_cast<uint32_t>(millis()); };
  receiver.begin(config);

  const uint8_t formats[] = {blit::kFormatMono1, blit::kFormatGray2};  // most preferred first
  receiver.setFormats(formats, 2);
}
```

With your NimBLE server:

```cpp
link.addService(server);                        // before server->start()
advertising->addServiceUUID(blit::kServiceUuid);
link.onConnect(handle); link.onDisconnect();   // from your server callbacks
link.reset();                                   // after NimBLEDevice::deinit()
```

In the main loop:

```cpp
blit::Buffer frame, region;  // PSRAM when there is some
blit::FrameHeader shown;     // the full frame on screen

blit::FrameHeader h;
if (receiver.takeFrame(h, frame, region)) {
  if (h.region()) {
    blit::applyRegion(frame.data(), shown, h, region.data());
  } else {
    shown = h;
    receiver.setRegionBase(h);  // regions may patch it now
  }
  if (h.hold()) {
    receiver.notifyDone(0);     // more regions follow; nothing to show yet
  } else {
    drawToPanel(frame.data(), shown);                 // your code
    receiver.notifyDone(receiver.sleepFor(h));        // once it's on the panel
    // ...then sleep for that many seconds, if it's more than 0.
  }
}

// When things change:
receiver.setArea(w, h);                  // the frame area (e.g. chrome shown or hidden)
receiver.setFormats(formats, n);         // e.g. a greyscale mode turned on
receiver.setFrameSleep(true);            // only if the device can wake on a timer
receiver.setPower(percent, charging, onUsb);
receiver.sendKey(blit::Key::Down);       // only reaches a host whose hello asked for keys
```

Each of those sends the host a `caps` (or `power`) event if one is connected
and something changed.

**One reply per frame.** `takeFrame()` leaves the frame owing a reply
(`owesReply()`), and `notifyDone()` or `notifyError()` pays it. Send `done`
only once the frame is on the panel: hosts take it as the go-ahead for the
next one. If saving a persisted frame fails, send `notifyError(Error::SaveFailed)`
instead (the frame is still shown).

**Tasks.** NimBLE calls the link on its own task, which validates, decodes
and replies there; frames are the bulk of the traffic, so there's no queue
and no second copy. The main loop only meets it in `takeFrame()` and a few
atomics. (blat does the opposite, queueing everything to the main loop,
because its messages are small and its work is crypto.)

### Frames and files

- `blit::level(row, format, x)`: a pixel's ink level in a grey format.
- `blit::applyRegion()`: patch a region into the full frame.
- `blit::encodeBmp()` / `blit::decodeBmp()`: grey frames as indexed BMPs
  (1, 2, 4 or 8 bits, palette white to black), for displays that persist
  frames. Encoding builds the whole file in a `Buffer`, so it goes to storage
  in one write; decoding reads through a `blit::Reader` you implement over a
  file.

## What's implemented

| Part of the spec | State |
| --- | --- |
| Caps (Info and `caps` events), hello | done |
| Frames: every format in the spec, no encoding or PackBits, flow control, CRC | done |
| Regions, `hold`, region base per connection and area | done |
| Events: `key`, `pointer`, `power` | done |
| Frame sleep: `sleepFor()` from the config's gap and margin | done; sleeping is the firmware's |
| Timeouts (dropping a transfer after 10 s of silence) | not yet |
| Stream transports | the Receiver takes any `Transport`; only `NimBleLink` ships |
| Protocol v1 | no: v2 only. A v1 header is rejected (error 1 as too short, or error 2 for its version). |

## Tests

```sh
make -C blit/firmware/test      # needs g++ (C++20) and Node 22+
```

- `test_receiver`: the Receiver against a fake transport (caps, every error
  code, PackBits across writes, acks, regions and their base, hello, events,
  power, sleep), and the frame and BMP helpers.
- `host.test.mjs`: the JS host library ([`../js/blit.js`](../js/blit.js))
  against `sim_display`, which is this library on stdin/stdout with the
  stream framing. Caps, plain and PackBits frames over several MTUs, regions
  and their fallback to full frames, errors, keys and caps changes.

The host and the Receiver were written separately from the spec, so if they
agree, the spec describes both.
