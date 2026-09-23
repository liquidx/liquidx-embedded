#!/usr/bin/env python3
"""Grab the device framebuffer over USB serial and save it as a PNG.

    python3 scripts/screenshot.py [out.png] [--port /dev/cu.usbmodemXXXX]

The framebuffer is the panel-native 800x480, 1 bit per pixel, MSB first,
1 = white. In the shell's landscape orientation that is exactly what you see.
Needs pyserial and Pillow.
"""
import argparse
import glob
import sys
import time

import serial
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="screenshot.png")
    ap.add_argument("--port", default=None)
    args = ap.parse_args()

    port = args.port or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
    if not port:
        sys.exit("No /dev/cu.usbmodem* port. Is the device awake and connected?")

    with serial.Serial(port, 115200, timeout=2) as s:
        s.reset_input_buffer()
        s.write(b"SCREENSHOT\n")
        deadline = time.time() + 10
        while time.time() < deadline:
            line = s.readline().decode(errors="replace").strip()
            if line.startswith("SCREENSHOT_START:"):
                size, width, height = (int(v) for v in line.split(":")[1:4])
                break
        else:
            sys.exit("Device did not answer SCREENSHOT")

        data = b""
        while len(data) < size and time.time() < deadline:
            data += s.read(size - len(data))
        if len(data) != size:
            sys.exit(f"Short read: {len(data)}/{size} bytes")

    # PIL "1" mode raw data is also MSB-first with 1 = white.
    Image.frombytes("1", (width, height), data).save(args.out)
    print(f"Saved {width}x{height} to {args.out}")


if __name__ == "__main__":
    main()
