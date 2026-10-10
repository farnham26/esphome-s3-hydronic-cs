#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Reset the CoreS3 and stream its console from the first boot line.

The ESP32-S3 routes its console through USB-Serial-JTAG. That link does not
re-attach after flashing, so a plain `esphome logs` sits on a silent port while
the board runs fine. Resetting while already holding the port is the only way
to catch the banner and component setup.

Usage: cores3_bootlog.py <by-id path> [seconds]
"""

import sys
import time

import serial

RESET_PULSE_S = 0.2


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2

    device = sys.argv[1]
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0

    with serial.Serial(device, 115200, timeout=0.1) as port:
        # RTS drives EN on the USB-JTAG bridge: pulse it low to reset into the
        # application, not into the download stub.
        port.setDTR(False)
        port.setRTS(True)
        time.sleep(RESET_PULSE_S)
        port.setRTS(False)
        port.reset_input_buffer()

        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            chunk = port.read(4096)
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
