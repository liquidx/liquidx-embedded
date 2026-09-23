# xteink-x4-platformio

A small landscape "OS shell" firmware for the Xteink X4 Classic (and original X4) e-paper device, built
on the [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) (the hardware
layer behind [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)).
No ebook support: full-screen apps, with a key-hint gutter beside the front keys.

```
 chrome shown                                  chrome hidden (top-edge key)
┌──────────────────────────────────┬──────┐   ┌─────────────────────────────────────────┐
│ Home                             │ 87%  │   │                                         │
│ ▓ Images                       ▓ │⌂ Home│   │                                         │
│   Settings                       │● Open│   │          content, 800 × 480             │
│                                  │▲ Up  │   │                                         │
│                                  │▼ Down│   │                                         │
└──────────────────────────────────┴──────┘   └─────────────────────────────────────────┘
          content 728 × 480          gutter 72
```

## Hardware

- **Xteink X4 Classic (X4C)**, the primary target (`-e x4c`, default): ESP32-S3, 16MB
  flash, 8MB PSRAM, 800×480 panel (SSD1677, UC8179 or UC8279, auto-detected at
  boot), native SDMMC microSD, no touch/frontlight. Details:
  [freeink-sdk/docs/xteink-x4c-support.md](freeink-sdk/docs/xteink-x4c-support.md).
- Original Xteink X4 (`-e x4`): ESP32-C3, no PSRAM. Builds, untested on hardware.
- Hold it in landscape with the four front keys on the **right** edge. On the
  X4C the two side keys are then on the top and bottom edges, near the left.

The front keys drive the whole UI. The gutter labels what each one does right
now; a faint stub means the key does nothing on this screen.

| Key (landscape) | Action |
| --- | --- |
| Front 1, top (portrait "Right") | **Home** at an app's top level, **Back** once drilled in (nothing on Home) |
| Front 2 (portrait "Left") | **Select** / Open / View |
| Front 3 (portrait "Confirm") | **Up** (previous image in the viewer) |
| Front 4, bottom (portrait "Back") | **Down** (next image in the viewer) |
| Side key, top edge (next to Power) | Show / hide chrome: gutter, titles, captions |
| Side key, bottom edge | unassigned |
| Power (hold 1s) | Sleep |

Navigation: **Home** is a full-screen list of apps. **Images** lists `/images`,
Select views one full screen, and Up/Down step through them. **Settings** lists
Wi-Fi and About; those pages scroll with Up/Down.

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
```

## Images

Put `.bmp` files in `/images` on the SD card. Any uncompressed BMP (1–32 bpp)
works: the device scales it down to fit and dithers it. Pre-sizing makes
drawing faster. On macOS:

```sh
sips -Z 620 -s format bmp photo.jpg --out photo.bmp
```

## Layout

```
src/
  main.cpp            boot, wake handling, sleep, main loop
  Fonts.*             font IDs and registration
  shell/
    Layout.h          screen geometry and orientation
    Input.*           physical keys → semantic Actions (the only place keys are named)
    App.h             app interface: render(area, focused) + handle(action)
    Shell.*           home list, key-hint gutter, chrome toggle, refresh policy
  ui/
    Widgets.*         ListView (scrolling list), InfoView (scrolling label/value rows)
  apps/
    ImageApp.*        /images list + full-screen BMP viewer
    SettingsApp.*     Wi-Fi (placeholder), About
lib/                  code vendored from CrossPoint, see lib/README.md
freeink-sdk/          hardware SDK (git submodule)
```

To add an app, subclass `App`, then `shell.addApp(&myApp)` in `main.cpp`.

## Status / next

- [x] Landscape shell, key mapping
- [x] Key-hint gutter beside the front keys
- [x] Home / Select / Up / Down navigation, chrome toggle, scrolling pages
- [x] Serial screenshot tool
- [x] BMP image viewer
- [x] Settings → About
- [ ] Wi-Fi: scan, join, save credentials (start with `/wifi.txt` on SD)
- [ ] Persist last app / settings
- [ ] On-device keyboard, or a hotspot + web form for entering Wi-Fi details
