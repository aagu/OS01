#!/usr/bin/env python3
"""test_cursor_icon.py — 光标资源生成器与黄金图的契约测试。

覆盖 spec §6.1 / §8.3：

* 生成物与生成器一致（``--check`` 必须绿）；
* ``--check`` 真的有判别力（改一个字节就红）；
* 黄金图的结构契约（16x16、字符集、箭尖在 (0,0)）；
* C 源文件里的像素与黄金图逐格一致；
* PNG 解码器本身正确（自造最小 PNG 解一遍）。
"""

from __future__ import annotations

import os
import re
import struct
import sys
import tempfile
import unittest
import zlib

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))

import gen_cursor_icon as gen  # noqa: E402


def load_golden_rows() -> list:
    rows = []
    with open(gen.OUT_GOLDEN, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            rows.append(line)
    return rows


def make_png(width: int, height: int, rows: list) -> bytes:
    """造一个 8 bit RGBA 的最小 PNG（filter 0）。"""
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for (r, g, b, a) in row:
            raw += bytes((r, g, b, a))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(bytes(raw)))
            + chunk(b"IEND", b""))


class TestGeneratedAssets(unittest.TestCase):
    def test_check_mode_passes(self) -> None:
        self.assertEqual(0, gen.main(["--check"]))

    def test_check_detects_a_single_changed_byte(self) -> None:
        expected, _golden = gen.build_outputs()
        match = re.search(r"0x[0-9A-Fa-f]{8}u", expected)
        self.assertIsNotNone(match)
        original = match.group(0)
        flipped = original[:-1] + ("0" if original[-1] != "0" else "1") + "u"
        broken = expected.replace(original, flipped, 1)
        self.assertNotEqual(expected, broken)
        self.assertIsNotNone(gen.diff_text(expected, broken, "cursor_icon.c"))

    def test_palette_is_the_documented_one(self) -> None:
        self.assertEqual(0x1A1C1E, gen.FILL_RGB)
        self.assertEqual(0xFDFCFF, gen.OUTLINE_RGB)

    def test_golden_shape_contract(self) -> None:
        rows = load_golden_rows()
        self.assertEqual(gen.ICON_H, len(rows))
        for row in rows:
            self.assertEqual(gen.ICON_W, len(row))
            for ch in row:
                self.assertIn(ch, ".FOa")
        self.assertTrue(any("F" in row for row in rows))
        self.assertTrue(any("O" in row for row in rows))
        # 热点 (0, 0) 必须有内容：箭尖不能整块透明。
        self.assertNotEqual(".", rows[0][0])

    def test_c_source_pixels_match_the_golden(self) -> None:
        with open(gen.OUT_C, "r", encoding="utf-8") as handle:
            text = handle.read()
        values = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{8})u", text)]
        self.assertEqual(gen.ICON_W * gen.ICON_H, len(values))
        rows = load_golden_rows()
        for j, row in enumerate(rows):
            for i, ch in enumerate(row):
                value = values[j * gen.ICON_W + i]
                alpha = (value >> 24) & 0xFF
                rgb = value & 0xFFFFFF
                if ch == ".":
                    self.assertEqual(0, alpha)
                elif ch == "F":
                    self.assertEqual(255, alpha)
                    self.assertEqual(gen.FILL_RGB, rgb)
                elif ch == "O":
                    self.assertEqual(255, alpha)
                    self.assertEqual(gen.OUTLINE_RGB, rgb)
                else:
                    self.assertTrue(0 < alpha < 255)

    def test_png_decoder_reads_a_handmade_image(self) -> None:
        blob = make_png(2, 1, [[(10, 20, 30, 255), (40, 50, 60, 128)]])
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "tiny.png")
            with open(path, "wb") as handle:
                handle.write(blob)
            width, height, pixels = gen.decode_png(path)
        self.assertEqual(2, width)
        self.assertEqual(1, height)
        self.assertEqual((10, 20, 30, 255), pixels[0][0])
        self.assertEqual((40, 50, 60, 128), pixels[0][1])


if __name__ == "__main__":
    unittest.main()
