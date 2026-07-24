#!/usr/bin/env python3
"""Bare serial probe for the BSC-16 usb-cold-boot question — no HIL harness.

Purpose: find out, from the wire, what the board actually prints when you do a
USB cold boot with the 18650 installed. It answers one question the failed
adapter runs cannot, because they fail closed before persisting a serial log:

  Does the firmware ever emit a "usb" power-source classification in this
  configuration, and if so, on what timeline?

This opens the port with DTR/RTS de-asserted (never resets the ESP32), tolerates
the port disappearing and re-enumerating across the cold boot, timestamps every
line from probe start, highlights every power-source line, and prints a verdict.

Usage:
  1. python3 tools/bsc16_serial_probe.py --port /dev/tty.usbmodemXXXX --seconds 20
     (find the port with `pio device list`; or pass --auto to pick the first
      USB CDC port that appears)
  2. Start the probe FIRST, then physically plug the FULL cable to cold-boot.
  3. Read the verdict at the end.

Exit code is 0 if a usb classification was captured, 2 if only battery/unknown
was seen, 3 if nothing was captured at all.
"""
from __future__ import annotations

import argparse
import re
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    raise SystemExit(1)

POWER_LINE_RE = re.compile(r"\[Battery\] Power (?:detection|source)", re.IGNORECASE)
USB_RE = re.compile(r"(?:classification=usb|source (?:changed|stable): usb)", re.IGNORECASE)
BATTERY_RE = re.compile(r"(?:classification=battery|source (?:changed|stable): battery)", re.IGNORECASE)


def pick_auto_port() -> str | None:
    ports = [p.device for p in list_ports.comports() if "usb" in (p.device or "").lower()
             or "usbmodem" in (p.device or "").lower() or "ACM" in (p.device or "")]
    return ports[0] if ports else None


def open_port(port: str) -> "serial.Serial | None":
    try:
        handle = serial.Serial(port=None, baudrate=115200, timeout=0.2)
        handle.dtr = False
        handle.rts = False
        handle.port = port
        handle.open()
        return handle
    except (OSError, serial.SerialException):
        return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial device, e.g. /dev/tty.usbmodem1101")
    parser.add_argument("--auto", action="store_true", help="auto-pick first USB CDC port")
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args(argv)

    port = args.port
    if not port and args.auto:
        port = pick_auto_port()
    if not port:
        print("No --port given and --auto found nothing. Run `pio device list`.", file=sys.stderr)
        return 1

    print(f"# probing {port} for {args.seconds:g}s — plug FULL now if you haven't", file=sys.stderr, flush=True)
    start = time.monotonic()
    deadline = start + args.seconds
    saw_usb = False
    saw_battery = False
    captured_any = False
    ever_opened = False
    handle: "serial.Serial | None" = None

    while time.monotonic() < deadline:
        if handle is None:
            handle = open_port(port)
            if handle is None:
                # Port not present yet (pre-enumeration / mid-reboot) — keep trying.
                time.sleep(0.25)
                continue
            ever_opened = True
        try:
            raw = handle.readline()
        except (OSError, serial.SerialException):
            # Re-enumeration across the cold boot — drop and reopen.
            try:
                handle.close()
            except Exception:
                pass
            handle = None
            continue
        if not raw:
            continue
        captured_any = True
        text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        elapsed = time.monotonic() - start
        marker = ""
        if POWER_LINE_RE.search(text):
            if USB_RE.search(text):
                saw_usb = True
                marker = "  <<< USB"
            elif BATTERY_RE.search(text):
                saw_battery = True
                marker = "  <<< battery"
            else:
                marker = "  <<< power"
        print(f"[+{elapsed:6.2f}s] {text}{marker}", flush=True)

    print("\n===== VERDICT =====", file=sys.stderr)
    if not ever_opened:
        available = [p.device for p in list_ports.comports()]
        print(f"COULD NOT OPEN {port!r} even once — this is a wrong/placeholder port path,\n"
              "not a board result. Nothing was measured about the firmware.\n"
              f"Ports currently visible: {available or '(none)'}\n"
              "Re-run with a real path (or --auto), e.g.:\n"
              f"  python3 tools/bsc16_serial_probe.py --port {(available or ['/dev/tty.usbmodemXXXX'])[0]} --seconds 20",
              file=sys.stderr)
        return 4
    if not captured_any:
        print("Port opened but delivered NO bytes in the window. Now this is a real signal:\n"
              "the classifier isn't the first suspect — the port went silent across the boot.\n"
              "Run `pio device list` right after the plug; the tty likely re-enumerated to a\n"
              "new path. Re-run the probe pointed at the post-plug path, or use --auto.",
              file=sys.stderr)
        return 3
    if saw_usb:
        print("usb classification WAS emitted (see '<<< USB' above). Cause is (A) capture:\n"
              "the firmware is right; the adapter isn't reading this port/timing. Fixable in\n"
              "the adapter — share the timestamps of the '<<< USB' lines.", file=sys.stderr)
        return 0
    if saw_battery:
        print("Only BATTERY/unknown was emitted — no usb line at all. Cause is (B): with the\n"
              "18650 installed the board never classifies this USB cold boot as usb, so the\n"
              "checkpoint is demanding an impossible line. The test premise is wrong for this\n"
              "configuration (try the same probe with the cell OUT to confirm).", file=sys.stderr)
        return 2
    print("Serial delivered lines but no power-source classification appeared in the window.\n"
          "Widen --seconds or check the firmware build actually has battery logging enabled.",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
