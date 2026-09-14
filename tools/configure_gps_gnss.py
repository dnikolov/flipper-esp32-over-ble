"""Configure and persist NEO-M8N GNSS constellations over UART.

The tool defaults to a dry run. Use --apply to send UBX-CFG-GNSS and
UBX-CFG-CFG to the receiver. It is intended for one-time provisioning with
the GPS module connected directly to a 3.3 V USB-UART adapter, or with the
ESP32 TX path connected to the module RX pin.
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from typing import Iterable

import serial

UBX_SYNC = b"\xB5\x62"
UBX_CFG_CLASS = 0x06
UBX_CFG_GNSS_ID = 0x3E
UBX_CFG_CFG_ID = 0x09
UBX_ACK_CLASS = 0x05
UBX_ACK_ACK_ID = 0x01
UBX_ACK_NAK_ID = 0x00

# u-blox M8 GNSS identifiers and conservative channel allocations used by
# the NEO-M8 family configuration protocol.
GNSS_BLOCKS = {
    "gps": (0, 8, 16),
    "beidou": (3, 8, 14),
    "glonass": (6, 8, 14),
}
GNSS_NAMES = {0: "gps", 1: "sbas", 2: "galileo", 3: "beidou", 4: "imes", 5: "qzss", 6: "glonass"}


def ubx_checksum(message: bytes) -> bytes:
    ck_a = 0
    ck_b = 0
    for value in message:
        ck_a = (ck_a + value) & 0xFF
        ck_b = (ck_b + ck_a) & 0xFF
    return bytes((ck_a, ck_b))


def ubx_frame(message_class: int, message_id: int, payload: bytes) -> bytes:
    body = bytes((message_class, message_id)) + struct.pack("<H", len(payload)) + payload
    return UBX_SYNC + body + ubx_checksum(body)


def cfg_gnss_payload(constellations: Iterable[str]) -> bytes:
    selected = set(constellations)
    blocks = []
    for name in ("gps", "glonass", "beidou"):
        gnss_id, reserved_channels, max_channels = GNSS_BLOCKS[name]
        enabled = name in selected
        flags = 0x00010001 if enabled else 0x00010000
        blocks.append(struct.pack("<BBBBI", gnss_id, reserved_channels, max_channels, 0, flags))

    # The receiver allocates channels across the enabled blocks. The M8
    # CFG-GNSS protocol uses version 0 and a 32-channel hardware/use budget.
    return struct.pack("<BBBB", 0, 32, 32, len(blocks)) + b"".join(blocks)


def cfg_save_payload() -> bytes:
    # Save all current configuration items to the receiver's available
    # nonvolatile storage: battery-backed RAM, flash, and EEPROM where present.
    return struct.pack("<IIIB", 0, 0x0000FFFF, 0, 0x07)


def read_ubx_frame(port: serial.Serial, timeout: float) -> tuple[int, int, bytes] | None:
    deadline = time.monotonic() + timeout
    buffer = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(64)
        if chunk:
            buffer.extend(chunk)
        while len(buffer) >= 8:
            start = buffer.find(UBX_SYNC)
            if start < 0:
                buffer.clear()
                break
            if start:
                del buffer[:start]
            payload_length = buffer[4] | (buffer[5] << 8)
            frame_length = 8 + payload_length
            if len(buffer) < frame_length:
                break
            frame = bytes(buffer[:frame_length])
            del buffer[:frame_length]
            body = frame[2:-2]
            if ubx_checksum(body) != frame[-2:]:
                continue
            return frame[2], frame[3], frame[6:-2]
    return None


def wait_for_ack(port: serial.Serial, target_id: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        response = read_ubx_frame(port, min(remaining, 1.0))
        if response is None:
            continue
        message_class, message_id, payload = response
        if message_class != UBX_ACK_CLASS or message_id not in (UBX_ACK_ACK_ID, UBX_ACK_NAK_ID):
            continue
        if len(payload) == 2 and payload[0] == UBX_CFG_CLASS and payload[1] == target_id:
            return message_id == UBX_ACK_ACK_ID
    return False


def read_cfg_gnss(port: serial.Serial, timeout: float) -> bytes | None:
    port.write(ubx_frame(UBX_CFG_CLASS, UBX_CFG_GNSS_ID, b""))
    port.flush()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        response = read_ubx_frame(port, min(deadline - time.monotonic(), 1.0))
        if response is None:
            continue
        message_class, message_id, payload = response
        if message_class == UBX_CFG_CLASS and message_id == UBX_CFG_GNSS_ID:
            return payload
    return None


def print_cfg_gnss(payload: bytes) -> None:
    if len(payload) < 4:
        raise ValueError("CFG-GNSS response is shorter than its four-byte header")
    version, hardware_channels, used_channels, block_count = payload[:4]
    expected_length = 4 + block_count * 8
    if len(payload) != expected_length:
        raise ValueError(
            f"CFG-GNSS response length mismatch: expected {expected_length}, got {len(payload)}"
        )
    print(f"CFG-GNSS version={version}, hardware_channels={hardware_channels}, used_channels={used_channels}")
    for offset in range(4, len(payload), 8):
        gnss_id, reserved_channels, max_channels, _reserved, flags = struct.unpack(
            "<BBBBI", payload[offset:offset + 8]
        )
        name = GNSS_NAMES.get(gnss_id, f"id-{gnss_id}")
        state = "enabled" if flags & 1 else "disabled"
        print(
            f"  {name}: {state}, reserved_channels={reserved_channels}, "
            f"max_channels={max_channels}, flags=0x{flags:08x}"
        )


def parse_constellations(value: str) -> list[str]:
    names = [part.strip().lower() for part in value.split(",") if part.strip()]
    allowed = set(GNSS_BLOCKS)
    unknown = sorted(set(names) - allowed)
    if unknown:
        raise argparse.ArgumentTypeError(
            "unknown constellation(s): " + ", ".join(unknown) + "; use gps,glonass,beidou"
        )
    if "gps" not in names:
        raise argparse.ArgumentTypeError("gps must remain enabled")
    return list(dict.fromkeys(names))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM9", help="serial port (default: COM9)")
    parser.add_argument("--baud", type=int, default=9600, help="GPS UART baud (default: 9600)")
    parser.add_argument(
        "--constellations",
        type=parse_constellations,
        default=parse_constellations("gps,glonass,beidou"),
        help="comma-separated selection (default: gps,glonass,beidou)",
    )
    operation = parser.add_mutually_exclusive_group()
    operation.add_argument("--read", action="store_true", help="read and print the receiver's current CFG-GNSS")
    operation.add_argument("--apply", action="store_true", help="send and persist the configuration")
    parser.add_argument("--ack-timeout", type=float, default=3.0, help="ACK timeout in seconds")
    args = parser.parse_args()

    gnss_payload = cfg_gnss_payload(args.constellations)
    gnss_frame = ubx_frame(UBX_CFG_CLASS, UBX_CFG_GNSS_ID, gnss_payload)
    save_frame = ubx_frame(UBX_CFG_CLASS, UBX_CFG_CFG_ID, cfg_save_payload())

    print(f"Port: {args.port} at {args.baud} baud")
    print(f"Constellations: {', '.join(args.constellations)}")
    print(f"CFG-GNSS frame: {gnss_frame.hex(' ')}")
    print(f"CFG-CFG save frame: {save_frame.hex(' ')}")
    if args.read:
        try:
            with serial.Serial(args.port, args.baud, timeout=0.25) as port:
                port.reset_input_buffer()
                payload = read_cfg_gnss(port, args.ack_timeout)
                if payload is None:
                    print("ERROR: receiver did not return CFG-GNSS", file=sys.stderr)
                    return 1
                print_cfg_gnss(payload)
                return 0
        except (serial.SerialException, ValueError) as exc:
            print(f"ERROR: could not read CFG-GNSS: {exc}", file=sys.stderr)
            return 1
    if not args.apply:
        print("Dry run: no bytes sent. Re-run with --apply to configure and save the receiver.")
        return 0

    try:
        with serial.Serial(args.port, args.baud, timeout=0.25) as port:
            port.reset_input_buffer()
            port.write(gnss_frame)
            port.flush()
            if not wait_for_ack(port, UBX_CFG_GNSS_ID, args.ack_timeout):
                print("ERROR: receiver did not ACK CFG-GNSS", file=sys.stderr)
                return 1
            print("CFG-GNSS acknowledged")

            port.write(save_frame)
            port.flush()
            if not wait_for_ack(port, UBX_CFG_CFG_ID, args.ack_timeout):
                print("ERROR: receiver did not ACK CFG-CFG save", file=sys.stderr)
                return 1
            print("Configuration saved")
    except serial.SerialException as exc:
        print(f"ERROR: could not open or use {args.port}: {exc}", file=sys.stderr)
        return 1

    print("Power-cycle the GPS receiver and verify the constellations in u-center or UBX status output.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
