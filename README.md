# Embedded projects

A single repo for my embedded / hardware projects. Each top-level folder is a
self-contained project for one platform + toolchain combination, with its own
README, build config, and helper scripts.

## Projects

| Folder | Device | Toolchain | Notes |
| --- | --- | --- | --- |
| [`m5stack-resident/`](m5stack-resident/) | M5StickS3 (ESP32-S3) | PlatformIO + [Resident](https://resident.inanimate.tech/) | Lua apps pushed over the Resident relay |
| [`xteink-x4-platformio/`](xteink-x4-platformio/) | Xteink X4 (ESP32-C3, e-paper) | PlatformIO + [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) | Landscape shell firmware: image viewer, Bluetooth receiver, settings |

## Shared protocols

Some folders aren't tied to one device: they define how devices talk to each
other, with implementations for several hosts. These use a plain name.

| Folder | What | Notes |
| --- | --- | --- |
| [`blit/`](blit/) | [blit protocol](blit/PROTOCOL.md): pixels to low-power displays over BLE, button presses back | JS host library, web demo, Chrome extension, simulated display. The X4 is a display. |

## Conventions

- **One folder per project**, named `<platform>-<toolchain>-<project>`
  (e.g. `m5stack-resident-clock`, `m5stack-arduino-sensor`,
  `rp2040-picosdk-blinky`, `esp32-idf-weather`).
  - `<platform>` is the device or board family (`m5stack`, `xteink-x4`, `rp2040`).
  - `<toolchain>` is the build system / SDK (`platformio`, `arduino`, `idf`).
  - `<project>` is a short name for what the project does.

  Including the project name means the same platform + toolchain can host
  several independent projects side by side, and a platform used with a
  different toolchain still gets its own folder.
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

1. `mkdir <platform>-<toolchain>-<project>` at the repo root.
2. Add a `README.md` following the sections above.
3. Add a row to the **Projects** table.
