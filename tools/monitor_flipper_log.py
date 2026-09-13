#!/usr/bin/env python3
"""Passive Flipper CLI log monitor.

Opens the Flipper's CLI serial port (COM8 by default, 230400 baud per the pinned
Unleashed checkout's scripts/serial_cli.py), sends the `log` command to start
FURI_LOG streaming, then captures everything to a timestamped log file without
interpreting it live. Mirrors monitor_esp32_raw.py's shape for the ESP32 side.
"""

from __future__ import annotations

import argparse
import datetime as dt
import sys
import time
from pathlib import Path

import serial


def make_log_path(log_dir: Path, port: str) -> Path:
    stamp = dt.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    port_name = port.replace("/", "_").replace("\\", "_")
    return log_dir / f"flipper_{port_name}_{stamp}.log"


def start_log_stream(ser: serial.Serial) -> None:
    # Wake the CLI prompt, then start FURI_LOG streaming. The Flipper CLI ignores
    # a bare newline sent before it's ready, so a couple of retries with small
    # delays is more reliable than a single shot.
    ser.write(b"\r\n")
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write(b"log\r\n")
    time.sleep(0.3)


def monitor(port: str, baud: int, timeout: float, retry_delay: float, log_dir: Path) -> None:
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = make_log_path(log_dir, port)
    with open(log_path, "a", encoding="utf-8", errors="replace") as log_file:
        started_at = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        banner = f"Monitor started at {started_at} on {port}\n"
        log_file.write(banner)
        log_file.flush()
        print(f"LOG={log_path}", flush=True)

        while True:
            try:
                ser = serial.Serial(port, baud, timeout=timeout)
                print(f"OPEN={ser.name}", flush=True)
                start_log_stream(ser)
                while True:
                    chunk = ser.read(256)
                    if not chunk:
                        continue
                    timestamp = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                    text = chunk.decode("utf-8", "replace")
                    record = f"[{timestamp}] {text}"
                    log_file.write(record)
                    log_file.flush()
                    sys.stdout.write(record)
                    sys.stdout.flush()
            except serial.SerialException as exc:
                print(f"WAIT={exc}", flush=True)
                time.sleep(retry_delay)
            except KeyboardInterrupt:
                print("\nSTOPPED", flush=True)
                return
            except Exception as exc:  # pragma: no cover - defensive fallback
                print(f"ERR={type(exc).__name__}: {exc}", flush=True)
                time.sleep(retry_delay)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Passive Flipper CLI log monitor (sends `log`, captures the stream)."
    )
    parser.add_argument("--port", default="COM8", help="Serial port to monitor, e.g. COM8 or /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=230400, help="Baud rate")
    parser.add_argument("--timeout", type=float, default=0.25, help="Serial read timeout in seconds")
    parser.add_argument("--retry-delay", type=float, default=2.0, help="Retry delay when the port is locked or unavailable")
    parser.add_argument("--log-dir", default="logs", help="Directory to write timestamped raw logs to")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    monitor(args.port, args.baud, args.timeout, args.retry_delay, Path(args.log_dir))
