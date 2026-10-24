#!/usr/bin/env python3
"""gen_cursor_icon.py — 把箭头源图缩放成 libgui 的 ARGB8888 光标资源。

用法::

    python3 tools/gen_cursor_icon.py            # 重写生成物
    python3 tools/gen_cursor_icon.py --check    # 校验生成物与生成器一致（CI 用）

算法见 docs/superpowers/specs/2026-10-24-gui-3-pointer-and-wallpaper-design.md
§4.2：

    第 0 步  用 alpha > 128 的像素定出本体 bbox 并裁剪
    第 1 步  剔投影：alpha <= 128 的像素不参与剪影
    第 2 步  按整数亮度拆成填充区 / 描边区两张覆盖图
    第 3 步  两张覆盖图各自面积加权缩放到 16x24
    第 4 步  合成：alpha = 两区覆盖之和，颜色按覆盖占比归属

只依赖标准库（zlib + struct 自带 PNG 解码），不引入第三方依赖。
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib
from typing import List, Optional, Tuple

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_PNG = os.path.join(REPO_ROOT, "libgui", "assets", "cursor-arrow.png")
OUT_C = os.path.join(REPO_ROOT, "libgui", "src", "cursor_icon.c")
OUT_GOLDEN = os.path.join(REPO_ROOT, "libgui", "assets", "cursor-arrow-16x24.txt")

ICON_W = 16
ICON_H = 24

# 图标烘焙的调色板：填充 #1A1C1E / 描边 #FDFCFF。
# 一深一浅 —— 浅底显黑、深底显白（spec §4.3）。
FILL_RGB = 0x1A1C1E
OUTLINE_RGB = 0xFDFCFF

ALPHA_BODY = 128   # alpha > 128 属于箭头本体
LUMA_SPLIT = 128   # 亮度 < 128 属于填充区

Pixel = Tuple[int, int, int, int]
Mask = List[List[int]]
Coverage = List[List[float]]


class PngError(Exception):
    """PNG 解析失败。"""


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png(path: str) -> Tuple[int, int, List[List[Pixel]]]:
    """解一个 8 bit、非隔行的 PNG，返回 (w, h, pixels)。

    支持 color type 2（RGB）与 6（RGBA）。其它组合直接报错 —— 生成器的
    输入是固定的源图，不需要更宽的兼容面。
    """
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise PngError("not a PNG file: %s" % path)

    width = height = 0
    depth = color_type = 0
    idat = bytearray()
    offset = 8
    while offset + 8 <= len(data):
        (length,) = struct.unpack(">I", data[offset:offset + 4])
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, color_type, _c, _f, interlace = struct.unpack(
                ">IIBBBBB", body)
            if depth != 8:
                raise PngError("only 8 bit PNG is supported, got %d" % depth)
            if interlace != 0:
                raise PngError("interlaced PNG is not supported")
            if color_type not in (2, 6):
                raise PngError(
                    "only color type 2/6 is supported, got %d" % color_type)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break

    if width == 0:
        raise PngError("PNG without IHDR: %s" % path)

    channels = 3 if color_type == 2 else 4
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    if len(raw) != (stride + 1) * height:
        raise PngError("PNG payload size mismatch: %s" % path)

    pixels: List[List[Pixel]] = []
    previous = bytearray(stride)
    pos = 0
    for _ in range(height):
        filt = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if filt == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filt == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filt == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                up = previous[i]
                upleft = previous[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(left, up, upleft)) & 0xFF
        elif filt != 0:
            raise PngError("unknown PNG filter %d" % filt)
        row: List[Pixel] = []
        for x in range(width):
            base = x * channels
            alpha = line[base + 3] if channels == 4 else 255
            row.append((line[base], line[base + 1], line[base + 2], alpha))
        pixels.append(row)
        previous = line

    return width, height, pixels


def luma(rgb: Tuple[int, int, int]) -> int:
    """整数亮度（0..255）。"""
    r, g, b = rgb
    return (r * 299 + g * 587 + b * 114) // 1000


def body_bbox(pixels: List[List[Pixel]]) -> Tuple[int, int, int, int]:
    """本体 bbox：alpha > ALPHA_BODY 的像素集合的外接矩形。"""
    xs: List[int] = []
    ys: List[int] = []
    for y, row in enumerate(pixels):
        for x, pixel in enumerate(row):
            if pixel[3] > ALPHA_BODY:
                xs.append(x)
                ys.append(y)
    if not xs:
        raise PngError("the source PNG has no opaque body")
    return min(xs), min(ys), max(xs), max(ys)


def crop(pixels: List[List[Pixel]], bbox: Tuple[int, int, int, int]) -> List[List[Pixel]]:
    """按 bbox 裁剪。"""
    x0, y0, x1, y1 = bbox
    return [row[x0:x1 + 1] for row in pixels[y0:y1 + 1]]


def split_masks(body: List[List[Pixel]]) -> Tuple[Mask, Mask]:
    """拆成填充区 / 描边区两张 0/1 覆盖图。"""
    fill: Mask = []
    outline: Mask = []
    for row in body:
        frow: List[int] = []
        orow: List[int] = []
        for (r, g, b, a) in row:
            solid = a > ALPHA_BODY
            dark = luma((r, g, b)) < LUMA_SPLIT
            frow.append(1 if (solid and dark) else 0)
            orow.append(1 if (solid and not dark) else 0)
        fill.append(frow)
        outline.append(orow)
    return fill, outline


def resize_coverage(mask: Mask, dst_w: int, dst_h: int) -> Coverage:
    """面积加权缩放：每格返回覆盖占比 ∈ [0,1]。

    输出像素 (i, j) 的足迹是源图矩形
    [i*src_w/dst_w, (i+1)*src_w/dst_w) x [j*src_h/dst_h, (j+1)*src_h/dst_h)；
    落在足迹内的源像素按**重叠面积**加权累加。
    """
    src_h = len(mask)
    src_w = len(mask[0])
    out: Coverage = [[0.0] * dst_w for _ in range(dst_h)]
    step_x = src_w / dst_w
    step_y = src_h / dst_h
    for j in range(dst_h):
        y0 = j * step_y
        y1 = (j + 1) * step_y
        for i in range(dst_w):
            x0 = i * step_x
            x1 = (i + 1) * step_x
            area = 0.0
            for r in range(int(y0), min(int(y1) + 1, src_h)):
                oy = min(y1, r + 1) - max(y0, float(r))
                if oy <= 0.0:
                    continue
                mrow = mask[r]
                for c in range(int(x0), min(int(x1) + 1, src_w)):
                    if not mrow[c]:
                        continue
                    ox = min(x1, c + 1) - max(x0, float(c))
                    if ox <= 0.0:
                        continue
                    area += ox * oy
            out[j][i] = area / (step_x * step_y)
    return out


def compose(fill_cov: Coverage, outline_cov: Coverage) -> List[int]:
    """合成 16x24 的 ARGB8888 像素（0xAARRGGBB）。"""
    pixels: List[int] = []
    for j in range(ICON_H):
        for i in range(ICON_W):
            cf = fill_cov[j][i]
            co = outline_cov[j][i]
            alpha = int(round(min(1.0, cf + co) * 255))
            rgb = FILL_RGB if cf >= co else OUTLINE_RGB
            # 打包成 0xAARRGGBB：alpha 占最高字节，RGB 占低 24 位。
            # LVGL 的 ARGB8888 就是这个布局（内存字节序 B,G,R,A），
            # 生成物头注释里的 "uint32 值即 0xAARRGGBB" 也是这个约定。
            pixels.append((alpha << 24) | rgb)
    return pixels


def golden_rows(pixels: List[int]) -> List[str]:
    """黄金图每格的字符：'.' 透明 / 'F' 填充 / 'O' 描边 / 'a' 抗锯齿。"""
    rows: List[str] = []
    for j in range(ICON_H):
        row: List[str] = []
        for i in range(ICON_W):
            value = pixels[j * ICON_W + i]
            alpha = (value >> 24) & 0xFF
            rgb = value & 0xFFFFFF
            if alpha == 0:
                row.append(".")
            elif alpha == 255 and rgb == FILL_RGB:
                row.append("F")
            elif alpha == 255 and rgb == OUTLINE_RGB:
                row.append("O")
            else:
                row.append("a")
        rows.append("".join(row))
    return rows


def render_c_source(pixels: List[int]) -> str:
    """产出 libgui/src/cursor_icon.c 的全文。"""
    lines: List[str] = []
    lines.append("/*")
    lines.append(" * cursor_icon.c — 由 tools/gen_cursor_icon.py 生成，请勿手改。")
    lines.append(" *")
    lines.append(" * 源图：  libgui/assets/cursor-arrow.png")
    lines.append(" * 黄金图：libgui/assets/cursor-arrow-16x24.txt")
    lines.append(" * 规范：  docs/superpowers/specs/2026-10-24-gui-3-pointer-and-wallpaper-design.md §4")
    lines.append(" */")
    lines.append("")
    lines.append("#include <gui_cursor_icon.h>")
    lines.append("")
    lines.append("/* 16x24 ARGB8888，straight alpha（小端机上 uint32 值即 0xAARRGGBB）。 */")
    lines.append("static const uint32_t gui_cursor_pixels[%d * %d] = {" % (ICON_W, ICON_H))
    for j in range(ICON_H):
        row = pixels[j * ICON_W:(j + 1) * ICON_W]
        lines.append("    " + " ".join("0x%08Xu," % value for value in row))
    lines.append("};")
    lines.append("")
    lines.append("const lv_image_dsc_t gui_cursor_icon = {")
    lines.append("    .header = {")
    lines.append("        .magic  = LV_IMAGE_HEADER_MAGIC,")
    lines.append("        .cf     = LV_COLOR_FORMAT_ARGB8888,")
    lines.append("        .w      = %d," % ICON_W)
    lines.append("        .h      = %d," % ICON_H)
    lines.append("        .stride = %d * 4," % ICON_W)
    lines.append("    },")
    lines.append("    .data_size = sizeof(gui_cursor_pixels),")
    lines.append("    .data      = (const uint8_t *)gui_cursor_pixels,")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def render_golden(rows: List[str]) -> str:
    """产出 libgui/assets/cursor-arrow-16x24.txt 的全文。"""
    lines: List[str] = []
    lines.append("# cursor-arrow-16x24.txt — 由 tools/gen_cursor_icon.py 生成，请勿手改。")
    lines.append("#")
    lines.append("# 16 行 x 16 列，每格一个字符：")
    lines.append("#   .  完全透明（alpha == 0）")
    lines.append("#   F  完全不透明的填充色（#1A1C1E）")
    lines.append("#   O  完全不透明的描边色（#FDFCFF）")
    lines.append("#   a  抗锯齿边缘（0 < alpha < 255）")
    lines.append("#")
    lines.append("# 行号从上到下，列号从左到右；左上角 (0, 0) 是光标的热点。")
    lines.append("")
    for row in rows:
        lines.append(row)
    lines.append("")
    return "\n".join(lines)


def build_outputs() -> Tuple[str, str]:
    """跑完整条流水线，返回 (c_source, golden)。"""
    _width, _height, pixels = decode_png(SRC_PNG)
    body = crop(pixels, body_bbox(pixels))
    fill, outline = split_masks(body)
    fill_cov = resize_coverage(fill, ICON_W, ICON_H)
    outline_cov = resize_coverage(outline, ICON_W, ICON_H)
    composed = compose(fill_cov, outline_cov)
    return render_c_source(composed), render_golden(golden_rows(composed))


def diff_text(expected: str, actual: str, path: str) -> Optional[str]:
    """返回第一处差异的描述；一致返回 None。"""
    if expected == actual:
        return None
    exp_lines = expected.splitlines()
    act_lines = actual.splitlines()
    for index in range(max(len(exp_lines), len(act_lines))):
        exp = exp_lines[index] if index < len(exp_lines) else "<missing>"
        act = act_lines[index] if index < len(act_lines) else "<missing>"
        if exp != act:
            return "%s line %d:\n  expected: %s\n  actual:   %s" % (
                path, index + 1, exp, act)
    return "%s differs" % path


def _read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def _write_text(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="生成 libgui 的光标图像资源")
    parser.add_argument(
        "--check", action="store_true",
        help="只校验生成物与生成器是否一致（不一致则非零退出）")
    args = parser.parse_args(argv)

    c_source, golden = build_outputs()

    if args.check:
        problems: List[str] = []
        for path, expected in ((OUT_C, c_source), (OUT_GOLDEN, golden)):
            rel = os.path.relpath(path, REPO_ROOT)
            if not os.path.isfile(path):
                problems.append("%s is missing" % rel)
                continue
            issue = diff_text(expected, _read_text(path), rel)
            if issue:
                problems.append(issue)
        if problems:
            sys.stderr.write("cursor icon assets are out of date:\n")
            for problem in problems:
                sys.stderr.write("  %s\n" % problem)
            sys.stderr.write("run: python3 tools/gen_cursor_icon.py\n")
            return 1
        print("cursor icon assets are up to date")
        return 0

    _write_text(OUT_C, c_source)
    _write_text(OUT_GOLDEN, golden)
    print("wrote %s" % os.path.relpath(OUT_C, REPO_ROOT))
    print("wrote %s" % os.path.relpath(OUT_GOLDEN, REPO_ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
