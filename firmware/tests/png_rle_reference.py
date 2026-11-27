#!/usr/bin/env python3
"""Emit independent raw icon pixels for tests/rle565_icons.c."""
import struct
import sys
from pathlib import Path
from PIL import Image


def write_reference(source, destination, max_size):
    with Image.open(source) as source_image:
        image = source_image.convert("RGBA")
    ratio = max_size / max(image.size)
    if ratio < 1:
        image = image.resize((int(image.width * ratio), int(image.height * ratio)), Image.LANCZOS)
    with Path(destination).open("wb") as out:
        for y in range(image.height):
            for x in range(image.width):
                r, g, b, alpha = image.getpixel((x, y))
                if alpha < 128:
                    r = g = b = 0
                pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                out.write(struct.pack(">H", pixel))


if __name__ == "__main__":
    write_reference(sys.argv[1], sys.argv[2], int(sys.argv[3]))
