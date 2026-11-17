#!/usr/bin/env python3
"""Persist ESP32 serial logs across USB disconnects."""

from __future__ import annotations

import argparse
import time
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports


def choose_port(requested: str | None) -> str | None:
    if requested:
        return requested
    ports = list(list_ports.comports())
    preferred = [
        port.device
        for port in ports
        if "303A:1001" in port.hwid.upper() or "ESP32" in port.description.upper()
    ]
    if len(preferred) == 1:
        return preferred[0]
    return None


def append(log_path: Path, message: str) -> None:
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    with log_path.open("a", encoding="utf-8") as handle:
        handle.write(f"[{timestamp}] {message}")
        if not message.endswith("\n"):
            handle.write("\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--log", type=Path, default=Path("esp32_serial.log"))
    args = parser.parse_args()

    append(args.log, "serial monitor starting")
    while True:
        port = choose_port(args.port)
        if port is None:
            append(args.log, "waiting for ESP32 USB serial device")
            time.sleep(1)
            continue
        try:
            with serial.Serial(port, args.baud, timeout=0.25) as connection:
                connection.dtr = False
                connection.rts = False
                append(args.log, f"opened {port} at {args.baud} 8N1")
                while True:
                    data = connection.read(connection.in_waiting or 1)
                    if data:
                        append(args.log, data.decode("utf-8", errors="replace"))
        except Exception as exc:
            append(args.log, f"serial disconnected: {exc}")
            time.sleep(1)


if __name__ == "__main__":
    main()
