"""Identify a raw bench's ESP32-S3 USB target without opening the port."""

import os
from typing import NamedTuple


BENCH_USB_SERIAL_ENV = "V1SIMPLE_BENCH_USB_SERIAL"
ESP32_S3_NATIVE_USB = (0x303A, 0x1001)


class UsbTarget(NamedTuple):
    port: str
    serial_number: str


def find_usb_target(preferred: str = "", *, serial_number: str | None = None) -> UsbTarget | None:
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise RuntimeError("bench USB target selection requires pyserial") from exc

    native = [item for item in list_ports.comports() if (item.vid, item.pid) == ESP32_S3_NATIVE_USB]
    candidates = [item for item in native
                  if (not preferred or os.path.realpath(item.device) == os.path.realpath(preferred))
                  and (serial_number is None or item.serial_number == serial_number)]
    if not candidates:
        return None
    if len(candidates) != 1:
        raise RuntimeError("multiple ESP32-S3 USB targets match; select one with DEVICE_PORT or --port")
    selected = candidates[0]
    if not selected.serial_number or sum(item.serial_number == selected.serial_number for item in native) != 1:
        raise RuntimeError("the bench requires a unique ESP32-S3 USB serial number")
    return UsbTarget(selected.device, selected.serial_number)


def require_usb_target(port: str, serial_number: str) -> None:
    if not port or not serial_number or find_usb_target(port, serial_number=serial_number) is None:
        raise RuntimeError("the pinned bench USB target no longer matches the selected port")
