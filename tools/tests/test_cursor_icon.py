#!/usr/bin/env python3
"""test_cursor_icon.py — 光标资源生成器与黄金图的契约测试。

覆盖 spec §6.1 / §8.3：

* 生成物与生成器一致（``--check`` 必须绿）；
* ``--check`` 真的有判别力：改一个像素后 ``--check`` 必须非零退出；
* 黄金图的结构契约（16 宽 x 24 高、字符集、箭尖在 (0, 0)）；
* C 源文件里的像素与黄金图逐格一致；
* PNG 解码器本身正确（自造最小 PNG 解一遍，含生产路径用的 Up 滤波）；
* 核心算法直接单测：``body_bbox`` / ``split_masks`` / ``resize_coverage``。
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


def make_png(width: int, height: int, rows: list, filter_type: int = 0) -> bytes:
    """造一个 8 bit RGBA 的最小 PNG。

    filter_type 0 = None（逐字节原样）；2 = Up（减去上一行同位置的字节）。
    生产路径上的源图 56 行全是 Up（filter 2），所以解码器必须走通它。
    """
    raw = bytearray()
    previous = bytearray(width * 4)
    for row in rows:
        line = bytearray()
        for (r, g, b, a) in row:
            line += bytes((r, g, b, a))
        raw.append(filter_type)
        if filter_type == 0:
            raw += line
        elif filter_type == 2:
            raw += bytes((line[i] - previous[i]) & 0xFF for i in range(len(line)))
        else:
            raise ValueError("unsupported filter type %d" % filter_type)
        previous = line

    def chunk(kind: bytes, body: bytes) -> bytes:
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(bytes(raw)))
            + chunk(b"IEND", b""))


def run_check_quiet() -> int:
    """跑一次 ``gen.main(["--check"])``，吞掉它的输出，只返回退出码。"""
    devnull = open(os.devnull, "w")
    saved_out, saved_err = sys.stdout, sys.stderr
    sys.stdout = sys.stderr = devnull
    try:
        return gen.main(["--check"])
    finally:
        sys.stdout, sys.stderr = saved_out, saved_err
        devnull.close()


class TestGeneratedAssets(unittest.TestCase):
    def test_check_mode_passes(self) -> None:
        self.assertEqual(0, gen.main(["--check"]))

    def test_check_mode_reports_a_tampered_artifact(self) -> None:
        """``--check`` 必须在生成物被改过时非零退出（端到端走 main()）。

        用临时目录里的副本代替仓库里的真生成物：测试只改副本，真生成物
        一个字节都不动。
        """
        c_source, golden = gen.build_outputs()
        with tempfile.TemporaryDirectory() as tmp:
            c_copy = os.path.join(tmp, "cursor_icon.c")
            golden_copy = os.path.join(tmp, "cursor-arrow-16x24.txt")
            with open(c_copy, "w", encoding="utf-8") as handle:
                handle.write(c_source)
            with open(golden_copy, "w", encoding="utf-8") as handle:
                handle.write(golden)

            # 让 --check 看临时副本，而不是仓库里的真生成物。
            saved = (gen.OUT_C, gen.OUT_GOLDEN)
            gen.OUT_C, gen.OUT_GOLDEN = c_copy, golden_copy
            try:
                # 副本与生成器一致：--check 必须绿。
                self.assertEqual(0, run_check_quiet())

                # 改一个像素：换成一个真实合法的 0xAARRGGBB 字面量。
                match = re.search(r"0x[0-9A-Fa-f]{8}u", c_source)
                self.assertIsNotNone(match)
                original = match.group(0)
                tampered = "0x00000000u"
                self.assertNotEqual(original, tampered)
                self.assertRegex(tampered, r"^0x[0-9A-Fa-f]{8}u$")
                with open(c_copy, "w", encoding="utf-8") as handle:
                    handle.write(c_source.replace(original, tampered, 1))

                # 副本与生成器不一致：--check 必须非零退出。
                self.assertEqual(1, run_check_quiet())
            finally:
                gen.OUT_C, gen.OUT_GOLDEN = saved

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

    def test_png_decoder_reads_an_up_filtered_image(self) -> None:
        """生产路径的源图 56 行全是 Up（filter 2），解码器必须走通它。"""
        blob = make_png(
            2, 2,
            [[(10, 20, 30, 255), (40, 50, 60, 128)],
             [(70, 80, 90, 200), (100, 110, 120, 0)]],
            filter_type=2)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "up.png")
            with open(path, "wb") as handle:
                handle.write(blob)
            width, height, pixels = gen.decode_png(path)
        self.assertEqual(2, width)
        self.assertEqual(2, height)
        self.assertEqual((10, 20, 30, 255), pixels[0][0])
        self.assertEqual((40, 50, 60, 128), pixels[0][1])
        self.assertEqual((70, 80, 90, 200), pixels[1][0])
        self.assertEqual((100, 110, 120, 0), pixels[1][1])


class TestCoreAlgorithm(unittest.TestCase):
    """核心算法的直接单测 —— 不依赖生成物快照。"""

    def test_resize_coverage_weights_the_source_area(self) -> None:
        # 2x2 掩膜四格里只有一格置位：缩到 1x1 时覆盖占比是 1/4。
        coverage = gen.resize_coverage([[0, 0], [0, 1]], 1, 1)
        self.assertEqual(1, len(coverage))
        self.assertEqual(1, len(coverage[0]))
        self.assertAlmostEqual(0.25, coverage[0][0])

    def test_resize_coverage_keeps_the_row_and_column_mapping(self) -> None:
        # 2x2 -> 2x2 恒等缩放：左上角的置位必须落在输出的 (0, 0)。
        coverage = gen.resize_coverage([[1, 0], [0, 0]], 2, 2)
        self.assertAlmostEqual(1.0, coverage[0][0])
        self.assertAlmostEqual(0.0, coverage[0][1])
        self.assertAlmostEqual(0.0, coverage[1][0])
        self.assertAlmostEqual(0.0, coverage[1][1])

    def test_body_bbox_excludes_alpha_128(self) -> None:
        # 规则是 alpha > 128：正好 128 的像素不属于本体。
        pixels = [[(0, 0, 0, 128), (0, 0, 0, 129), (0, 0, 0, 255)]]
        self.assertEqual((1, 0, 2, 0), gen.body_bbox(pixels))

    def test_split_masks_luma_split_boundary(self) -> None:
        # 亮度正好 128 属于描边区（规则是 luma < 128 才算填充）。
        body = [[(128, 128, 128, 255), (0, 0, 0, 255), (255, 255, 255, 255)]]
        fill, outline = gen.split_masks(body)
        self.assertEqual([[0, 1, 0]], fill)
        self.assertEqual([[1, 0, 1]], outline)

    def test_split_masks_excludes_transparent_pixels(self) -> None:
        # alpha == 128 的像素既不是填充也不是描边。
        body = [[(0, 0, 0, 128), (0, 0, 0, 255)]]
        fill, outline = gen.split_masks(body)
        self.assertEqual([[0, 1]], fill)
        self.assertEqual([[0, 0]], outline)


if __name__ == "__main__":
    unittest.main()
