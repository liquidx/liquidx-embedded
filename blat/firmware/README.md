# blat firmware library

The device side of [blat](../PROTOCOL.md), for ESP32 with Arduino and
NimBLE. A device declares its controls in **one table**. That table is:

- **its settings**: each value's type, limits, default, and where it's saved;
- **its exposure over Bluetooth**: a control that isn't in the table doesn't
  exist over the air, and each one that is says who may read and write it.

The library keeps the values, saves them, checks every change against the
declaration, and speaks the protocol, including pairing with a code. The
device's own UI reads the same table, so a setting is declared once.

[`xteink-x4-platformio`](../../xteink-x4-platformio/) is the example:
[`src/Controls.h`](../../xteink-x4-platformio/src/Controls.h) is its table.

## Declaring controls

```cpp
#include <blat.h>

inline constexpr blat::Option kSleepOptions[] = {{1, "1 min"}, {5, "5 min"}, {0, "Never"}};

inline constexpr blat::Control kControls[] = {
    blat::group("power", "Power"),
    blat::choice("power.sleep", kSleepOptions)
        .in("power")
        .label("Sleep after")
        .help("Time without a key press before the device sleeps.")
        .initial(5)
        .saveAs("sleep")
        .live(),
    blat::number("power.battery").in("power").label("Battery").unit("%").range(0, 100).readOnly().live(),
    blat::secret("wifi.password").label("Password").length(8, 63).write(blat::kLevelPresent).saveAs("wifipass"),
    blat::action("system.restart").label("Restart").confirm().write(blat::kLevelPresent),
};
static_assert(blat::check(kControls));

constexpr blat::Id kSleep = blat::idOf(kControls, "power.sleep");
```

Each entry starts from a function for its type and chains setters. The
whole table is `constexpr`, so it lives in flash and is checked when the
firmware compiles.

### Types

| Start with | Protocol type | Value | Default unless `.initial()` |
| --- | --- | --- | --- |
| `group(key, label)` | `group` | none: a section heading | |
| `toggle(key)` | `bool` | 0 or 1 | off |
| `number(key)` | `int` | `int32_t`, fixed point with `.scale()` | 0 |
| `choice(key, options)` | `enum` | one of the options' `uint16_t` values | the first option |
| `text(key)` | `text` | up to `.length()` bytes of UTF-8 (default 0–32) | `""` |
| `secret(key)` | `secret` | like text, but hosts can only write it (default 0–64) | always empty |
| `action(key)` | `action` | none: a button. Its params are controls declared `.in()` it | |

`bytes`, `file` and `dir` are in the protocol but not in the library yet.

### Setters

| Setter | Meaning |
| --- | --- |
| `.label(s)` | Name shown to people, ≤ 32 bytes |
| `.help(s)` | A sentence or two, ≤ 160 bytes |
| `.in(key)` | The group it's in, or the action it's a param of. It must come earlier in the table. |
| `.initial(v)` | **The default**: a number, `true`/`false`, or a string. It's the value before anything is saved, after `Values::reset()`, and when a saved value is no longer valid (say an update removed that option). It's also in the schema, so hosts can offer "reset to default". |
| `.saveAs(nvsKey)` | Save the value across restarts, under this NVS key (≤ 15 bytes). Without it the value lives in RAM and starts at its default each boot, which suits readouts like the battery. |
| `.read(level)`, `.write(level)` | [Access levels](../PROTOCOL.md#access-levels): 0 any host, 1 a paired host, 2 a host that entered a fresh code. Defaults: read 0, write 1. Nothing can be written at level 0. |
| `.readOnly()` | Hosts can read it but never write it. The firmware still can. |
| `.live()` | Hosts are told when it changes, however it changed. |
| `.range(min, max, step)`, `.scale(d)`, `.unit(s)` | For numbers. `range(0, 300).scale(1)` is 0.0 to 30.0. |
| `.length(min, max)` | For text and secrets. |
| `.hint(h)` | Input hint for hosts: `Hint::Url`, `Hint::Hostname`, `Hint::TimeOfDay`, … |
| `.confirm()`, `.restart()`, `.advanced()`, `.required()`, `.hidden()` | [Flags](../PROTOCOL.md#flags) for hosts' UI. `.required()` is for action params. |

### Rules, checked at compile time

`static_assert(blat::check(kControls))` refuses a table where:

- keys are missing, repeated, longer than 32 bytes, or use anything but
  letters, digits, `.` and `_`;
- a default isn't one of the options, or is outside the range or length;
- a writable control or action has write level 0;
- a parent comes later in the table, or isn't a group or action;
- `saveAs` keys repeat or are longer than NVS allows;
- a secret has a default or is `live`;
- a [well-known key](../PROTOCOL.md#well-known-keys) (`wifi.password`,
  `system.restart`, …) has the wrong type;
- labels, help or units are too long.

A failed check stops the build at a function named after the problem:

```
error: call to non-'constexpr' function 'void blat::detail::error_initial_value_is_not_an_option()'
```

`blat::idOf(kControls, "power.sleep")` is also checked: a key that isn't in
the table doesn't compile.

**Changing a table.** Ids are positions in the table, so hosts cache the
schema by its CRC and pick up any change. Saved values follow `saveAs` keys,
not ids, so reordering is safe. Renaming a `saveAs` key starts that setting
over at its default, unless you move the old value across (the X4 does this
in `Settings.cpp` for the settings it had before blat).

## Using values in firmware

```cpp
blat::PrefsStore store;
blat::Values values;

store.begin("settings");  // an NVS namespace
values.begin(blat::table(kControls), &store);

int minutes = values.get(kSleep);
values.set(kSleep, 10);                  // from the device's own UI; saved, and live hosts are told
values.set(kBattery, 87);               // read-only to hosts, not to the firmware
if (values.takeHostChanges()) { ... }  // a host changed something: re-read what you cache
```

`set()` refuses values the declaration doesn't allow and returns
`Status::InvalidValue`.

## Adding blat to a device

```cpp
class MyDelegate : public blat::Delegate {
  void showCode(const char* code) override { /* draw it, or hide it when null */ }
  blat::Status invoke(blat::Id action, const blat::Arg* params, size_t count) override {
    if (action == kRestart) { /* restart shortly, after the reply goes out */ return blat::Status::Ok; }
    return blat::Status::ActionFailed;
  }
};

blat::crypto::setRandom([](uint8_t* out, size_t n) { esp_fill_random(out, n); });
blat::Identity id;  // name, model, firmware, 8-byte device id
device.begin(values, &store, id, delegate, millis);

// With your NimBLE server:
link.addService(server);                        // before server->start()
link.onConnect(handle); link.onDisconnect();   // from your server callbacks
link.poll();                                    // from the main loop
```

`blat::Device` does the protocol and runs on one task, the main loop, so
values, NVS writes and the pairing maths never race the BLE task.
`blat::NimBleLink` is the GATT service: NimBLE's callbacks only queue what
arrives, and `poll()` hands it to the device. The device can also run over
any other transport (see `test/sim_device.cpp`, which uses stdin and stdout).

The X4 splits this across [`Settings.cpp`](../../xteink-x4-platformio/src/Settings.cpp)
(store and values), [`ble/Remote.cpp`](../../xteink-x4-platformio/src/ble/Remote.cpp)
(device, delegate, link) and [`ble/Radio.cpp`](../../xteink-x4-platformio/src/ble/Radio.cpp)
(the NimBLE server it shares with blit).

## What's implemented

| Part of the spec | State |
| --- | --- |
| Info, hello, schema transfer, get, set, invoke, options | done |
| Events: `changed` for `live` controls | done; `schema`, `progress`, `actionDone`, `log` not yet |
| Pairing with a code (SPAKE2), remembering and resuming hosts, sealing | done |
| Access levels 0 / 1 / 2, lockout after wrong codes | done |
| Uploads, `file` and `dir` controls, `bytes` | not yet: `write-open`, `list` and `delete` answer `bad request` |
| `dynamic` enums | `options` returns the declared options; no runtime lists yet |
| QR code on the pairing screen | not yet (needs a host page for the URL) |
| Several hosts at once | no: one connection at a time |

## Tests

```sh
sudo apt install libmbedtls-dev   # and Python 3 with `cryptography`
make -C blat/firmware/test
```

- `test_values`: declarations, defaults, saving and validation.
- `bad-tables`: nine broken tables, each of which must fail to compile with
  the right error.
- `host_test.py`: a host written from the spec in Python (its own P-256,
  SPAKE2 and HKDF) against `sim_device`, a blat device on stdin/stdout. It
  covers pairing, wrong codes, lockout, expiry, sealing, tampering,
  remembering and resuming, atomic sets, actions, events and small MTUs.

The Python host is an independent implementation, so agreement means the
spec describes both. Neither has been checked against RFC 9382's own test
vectors yet.
