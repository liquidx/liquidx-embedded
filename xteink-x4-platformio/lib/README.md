# Vendored libraries

Copied from [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
at commit `3f70f5dfc6254a6d5e308218841fe21106a9aa11` (MIT, see
[`CROSSPOINT_LICENSE`](CROSSPOINT_LICENSE)). Hardware support itself comes from
the `freeink-sdk` submodule, not from here.

| Library | Why it's here | Local changes |
| --- | --- | --- |
| `GfxRenderer` | Drawing, text, BMP decode, landscape orientation | none |
| `EpdFont` | Bitmap font rendering | Replaced the built-in fonts with IBM Plex Mono made by `scripts/fontconvert.py`; dropped font sources and CrossPoint's generator scripts |
| `hal` | Thin layer over the SDK used by `GfxRenderer` | Dropped `HalClock`, `HalFrontlight`, `HalSystem` (panic capture), `HalTiltSensor`; `HalGPIO.h` gained `beginAsyncInput()`/`popPress()` pass-throughs to the SDK's background button sampling |
| `Logging`, `Utf8`, `Memory` | Required by the above | none |
| `MiniBidi`, `InflateReader`, `uzlib` | Required by `GfxRenderer` text layout and compressed fonts | none |

The built-in fonts are regenerated from `fonts/IBMPlexMono/` by
`scripts/fonts.sh`. To add a size, add a line there, then include the header in
`EpdFont/builtinFonts/all.h` and register it in `src/Fonts.cpp`.
