# Embedded projects

A single repo for my embedded / hardware projects. Each top-level folder is a
self-contained project for one device + toolchain combination, with its own
README, build config, and helper scripts.

## Projects

| Folder | Device | Toolchain | Notes |
| --- | --- | --- | --- |
| [`m5stack-resident/`](m5stack-resident/) | M5StickS3 (ESP32-S3) | PlatformIO + [Resident](https://resident.inanimate.tech/) | Lua apps pushed over the Resident relay |
| [`xteink-x4-platformio/`](xteink-x4-platformio/) | Xteink X4 (ESP32-C3, e-paper) | PlatformIO + [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) | Landscape shell firmware: image viewer, Bluetooth receiver, settings |
| [`ble-cast/`](ble-cast/) | Any Chrome/Edge browser | Web Bluetooth, no build | Sends images and live page captures to the X4's Bluetooth app |

## Conventions

- **One folder per device + toolchain**, named `<device>-<toolchain>`
  (e.g. `m5stack-resident`, `m5stack-arduino`, `rp2040-pico-sdk`, `esp32-idf`).
  If the same device is used with a different toolchain, it gets its own folder.
- **Everything a project needs lives inside its folder**: build config
  (`platformio.ini`, `CMakeLists.txt`, ...), scripts, docs, and local state
  such as device IDs or cached checkouts.
- **Run commands from inside the project folder.** Scripts use paths relative
  to their project, so `cd <project>` first.
- **Each project has a `README.md`** covering: hardware, toolchain install,
  build/flash steps, and how to deploy apps.
- Local-only files (`external/` checkouts, `.pio/`, `build/`, device IDs) are
  git-ignored repo-wide — see [`.gitignore`](.gitignore).

## Adding a new project

1. `mkdir <device>-<toolchain>` at the repo root.
2. Add a `README.md` following the sections above.
3. Add a row to the **Projects** table.
