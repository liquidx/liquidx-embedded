#!/usr/bin/env python3
"""Drive the device over USB serial for testing.

    python3 scripts/devctl.py key select down down   # inject key actions
    python3 scripts/devctl.py status                  # version, memory, battery, keys held
    python3 scripts/devctl.py shot out.png            # screenshot of what's on the LCD
    python3 scripts/devctl.py time                    # set the clock to this Mac's local time

Key names: back, select, up, down, chrome. Needs pyserial, and Pillow for shot.
"""
import datetime
import glob
import sys
import time

import serial


def port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("No /dev/cu.usbmodem* port. Is the device awake and connected?")
    return ports[0]


def shot(s, path):
    from PIL import Image

    s.timeout = 5
    s.reset_input_buffer()
    s.write(b"SCREENSHOT\n")
    while True:
        line = s.readline()
        if not line:
            sys.exit("No reply to SCREENSHOT")
        if line.startswith(b"SCREENSHOT_START:"):
            break
    size, width, height = (int(v) for v in line.decode().strip().split(":")[1:])
    raw = s.read(size)
    if len(raw) != size:
        sys.exit(f"Short read: {len(raw)} of {size} bytes")
    # rgb565, high byte first.
    image = Image.new("RGB", (width, height))
    pixels = []
    for i in range(0, size, 2):
        c = (raw[i] << 8) | raw[i + 1]
        r, g, b = c >> 11, (c >> 5) & 0x3F, c & 0x1F
        pixels.append((r * 255 // 31, g * 255 // 63, b * 255 // 31))
    image.putdata(pixels)
    image.save(path)
    print(f"{path}: {width}x{height}")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    with serial.Serial(port(), 115200, timeout=0.3) as s:
        if cmd == "key":
            for k in args:
                s.write(f"KEY {k}\n".encode())
                time.sleep(0.4)  # let the transition play
        elif cmd == "shot":
            shot(s, args[0] if args else "shot.png")
        elif cmd == "time":
            now = datetime.datetime.now()
            s.reset_input_buffer()
            s.write(f"TIME {now:%Y-%m-%d %H:%M:%S} {now.isoweekday() % 7}\n".encode())
            time.sleep(0.5)
            print(s.read(4096).decode(errors="replace").strip())
        elif cmd == "status":
            s.reset_input_buffer()
            s.write(b"STATUS\n")
            time.sleep(0.5)
            print(s.read(4096).decode(errors="replace"))
        else:
            sys.exit(__doc__)


if __name__ == "__main__":
    main()
