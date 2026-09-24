#!/usr/bin/env python3
"""Drive the device over USB serial for testing.

    python3 scripts/devctl.py key select down down   # inject key actions
    python3 scripts/devctl.py status                  # input diagnostics
    python3 scripts/devctl.py shot out.png            # screenshot (see screenshot.py)
    python3 scripts/devctl.py time                    # set the RTC to this Mac's local time

Key names: back, select, up, down, chrome. Each key waits for the redraw.
"""
import glob
import datetime
import subprocess
import sys
import time

import serial


def port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("No /dev/cu.usbmodem* port. Is the device awake and connected?")
    return ports[0]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "shot":
        here = __file__.rsplit("/", 1)[0]
        sys.exit(subprocess.call([sys.executable, f"{here}/screenshot.py", *args]))
    with serial.Serial(port(), 115200, timeout=0.3) as s:
        if cmd == "key":
            for k in args:
                s.write(f"KEY {k}\n".encode())
                time.sleep(1.8)  # one fast refresh is ~0.5s, a half refresh ~1.3s
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
