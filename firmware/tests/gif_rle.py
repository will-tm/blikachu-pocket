#!/usr/bin/env python3
"""Emit independent raw RGB565 reference pixels for tests/rle565.c."""

import struct
import sys
from pathlib import Path

from PIL import Image


def write_reference(source, destination):
    with Image.open(source) as image, Path(destination).open("wb") as out:
        canvas = Image.new("RGBA", image.size, (0, 0, 0, 255))
        while True:
            canvas.alpha_composite(image.convert("RGBA"))
            frame = canvas.convert("RGB")
            for y in range(frame.height):
                for x in range(frame.width):
                    r, g, b = frame.getpixel((x, y))
                    native = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                    # Wire bytes equal little-endian storage of the byte-swapped value.
                    out.write(struct.pack(">H", native))
            try:
                image.seek(image.tell() + 1)
            except EOFError:
                break


if __name__ == "__main__":
    write_reference(sys.argv[1], sys.argv[2])
