# P3 GUI-3 指针图标与壁纸配色设计规范

> 日期：2026-10-24
> 状态：设计定稿（待实施）
> 依赖：GUI-3 ✅（Material 风格桌面，spec `2026-10-24-gui-3-material-desktop-design.md`）
> 后续：GUI-4 字体与资源、GUI-5 Window Server + compositor

---

## 1. 目标、范围与设计选择

### 1.1 完成条件

本次增量只做两件事：**把桌面鼠标光标换成用户提供的箭头图标**，**把桌面壁纸底色改成新增的 `surface_dim` 角色**。交付需满足以下可核验条件：

1. **光标换脸**：桌面指针不再是"纯白 + 程序绘制的镜像数字 4"，而是用户提供的箭头图标，尺寸 **16×24**，热点在左上角 (0,0)。
2. **光标在任何底色上可见**：光标自带**深色主体 + 浅色描边**两层，浅底显黑、深底显白；不依赖"壁纸恰好是某个颜色"才看得见。
3. **壁纸换色**：桌面默认壁纸底色从 `#FDFCFF` 改为 **`#D8D2E4`**（新增令牌角色 `surface_dim`），并作为设置项壁纸列表的**新默认项**。
4. **契约同步且不退化**：所有钉死旧光标（12×18 / `0x00FFFFFF`）与旧壁纸（`surface`）的断言必须同步更新；更新后的断言仍然钉**字面量**，不得退化成"实现和自己比"。
5. **证据可复跑**：宿主契约测试覆盖令牌表、光标资源与光标绘制结果；QEMU E2E 覆盖最终画面里的壁纸与光标。

### 1.2 不包含

| 不做 | 理由 / 归属 |
|------|------------|
| 深色主题 / 主题切换 | 沿用 GUI-3 决策 2：本期只有浅色 Material |
| 指针自适应反色（读底色像素再决定黑白） | 每次移动都要读 framebuffer，代价高且会闪烁；"深主体 + 浅描边"已经解决对比度问题 |
| 光标的其他形态（热点形状、按下态、拖拽态、忙态） | 本期只换外观，不改交互语义 |
| 运行时加载 PNG / 引入图片解码器 | 运行时没有文件系统解析器；资源离线生成后编译进静态库 |
| 壁纸设置持久化 | 壁纸只是 `shell->wallpaper` 字段，本来就不持久化（GUI-3 规范未承诺持久化） |
| 把光标颜色提升为设计令牌 | 见 §4.4：光标是系统级控件，且"深 + 浅"两色关系是自适应可见性的前提 |
| GUI-4 / GUI-5 / GUI-6 的任何内容 | 见 GUI-3 规范 §1.2 |

### 1.3 方案取舍

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 图标尺寸 | **16×24** | 与 X11 经典箭头可见部分的长宽比（0.63）接近；比原光标 12×18 大，细节（V 形缺口、细尾巴）在 12×18 下会糊掉。16×24 在 1440×900 的桌面上清晰又不喧宾夺主 |
| 2 | 实现方式 | **LVGL 图像资源**（ARGB8888 + `lv_draw_image`） | 像素艺术（`lv_draw_rect` 组合）无法表达斜边与缺口；运行时图片资源要引入文件系统与解码器。静态 ARGB8888 资源是唯一既能表达原图剪影、又不引入新依赖的做法 |
| 3 | 资源来源 | **用户提供的 PNG + 离线生成器** | 运行时不解码 PNG；`tools/gen_cursor_icon.py` 在开发期把 PNG 缩放成 ARGB8888 数组，产物 `libgui/src/cursor_icon.c` 提交进仓库。仓库已有二进制资产先例（`kernel/driver/font.psf`、`user/terminal/terminal_font.psf`） |
| 4 | 缩放算法 | **描边区 / 填充区分别面积加权** | 二值阈值缩放会把 V 形缺口填平、把尾巴变成方块；两区分开做覆盖积分就没有这个问题（§4.2） |
| 5 | 光标颜色 | **烘焙进资源，不进令牌表** | 光标是系统级控件，不是应用 UI；"深主体 + 浅描边"是两色自适应可见性的前提，做成单一令牌就会退化成"换底色又看不见"。代价与出路见 §4.4 |
| 6 | 壁纸色 | **新增 `surface_dim` 角色**，不改 `surface` 的值 | `surface` 被 elevation 0、卡片、列表共用，改它会牵动整套层次关系；新增一个只服务壁纸的角色是隔离做法 |
| 7 | 壁纸枚举 | **重排为 `SURFACE_DIM(0) / SURFACE / CONTAINER_LOW / CONTAINER_HIGH / CONTAINER_HIGHEST`** | `CONTAINER_HIGHEST` 留最后 → 计数表达式 `(int)(DESKTOP_WALLPAPER_CONTAINER_HIGHEST + 1)` 一字不改仍等于 5；`SURFACE_DIM` 取 0 → 天然成为新默认 |
| 8 | 验证主战场 | **光标像素断言放宿主，QEMU 只做门禁** | 沿用 GUI-3 决策 9：宿主离屏平台能逐像素读回，QEMU 只证明"真机上画面确实是这样" |

---

## 2. 现状与根因（已核实）

### 2.1 光标：形状与可见性

| 事实 | 证据 |
|------|------|
| 光标是**程序绘制**的：8 行递增的三角头 + 贴左 2×10 竖杆 | `libgui/src/input_lvgl.c:204-244` `cursor_draw_cb` |
| 固定 12×18，热点在左上角 (0,0) | `libgui/internal/gui_input_private.h:59-60` |
| 像素颜色是纯白 `0x00FFFFFF` | `libgui/internal/gui_input_private.h:62` |
| 桌面壁纸底色是 `surface` = `#FDFCFF`（近白） | `libgui/src/theme.c:39` |
| 两个近白相撞 → 光标实际不可见 | 门禁截图 `02_desktop_home.ppm`：`#FDFCFF` 占 91.8% 像素，光标的 56 个纯白像素混在其中 |
| 现状形状（三角 + 竖杆）看起来像阿拉伯数字 4 的左右镜像 | 目视比对 |

**不可见的根因是"光标颜色"与"壁纸颜色"都是近白**，不是形状本身难看。把光标换成自带深浅两色的图标即可根治。

### 2.2 门禁截图里的光标位置（已核实）

对 `build/x86_64-clang/logs/tests/gui-desktop/<ts>-<hash>/smp1/02_desktop_home.ppm`（1440×900）的像素分析：

| 事实 | 值 |
|------|-----|
| 光标绘制区域 | x 720..727、y 450..467（12×18 的预期形状） |
| 热点坐标 | (720, 450) = **屏幕正中**（1440/2, 900/2） |
| 周围 35×29 窗口内的像素 | 959 个壁纸色 `#FDFCFF` + 56 个纯白（光标），**没有第三种颜色** |
| 原因 | 门禁只注入键盘事件（`qmp_key_event`），**从不移动鼠标** → 指针停在初始位置（屏幕中心），且该位置落在**空白壁纸区**（欢迎卡片在上方、启动器条在下方） |

这组事实是 §6.3 光标可见性门禁的设计依据。

### 2.3 壁纸现状

| 事实 | 证据 |
|------|------|
| 壁纸只改屏幕底色，不持久化 | `user/desktop/desktop_shell.c:332-340` `apply_wallpaper` |
| 壁纸取值来自 `desktop_wallpaper_t` 枚举（4 选 1） | `user/desktop/desktop_shell.h:69-73` |
| 默认壁纸是 `DESKTOP_WALLPAPER_SURFACE` | `user/desktop/desktop_shell.c:1053` |
| 设置项循环：`(wallpaper + 1) % COUNT` | `user/desktop/desktop_shell.c:975-977` |
| `c->surface`（`#FDFCFF`）在桌面 UI 里**只**被壁纸用 | 控件底色一律走 `gui_theme_elevation_surface()` 或 `surface_container_low`（`desktop_shell.c:128,356,614,706,757`） |

### 2.4 必须补齐的边界

1. **仓库首次使用 LVGL 图像绘制路径**：`lv_draw_image` + `lv_image_dsc_t` 在本仓库从未被用过。header 的 `magic` 填错后果很严重：`lv_image_src_get_type()` 靠源指针的**首字节**判断类型（`>= 0x20` 视为文件路径，`>= 0x80` 视为符号，否则才是变量），而 `lv_image_dsc_t` 的首字节正是 `header.magic` —— 填错不是"画不出来"，而是**拿这段内存当文件名去打开**。
2. **断言不能比实现弱**：现有宿主用例 `test_cursor_is_a_12x18_program_drawn_arrow_with_hotspot_at_zero` 刻意断言字面量 12/18（注释明确写着"避免实现和自己比"）。改尺寸后必须改成钉死 **16/24**。
3. **抗锯齿让"像素 == 颜色"断言变脆**：尖端与边缘是半透明混合值。取样点必须落在**完全不透明**的像素上才能做等值断言。
4. **guest 探针硬编码了旧光标色**：`user/test_gui_input/test_gui_input.c:107` 的 `PROBE_CURSOR_COLOR` 与 `:1295` 的等值断言必须同步。
5. **门禁基色常量写死了旧壁纸色**：`qemutests/gui_desktop_test.py:87` 的 `MATERIAL_SURFACE_RGB = (253, 252, 255)` 必须同步。

---

## 3. 分层、文件与所有权

### 3.1 文件清单

**新增**

| 文件 | 职责 |
|------|------|
| `libgui/assets/cursor-arrow.png` | 用户提供的源图（44×56 RGBA），本次唯一的手工资产 |
| `libgui/assets/cursor-arrow-16x24.txt` | **黄金图**：16 行 × 16 字符的 ASCII 剪影，人工可读、可 diff，是图形的评审面 |
| `libgui/src/cursor_icon.c` | **生成物**：ARGB8888 像素数组 + `lv_image_dsc_t` |
| `libgui/internal/gui_cursor_icon.h` | `extern const lv_image_dsc_t gui_cursor_icon;` |
| `tools/gen_cursor_icon.py` | 离线生成器（自带 PNG 解码，无第三方依赖），带 `--check` |
| `tools/tests/test_cursor_icon.py` | 生成器与黄金图的单元测试 |

**修改**

| 文件 | 改动 |
|------|------|
| `libgui/internal/gui_input_private.h` | `GUI_INPUT_CURSOR_WIDTH/HEIGHT` 12×18 → 16×24；`GUI_INPUT_CURSOR_COLOR` 拆成 `GUI_INPUT_CURSOR_FILL_COLOR` / `GUI_INPUT_CURSOR_OUTLINE_COLOR` |
| `libgui/src/input_lvgl.c` | `cursor_draw_cb` 从"画 18 个矩形"改成"画一张图"；`cursor_create` 的尺寸改 16×24 |
| `libgui/Makefile` | `PLATFORM_C_SOURCES` 增加 `cursor_icon.c` |
| `qemutests/gui_build_audit.py` | `GUI_PRIVATE_HEADERS` 登记 `gui_cursor_icon.h`（见下） |
| `libgui/include/gui/gui_theme.h` | 令牌表新增 `surface_dim` 字段（紧跟 `surface`） |
| `libgui/src/theme.c` | `surface_dim` = `0x00D8D2E4u` |
| `user/desktop/desktop_shell.h` | 壁纸枚举重排 |
| `user/desktop/desktop_shell.c` | `wallpaper_color` / `wallpaper_label` / 默认值 |
| `hosttests/Makefile` | `GUI_INPUT_UNIT_OBJS` 增加 `cursor_icon.o` |
| `hosttests/cases/test_gui_theme.c` | 新增 `surface_dim` 断言 |
| `hosttests/cases/test_gui_input_lvgl.c` | 光标用例改写（16×24 + 资源 + 绘制结果） |
| `hosttests/cases/test_desktop_shell.c` | 壁纸默认与循环断言同步 |
| `user/test_gui_input/test_gui_input.c` | `PROBE_CURSOR_*` 同步 |
| `qemutests/gui_desktop_test.py` | 壁纸基色常量 + 光标可见性检查 |
| `mk/components/run.mk` | `test-host` 增加 `python3 tools/tests/test_cursor_icon.py` |
| `docs/changelog.md` / `docs/gui/gui.md` / `docs/roadmap.md` | 记录本次增量 |

### 3.2 所有权规则

| 对象 | 所有者 | 规则 |
|------|--------|------|
| `libgui/assets/cursor-arrow.png` | 手工资产 | 只允许替换为等价的新源图；替换后必须重跑生成器并提交新的黄金图 |
| `libgui/src/cursor_icon.c` | **生成物** | 不得手改；文件头写明"由 `tools/gen_cursor_icon.py` 生成，勿手改"。改了生成物而没改生成器 → `--check` 必红 |
| `libgui/assets/cursor-arrow-16x24.txt` | **生成物** | 同上；它是人工评审面，改算法 → 黄金图变 → diff 里一眼看出剪影变了 |
| `gui_cursor_icon` | libgui | `const` 全局，生命周期与程序相同；**不**随 `gui_platform_create/destroy` 创建销毁 |
| `surface_dim` 令牌 | libgui 令牌表 | 只读全局常量；桌面只能引用 `c->surface_dim`，不得写裸色值 |
| 壁纸取值 | `desktop_shell_t::wallpaper` | 外壳自己的字段，不持久化（既有行为） |

### 3.3 新内部头的可达性与发布卫生

`gui_cursor_icon.h` 放在 **`libgui/internal/`** 而不是 `libgui/src/`，因为它必须同时被两套编译看到：

| 消费者 | include 路径 | 结论 |
|--------|-------------|------|
| `libgui/src/input_lvgl.c` | `-I$(LIBGUI_DIR)/internal` | `<gui_cursor_icon.h>` 可达 ✓ |
| `hosttests/cases/test_gui_input_lvgl.c` | `-I$(TESTS_DIR)/libgui/include -I$(TESTS_DIR)/libgui/internal` | `<gui_cursor_icon.h>` 可达 ✓（`libgui/src` **不在**宿主测试的 include 路径上） |

它带 `lv_image_dsc_t`，因此会包含 `<lvgl/lvgl.h>` —— 这是内部头允许的（发布头才禁止 LVGL 符号）。

**发布卫生**：新增的内部头必须登记进 `qemutests/gui_build_audit.py` 的 `GUI_PRIVATE_HEADERS`。该审计用这张名单检查"任何私有头都不得出现在 sysroot 生成物里"；**新头不登记就等于这条保证对它失效**（既不报错也不拦）。`libgui/Makefile` 的 install 规则只拷贝 `gui.h` / `gui_platform.h` / `gui_input.h` / `gui_theme.h` 四个发布头，本次的头**不**进发布树，因此发布头清单不变、已发布头文件数量不变。

`libgui.a` 的符号审计只要求"必须导出这些符号"，不是"精确等于这组符号"，因此新增全局符号 `gui_cursor_icon` 不影响该审计。

---

## 4. 指针图标设计

### 4.1 几何与热点

| 项 | 值 |
|----|----|
| 画布 | **16×24** 像素 |
| 格式 | ARGB8888，**straight alpha**（非预乘） |
| 热点 | **(0,0)** —— 箭尖在左上角；对象左上角即指针位置（与旧光标一致，`lv_indev_set_cursor` 的语义不变） |
| 剪影 | X11 经典左箭头：尖点朝左上，头部向右下展开，尾部沿右下延伸，头尾之间有一个 V 形缺口 |
| 源图 | 44×56 RGBA，箭头本体 28×43（长宽比 0.65；目标 16/24 = 0.67；X11 可见部分 0.63） |

**热点与裁剪**：源图画布有左边距 8 px、上边距 7 px。生成器先把本体 bbox（28×43）裁出来再缩放（§4.2 第 0 步），因此**箭尖恰好落在输出画布的 (0,0)** —— 这正是热点必须所在的位置（LVGL 的 indev 光标以对象左上角为热点）。

**源图结构（实测）**：源图是 RGBA（color type 6，8 bit/channel，无隔行）。按"alpha > 128 属于本体"切出本体后，本体 bbox 为 x 8..35、y 7..49（28×43），其中填充（黑）390 px、描边（白）260 px。下图是本体区域的 ASCII 投影（`F` = 填充，`O` = 描边，`s` = alpha ≤ 128 的阴影/透明，` ` = 全透明；**行号是源图 y，列从源图 x=8 起算**）：

```
  7 OOOssssss
  8 OOOOssssss
  9 OOOOOssssss
 10 OOFOOOssssss
 11 OOFFOOOssssss
 12 OOFFFOOOssssss
 13 OOFFFFOOOssssss
 14 OOFFFFFOOOsssssss
 15 OOFFFFFFOOOOssssss
 16 OOFFFFFFFFOOOssssss
 17 OOFFFFFFFFFOOOssssss
 18 OOFFFFFFFFFFOOOssssss
 19 OOFFFFFFFFFFFOOOssssss
 20 OOFFFFFFFFFFFFOOOssssss
 21 OOFFFFFFFFFFFFFOOOssssss
 22 OOFFFFFFFFFFFFFFOOOssssss
 23 OOFFFFFFFFFFFFFFOOOssssss
 24 OOFFFFFFFFFFFFFFFFOOOssssss
 25 OOFFFFFFFFFFFFFFFFOOOssssss
 26 OOFFFFFFFFFFFFFFFFFFOOOOssss
 27 OOFFFFFFFFFFFFFFFFFFFOOOssss
 28 OOFFFFFFFFFFFFFFFFFFFFFOOOss
 29 OOFFFFFFFFFFFFFFFFFFFFFFOOOs
 30 OOFFFFFFFFFFFFFFFFFFFFFFFOOO
 31 OOFFFFFFFFFFFFFFFFFFFFOOOOOO
 32 OOFFFFFFFFFFFFFFOOOOOOOOOOOO
 33 OOFFFFFFFFFFFFFOOOOOOOOOssss
 34 OOFFFFFFFFFFFFFOOOFFssssssss
 35 OOFFFFFFFOOFFFFFOOFsssssssss
 36 OOFFFFFOOOOOFFFFOOOssssssss
 37 OOFFFFOOOFOOFFFFFOOsssssss
 38 OOFFOOOOssOOOFFFFOOFsssss
 39 OOFOOOOssssOOFFFFFOOsssss
 40 OOOOOssssssOOFFFFFOOsssss
 41 OOOOssssssssOOFFFFFOOsssss
 42 sOssssssssssOOFFFFFOOsssss
 43   sssssssssssOOFFFFOOOssss
 44 sssss  ssssssOOFFFFFOOssss
 45         ssssssOOFFFFFOOsss
 46          sssssOOFFFFFOOsss
 47           ssssOOOFOOOOOsss
 48           sssssOOOOOOsssss
 49            sssssOOOssssss
```

三点可读出的事实：① 剪影确实是 X11 经典左箭头（大三角头 + 右下竖杆 + 头杆之间的 V 形缺口）；② 白色描边是**真实的不透明描边**（左边缘连续两列 `O`），不是黑色形状的抗锯齿羽化；③ 竖杆的黑色填充约 5 px 宽，缩到 16×24 后约 2.9 px —— 这正是不能用二值缩放的原因（§4.2）。

### 4.2 生成算法

`tools/gen_cursor_icon.py` 的五步，全部确定性、可复现：

**第 0 步 — 定本体 bbox 并裁剪。**
源图画布 44×56，箭头本体只占 x 8..35、y 7..49（**左边距 8 px、上边距 7 px**）。用 `alpha > 128` 的像素集合算出 bbox，裁出 28×43 的本体画布。

必须裁剪，否则缩放后箭尖落不到画布左上角，**热点 (0,0) 会落在空白上**。

**第 1 步 — 剔投影。**
在裁剪后的画布里，`alpha > 128` 的像素属于箭头本体；其余（投影与最外层半透明羽化）不参与剪影构造。

**第 2 步 — 拆描边区与填充区。**
在第 1 步留下的像素里按**整数亮度**分两类（亮度 `L = (r*299 + g*587 + b*114) / 1000`，整数除法；源图填充为黑、描边为白）：
- `L < 128` → **填充区**
- `L ≥ 128` → **描边区**

得到两张 28×43 的覆盖图。

**第 3 步 — 各自面积加权缩放到 16×24。**
两张覆盖图（此时是裁剪后的 28×43）**分别**做面积加权重采样：

- 输出像素 `(i, j)` 的足迹是源图矩形 `[i*28/16, (i+1)*28/16) × [j*43/24, (j+1)*43/24)`（浮点边界，不做整数取整）
- 每个落在足迹内的源像素，按它与足迹的**重叠面积**加权累加到该输出像素
- 覆盖占比 = 累加面积 / 足迹面积

得到两张 16×24 的覆盖图：
- `cf` = 填充区覆盖占比 ∈ [0,1]
- `co` = 描边区覆盖占比 ∈ [0,1]

**缩放是各向异性的**：横向因子 16/28 = 0.571、纵向因子 24/43 = 0.558，箭头被纵向压扁约 2.3%。这是"固定 16×24 画布"与"保持原长宽比"之间取前者必然的代价，肉眼不可辨；不采用留白或裁边的等比方案。

**第 4 步 — 合成。**
- `alpha = round(255 × min(1, cf + co))`
- 颜色归属：**占比大者胜出**（`cf ≥ co` → 填充色 `#1A1C1E`，否则描边色 `#FDFCFF`）

输出图像因此只有 **两种 RGB**（填充 / 描边），边缘过渡全部由 alpha 表达。

**为什么不二值缩放**（以下均据源图实测，尺寸为裁剪后 28×43 本体画布上的像素）：

- **V 形缺口**由两部分组成：缺口**边缘的白色描边**（alpha > 128，属于描边区）与缺口**中间的间隙**（alpha ≤ 128，是穿过缺口的阴影/透明，被第 1 步剔除）。二值阈值缩放会把间隙按"有/无"判定、由邻近实心像素填掉 → 缺口糊成一片实心；覆盖积分则让间隙处 `cf = co = 0` → alpha = 0 → **间隙保留为透明**，描边部分按真实覆盖占比半透明呈现。
- **竖杆（尾巴）**的黑色填充约 5 px 宽（本体画布第 33~39 行、列 13~19 一带），缩到 16×24 后约 **2.9 px**（`5 × 16/28`）。二值化按"有/无"采样，宽度会失真成方块；覆盖积分保留它真实的亚像素宽度 → 半透明细杆。
- **箭头头部**最宽约 23 px（本体第 23 行，列 2~24），缩后约 **13 px**（占满 16 列里的 13 列），主体在两种算法下都能保住；分区分采样只是为了同时照顾缺口与竖杆这两处细节。

### 4.3 颜色

| 角色 | 值 | 说明 |
|------|----|------|
| 填充（主体） | `#1A1C1E` | 与令牌 `on_surface` 同值 |
| 描边（外圈） | `#FDFCFF` | 与令牌 `surface` 同值 |

两色一深一浅，是"浅底显黑、深底显白"的前提。

### 4.4 为什么光标颜色不进令牌表

令牌表的唯一消费者是**应用侧 UI**（`gui_theme.h` 是给应用用的公开数据）。光标是系统级控件（LVGL indev cursor），由 libgui 自己画，不该出现在应用的 UI 语言里。

更关键的理由是**语义**：光标必须**同时**携带"深"与"浅"两种颜色（一深一浅的两层），才能在任意底色上都有对比度——浅底显黑、深底显白。若把光标做成令牌（单一颜色），换一个底色就会重新出现"看不见"的问题；要真正自适应就得读底色像素，代价高且会闪烁（§1.2 已排除）。

**代价**：将来换主题不能改光标颜色。
**出路**（本期不实现，备查）：把资源拆成"填充遮罩 + 描边遮罩"两张 A8 图，绘制时按当前主题分别上色再叠加。

### 4.5 资源落地

`libgui/internal/gui_cursor_icon.h`：

```c
#ifndef _GUI_CURSOR_ICON_H
#define _GUI_CURSOR_ICON_H

#include <lvgl/lvgl.h>

/* 16×24 的箭头光标资源（ARGB8888）。热点在左上角 (0,0)。 */
extern const lv_image_dsc_t gui_cursor_icon;

#endif
```

`libgui/src/cursor_icon.c`（生成物）：

```c
#include <lvgl/lvgl.h>

/* 16×24 ARGB8888，straight alpha（小端机上 uint32 值即 0xAARRGGBB）。 */
static const uint32_t gui_cursor_pixels[16 * 24] = { ... };

const lv_image_dsc_t gui_cursor_icon = {
    .header = {
        .magic  = LV_IMAGE_HEADER_MAGIC,      /* 0x19 —— 必须小于 0x20 */
        .cf     = LV_COLOR_FORMAT_ARGB8888,
        .w      = 16,
        .h      = 24,
        .stride = 16 * 4,                      /* 64 */
    },
    .data_size = sizeof(gui_cursor_pixels),    /* 1536 */
    .data      = (const uint8_t *)gui_cursor_pixels,
};
```

**`magic` 必须精确取 `LV_IMAGE_HEADER_MAGIC`（0x19），不得写字面量**：`lv_image_src_get_type()` 靠源指针的首字节判断类型（`>= 0x20` 视为文件路径，`>= 0x80` 视为符号，否则才是变量）。`lv_image_dsc_t` 的首字节就是 `header.magic`，所以 magic 一旦填错，LVGL 会把这段内存当文件名去打开。

**straight alpha 的依据**：LVGL 的 `LV_COLOR_FORMAT_ARGB8888` 与 `LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED` 是两个独立格式；前者按 straight alpha 混合。离屏画布是 `LV_COLOR_FORMAT_XRGB8888`，因此实际路径是 `thirdpart/lvgl/src/draw/sw/blend/lv_draw_sw_blend_to_rgb888.c` 的 `argb8888_image_blend()` → `lv_color_24_24_mix()`：`mix == 0` 时目标像素原样不动，`mix >= LV_OPA_MAX` 时直接拷贝源字节，中间按 straight alpha 混合。`config/lv_conf.h` 已启用 `LV_DRAW_SW_SUPPORT_ARGB8888 1`。

**字节序**：ARGB8888 以 `uint32_t` 存储，小端机上内存字节序为 B,G,R,A —— 与 LVGL 的 `lv_color32_t { blue, green, red, alpha }` 一致。生成器直接产出 `0xAARRGGBB` 的 `uint32_t` 字面量即可。

### 4.6 绘制路径

`libgui/src/input_lvgl.c` 的 `cursor_draw_cb` 从"画 18 个矩形"改成"画一张图"：

```c
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

- `lv_draw_image_dsc_init()` 把 `scale_x/scale_y` 置为 `LV_SCALE_NONE`（256），因此是 **1:1** 绘制；画布与对象同为 16×24，不会发生缩放。
- `lv_draw_image()` 内部会调 `lv_image_decoder_get_info()` 重新取 header（`lv_draw_image_dsc_t.header` 由它自己填），所以 header 填错的后果是"静默不画"，不会崩——这也是必须写宿主测试证明"能解码并画出来"的原因（§8.1 第 1 步）。
- `lv_draw_image()` 把任务挂到传入的 layer 上（`lv_draw_add_task(layer, ...)`）；`dsc.base.layer` 赋值只为与既有 `lv_draw_rect` 写法保持一致。

### 4.7 生成器与黄金图

`tools/gen_cursor_icon.py`：**纯标准库**（`zlib` + `struct` 自带 PNG 解码），无第三方依赖。

- `python3 tools/gen_cursor_icon.py` → 重写 `libgui/src/cursor_icon.c` 与 `libgui/assets/cursor-arrow-16x24.txt`
- `python3 tools/gen_cursor_icon.py --check` → 重新生成到内存，与磁盘上的两个文件逐字节比对；不一致 → 非零退出并打印差异

**黄金图** `libgui/assets/cursor-arrow-16x24.txt`：16 行 × 16 字符，每格一个字符：

| 字符 | 含义 |
|------|------|
| `.` | alpha == 0（完全透明） |
| `F` | alpha == 255 且颜色 = 填充色 `#1A1C1E` |
| `O` | alpha == 255 且颜色 = 描边色 `#FDFCFF` |
| `a` | 0 < alpha < 255（抗锯齿边缘） |

黄金图是**人工评审面**：改算法 → 黄金图变 → diff 里一眼看出剪影变了。生成器的 `--check` 保证 `cursor_icon.c` 与黄金图同源。

---

## 5. 壁纸配色设计

### 5.1 新增 `surface_dim` 角色

| 角色 | 值 | 用途 |
|------|----|------|
| `surface_dim` | `#D8D2E4` | 桌面壁纸底色（新的默认壁纸） |

- 加在 `gui_theme_colors_t` 的 `surface` **之后**（语义相邻）。该结构体是源码级公开数据，全仓库从源码重编，**无 ABI 约束**。
- **不改 `surface` 的值**：`surface` 同时是 elevation 0 的表面色、被卡片 / 列表共用，改它会牵动整套层次关系。
- `surface_dim` **不进入 elevation 阶梯**：`gui_theme_elevation_surface()` 的序列断言一字不改。

### 5.2 壁纸枚举重排

```c
typedef enum {
    DESKTOP_WALLPAPER_SURFACE_DIM = 0,   /* #D8D2E4，最深 */
    DESKTOP_WALLPAPER_SURFACE,            /* #FDFCFF，最浅 */
    DESKTOP_WALLPAPER_CONTAINER_LOW,      /* #F7F2FA */
    DESKTOP_WALLPAPER_CONTAINER_HIGH,     /* #ECE6F0 */
    DESKTOP_WALLPAPER_CONTAINER_HIGHEST   /* #E6E0E9 */
} desktop_wallpaper_t;
```

- `CONTAINER_HIGHEST` 仍是**最后一个成员** → 计数表达式 `(int)(DESKTOP_WALLPAPER_CONTAINER_HIGHEST + 1)` 一字不改仍等于 5。
- 新的默认壁纸 = `DESKTOP_WALLPAPER_SURFACE_DIM`（`desktop_shell.c:1053` 的初始化改为它）。

### 5.3 标签阶梯

| 枚举 | 颜色 | 标签 | 备注 |
|------|------|------|------|
| `SURFACE_DIM` | `#D8D2E4` | `Deepest` | 新增 |
| `SURFACE` | `#FDFCFF` | `Standard` | 原样 |
| `CONTAINER_LOW` | `#F7F2FA` | `Light grey` | 原样 |
| `CONTAINER_HIGH` | `#ECE6F0` | `Grey` | 原 `Dark grey` |
| `CONTAINER_HIGHEST` | `#E6E0E9` | `Dark grey` | 原 `Darkest` |

标签按**明度**排成单调的一组：`Standard`（最浅）→ `Light grey` → `Grey` → `Dark grey` → `Deepest`（最深）。全部是 ASCII（Montserrat 没有汉字字形，GUI-3 规范 §1.2 的约束）。

**已知副作用（如实记录）**：设置项的循环方向是 `(wallpaper + 1) % COUNT`，即按枚举顺序走，实际循环序是
`Deepest → Standard → Light grey → Grey → Dark grey → Deepest`。
因为新默认是最深的 `Deepest`，**第一次点击**会跳到最浅的 `Standard`，之后才是逐级变深。若日后觉得这个跳跃别扭，最小改动是把枚举改成 `SURFACE / CONTAINER_LOW / CONTAINER_HIGH / CONTAINER_HIGHEST / SURFACE_DIM` 并引入显式的 `DESKTOP_WALLPAPER_COUNT`（届时计数表达式要跟着改）。**本期不做。**

### 5.4 壁纸救不了光标

实测：在 `surface` / `surface_container_low` 等底色下，**纯白光标依然不可见**（§2.1 的截图分析已证明）。壁纸改色属于审美改善，**光标可见性由图标本身解决**（§4.3 的深主体 + 浅描边）。两者是独立的，不要把"换了壁纸"当成"光标可见了"的理由。

---

## 6. 契约同步

### 6.1 宿主契约测试

**`hosttests/cases/test_gui_theme.c`**（令牌表）

| # | 断言 |
|---|-------|
| 1 | `assert_color_role(c->surface_dim, "surface_dim")` —— 最高字节为 0、RGB 不全 0 |
| 2 | `c->surface_dim != c->surface` —— 两者必须可区分（否则壁纸色与卡片色撞车） |

**`hosttests/cases/test_gui_input_lvgl.c`**（改写光标用例）

| # | 断言 |
|---|-------|
| 3 | 光标对象尺寸 == **字面量** 16 / 24（**不是** `GUI_INPUT_CURSOR_WIDTH/HEIGHT`，避免"实现和自己比"；沿用旧用例注释里的这条理由） |
| 4 | 热点 (0,0)：对象左上角 == 指针位置（沿用旧断言） |
| 5 | 光标仍然不是 CLICKABLE、不截获 hit-test（沿用旧断言） |
| 6 | 资源可解码：`lv_image_decoder_get_info(&gui_cursor_icon, &h)` 返回 `LV_RESULT_OK`，且 `h.w == 16`、`h.h == 24`、`h.cf == LV_COLOR_FORMAT_ARGB8888`、`h.stride == 64` |
| 7 | 资源数据里只有**两种** RGB（填充色与描边色），不得出现第三种颜色 |
| 8 | 存在至少一个 alpha == 255 的填充像素、至少一个 alpha == 255 的描边像素，且至少一个 alpha == 0 的像素 |
| 9 | 热点像素 (0,0) 的 alpha > 0（箭尖必须可见，不能整块透明；按 §4.2 的裁剪 + 缩放实测该像素落在本体左上角的描边区内，是完全不透明的） |
| 10 | **绘制结果 · 填充**：取一个 alpha == 255 的填充像素，画布坐标 P → 屏幕坐标 `cursor_origin + P`，读回的像素**精确等于**填充色 `#1A1C1E` |
| 11 | **绘制结果 · 描边**：取一个 alpha == 255 的描边像素，同上，读回的像素**精确等于**描边色 `#FDFCFF` |
| 12 | **绘制结果 · 透明**：取一个 alpha == 0 的像素，读回的像素等于光标**旁边**的背景像素（两者相等即可，不断言底色具体是什么） |

断言 10/11 只在 **alpha == 255** 的像素上做：LVGL 的 ARGB8888 走 straight alpha，离屏画布是 XRGB8888，实际路径是 `lv_draw_sw_blend_to_rgb888.c` 的 `lv_color_24_24_mix()`，在 `mix >= LV_OPA_MAX` 时直接拷贝源字节，因此这类像素画出来是**精确色值**；半透明像素会被底色混合，不能做等值断言。断言 12 的两个像素取自**同一块画布**，避免把"底色是什么"也写进断言。

**读取资源像素时的字节序**：`gui_cursor_icon.data` 是 `const uint8_t *`，内存里每像素 4 字节，顺序为 **B, G, R, A**（小端机上等价于 `uint32` 值 `0xAARRGGBB`）。宿主用例按字节读：`r = data[i*4+2]`、`g = data[i*4+1]`、`b = data[i*4+0]`、`a = data[i*4+3]`。

**宿主用例的 include**：`<gui_cursor_icon.h>`（§3.3 说明它为什么必须在 `libgui/internal/`）。

**`hosttests/cases/test_desktop_shell.c`**（壁纸）

| # | 断言 |
|---|-------|
| 13 | 主屏壁纸区采样 == `c->surface_dim`（不再是 `c->surface`） |
| 14 | 新建外壳的默认壁纸 == `DESKTOP_WALLPAPER_SURFACE_DIM` |
| 15 | 点击一次壁纸设置项 → `DESKTOP_WALLPAPER_SURFACE`，壁纸区采样 == `c->surface` |
| 16 | 再点一次 → `DESKTOP_WALLPAPER_CONTAINER_LOW`，壁纸区采样 == `c->surface_container_low`（沿用旧用例的节奏） |

宿主测试**不**断言壁纸标签（既有事实），因此标签改名不受影响。

### 6.2 guest 探针

`user/test_gui_input/test_gui_input.c`：

- `PROBE_CURSOR_COLOR 0x00FFFFFFu` → 拆成
  `PROBE_CURSOR_FILL_COLOR 0x001A1C1Eu` 与 `PROBE_CURSOR_OUTLINE_COLOR 0x00FDFCFFu`
- `:1295` 的等值断言改为同时比对两个常量

### 6.3 QEMU E2E（`qemutests/gui_desktop_test.py`）

| # | 检查 | 改法 |
|---|------|------|
| 17 | 画面被壁纸色主导 | `MATERIAL_SURFACE_RGB (253,252,255)` → 壁纸色 `(216, 210, 228)`；常量改名 `WALLPAPER_RGB`；占比下限 `MIN_MATERIAL_FRACTION = 0.5` 不变（实测壁纸占比约 0.918） |
| 18 | **光标在画面里可见且是两色** | 取**屏幕中心 16×24 窗口**（`(w//2, h//2)`，依据 §2.2：门禁从不移动鼠标，指针恒在中心，且该处是空白壁纸区）：断言窗口内同时存在 ① 红通道 < 壁纸红通道的像素（深色主体）与 ② 蓝通道 > 壁纸蓝通道的像素（浅色描边） |

断言 18 的两个条件都基于**通道比较**而不是精确色值，因此不受抗锯齿影响：

- 改之前：旧光标是纯白，红 255 > 壁纸红 216 → 条件 ① 不成立 → 门禁**能抓到回归**。
- 改之后：填充色 `#1A1C1E` 与描边色 `#FDFCFF` 的混合结果必然同时满足两个条件。

`#FDFCFF` 在桌面 UI 里没有别的用途（§2.3），因此这条判据不会误判。

### 6.4 静态审计

- `user/desktop` 仍不得出现裸颜色字面量与裸字号：新增的 `surface_dim` 只以 `c->surface_dim` 形式出现 ✓
- `libgui` 不受该审计约束；`cursor_icon.c` 里的 `#1A1C1E` / `#FDFCFF` 是**烘焙进像素的资源内容**，不是令牌违规。

---

## 7. 风险与对策

| # | 风险 | 影响 | 对策 |
|---|------|------|------|
| 1 | **仓库首次走 LVGL 图像绘制路径**：`lv_image_dsc_t` 的 header（magic / cf / w / h / stride）填错 | LVGL 静默不画；`magic` 填成 ≥ 0x20 还会把指针当文件名去打开 | 实施的第一步就写**最小宿主测试**证明"资源能被解码并画出"（§6.1 断言 6/10/11）；`magic` 用 `LV_IMAGE_HEADER_MAGIC` 宏而非字面量 |
| 2 | **抗锯齿让等值断言变脆** | 宿主测试频繁变红 | 取样点只选 **alpha == 255** 的像素；半透明像素只做"不等于背景"的弱断言（§6.1 断言 10–12） |
| 3 | 旧断言钉死 12×18 与 `0x00FFFFFF` | 宿主测试 / guest 探针必红 | §6.1、§6.2 同步清单；新断言仍钉**字面量**，不退化成"实现和自己比" |
| 4 | 生成物与生成器脱节 | 资源被手改，`--check` 失效 | `--check` 进 `make test-host`（§8.2）；生成物文件头写明"勿手改" |
| 5 | 门禁的壁纸基色常量写死 `#FDFCFF` | `gui-desktop` 门禁必红 | §6.3 第 17 条改成壁纸色 |
| 6 | 宿主测试链接缺 `cursor_icon.o` | 链接期未定义引用 | `hosttests/Makefile` 的 `GUI_INPUT_UNIT_OBJS` 增加 `cursor_icon.o` |
| 7 | 门禁的光标判据依赖"指针在屏幕中心且该处是空白壁纸" | 桌面布局变化后判据可能失真 | 判据写成"窗口内同时有更暗与更亮的像素"，布局变化只会让它更宽松；真正的精确判据在宿主侧（§6.1） |
| 8 | 换主题时改不动光标颜色 | 未来 GUI-4 / 深色主题的遗留问题 | §4.4 已记录出路（拆成两张遮罩分别上色），本期不实现 |
| 9 | 壁纸设置**不持久化** | 重启后回到 `surface_dim` | 既有行为（GUI-3 规范未承诺持久化），不引入回归 |
| 10 | 首次引入 PNG 二进制资产 | 仓库体积 / 许可证 | 源图是用户提供的箭头，无第三方许可问题；仓库已有 `.psf` 二进制资产先例 |

---

## 8. 验证与交付门禁

### 8.1 实施顺序（每一步都可单独验证）

1. **资源可解码**：先写最小宿主测试，证明 `lv_image_dsc_t` 能被 LVGL 解码并画出一个像素 —— 把风险 1 提前暴露。
2. **生成器与黄金图**：`tools/gen_cursor_icon.py` + `--check` + 黄金图人工评审。
3. **绘制路径改造**：`cursor_draw_cb` → `lv_draw_image`，尺寸 16×24，两个颜色常量。
4. **令牌**：`surface_dim` 入表。
5. **壁纸**：枚举重排 + 标签 + 默认值。
6. **契约同步**：宿主用例、guest 探针、QEMU 门禁。
7. **文档**：changelog / gui.md / roadmap。

### 8.2 回归入口

```bash
make PROFILE=x86_64-clang test-gui-host CASE=input-lvgl      # 光标用例
make PROFILE=x86_64-clang test-gui-host CASE=theme           # 令牌
make PROFILE=x86_64-clang test-gui-host CASE=desktop-shell   # 壁纸
make PROFILE=x86_64-clang test-host                          # 含 tools/tests/test_cursor_icon.py
make PROFILE=x86_64-clang test-static                        # 静态审计
make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop        # 端到端门禁（SMP 1/2）
```

（`test-gui-host` 的合法 CASE 名取自 `mk/components/run.mk` 的 `GUI_HOST_REGISTERED_CASES`。）

### 8.3 完成判据

| # | 判据 |
|---|------|
| 1 | 宿主：光标用例全绿，且断言钉的是字面量 16 / 24 |
| 2 | 宿主：资源可解码，两种 RGB，alpha==255 的填充 / 描边像素各存在至少一个，alpha==0 的像素存在，热点 alpha > 0 |
| 3 | 宿主：绘制结果 —— alpha==255 的填充 / 描边像素在画布上**精确**等于 `GUI_INPUT_CURSOR_FILL_COLOR` / `GUI_INPUT_CURSOR_OUTLINE_COLOR`；alpha==0 像素等于旁边背景 |
| 4 | 宿主：`surface_dim` 令牌存在、值 == `0x00D8D2E4u`、与 `surface` 可区分 |
| 5 | 宿主：桌面默认壁纸 == `surface_dim`，主屏壁纸区采样 == `surface_dim` |
| 6 | 工具：`tools/tests/test_cursor_icon.py` 全绿（`--check` 一致 + 黄金图结构契约 + PNG 解码器自测） |
| 7 | 静态审计：`make test-static` 绿（`user/desktop` 无裸颜色 / 裸字号） |
| 8 | 门禁：`make test-qemu SUITE=gui-desktop` 绿（SMP 1/2 都过），截图里壁纸为 `#D8D2E4` 且屏幕中心窗口同时存在更暗与更亮的像素 |
| 9 | 零孤儿 QEMU（`disk.img` 无人占用） |

---

## 9. 交付分期

| 阶段 | 内容 | 完成判据 |
|------|------|----------|
| **P1 光标资源** | 生成器 + 黄金图 + `cursor_icon.c` / `.h` + `libgui/Makefile` 接线 | `tools/tests/test_cursor_icon.py` 绿；`cursor_icon.c` 可被 LVGL 解码 |
| **P2 光标绘制** | `cursor_draw_cb` 改 `lv_draw_image`；尺寸 16×24；两个颜色常量 | 光标宿主用例全绿 |
| **P3 壁纸** | `surface_dim` 令牌 + 枚举重排 + 标签 + 默认值 | 令牌与外壳宿主用例全绿 |
| **P4 契约与门禁** | guest 探针 + QEMU 门禁常量 + 光标可见性检查 | `test-qemu SUITE=gui-desktop` 绿 |
| **P5 收口** | 文档（changelog / gui.md / roadmap）+ 全量回归 | 全量回归绿；文档同步 |

每一阶段独立提交、独立可回滚。
