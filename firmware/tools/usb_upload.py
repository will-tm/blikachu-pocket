#!/usr/bin/env python3
"""Send user ROMs or libretro saves over the CRC-checked CDC upload command."""
import argparse
import re
import time
import zlib
from pathlib import Path

import serial


def wait_for(port, pattern, timeout):
    end = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < end:
        data.extend(port.read(port.in_waiting or 1))
        if re.search(rb"UPLOAD ERR -?\d+", data):
            raise RuntimeError(data.decode(errors="replace"))
        match = re.search(pattern, data)
        if match:
            return match
    raise TimeoutError(f"Waiting for {pattern!r}: {data[-256:]!r}")


def upload(port, path):
    name = path.name
    if any(c in name for c in '\\"\r\n'):
        raise ValueError(f"Unsupported filename: {name!r}")
    data = path.read_bytes()
    crc = zlib.crc32(data)
    port.reset_input_buffer()
    port.write(f'\rrecv "{name}" {len(data)} {crc:08x}\r'.encode())
    chunk = int(wait_for(port, rb"READY (\d+)\r?\n", 10).group(1))
    start = time.monotonic()
    for pos in range(0, len(data), chunk):
        end = min(pos + chunk, len(data))
        port.write(data[pos:end])
        if end < len(data):
            ack = int(wait_for(port, rb"RX (\d+)\r?\n", 15).group(1))
            if ack != end:
                raise RuntimeError(f"Expected ACK {end}, got {ack}")
    match = wait_for(port, rb"UPLOAD OK (\d+) ([0-9a-fA-F]{8})\r?\n", 180)
    if int(match.group(1)) != len(data) or int(match.group(2), 16) != crc:
        raise RuntimeError("Device CRC/size receipt mismatch")
    elapsed = time.monotonic() - start
    print(f"{name}: {len(data):,} bytes, CRC {crc:08x}, {elapsed:.1f}s", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()
    with serial.Serial(args.port, 115200, timeout=0.2, write_timeout=5) as port:
        for file in args.files:
            upload(port, file)


if __name__ == "__main__":
    main()
