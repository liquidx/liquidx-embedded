# BLE cast protocol (v1)

How a sender (e.g. [`ble-cast`](../../ble-cast/)) pushes frames to the X4's
**Bluetooth** app over Bluetooth Low Energy. The device is the GATT server
(peripheral); it advertises only while the Bluetooth app is open.

All integers are little-endian.

## GATT layout

Service `b1ec0000-5f3a-4e62-9a47-0c3d8e5f2a10`, advertised with the device name
`X4-XXXX` (last two bytes of the Bluetooth address).

| Characteristic | UUID | Properties | Purpose |
| --- | --- | --- | --- |
| Info | `b1ec0001-5f3a-4e62-9a47-0c3d8e5f2a10` | read | JSON describing the device, see below |
| Control | `b1ec0002-5f3a-4e62-9a47-0c3d8e5f2a10` | write | Begin / commit / cancel a frame |
| Data | `b1ec0003-5f3a-4e62-9a47-0c3d8e5f2a10` | write without response | Frame payload chunks |
| Status | `b1ec0004-5f3a-4e62-9a47-0c3d8e5f2a10` | notify | Replies to control writes |

### Info

A UTF-8 JSON object, read fresh each time (it tracks the current screen):

```json
{"v":1,"name":"X4-1A2B","w":716,"h":480,"fullW":800,"fullH":480,
 "chunk":508,"window":16,"maxBytes":65536,"formats":[1],"frameSleep":false}
```

| Field | Meaning |
| --- | --- |
| `v` | Protocol version (1) |
| `w`, `h` | Pixels the frame area shows right now (the page card, or the whole screen with chrome hidden). Render to this size for a pixel-exact frame. |
| `fullW`, `fullH` | The whole panel |
| `chunk` | Max payload bytes per Data write (after its 4-byte offset): the negotiated MTU − 3, capped at the 512-byte attribute limit, − 4 |
| `window` | Flow control: the device acks after every `window` Data writes |
| `maxBytes` | Largest payload the device will accept |
| `formats` | Supported `format` codes |
| `frameSleep` | Whether Settings → Sleep between frames is on (see [Sleeping between frames](#sleeping-between-frames)) |

## Sending a frame

1. **Begin**: write a frame header to Control. Wait for a `ready` status.
2. **Data**: write the payload to Data in order, each write being
   `u32 offset` + up to `chunk` bytes. The device sends an `ack` after every
   `window` writes and after the last one; after each group of `window` writes,
   wait for its ack before sending more. (Writes without response have no flow
   control of their own: senders that run ahead lose chunks.)
3. **Commit**: write `0x02` to Control. Wait for `done` (or `error`).

`0x03` on Control cancels the frame in progress. A new Begin also discards any
partial frame.

### Frame header (Control, op `0x01`)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `op` = `0x01` (begin) |
| 1 | 1 | `version` = 1 |
| 2 | 1 | `format`, see below |
| 3 | 1 | `flags`: bit 0 `persist` (save to the SD card); other bits reserved, send 0 |
| 4 | 2 | `width` in pixels |
| 6 | 2 | `height` in pixels |
| 8 | 4 | `byteLength` of the payload |
| 12 | 4 | `crc32` of the payload (IEEE, as zlib's `crc32`) |
| 16 | 4 | `nextFrameSeconds`: when the sender expects to send the next frame; 0 = unknown |
| 20 | 1 | `nameLength` (0–64) |
| 21 | n | `name`: file name for a persisted frame, ASCII; optional |

### Formats

| Code | Name | Payload |
| --- | --- | --- |
| 1 | `raw1` | 1 bit per pixel, rows top to bottom, each row `ceil(width / 8)` bytes, most significant bit = leftmost pixel. **1 = black**, 0 = white. |

Other codes are reserved (e.g. 2-bit greyscale, deflate).

### Status (notify)

Six bytes: `u8 event`, `u8 code`, `u32 value`.

| Event | Meaning | `code` | `value` |
| --- | --- | --- | --- |
| 1 `ready` | Begin accepted, send Data | 0 | chunk size |
| 2 `done` | Frame received, checked and shown | 0 | seconds the device will sleep before listening again; 0 = staying connected |
| 3 `error` | Frame rejected; start again with Begin | error code | detail |
| 4 `ack` | Data received so far | 0 | bytes received |

| Error code | Meaning |
| --- | --- |
| 1 | Bad header or unknown op |
| 2 | Unsupported format or version |
| 3 | Payload too large, or doesn't match `width × height` |
| 4 | Busy: the previous frame is still being shown, retry shortly |
| 5 | Data out of order (`value` = expected offset) |
| 6 | Commit before all bytes arrived (`value` = bytes received) |
| 7 | CRC mismatch |
| 8 | Couldn't save to the SD card (the frame is still shown) |

## On the device

- The frame is drawn at native size, like the Images app: pinned top-left and
  cropped when larger than the frame area, centred when smaller.
- With `persist`, it's saved as a 1-bit BMP in `/images` (so the Images app
  lists it), named `name` (sanitised, `.bmp` added) or `ble-<date>-<time>.bmp`.
  The same name overwrites. Without it, the frame only lives in memory.
- The Bluetooth app shows the last frame while waiting for the next; after a
  restart, the last persisted one.

## Sleeping between frames

When Settings → **Sleep between frames** is on and a frame arrives with
`nextFrameSeconds` ≥ 30 (X4 Classic only), the device replies `done` with
`value` = the sleep length, disconnects, and deep-sleeps for
`nextFrameSeconds − 10` seconds with the frame left on screen. It then wakes
straight into the Bluetooth app and advertises again. The sender should
reconnect (retrying for ~30 s) when it sends the next frame.

With the setting off, `nextFrameSeconds` is ignored and the device stays
connected.
