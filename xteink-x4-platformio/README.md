# xteink-x4-platformio

A small landscape "OS shell" firmware for the Xteink X4 Classic (and original X4) e-paper device, built
on the [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) (the hardware
layer behind [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)).
No ebook support: each page is a card, with a button gutter beside the front keys.
The visual spec is [docs/design/](docs/design/README.md).

```
 home (depth 0)                     Settings › Refresh (depth 2)
┌──────────────────────────────╮ ┌───┐   ┌────────────────────────────╮││ ┌───┐
│ (Wed 23 Sep)                 │ │ ▭ │   │ (SETTINGS / REFRESH)       │││ │ ▭ │
│                              │ │ ◀ │   │                            │││ │ ◀ │
│  14:32                       │ │ ● │   │ Full refresh every    6    │││ │ ● │
│                              │ │ ▲ │   │ 12 pages           [ 12 ]  │││ │ ▲ │
│ [▣ Images               24 ] │ │ ▼ │   │ Clears ghosting…     24    │││ │ ▼ │
│  ⚙ Settings                  │ │   │   │                    Never   │││ │   │
└──────────────────────────────╯ └───┘   └────────────────────────────╯││ └───┘
       card 728 × 480           gutter 72     each level down is 12px narrower
```

Pages below the current one stay drawn underneath, so the back stack shows as
one card edge per level. The top-edge side key hides the cards, gutter and
captions to give the page the whole 800 × 480 screen.

## Hardware

- **Xteink X4 Classic (X4C)**, the primary target (`-e x4c`, default): ESP32-S3, 16MB
  flash, 8MB PSRAM, 800×480 panel (SSD1677, UC8179 or UC8279, auto-detected at
  boot), native SDMMC microSD, no touch/frontlight. Details:
  [freeink-sdk/docs/xteink-x4c-support.md](freeink-sdk/docs/xteink-x4c-support.md).
- Original Xteink X4 (`-e x4`): ESP32-C3, no PSRAM. Builds, untested on hardware.
- Hold it in landscape with the four front keys on the **right** edge. On the
  X4C the two side keys are then on the top and bottom edges, near the left.

The front keys drive the whole UI. The gutter has a round button beside each
one: ◀ ● ▲ ▼. A button shows inverted while its key is held down.

| Key (landscape) | Action |
| --- | --- |
| Front 1, top (portrait "Right") | **Back** one page; from an app's top level, home (nothing on home) |
| Front 2 (portrait "Left") | **Select** / Open |
| Front 3 (portrait "Confirm") | **Up** (previous image) |
| Front 4, bottom (portrait "Back") | **Down** (next image) |
| Side key, top edge (next to Power) | Show / hide chrome: gutter, titles, captions |
| Side key, bottom edge | unassigned |
| Power (hold 1s) | Sleep (blanks to "Sleeping") |

After the Settings → Sleep after timeout the device also sleeps, but leaves the
current page on screen with the chrome hidden and a ❚❚ badge in the bottom-right
corner. Power wakes it, back to home.

Navigation: **Home** shows the date, a large clock (from the X4C's BM8563 RTC)
and a list of apps (two rows show at a time; it scrolls); Up/Down move between
rows and Select opens one. **Images** opens straight onto the first BMP in
`/images`, filling the card, and Up/Down step through them. **Bluetooth** is a
remote screen: see [Bluetooth](#bluetooth). **Settings** lists Refresh, Sleep
after, Clock, Frame sleep, Storage and About with their current values. Select
opens a setting's page, where Up/Down change the value (saved to flash straight
away) and Select or Back returns.

The mapping lives only in [`src/shell/Input.cpp`](src/shell/Input.cpp). If the
screen comes up upside down, change `kOrientation` in
[`src/shell/Layout.h`](src/shell/Layout.h) to `LandscapeClockwise` and reverse
the front-key order in `Input.cpp`.

## Toolchain

[PlatformIO](https://platformio.org/) (`pio`). The first build downloads the
pioarduino ESP32 platform.

```sh
git submodule update --init xteink-x4-platformio/freeink-sdk   # from the repo root
```

The SDK is pinned to the commit CrossPoint uses. Don't `--recursive`: the SDK's
own icon submodule isn't needed.

## Build and flash

Run from this folder, with the X4 connected over USB-C and awake:

```sh
pio run                     # build (x4c)
pio run -t upload           # flash
pio device monitor          # serial log (115200); key presses log as [INPUT]
```

**Back up the device before the first flash.** Take a full 16MB read (goes into
the git-ignored `backups/`). Leave the baud rate at the default: 921600
dropped out partway through a read through a USB hub.

```sh
pio pkg exec -p tool-esptoolpy -- esptool --chip esp32s3 \
  read-flash 0x0 0x1000000 backups/x4-backup.bin
```

To restore everything: `esptool --chip esp32s3 write-flash 0x0 backups/x4-backup.bin`.

`partitions-x4c.csv` is byte-identical to the factory X4C table, and
`pio run -t upload` writes only to `app0`. Anything installed in `app1`
(e.g. CrossPoint via OTA) is left alone. Recovery if things go wrong:
[fix-bricked-xteink.md](https://github.com/crosspoint-reader/crosspoint-reader/blob/master/docs/fix-bricked-xteink.md).

## Screen

| | |
| --- | --- |
| Panel | 800 × 480, black/white e-paper (UC8279 on the unit tested; SSD1677/UC8179 on others) |
| Framebuffer | 1 bit per pixel, 100 bytes/row, 48,000 bytes, MSB-first, 1 = white |
| Orientation | `LandscapeCounterClockwise` is panel-native, so logical (x, y) == framebuffer (x, y) |
| Bezel | about 7 px top, 3 right, 7 bottom, 9 left are hidden (SDK estimate, not yet measured on the X4C) |
| Greys | `Color` gives 17 dithered levels in 1-bit mode; true 4-level greyscale uses two extra 48KB planes |
| Refresh | `FAST_REFRESH` for navigation, `HALF_REFRESH` for new content (about 1.3 s), plus one after every 12 fast refreshes |

Driving the device from the Mac (device awake, on USB):

```sh
python3 scripts/devctl.py shot shot.png          # screenshot of the panel
python3 scripts/devctl.py key select down back   # inject key actions (back/select/up/down/chrome)
python3 scripts/devctl.py status                 # input task + raw button pin states
python3 scripts/devctl.py time                   # set the RTC to the Mac's local time
```

The clock shows `--:--` until the RTC has been set once.

## Images

Put `.bmp` files in `/images` on the SD card. Any uncompressed BMP (1–32 bpp)
works. The device never scales: it draws at native resolution, pinned to the
top-left and cropped when larger than the card (716 × 480 with chrome, 800 × 480
without), centred when smaller. `scripts/img2bmp.py` (needs Pillow) turns PNGs and JPGs into
800×480 1-bit BMPs that the device draws as-is:

```sh
python3 scripts/img2bmp.py photo.jpg art.png -o out/       # fill and crop, dithered
python3 scripts/img2bmp.py sketch.png --dither none        # hard threshold for line art
python3 scripts/img2bmp.py photo.jpg --fit contain --preview   # letterbox, write a PNG preview
```

## Layout

```
src/
  main.cpp            boot, wake handling, sleep, main loop
  Fonts.*             font IDs and registration
  shell/
    Layout.h          screen geometry, card stack, orientation
    Input.*           physical keys → semantic Actions (the only place keys are named)
    App.h             app interface: render(area) + handle(action) + depth()
    Shell.*           home, card stack, gutter, chrome toggle, refresh policy
  ui/
    Widgets.*         pills, rows, ListView, InfoView, icons
  apps/
    ImageApp.*        /images viewer, one BMP per page
    BleApp.*          Bluetooth remote screen
    SettingsApp.*     settings list, choice pages, About
  ble/
    CastServer.*      BLE GATT server for the blit protocol (v1)
  Settings.*          persisted settings (NVS) and their options
fonts/                IBM Plex Mono TTFs (OFL); scripts/fonts.sh turns them into headers
docs/design/          visual spec and mockups
lib/                  code vendored from CrossPoint, see lib/README.md
freeink-sdk/          hardware SDK (git submodule)
```

To add an app, subclass `App`, then `shell.addApp(&myApp)` in `main.cpp`.

## Bluetooth

The **Bluetooth** app receives frames over Bluetooth LE and shows them, like a
remote screen. While it's open the device advertises as `X4-XXXX`; leaving it
turns the radio off. It shows the last frame received (after a restart, the
last one saved), or "Listening". The title pill shows the link status:
Listening, Connected, Receiving, "Next in N s" (the sender's interval), or
Transfer failed; a Bluetooth badge in the corner marks a frame as live.

- Send from a browser with the [blit web demo](../blit/web/) (Chrome/Edge,
  Web Bluetooth): images, slideshows, or a live capture of a web page. Or
  blit any tab with the [Chrome extension](../blit/chrome-extension/).
- Frames are 1-bit at the device's frame area (716 × 480, or 800 × 480 with
  chrome hidden) and are drawn at native size, like Images.
- Frames are saved to `/images` as BMPs unless the sender turns `persist` off.
- **Settings → Frame sleep** (off by default): when a sender says when its next
  frame is due, the X4 Classic deep-sleeps until just before it, frame left on
  screen, then wakes back into the Bluetooth app. Gaps under 30 s don't sleep.
  The original X4 can't wake on a timer, so it stays awake.

Wire format: the [blit protocol](../blit/PROTOCOL.md). The X4 speaks v1
(1-bit full frames); v2 adds greyscale, regions, compression and sending the
keys back to the host. The receiver is `src/ble/CastServer.*` (NimBLE) and
`src/apps/BleApp.*`.

## Status / next

- [x] Landscape shell, key mapping
- [x] Button gutter beside the front keys, card back stack
- [x] Home / Select / Up / Down navigation, chrome toggle, scrolling pages
- [x] Serial screenshot tool
- [x] BMP image viewer
- [x] Settings: refresh interval, sleep timer, 12/24h clock, storage, About
- [x] Bluetooth remote screen ([blit](../blit/) v1), sleep between frames
- [ ] blit v2: forward Up / Down / Select to the host, caps events on chrome
      toggle, 2-bit greyscale, PackBits, regions
- [ ] Wi-Fi: scan, join, save credentials (start with `/wifi.txt` on SD)
- [x] Persist settings
- [ ] Persist last app
- [ ] On-device keyboard, or a hotspot + web form for entering Wi-Fi details
