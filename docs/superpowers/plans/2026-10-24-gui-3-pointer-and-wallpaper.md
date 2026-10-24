# GUI-3 指针图标与壁纸配色实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把桌面鼠标光标从「纯白程序绘制的 12×18 箭头」换成用户提供的箭头图标（16×24，LVGL 图像资源），并把桌面壁纸底色从 `#FDFCFF` 换成新增令牌角色 `surface_dim` = `#D8D2E4`，同步宿主 / guest / QEMU 三层契约。

**Architecture:** 光标是一张**离线生成**的 ARGB8888 静态资源（`libgui/src/cursor_icon.c`），由 `tools/gen_cursor_icon.py` 从用户提供的 PNG 缩放产出，配套一份**人类可读的黄金图**（`libgui/assets/cursor-arrow-16x24.txt`）作为评审面；运行时用 `lv_draw_image` 1:1 绘制，**不**引入文件系统、PNG 解码器或任何新依赖。颜色（填充 `#1A1C1E` + 描边 `#FDFCFF`）**烘焙进像素**，不进令牌表 —— 光标是系统级控件，且「深主体 + 浅描边」是它在任意底色上都可见的前提。壁纸侧只做加法：新增一个 `surface_dim` 色彩角色 + 重排壁纸枚举，不改 `surface` 的值、不改 elevation 阶梯。

**Tech Stack:** C11、LVGL v9.5.0（`thirdpart/lvgl` submodule）、libgui、宿主 hosttests、Python 3 标准库（`zlib` + `struct` 自带 PNG 解码）、QEMU/QMP。

**Spec:** [2026-10-24-gui-3-pointer-and-wallpaper-design.md](../specs/2026-10-24-gui-3-pointer-and-wallpaper-design.md)。

**基线与分支：** 分支 `feature/gui-3-material-desktop`，worktree `/home/aagu/OS01/.worktrees/gui-3-material-desktop`，起点 `fe77e059`（spec 提交）。

## Global Constraints

- **光标是图像资源**：16×24 ARGB8888，热点在左上角 (0,0)。`lv_image_dsc_t.header.magic` 必须用 `LV_IMAGE_HEADER_MAGIC` 宏（0x19）—— `lv_image_src_get_type()` 靠源指针**首字节**判类型（`>= 0x20` 视为文件路径），填错会把这段内存当文件名去打开（spec §4.5）。
- **资源是 straight alpha，不是预乘**：LVGL 的 `LV_COLOR_FORMAT_ARGB8888` 与 `LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED` 是两个独立格式，前者按 straight alpha 混合；离屏画布是 XRGB8888，实际路径是 `lv_draw_sw_blend_to_rgb888.c` 的 `lv_color_24_24_mix()`（`mix >= LV_OPA_MAX` 直接拷贝源字节，`mix == 0` 原样保留目标像素）。
- **生成物不得手改**：`libgui/src/cursor_icon.c` 与 `libgui/assets/cursor-arrow-16x24.txt` 由 `tools/gen_cursor_icon.py` 产出；文件头必须写明生成器路径；`--check` 进 `make test-host`。
- **颜色不进令牌表**：`user/desktop` 与 `libgui` 的令牌体系里都**不得**新增光标颜色角色；光标两色只存在于生成器的调色板与 `gui_input_private.h` 的两个常量里。
- **令牌唯一来源**：桌面代码里不得出现裸颜色字面量与裸字号；`surface_dim` 只以 `c->surface_dim` 形式出现（静态审计强制）。
- **公共 API 不含 LVGL**：任何**已发布**头文件不得出现 `lv_*` 符号。`gui_cursor_icon.h` 是**内部**头，必须登记进 `qemutests/gui_build_audit.py` 的 `GUI_PRIVATE_HEADERS` —— 不登记等于「私有头不得进 sysroot」这条保证对它失效。
- **颜色比较口径**：LVGL 内部颜色值是 `0xFFRRGGBB`，令牌表与 `gui_platform_get_pixel()` 是 `0x00RRGGBB`；比较时掩掉最高字节。
- **界面文案必须 ASCII**：Montserrat 没有汉字字形；壁纸标签全部用英文。
- **断言必须钉字面量**：光标尺寸断言 16 / 24 **不得**写成 `GUI_INPUT_CURSOR_WIDTH` / `GUI_INPUT_CURSOR_HEIGHT`（那会退化成「实现和自己比」，spec §2.4）。
- **不扩张到**：深色主题 / 主题切换、指针自适应反色、光标的其他形态、运行时图片加载、壁纸持久化、GUI-4/5/6。

## Review Focus

以下六项是实施时最容易踩的坑；每一条都有对应的断言或检查，不以「人工看看」代替：

1. **`magic` 填错 → LVGL 把内存当文件名** —— 不是「画不出来」，是拿指针去 `open()`。对策：header 字段一律用 LVGL 宏，Task 2 第一步就是「资源能被解码」的最小宿主测试。
2. **抗锯齿让「像素 == 颜色」断言变脆** —— 尖端与边缘是半透明混合值。对策：等值断言只取 **alpha == 255** 的像素；半透明像素只做「不等于背景」的弱断言（spec §6.1 断言 10–12）。
3. **热点落空** —— 源图 44×56 里有左边距 8 px、上边距 7 px，箭头本体只占 28×43；不先裁剪，箭尖落不到输出画布的 (0,0)，热点指着空白。对策：生成器第 0 步裁剪；宿主断言「热点像素 alpha > 0」。
4. **生成物被手改** —— 改了 `cursor_icon.c` 而没改生成器，`--check` 形同虚设。对策：`--check` 进 `make test-host`，且工具测试断言 `--check` 对单字节改动**真的**会红。
5. **旧断言钉死旧值** —— `test_cursor_is_a_12x18_program_drawn_arrow_with_hotspot_at_zero` 钉 12/18 与 `0x00FFFFFF`，guest 探针钉 `PROBE_CURSOR_COLOR`，门禁钉 `MATERIAL_SURFACE_RGB = (253,252,255)`；不同步必红。对策：Task 2 / 4 / 5 各自的同步清单。
6. **新私有头没登记发布审计** —— `qemutests/gui_build_audit.py` 用 `GUI_PRIVATE_HEADERS` 名单检查「私有头不得出现在 sysroot 生成物里」；新头不登记 → 这条保证对它失效（既不报错也不拦）。对策：Task 2 同步名单。

## 验证入口

```bash
# 宿主 focused（每个 CASE 是一个 hosttests 子 make 目标）
make PROFILE=x86_64-clang test-gui-host CASE=input-lvgl
make PROFILE=x86_64-clang test-gui-host CASE=theme
make PROFILE=x86_64-clang test-gui-host CASE=desktop-shell

# 全量
make PROFILE=x86_64-clang test-host            # 含 tools/tests/test_cursor_icon.py
make PROFILE=x86_64-clang test-static          # 静态审计（含 gui_build_audit）
make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop
```

各 CASE 的 hosttests 子 make 目标命名 `test-gui-<case>`，由 root `os01_submake` 转发；**不得**从 shell 直接 `make -C hosttests`。

### 文件责任图

| 单元 | 文件与责任 |
|---|---|
| 光标源资产 | `libgui/assets/cursor-arrow.png`（用户提供的原图，仓库里唯一的二进制手工资产） |
| 光标生成器 | `tools/gen_cursor_icon.py`：PNG 解码 + 裁剪 + 分区 + 面积加权缩放 + 合成 + 产出 C 与黄金图；`--check` 校验生成物 |
| 光标生成物 | `libgui/src/cursor_icon.c`（ARGB8888 数组 + `lv_image_dsc_t`）、`libgui/assets/cursor-arrow-16x24.txt`（人类可读剪影） |
| 光标声明 | `libgui/internal/gui_cursor_icon.h`：`extern const lv_image_dsc_t gui_cursor_icon;`（内部头，带 LVGL 头） |
| 光标绘制 | `libgui/src/input_lvgl.c`：`cursor_draw_cb` 改 `lv_draw_image`；`libgui/internal/gui_input_private.h`：尺寸与两色常量 |
| 壁纸令牌 | `libgui/include/gui/gui_theme.h` + `libgui/src/theme.c`：`surface_dim` 角色 |
| 壁纸外壳 | `user/desktop/desktop_shell.{c,h}`：枚举重排、标签、默认值 |
| 构建 / 发布 | `libgui/Makefile`（编译 `cursor_icon.c`）、`hosttests/Makefile`（链接 `cursor_icon.o`）、`qemutests/gui_build_audit.py`（私有头名单）、`mk/components/run.mk`（工具测试接线） |
| 宿主测试 | `hosttests/cases/test_gui_input_lvgl.c`（光标）、`test_gui_theme.c`（令牌）、`test_desktop_shell.c`（壁纸）；`tools/tests/test_cursor_icon.py`（工具） |
| 端到端 | `qemutests/gui_desktop_test.py`（壁纸基色 + 光标可见性） |
| guest 探针 | `user/test_gui_input/test_gui_input.c`（`PROBE_CURSOR_*`） |
| 文档 | `docs/changelog.md`、`docs/gui/gui.md`、`docs/roadmap.md` |

---

### Task 1: 光标资源生成器、黄金图与生成物（纯工具层）

**Files:**
- Create: `libgui/assets/cursor-arrow.png`
- Create: `tools/gen_cursor_icon.py`
- Create: `tools/tests/test_cursor_icon.py`
- Create: `libgui/src/cursor_icon.c`（生成物）
- Create: `libgui/assets/cursor-arrow-16x24.txt`（生成物）
- Modify: `mk/components/run.mk`

**Interfaces:**
- Produces: `tools/gen_cursor_icon.py` 的模块级常量与函数（供 `tools/tests/test_cursor_icon.py` 导入）：
  ```python
  REPO_ROOT: str
  SRC_PNG: str        # <repo>/libgui/assets/cursor-arrow.png
  OUT_C: str          # <repo>/libgui/src/cursor_icon.c
  OUT_GOLDEN: str    # <repo>/libgui/assets/cursor-arrow-16x24.txt
  ICON_W = 16
  ICON_H = 24
  FILL_RGB = 0x1A1C1E
  OUTLINE_RGB = 0xFDFCFF

  def decode_png(path: str) -> Tuple[int, int, List[List[Tuple[int, int, int, int]]]]
  def body_bbox(pixels) -> Tuple[int, int, int, int]
  def crop(pixels, bbox) -> List[List[Tuple[int, int, int, int]]]
  def split_masks(body) -> Tuple[List[List[int]], List[List[int]]]
  def resize_coverage(mask, dst_w: int, dst_h: int) -> List[List[float]]
  def compose(fill_cov, outline_cov) -> List[int]          # 0xAARRGGBB
  def golden_rows(pixels: List[int]) -> List[str]
  def render_c_source(pixels: List[int]) -> str
  def render_golden(rows: List[str]) -> str
  def build_outputs() -> Tuple[str, str]                    # (c_source, golden)
  def diff_text(expected: str, actual: str, path: str) -> Optional[str]
  def main(argv: Optional[List[str]] = None) -> int
  ```

本任务**不碰** `libgui` 的编译单元 —— 它是一个纯离线工具链，可以独立跑、独立测、独立评审。

- [ ] **Step 1: 把源图放进仓库**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
mkdir -p libgui/assets
cp "/home/aagu/.minimax/v2/assets/2026/10/24/20-58-24-261-asset_20261024-205824-261_a32e81645cbf_a215ad33-clipboard-1792846688074.png" \
   libgui/assets/cursor-arrow.png
python3 - <<'PY'
import struct
data = open("libgui/assets/cursor-arrow.png", "rb").read()
assert data[:8] == b"\x89PNG\r\n\x1a\n"
w, h, depth, ctype = struct.unpack(">IIBB", data[16:26])
print("PNG", w, "x", h, "depth", depth, "color_type", ctype)
PY
```

Expected: `PNG 44 x 56 depth 8 color_type 6`。若不是 44×56 / 8bit / RGBA，说明源图不对 —— **停下报告**，不要改生成器去迁就。

- [ ] **Step 2: 写生成器 `tools/gen_cursor_icon.py`**

完整内容如下（逐字写入）：

```python
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
            pixels.append((alpha << 24) | (rgb << 8))
    return pixels


def golden_rows(pixels: List[int]) -> List[str]:
    """黄金图每格的字符：'.' 透明 / 'F' 填充 / 'O' 描边 / 'a' 抗锯齿。"""
    rows: List[str] = []
    for j in range(ICON_H):
        row: List[str] = []
        for i in range(ICON_W):
            value = pixels[j * ICON_W + i]
            alpha = (value >> 24) & 0xFF
            rgb = (value >> 8) & 0xFFFFFF
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
    lines.append("# %d 行 x %d 列，每格一个字符：" % (ICON_H, ICON_W))
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
```

- [ ] **Step 3: 跑生成器，产出生成物**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
python3 tools/gen_cursor_icon.py
cat libgui/assets/cursor-arrow-16x24.txt
```

Expected：黄金图 24 行 × 16 列；首行左端是箭尖（非 `.`）；能看到 `F`（黑色主体）与 `O`（白色描边）。**把黄金图贴给用户看**——它是这个图形的评审面。

- [ ] **Step 4: 写工具测试 `tools/tests/test_cursor_icon.py`**

```python
#!/usr/bin/env python3
"""test_cursor_icon.py — 光标资源生成器与黄金图的契约测试。

覆盖 spec §6.1 / §8.3：

* 生成物与生成器一致（``--check`` 必须绿）；
* ``--check`` 真的有判别力（改一个字节就红）；
* 黄金图的结构契约（24x16、字符集、箭尖在 (0,0)）；
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
                rgb = (value >> 8) & 0xFFFFFF
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
```

- [ ] **Step 5: 跑工具测试**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
python3 tools/tests/test_cursor_icon.py -v
```

Expected：6 个用例全绿（`test_check_mode_passes` / `test_check_detects_a_single_changed_byte` / `test_palette_is_the_documented_one` / `test_golden_shape_contract` / `test_c_source_pixels_match_the_golden` / `test_png_decoder_reads_a_handmade_image`）。

- [ ] **Step 6: 接线 `make test-host`**

在 `mk/components/run.mk` 的 `test-host` 目标里，`python3 tools/tests/test_kernel_headers.py` **之后**加一行：

```make
	python3 tools/tests/test_cursor_icon.py
```

- [ ] **Step 7: 验证并提交**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
python3 tools/gen_cursor_icon.py --check
make PROFILE=x86_64-clang test-host
git add libgui/assets/cursor-arrow.png libgui/assets/cursor-arrow-16x24.txt \
        libgui/src/cursor_icon.c tools/gen_cursor_icon.py \
        tools/tests/test_cursor_icon.py mk/components/run.mk
git commit -m "feat(gui): generate the arrow cursor image resource"
```

Expected：`--check` 打印 `cursor icon assets are up to date`；`make test-host` 绿。

---

### Task 2: 光标接入 libgui —— 绘制路径与宿主契约

**Files:**
- Create: `libgui/internal/gui_cursor_icon.h`
- Modify: `libgui/internal/gui_input_private.h`
- Modify: `libgui/src/input_lvgl.c`
- Modify: `libgui/Makefile`
- Modify: `hosttests/Makefile`
- Modify: `hosttests/cases/test_gui_input_lvgl.c`
- Modify: `qemutests/gui_build_audit.py`

**Interfaces:**
- Consumes: Task 1 的 `libgui/src/cursor_icon.c`（定义 `gui_cursor_icon`）与 `libgui/assets/cursor-arrow-16x24.txt`。
- Produces:
  - `libgui/internal/gui_cursor_icon.h`：
    ```c
    extern const lv_image_dsc_t gui_cursor_icon;
    ```
  - `libgui/internal/gui_input_private.h`：
    ```c
    #define GUI_INPUT_CURSOR_WIDTH          16
    #define GUI_INPUT_CURSOR_HEIGHT         24
    #define GUI_INPUT_CURSOR_FILL_COLOR     0x001A1C1Eu
    #define GUI_INPUT_CURSOR_OUTLINE_COLOR  0x00FDFCFFu
    ```
    （旧的 `GUI_INPUT_CURSOR_COLOR` **删除**，不得留成两个来源。）

- [ ] **Step 1: RED —— 先写「资源能被解码」的最小宿主测试**

在 `hosttests/cases/test_gui_input_lvgl.c` 顶部补 include（`lv_image_decoder.h` **不在** `<lvgl/lvgl.h>` 里，必须按仓库既有写法用路径 include）：

```c
#include <gui_cursor_icon.h>
#include <lvgl/src/draw/lv_image_decoder.h>
```

在光标用例区**新增**下面这个用例（先不实现，链接会缺 `gui_cursor_icon`）：

```c
TEST_FUNC(test_cursor_icon_resource_decodes_into_a_two_colour_image)
{
    lv_image_header_t header;
    const uint8_t *data;
    int fill_px = 0;
    int outline_px = 0;
    int clear_px = 0;
    int i;

    memset(&header, 0, sizeof(header));

    /* 资源必须能被 LVGL 解码：header 的 magic / cf / w / h / stride 全对。 */
    assert_eq(LV_RESULT_OK, lv_image_decoder_get_info(&gui_cursor_icon, &header));
    assert_eq(16, header.w);
    assert_eq(24, header.h);
    assert_eq(LV_COLOR_FORMAT_ARGB8888, header.cf);
    assert_eq(64, header.stride);

    data = gui_cursor_icon.data;
    assert_not_null(data);
    if (!data) {
        return;
    }

    /*
     * 资源里只允许两种 RGB（填充 / 描边）。ARGB8888 在内存里的字节序是
     * B, G, R, A。
     */
    for (i = 0; i < 16 * 24; i++) {
        uint32_t alpha = data[i * 4 + 3];
        uint32_t rgb = ((uint32_t)data[i * 4 + 2] << 16)
                     | ((uint32_t)data[i * 4 + 1] << 8)
                     | (uint32_t)data[i * 4 + 0];

        if (alpha == 0) {
            clear_px++;
            continue;
        }
        if (alpha != 255) {
            continue; /* 抗锯齿边缘不算实色 */
        }
        if (rgb == GUI_INPUT_CURSOR_FILL_COLOR) {
            fill_px++;
        } else if (rgb == GUI_INPUT_CURSOR_OUTLINE_COLOR) {
            outline_px++;
        } else {
            assert_true(0); /* 出现了第三种颜色 */
        }
    }

    /* 必须同时存在：实心填充、实心描边、完全透明。 */
    assert_true(fill_px > 0);
    assert_true(outline_px > 0);
    assert_true(clear_px > 0);

    /* 热点 (0,0) 必须有内容：箭尖不能整块透明。 */
    assert_true(data[3] > 0);
}
```

- [ ] **Step 2: 跑出 RED**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
make PROFILE=x86_64-clang test-gui-host CASE=input-lvgl
```

Expected：链接失败，报未定义引用 `gui_cursor_icon`（记下这条失败信息，它就是本任务的第一道 RED）。

- [ ] **Step 3: 新建 `libgui/internal/gui_cursor_icon.h`**

```c
#ifndef _GUI_CURSOR_ICON_H
#define _GUI_CURSOR_ICON_H

/*
 * gui_cursor_icon.h — 桌面鼠标光标的图像资源（内部头，不进 sysroot）。
 *
 * 资源是一张 16x24 的 ARGB8888 图像，由 tools/gen_cursor_icon.py 从
 * libgui/assets/cursor-arrow.png 离线生成到 libgui/src/cursor_icon.c。
 * 热点在左上角 (0,0)。
 *
 * 它带 lv_image_dsc_t，因此要包含 LVGL 头 —— 内部头允许，发布头不允许。
 */

#include <lvgl/lvgl.h>

extern const lv_image_dsc_t gui_cursor_icon;

#endif
```

- [ ] **Step 4: 改 `libgui/internal/gui_input_private.h`**

把光标常量段替换成：

```c
/* ── 光标与滚轮（spec §7.1） ────────────────────────────────────── */

/*
 * 箭头光标：一张 16x24 的 ARGB8888 图像资源（libgui/src/cursor_icon.c），
 * 热点在左上角 (0,0)。
 *
 * 光标是**图像资源**，不是程序画出来的形状：颜色烘焙在像素里（深色
 * 主体 + 浅色描边），下面这两个常量只是「资源里烘的是什么色」的声明，
 * 供宿主契约测试与 guest 探针比对。
 */
#define GUI_INPUT_CURSOR_WIDTH         16
#define GUI_INPUT_CURSOR_HEIGHT        24
/* 光标像素颜色（XRGB8888，与离屏画布的读回格式一致）。 */
#define GUI_INPUT_CURSOR_FILL_COLOR     0x001A1C1Eu
#define GUI_INPUT_CURSOR_OUTLINE_COLOR  0x00FDFCFFu
```

旧的 `#define GUI_INPUT_CURSOR_COLOR 0x00FFFFFFu` **删掉**。

- [ ] **Step 5: 改 `libgui/src/input_lvgl.c` 的绘制路径**

补 include（`lv_draw_image.h` 也不在 `<lvgl/lvgl.h>` 里）：

```c
#include <gui_cursor_icon.h>
#include <lvgl/src/draw/lv_draw_image.h>
```

把 `cursor_draw_cb` 整段替换成：

```c
/*
 * 光标的绘制回调。
 *
 * 光标是一张**图像资源**（libgui/src/cursor_icon.c），不是程序画出来的
 * 形状：16x24 的 ARGB8888，热点在左上角。lv_draw_image() 内部会重新取
 * 一次 header，所以资源 header 填错的后果是「静默不画」——这正是宿主
 * 契约测试必须证明「资源能被解码并画出来」的原因。
 *
 * 坐标是**绝对**坐标：lv_draw_image() 与 LVGL 自己的控件绘制一样使用
 * 屏幕坐标，因此这里用控件坐标（base）而不是局部偏移。
 */
static void cursor_draw_cb(lv_event_t *e)
{
    lv_obj_t *cursor = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_image_dsc_t dsc;
    lv_area_t base;

    lv_obj_get_coords(cursor, &base);

    lv_draw_image_dsc_init(&dsc);
    dsc.src = &gui_cursor_icon;
    dsc.base.layer = layer;
    lv_draw_image(layer, &dsc, &base);
}
```

`cursor_create` 里那条「光标固定 12×18」的注释改成「光标固定 16×24」；`lv_obj_set_size(cursor, GUI_INPUT_CURSOR_WIDTH, GUI_INPUT_CURSOR_HEIGHT)` **不用改**（用的是宏）。

- [ ] **Step 6: 编译接线**

`libgui/Makefile` 的 `PLATFORM_C_SOURCES` 改成：

```make
PLATFORM_C_SOURCES := $(LIBGUI_DIR)/src/platform.c $(LIBGUI_DIR)/src/tick.c \
                      $(LIBGUI_DIR)/src/input_ps2.c \
                      $(LIBGUI_DIR)/src/input.c $(LIBGUI_DIR)/src/input_lvgl.c \
                      $(LIBGUI_DIR)/src/input_focus.c \
                      $(LIBGUI_DIR)/src/cursor_icon.c
```

`hosttests/Makefile` 的 `GUI_INPUT_UNIT_OBJS` 改成：

```make
GUI_INPUT_UNIT_OBJS := $(GUI_INPUT_UNIT_DIR)/input.o \
                       $(GUI_INPUT_UNIT_DIR)/input_lvgl.o \
                       $(GUI_INPUT_UNIT_DIR)/input_focus.o \
                       $(GUI_INPUT_UNIT_DIR)/input_ps2.o \
                       $(GUI_INPUT_UNIT_DIR)/cursor_icon.o
```

- [ ] **Step 7: 同步发布审计**

`qemutests/gui_build_audit.py` 的 `GUI_PRIVATE_HEADERS` 加一项：

```python
GUI_PRIVATE_HEADERS = (
    "gui_input_private.h",
    "gui_platform_private.h",
    "gui_input_ps2.h",
    "gui_lvgl_private.h",
    "gui_tick.h",
    "gui_cursor_icon.h",
)
```

- [ ] **Step 8: 改写宿主光标用例**

把 `hosttests/cases/test_gui_input_lvgl.c` 里既有的
`test_cursor_is_a_12x18_program_drawn_arrow_with_hotspot_at_zero` **整条替换**为下面两条（新用例放在它前面）：

```c
TEST_FUNC(test_cursor_is_a_16x24_icon_with_hotspot_at_zero)
{
    fixture_t fx;
    lv_obj_t *cursor;
    lv_area_t area;
    lv_point_t point;
    lv_obj_t *hit;

    fixture_setup(&fx);

    /* 光标属于 input，并挂在该 display 的 system layer 上。 */
    cursor = fx.input->lvgl.cursor;
    assert_not_null(cursor);
    if (!cursor) {
        fixture_teardown(&fx);
        return;
    }
    assert_true(lv_obj_get_parent(cursor) == lv_display_get_layer_sys(fx.disp));

    /*
     * 没有鼠标源时（keyboard-only 降级）光标是隐藏的：键盘与焦点仍然可用。
     */
    assert_true(lv_obj_has_flag(cursor, LV_OBJ_FLAG_HIDDEN));

    /*
     * 移动指针：这一步同时让布局跑起来（控件的尺寸由布局写进坐标），
     * 并且第一个鼠标事件证明存在鼠标源 → 光标显示出来。
     */
    feed_mouse_one(&fx, 0, 10, 20, 0);
    step_at(&fx, 50);
    assert_false(lv_obj_has_flag(cursor, LV_OBJ_FLAG_HIDDEN));

    /*
     * 规格固定 16x24。
     *
     * 这里断言**字面量** 16 / 24，而不是 GUI_INPUT_CURSOR_WIDTH /
     * GUI_INPUT_CURSOR_HEIGHT：后者与实现用的是同一个宏，断言会退化成
     * "实现和自己比"，把光标改成 20 px 或 40x40 都不会被发现。
     */
    assert_eq(16, lv_obj_get_width(cursor));
    assert_eq(24, lv_obj_get_height(cursor));

    /* 热点 (0,0)：对象左上角就是指针位置。 */
    lv_obj_get_coords(cursor, &area);
    assert_eq(fx.input->lvgl.cursor_x, area.x1);
    assert_eq(fx.input->lvgl.cursor_y, area.y1);

    /* 命中透明：光标不拦截 hit-test。 */
    assert_false(lv_obj_has_flag(cursor, LV_OBJ_FLAG_CLICKABLE));
    point.x = fx.input->lvgl.cursor_x;
    point.y = fx.input->lvgl.cursor_y;
    hit = lv_indev_search_obj(lv_display_get_layer_sys(fx.disp), &point);
    assert_true(hit != cursor);

    fixture_teardown(&fx);
}

TEST_FUNC(test_cursor_icon_is_drawn_with_its_own_pixels)
{
    fixture_t fx;
    lv_obj_t *cursor;
    lv_area_t area;
    const uint8_t *data;
    int fill_x = -1;
    int fill_y = -1;
    int outline_x = -1;
    int outline_y = -1;
    int clear_x = -1;
    int clear_y = -1;
    int i;
    uint32_t background;

    fixture_setup(&fx);

    feed_mouse_one(&fx, 0, 10, 20, 0);
    step_at(&fx, 50);

    cursor = fx.input->lvgl.cursor;
    assert_not_null(cursor);
    if (!cursor) {
        fixture_teardown(&fx);
        return;
    }
    lv_obj_get_coords(cursor, &area);

    /*
     * 在资源里挑三个代表像素：完全不透明的填充 / 描边 / 完全透明。
     * 取样点必须落在**完全不透明**的像素上 —— ARGB8888 走 straight
     * alpha，半透明像素会被底色混合，不能做等值断言。
     */
    data = gui_cursor_icon.data;
    assert_not_null(data);
    if (!data) {
        fixture_teardown(&fx);
        return;
    }
    for (i = 0; i < 16 * 24; i++) {
        uint32_t alpha = data[i * 4 + 3];
        uint32_t rgb = ((uint32_t)data[i * 4 + 2] << 16)
                     | ((uint32_t)data[i * 4 + 1] << 8)
                     | (uint32_t)data[i * 4 + 0];

        if (alpha == 255 && rgb == GUI_INPUT_CURSOR_FILL_COLOR && fill_x < 0) {
            fill_x = i % 16;
            fill_y = i / 16;
        } else if (alpha == 255 && rgb == GUI_INPUT_CURSOR_OUTLINE_COLOR
                   && outline_x < 0) {
            outline_x = i % 16;
            outline_y = i / 16;
        } else if (alpha == 0 && clear_x < 0) {
            clear_x = i % 16;
            clear_y = i / 16;
        }
    }
    assert_true(fill_x >= 0);
    assert_true(outline_x >= 0);
    assert_true(clear_x >= 0);

    /* 画出来的实色像素必须与资源逐位一致（alpha == 255 时不做混合）。 */
    assert_eq(GUI_INPUT_CURSOR_FILL_COLOR,
              gui_platform_get_pixel(fx.platform,
                                     (uint32_t)(area.x1 + fill_x),
                                     (uint32_t)(area.y1 + fill_y)));
    assert_eq(GUI_INPUT_CURSOR_OUTLINE_COLOR,
              gui_platform_get_pixel(fx.platform,
                                     (uint32_t)(area.x1 + outline_x),
                                     (uint32_t)(area.y1 + outline_y)));

    /*
     * 完全透明的像素必须与光标**旁边**的背景一致。不断言底色具体是
     * 什么 —— 只断言「光标没有在这里画上东西」。
     */
    background = gui_platform_get_pixel(fx.platform,
                                        (uint32_t)(area.x1 + 40),
                                        (uint32_t)(area.y1 + 40));
    assert_eq(background,
              gui_platform_get_pixel(fx.platform,
                                     (uint32_t)(area.x1 + clear_x),
                                     (uint32_t)(area.y1 + clear_y)));

    fixture_teardown(&fx);
}
```

- [ ] **Step 9: 跑绿**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
make PROFILE=x86_64-clang test-gui-host CASE=input-lvgl
```

Expected：三个光标用例全绿（尺寸 16/24、资源可解码、绘制结果逐位一致）。

若 `test_cursor_icon_is_drawn_with_its_own_pixels` 报「填充像素颜色不等」：多半是 alpha != 255 的像素被当成了实色 —— 检查取样点选取逻辑（必须 `alpha == 255`）。若报「透明像素 != 背景」：多半是取到了光标盒外有控件的位置 —— 夹具没有控件，`area.x1 + 40 / area.y1 + 40` 处是屏幕底色。

- [ ] **Step 10: 验证并提交**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
git add libgui/internal/gui_cursor_icon.h libgui/internal/gui_input_private.h \
        libgui/src/input_lvgl.c libgui/Makefile hosttests/Makefile \
        hosttests/cases/test_gui_input_lvgl.c qemutests/gui_build_audit.py
git commit -m "feat(gui): draw the mouse cursor from an image resource"
```

Expected：`make test-host` 绿、`make test-static` 绿（发布头清单没变，私有头多了 `gui_cursor_icon.h`）。

---

### Task 3: `surface_dim` 色彩角色

**Files:**
- Modify: `libgui/include/gui/gui_theme.h`
- Modify: `libgui/src/theme.c`
- Modify: `hosttests/cases/test_gui_theme.c`

**Interfaces:**
- Produces: `gui_theme_colors_t` 新增字段
  ```c
  uint32_t surface_dim;   /* #D8D2E4 桌面壁纸底色 */
  ```
  位置：**紧跟 `surface` 之后**（语义相邻）。结构体是源码级公开数据，全仓库从源码重编，无 ABI 约束。`surface_dim` **不进入** elevation 阶梯，`gui_theme_elevation_surface()` 的序列断言一字不改。

- [ ] **Step 1: RED —— 先加断言**

`hosttests/cases/test_gui_theme.c`：

在 `test_color_roles_are_wellformed` 里，`assert_color_role(c->surface, "surface");` **之后**加一行：

```c
    assert_color_role(c->surface_dim, "surface_dim");
```

在 `test_elevation_surface_sequence` 之后**新增**一个用例（并在文件末尾的 `TEST_ENTRY` 列表里登记）：

```c
TEST_FUNC(test_surface_dim_is_a_distinct_wallpaper_role)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    assert_not_null(c);

    /* 壁纸角色必须是确定的取值。 */
    assert_eq(0x00D8D2E4u, c->surface_dim);

    /* 必须与页面底色可区分，否则壁纸与卡片撞色。 */
    assert_true(c->surface_dim != c->surface);
}
```

登记：在该文件的 `TEST_ENTRY(...)` 列表里加 `TEST_ENTRY(test_surface_dim_is_a_distinct_wallpaper_role),`。

- [ ] **Step 2: 跑出 RED**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
make PROFILE=x86_64-clang test-gui-host CASE=theme
```

Expected：编译失败 —— `gui_theme_colors_t` 没有 `surface_dim` 成员。

- [ ] **Step 3: 实现令牌**

`libgui/include/gui/gui_theme.h` 的结构体里：

```c
    uint32_t surface;                  /* #FDFCFF 页面底色 */
    uint32_t surface_dim;              /* #D8D2E4 桌面壁纸底色 */
    uint32_t on_surface;               /* #1A1C1E 页面底色之上的主要文字 */
```

`libgui/src/theme.c` 的初始化表里，紧跟 `.surface` 之后：

```c
    .surface                  = 0x00FDFCFFu,  /* #FDFCFF */
    .surface_dim              = 0x00D8D2E4u,  /* #D8D2E4 */
    .on_surface               = 0x001A1C1Eu,  /* #1A1C1E */
```

- [ ] **Step 4: 跑绿并提交**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
make PROFILE=x86_64-clang test-gui-host CASE=theme
make PROFILE=x86_64-clang test-static
git add libgui/include/gui/gui_theme.h libgui/src/theme.c hosttests/cases/test_gui_theme.c
git commit -m "feat(gui): add the surface_dim wallpaper colour role"
```

Expected：`CASE=theme` 绿；`make test-static` 绿（`surface_dim` 是令牌，不是裸字面量，不触发静态审计）。

---

### Task 4: 壁纸枚举、标签与默认值

**Files:**
- Modify: `user/desktop/desktop_shell.h`
- Modify: `user/desktop/desktop_shell.c`
- Modify: `hosttests/cases/test_desktop_shell.c`

**Interfaces:**
- Produces: `desktop_wallpaper_t` 重排为
  ```c
  typedef enum {
      DESKTOP_WALLPAPER_SURFACE_DIM = 0,   /* #D8D2E4，最深 */
      DESKTOP_WALLPAPER_SURFACE,            /* #FDFCFF，最浅 */
      DESKTOP_WALLPAPER_CONTAINER_LOW,      /* #F7F2FA */
      DESKTOP_WALLPAPER_CONTAINER_HIGH,     /* #ECE6F0 */
      DESKTOP_WALLPAPER_CONTAINER_HIGHEST   /* #E6E0E9 */
  } desktop_wallpaper_t;
  ```
  `CONTAINER_HIGHEST` 仍是**最后一个成员** → 计数表达式
  `(int)(DESKTOP_WALLPAPER_CONTAINER_HIGHEST + 1)` 一字不改仍等于 5。
  新的默认壁纸是 `DESKTOP_WALLPAPER_SURFACE_DIM`。

- [ ] **Step 1: RED —— 先改宿主断言**

`hosttests/cases/test_desktop_shell.c`：

① `test_home_surface_matches_the_token_table` 里，壁纸区那一行：

```c
    /* 壁纸区 */
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_WALLPAPER, &x, &y));
    assert_eq(c->surface, gui_platform_get_pixel(p, x, y));
```

改成：

```c
    /* 壁纸区 */
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_WALLPAPER, &x, &y));
    assert_eq(c->surface_dim, gui_platform_get_pixel(p, x, y));
```

② `test_setting_change_rebuilds_the_surface` 里，默认值与换壁纸那一段：

```c
    desktop_wallpaper_t wallpaper = DESKTOP_WALLPAPER_SURFACE_DIM;
    desktop_density_t density = DESKTOP_DENSITY_COMFORTABLE;
    bool clock_24h = false;

    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_WALLPAPER_SURFACE_DIM, wallpaper);
    assert_eq(DESKTOP_DENSITY_COMFORTABLE, density);
    assert_true(clock_24h);

    uint32_t x = 0;
    uint32_t y = 0;
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_WALLPAPER, &x, &y));
    assert_eq(gui_theme_colors()->surface_dim, gui_platform_get_pixel(p, x, y));

    /* 换壁纸：变更在主循环里执行，并立即重建当前表面。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_WALLPAPER);
    desktop_shell_apply_pending(shell);

    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_WALLPAPER_SURFACE, wallpaper);
    render(p);
    assert_eq(gui_theme_colors()->surface, gui_platform_get_pixel(p, x, y));

    /* 再换一次：走到 container_low。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_WALLPAPER);
    desktop_shell_apply_pending(shell);

    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_WALLPAPER_CONTAINER_LOW, wallpaper);
    render(p);
    assert_eq(gui_theme_colors()->surface_container_low,
              gui_platform_get_pixel(p, x, y));

    /* 换时钟格式：只改设置，不改壁纸。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_CLOCK_FORMAT);
    desktop_shell_apply_pending(shell);
    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_false(clock_24h);
    assert_eq(DESKTOP_WALLPAPER_CONTAINER_LOW, wallpaper);
```

（其余部分不动。）

- [ ] **Step 2: 跑出 RED**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
make PROFILE=x86_64-clang test-gui-host CASE=desktop-shell
```

Expected：编译失败 —— `DESKTOP_WALLPAPER_SURFACE_DIM` 未定义；即便编过，壁纸采样也不等于 `surface_dim`。

- [ ] **Step 3: 重排枚举**

`user/desktop/desktop_shell.h`：

```c
typedef enum {
    DESKTOP_WALLPAPER_SURFACE_DIM = 0, /* #D8D2E4，最深 */
    DESKTOP_WALLPAPER_SURFACE,          /* #FDFCFF，最浅 */
    DESKTOP_WALLPAPER_CONTAINER_LOW,    /* #F7F2FA */
    DESKTOP_WALLPAPER_CONTAINER_HIGH,   /* #ECE6F0 */
    DESKTOP_WALLPAPER_CONTAINER_HIGHEST /* #E6E0E9 */
} desktop_wallpaper_t;
```

- [ ] **Step 4: 改外壳实现**

`user/desktop/desktop_shell.c` 的 `wallpaper_color()`：

```c
static uint32_t wallpaper_color(const desktop_shell_t *shell)
{
    const gui_theme_colors_t *c = gui_theme_colors();

    switch (shell->wallpaper) {
    case DESKTOP_WALLPAPER_SURFACE:            return c->surface;
    case DESKTOP_WALLPAPER_CONTAINER_LOW:      return c->surface_container_low;
    case DESKTOP_WALLPAPER_CONTAINER_HIGH:     return c->surface_container_high;
    case DESKTOP_WALLPAPER_CONTAINER_HIGHEST:  return c->surface_container_highest;
    case DESKTOP_WALLPAPER_SURFACE_DIM:
    default:                                   return c->surface_dim;
    }
}
```

`wallpaper_label()`（标签按明度排成单调的一组，全部 ASCII）：

```c
static const char *wallpaper_label(const desktop_shell_t *shell)
{
    switch (shell->wallpaper) {
    case DESKTOP_WALLPAPER_SURFACE:            return "Standard";
    case DESKTOP_WALLPAPER_CONTAINER_LOW:      return "Light grey";
    case DESKTOP_WALLPAPER_CONTAINER_HIGH:     return "Grey";
    case DESKTOP_WALLPAPER_CONTAINER_HIGHEST:  return "Dark grey";
    case DESKTOP_WALLPAPER_SURFACE_DIM:
    default:                                   return "Deepest";
    }
}
```

默认值（`desktop_shell_create()` 里的初始化）：

```c
    shell->wallpaper = DESKTOP_WALLPAPER_SURFACE_DIM;
```

- [ ] **Step 5: 跑绿并提交**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
make PROFILE=x86_64-clang test-gui-host CASE=desktop-shell
make PROFILE=x86_64-clang test-static
git add user/desktop/desktop_shell.h user/desktop/desktop_shell.c \
        hosttests/cases/test_desktop_shell.c
git commit -m "feat(desktop): switch the wallpaper to the surface_dim role"
```

Expected：`CASE=desktop-shell` 绿；`make test-static` 绿（`user/desktop` 里只有 `c->surface_dim` 这样的令牌引用，没有裸色值）。

---

### Task 5: 契约同步 —— guest 探针与端到端门禁

**Files:**
- Modify: `user/test_gui_input/test_gui_input.c`
- Modify: `qemutests/gui_desktop_test.py`

**Interfaces:**
- Consumes: Task 2 的 `GUI_INPUT_CURSOR_FILL_COLOR` / `GUI_INPUT_CURSOR_OUTLINE_COLOR`；Task 3 的 `surface_dim` = `#D8D2E4`。
- Produces: 门禁新增 `cursor_visible(surface) -> bool`；壁纸基色常量改名 `WALLPAPER_RGB`。

- [ ] **Step 1: 同步 guest 探针**

`user/test_gui_input/test_gui_input.c`：

```c
/* Fixed colours: the focus outline and the pointer cursor are painted by
 * libgui with these exact values so the runner can sample them from a
 * screenshot.  The probe only asserts that the constants it inherits are
 * the expected ones. */
#define PROBE_OUTLINE_COLOR 0x00FF00u
#define PROBE_CURSOR_FILL_COLOR    0x001A1C1Eu
#define PROBE_CURSOR_OUTLINE_COLOR 0x00FDFCFFu
```

等值断言那处：

```c
    /* The fixed outline / cursor colours come from libgui. */
    if (GUI_INPUT_FOCUS_OUTLINE_COLOR != PROBE_OUTLINE_COLOR ||
        GUI_INPUT_CURSOR_FILL_COLOR != PROBE_CURSOR_FILL_COLOR ||
        GUI_INPUT_CURSOR_OUTLINE_COLOR != PROBE_CURSOR_OUTLINE_COLOR) {
        rc = fail(1, EINVAL, "focus_outline_or_cursor_color"); goto cleanup;
    }
```

- [ ] **Step 2: 同步门禁的壁纸基色与光标可见性**

`qemutests/gui_desktop_test.py`：

① 把基色常量整段替换：

```python
# 桌面壁纸底色：令牌表的 surface_dim 角色（#D8D2E4）。桌面接管显示后
# 会把整块屏幕刷成这个颜色 —— 这就是「桌面真的接管了显示」的判据。
WALLPAPER_RGB = (216, 210, 228)

# 画面里壁纸底色的占比下限（桌面接管后应接近 0.918）。
MIN_MATERIAL_FRACTION = 0.5

# 指针光标是 16x24，热点在左上角。门禁从不移动鼠标（只注入按键），所以
# 指针恒在屏幕正中 —— 而正中落在空白壁纸区（欢迎卡片在上方、启动器条
# 在下方）。
CURSOR_BOX_W = 16
CURSOR_BOX_H = 24
```

`MATERIAL_SURFACE_RGB` 在全文件里**只有 `material_fraction()` 一处引用**，改名不会漏掉别的调用点。`MIN_MATERIAL_FRACTION` 的名字**保留** —— 它表示的是「壁纸底色占画面的比例下限」，两个引用点（`:240` 的等待条件与 `:405` 的判定）都不需要改。

② `material_fraction()` 里的比较改成用 `WALLPAPER_RGB`：

```python
def material_fraction(surface: Surface, step: int = 4) -> float:
    """画面里壁纸底色的占比（抽样统计）。"""
    if surface.height == 0 or surface.width == 0:
        return 0.0
    hit = 0
    total = 0
    for y in range(0, surface.height, step):
        for x in range(0, surface.width, step):
            total += 1
            if surface.pixel(x, y) == WALLPAPER_RGB:
                hit += 1
    return hit / total if total else 0.0
```

③ **新增**光标可见性检查（放在 `material_fraction` 之后）：

```python
def cursor_visible(surface: Surface) -> bool:
    """屏幕中心的窗口里既有比壁纸更暗的像素，也有比壁纸更亮的像素。

    光标是深色箭头 + 浅色描边，所以它的窗口里必须两者都有。旧光标是
    纯白：窗口里没有更暗的像素 —— 这条判据因此能抓到回归。
    """
    x0 = surface.width // 2
    y0 = surface.height // 2
    darker = False
    lighter = False
    for y in range(y0, min(y0 + CURSOR_BOX_H, surface.height)):
        for x in range(x0, min(x0 + CURSOR_BOX_W, surface.width)):
            r, _g, b = surface.pixel(x, y)
            if r < WALLPAPER_RGB[0]:
                darker = True
            if b > WALLPAPER_RGB[2]:
                lighter = True
            if darker and lighter:
                return True
    return False
```

④ 在「桌面接管显示」那段的判定里，紧跟着 `material_fraction` 的检查之后加：

```python
            if not cursor_visible(home):
                return self._fail(
                    reasons, outcomes, case_name,
                    "the pointer cursor is not visible on the wallpaper",
                )
```

- [ ] **Step 3: 跑端到端门禁**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop
```

Expected：SMP 1 / SMP 2 都过。门禁存档里的 `02_desktop_home.ppm` 现在应当能**肉眼**看到深色箭头光标停在屏幕正中。

验证门禁真有判别力（**反向验证**）：把 `cursor_visible()` 里的 `darker` 判据临时改成恒假（`if False and r < ...`），重跑门禁，**必须失败**并报「the pointer cursor is not visible on the wallpaper」；改回来。

- [ ] **Step 4: 验证并提交**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
make PROFILE=x86_64-clang test-host
git add user/test_gui_input/test_gui_input.c qemutests/gui_desktop_test.py
git commit -m "test(gui): assert the new cursor and wallpaper on the real machine"
```

---

### Task 6: 文档收口与全量回归

**Files:**
- Modify: `docs/changelog.md`
- Modify: `docs/gui/gui.md`
- Modify: `docs/roadmap.md`

- [ ] **Step 1: 更新 `docs/changelog.md`**

在最新一节里追加一条（保持既有条目的格式与语言）：

```markdown
### GUI-3：桌面指针换成图标光标，壁纸改用 `surface_dim`

- 鼠标光标从「纯白程序绘制的 12×18 箭头」换成用户提供的箭头图标（16×24，
  LVGL 图像资源，热点在左上角）。纯白箭头在近白壁纸（`#FDFCFF`）上实际
  不可见；新光标自带深色主体 + 浅色描边，任意底色上都有对比度。
- 桌面壁纸底色改用新增的色彩角色 `surface_dim`（`#D8D2E4`），`surface`
  的值不变（它被 elevation 0 / 卡片 / 列表共用）。
- 光标资源由 `tools/gen_cursor_icon.py` 从 `libgui/assets/cursor-arrow.png`
  离线生成，黄金图 `libgui/assets/cursor-arrow-16x24.txt` 是它的可读评审面；
  `make test-host` 会校验生成物与生成器一致。
```

- [ ] **Step 2: 更新 `docs/gui/gui.md`**

在 GUI-3 的验证表里补一行，并加一小节说明光标与壁纸的现状：

```markdown
### 指针光标与壁纸

- 光标是一张 16×24 的 ARGB8888 **图像资源**（`libgui/src/cursor_icon.c`），
  由 `tools/gen_cursor_icon.py` 从 `libgui/assets/cursor-arrow.png` 离线
  生成；运行时用 `lv_draw_image` 1:1 绘制，不引入图片解码器。
- 光标颜色（填充 `#1A1C1E` + 描边 `#FDFCFF`）**烘焙在像素里**，不在令牌
  表里：光标是系统级控件，且「深主体 + 浅描边」是它在任意底色上都可见
  的前提。将来换主题若要改光标颜色，出路是把资源拆成填充 / 描边两张遮罩
  分别上色。
- 壁纸由新增的色彩角色 `surface_dim`（`#D8D2E4`）承载；`surface` 的值不变。
  壁纸设置项的标签按明度排成 `Standard / Light grey / Grey / Dark grey /
  Deepest`。
```

- [ ] **Step 3: 更新 `docs/roadmap.md`**

在 GUI-3 那一行补一句：

```markdown
- GUI-3：指针光标已换成 16×24 图像资源（深主体 + 浅描边，任意底色可见）；
  壁纸底色用新增的 `surface_dim` 角色。
```

- [ ] **Step 4: 全量回归**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop
```

Expected：全绿。

零孤儿 QEMU 检查（**不得**用 `pkill -f "qemu-system-x86_64.*disk.img"` —— 那会匹配到执行它的那条 bash 命令行本身并把 shell 杀掉）：

```bash
ps -eo pid,comm,args | awk '$2 ~ /^qemu/ && /disk\.img/ {print $1}'
```

Expected：**空输出**。

- [ ] **Step 5: 提交**

```bash
cd /home/aagu/OS01/.worktrees/gui-3-material-desktop
git add docs/changelog.md docs/gui/gui.md docs/roadmap.md
git commit -m "docs(gui): record the icon cursor and the surface_dim wallpaper"
```

---

## 完成判据

全部任务完成后，下列每一条都必须成立：

1. `make PROFILE=x86_64-clang test-host` 绿（含 `tools/tests/test_cursor_icon.py`）。
2. `make PROFILE=x86_64-clang test-static` 绿（发布头清单未变；`gui_cursor_icon.h` 已登记为私有头）。
3. `make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop` 绿（SMP 1/2），截图里壁纸是 `#D8D2E4`、屏幕中心能看到深色箭头光标。
4. `python3 tools/gen_cursor_icon.py --check` 打印 `cursor icon assets are up to date`。
5. `qemutests/gui_desktop_test.py` 的 `cursor_visible()` 反向验证：判据恒假时门禁必须红。
6. 宿主断言钉的是**字面量** 16 / 24 与 `0x00D8D2E4u`，不是实现里的宏。
7. 零孤儿 QEMU（`disk.img` 无人占用）。
8. 工作区干净，`git log` 里六个任务各自独立成 commit。
