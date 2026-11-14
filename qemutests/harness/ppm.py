"""Shared P6 PPM parser and Surface container for QEMU screendumps."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Tuple


@dataclass
class Surface:
    width: int
    height: int
    pixels: bytes

    def pixel(self, x: int, y: int) -> Tuple[int, int, int]:
        idx = (y * self.width + x) * 3
        if idx + 2 >= len(self.pixels):
            return (0, 0, 0)
        return (self.pixels[idx], self.pixels[idx + 1], self.pixels[idx + 2])


def parse_ppm_bytes(data: bytes) -> tuple[int, int, bytes]:
    """Parse a P6 PPM byte buffer and return ``(width, height, rgb_bytes)``."""
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 PPM: %r" % data[:16])
    idx = 2
    fields: list[int] = []
    while len(fields) < 3:
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if idx < len(data) and data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx:idx + 1] not in (b"\n", b"\r"):
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx + 1].isspace():
            idx += 1
        if start == idx:
            raise ValueError("malformed PPM header")
        fields.append(int(data[start:idx]))
    idx += 1  # single whitespace after maxval
    w, h, maxval = fields
    if maxval != 255:
        raise ValueError("unexpected maxval %d" % maxval)
    body = data[idx:]
    if len(body) < w * h * 3:
        raise ValueError("truncated PPM: %d bytes for %dx%d" % (len(body), w, h))
    return w, h, body[:w * h * 3]


def parse_ppm(data: bytes) -> Surface:
    """Parse a P6 PPM byte buffer into a ``Surface``."""
    w, h, px = parse_ppm_bytes(data)
    return Surface(w, h, px)


def read_ppm(path: str | Path) -> tuple[int, int, bytes]:
    """Read a P6 PPM file from ``path`` and return ``(width, height, rgb_bytes)``."""
    with open(path, "rb") as fh:
        data = fh.read()
    return parse_ppm_bytes(data)
