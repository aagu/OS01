# QEMU 窗口化运行的鼠标指针：速度比例实测与"穿透窗口"归因（2026-10-25）

## 结论

- **指针速度是 1:1 的，与 DPI 无关。** 把 QEMU 的宿主侧事件与 guest 侧
  ps2 包**按 trace 顺序逐包配对**，实测 **885 个包里 879 个与宿主位移
  精确相等**（dx、dy 都相等），匹配包的位移和 **x=193/y=−155 vs
  x=193/y=−155，比值 1.0000 / 1.0000**；剩下 6 个是"指针经过屏幕边缘"
  时被 QEMU 主动丢弃的陈旧位移事件。QEMU 的相对鼠标换算全程用 **GDK 逻辑
  像素**，链路里没有任何 DPI 因子。
- **"挪动一大就穿透出 QEMU 窗口"的根因不在 DPI，而在 QEMU 的相对鼠标机制**，
  由三个缺陷叠加：
  1. **相对位移只在 QEMU 抓住指针时才会送给 guest**
     （`ui/gtk.c:1036` 要求 `s->ptr_owner == vc`）。抓取由**窗口内第一次
     左键点击**隐式开启（`ui/gtk.c:1085-1092`，`relative-mode-click`）。
     没抓取时，窗口内怎么移动鼠标 guest 都收不到。
  2. **抓取后的"指针撞显示器边缘 → 回卷到屏幕中心"在 Wayland 下是空操作**。
     QEMU 调用 `gdk_device_warp()`（`ui/gtk.c:1069`），而 GTK3 的 Wayland
     后端把 `gdk_wayland_device_warp()` 写成**空函数**
     （`gdk/wayland/gdkdevice-wayland.c:651`）；指针不会被拉回，但那一次
     位移仍被丢弃（`s->last_set = FALSE`，`ui/gtk.c:1071`）。
  3. **指针离开窗口再回来会带出一次"陈旧位移"**。`gd_leave_event()`
     （`ui/gtk.c:1860`）**不重置 `s->last_set`**，所以回来后的第一个事件
     会拿陈旧的 `last_x/last_y` 做差，宿主侧实测出现过 **−1023** 计数
     （一个画布宽度量级）的跳变。这部分**是否真的发给 guest，取决于离开
     时的路径**：
     - 从**屏幕边缘**方向离开 → QEMU 的边缘分支先触发
       （`ui/gtk.c:1045-1073`）→ `s->last_set = FALSE` → 回来时那一次
       陈旧位移被**丢弃**，guest 收不到（实测 6 次，全部被丢）。
     - 从**画布内边**（例如上边界）离开 → 边缘分支不触发 → 陈旧位移会
       **真的发给 guest**（QEMU 会把它拆成多个 ±127 的包），guest 光标
       因此被一次性甩到屏幕边缘。

  抓取本身不会把事件引走：motion 回调挂在画布自己的 `"event"` 信号上
  （`ui/gtk.c:2212`），抓取目标也是画布自己的 GdkWindow
  （`gd_grab_update()` 里的 `gtk_widget_get_window(vc->gfx.drawing_area)`）。
  **两个后端的差别在于抓取能否约束指针**：

  - **Wayland**：GTK3 的 `gdk_wayland_seat_grab()` 只在 GDK 内部记一个
    grab window，**没有把约束转达给合成器**（Wayland 协议本身也没有
    指针约束的能力），因此指针可以自由离开 QEMU 窗口 —— 这正是
    "挪动一大就穿透出窗口"的直接成因。
  - **X11**：抓取落在 X 服务器的指针抓取上；实测指针仍能被
    `XWarpPointer` 移到窗口外，因此"离开再回来"的路径在两个后端都存在。
- **与 DPI 真正相关的只有清晰度**：当前 GNOME 分数缩放 1.25，guest 帧缓冲
  1440×900 会被放大 1.25×（发虚），但光标与宿主指针在屏幕上仍然严格对齐。
- **OS01 侧当前无需改动即可自保**：`libgui/src/input_lvgl.c:971-987` 把
  光标钳在视口内，假位移只表现为"光标被甩到屏幕边缘后卡住"。
- **根治条件是绝对定位设备**（QEMU `-device usb-tablet`）。OS01 目前
  **完全没有 USB 栈**（`kernel/driver/` 下无 xhci / ehci / hid），因此
  这条路尚未打通；PS/2 路径需要保留。

## 宿主显示链：三个数字不是一回事

初查时容易把 xrandr 报的 `4096x2304` 当成"物理像素"，实际面板是
2560×1440。三个数字各属不同空间：

| 数字 | 含义 | 权威来源 |
|------|------|----------|
| **2560×1440** | 面板物理分辨率（唯一真实值） | EDID 首选时序 `2560x1440@59.95Hz / 241.5MHz`；DRM `fb0 virtual_size = 2560,1440` |
| **2048×1152** | 合成器逻辑空间 | mutter 51.1 的 logical monitor `scale=1.25` → 2560/1.25 |
| **4096×2304** | **X11 虚拟屏幕**（非物理像素） | `xrandr` 的 `Screen 0: current 4096 x 2304` |

算术链闭合：

```
面板 2560x1440 ──÷ 1.25（mutter 分数缩放）──▶ 逻辑空间 2048x1152
                                              │ ×2（只支持整数缩放的客户端拿到的 scale）
                                              ▼
                                       X11 虚拟屏幕 4096x2304
```

GDK3 的 X11 后端是**独立**从 X 屏幕推缩放因子的：4096 px / 600 mm =
173 DPI → 173/96 = 1.8 → 取整 → **scale = 2** → 显示器几何 = 4096/2 =
2048×1152，与 mutter 原生 Wayland 通道报的值完全一致。

**不是配置残留**：

- DRM 里 DP-1 / DP-2 / HDMI-A-1 全部 `disconnected`，只有 DP-3 在用；
  mutter DBus 也只有一个 logical monitor。
- mutter 报当前模式 `2560x1440@59.951 (is-current: true)`，模式表就是当前
  EDID 的那套 —— mutter 侧是活的。
- xrandr 报的 600 mm×340 mm 已是当前 EDID 的物理尺寸，说明该输出在持续更新。
- 4096 = 2048×2 在两个轴上同时精确成立，且与原生 Wayland 通道独立算出的
  逻辑空间一致；随机残留值不可能同时满足这两条。

唯一"残留"的是 `org.gnome.mutter experimental-features =
['scale-monitor-framebuffer']`（分数缩放开关），那是主动开启的。

## 复现方法

工具在 [`tools/qemu-mouse/`](../../tools/qemu-mouse/)（宿主侧，不参与
OS01 构建，不进 profile 依赖）：

| 工具 | 作用 |
|------|------|
| `collect_host_display.sh` | 一次采齐 EDID / DRM / mutter / xrandr / GDK 五处事实，并给出三个数字的算术关系 |
| `gdk_probe` | 打印 GDK 的全部坐标系，并复刻 QEMU `gd_motion_event()` 的换算与边缘判定 |
| `xdrive` | X11 指针驱动（warp 绝对/相对移动、左键点击） |
| `measure_mouse.sh` | 端到端：真镜像 + QEMU trace + 指针驱动，产出比例 |
| `analyse_mouse.py` | 解析 trace，给出假位移清单与 guest/宿主比例 |

```bash
cd tools/qemu-mouse && make && ./collect_host_display.sh
./measure_mouse.sh --work-dir /tmp/mm
python3 analyse_mouse.py /tmp/mm/qemu-trace.log /tmp/mm/guest-serial.log
```

两个环境约束（否则测不出来）：

- Xwayland 下 `XTestFakeMotionEvent` 会被合成器忽略（指针位置不跟随），
  必须用 `XWarpPointer` 驱动指针 —— `xdrive` 已按此实现。
- 相对位移要送达 guest，必须先让 QEMU 抓住指针：`xdrive --click` 在窗口内
  点一次即可。
- **必须把 X 窗口抬到最上层**。GNOME 下若某个 Wayland 窗口（常见于全屏的
  Electron 应用）盖在 QEMU 窗口之上，合成器会把指针路由给那个 Wayland
  窗口，**X 客户端一个 motion 事件都收不到** —— 表现为"warp 成功、指针
  位置也变了，但 QEMU 里一条事件都没有"。实测：`XRaiseWindow` +
  `XSetInputFocus` 之后立刻恢复（QEMU 立刻收到 173 条 motion）。原生
  Wayland 客户端不受影响（同样的状态下照常收到事件）。
- **步骤顺序有讲究**：X11 下点击之后指针会被约束在画布内，"移出窗口再回来"
  就 warp 不动了。所以 `measure_mouse.sh` 先做离开/回来的复现，**再**点击
  开启抓取，最后在画布内做比例测量。

## 本次实测

QEMU 11.1.2，GTK3 后端（`/usr/lib/qemu/ui-gtk.so → libgtk-3.so.0`），
真实 OS01 镜像，两个 trace 事件同时开启：
`gd_motion_event`（宿主侧）与 `ps2_mouse_send_packet`（guest 侧字节流）。

**坐标系核对**（`gdk_probe`）：

| 输入 | GDK 事件 |
|------|----------|
| X 根坐标（设备像素）2000,500 | `root=(1000,250)`，`local=(589,33)` |
| X 根坐标（设备像素）2400,700 | `root=(1200,350)`，`local=(846,133)`，`delta=(200,100)` |

即：GDK 事件坐标 = 设备像素 ÷ 2 = **逻辑像素**，与
`gdk_monitor_get_geometry()` 的 2048×1152 同单位。

**链路比例**（`measure_mouse.sh` 一次完整运行，n=885 个 guest 包）：

| 量 | 值 |
|----|-----|
| 宿主侧 `gd_motion_event` 事件数 | 1050（`ww=1440, wh=900, ws=2`） |
| guest 侧 ps2 包数 | 885 |
| **逐包与宿主位移精确相等的包** | **879 / 885**（dx 与 dy 都相等） |
| 匹配包的位移和 | 宿主 `x=193 y=-155`　guest `x=193 y=-155` |
| **比值 guest/宿主** | **x = 1.0000　y = 1.0000** |
| 不匹配的包 | 6 —— 全部是"指针经过屏幕边缘"时被 QEMU 丢弃的陈旧位移事件 |
| guest 侧包符号位字节 | `0x8 / 0x18 / 0x28 / 0x38` |

配对规则必须**按 trace 出现顺序**做，不能按下标对齐两个数组：

- QEMU 的 ps2 鼠标把每个事件累积后裁到 ±127 再发包，余量结转到下一个包
  （`hw/input/ps2.c` `ps2_mouse_send_packet()`）；
- **包里的 dy = −Δfby**（`ps2_mouse_event()` 里 `s->mouse_dy -= value`，
  而画布 y 向下为正）；
- 未抓住指针时（`s->ptr_owner != vc`）相对位移根本不进设备，guest 收不到包；
  所以 trace 开头那段"有 motion 事件但没有包"是正常的。

**宿主单事件位移 >127 的清单**（陈旧位移 / 快速甩动）：

```
event#384: fbx 938 -> 939   step=-439
event#416: fbx  52 -> 38    step=+528
event#443: fbx 805 -> 804   step=-319
event#453: fbx  69 -> 29    step=+477
event#670: fbx 921 -> 853   step=-576
event#694: fbx 315 -> 339   step=+348
event#714: fbx 848 -> 804   step=-414
event#964: fbx 1101 -> 1102 step=-656
event#990: fbx  75 -> 27    step=+912
```

**guest 侧包字节**：`bs` 出现 `0x8 / 0x18 / 0x28 / 0x38`，与 QEMU 的
`b = 0x08 | (dx1 < 0) << 4 | (dy1 < 0) << 5` 一致；OS01 的解码
（`kernel/driver/mouse_proto.c:24-28`，X 符号位 `0x10`、Y 符号位 `0x20`）
与之**完全吻合**，guest 侧不需要为符号位改动。

## 根因与源码位置

**QEMU 侧（上游缺陷，非 OS01 可改）**

| 位置 | 内容 |
|------|------|
| `ui/gtk.c:982` `gd_motion_event()` | 宿主侧换算入口 |
| `ui/gtk.c:1019-1020` | `fbx = (motion->x - wx_offset) / vc->gfx.scale_x` —— **只按 zoom 缩放，无 DPI 因子** |
| `ui/gtk.c:1036` | 相对位移分支要求 `s->last_set && s->ptr_owner == vc` —— **没抓取就不送位移** |
| `ui/gtk.c:1045-1073` | 边缘判定 + `gdk_device_warp()` 回卷 + `s->last_set = FALSE` 丢一次位移 |
| `ui/gtk.c:1085-1092` | 相对模式下第一次左键点击隐式开启抓取 |
| `ui/gtk.c:1860` `gd_leave_event()` | **不重置 `s->last_set`** → 再进入时注入假位移 |
| `hw/input/ps2.c` `ps2_mouse_send_packet()` | 每包裁到 ±127，**余量保留**到下一包（不丢位移） |
| `gdk/wayland/gdkdevice-wayland.c:651` | `gdk_wayland_device_warp()` 是**空函数** |

**OS01 侧（现状正确）**

| 位置 | 内容 |
|------|------|
| `kernel/driver/mouse_proto.c:24-28` | 位移解码，符号位与 QEMU 一致 |
| `kernel/driver/mouse.c`（`mouse_init`） | IntelliMouse 敲击序列 `F3 200 / F3 100 / F3 80` → 协商到 4 字节包 |
| `libgui/src/input_lvgl.c:971-987` | **视口钳制**：光标不会画到 guest 屏幕之外 |
| `libgui/src/input_lvgl.c:482-489` | 光标对象初始化在视口中心 |
| `kernel/driver/bga.c:30` | guest 帧缓冲 1440×900×32（与 QEMU 画布尺寸一致，故图像 1:1） |

## 对 OS01 的影响与后续边界

**现在可以做的（可选加固）**

在 libgui 的指针路径里把"不可能的位移"识别为 desync：单轮泵内累计位移
超过视口宽度时判定为失步并重新锚定，而不是照单全收。这样假位移不再把
光标甩到边缘。属于 guest 侧健壮性，不依赖 USB。

**根治所需（尚未具备）**

- QEMU 侧：q35 默认没有 USB 控制器，需显式 `-device qemu-xhci`，
  再加 `-device usb-tablet`（绝对坐标）。
- guest 侧：需要 xHCI/EHCI 主机控制器驱动 + HID（Digitizer usage page）
  类驱动。OS01 目前 `kernel/driver/` 下没有任何 USB 相关代码，这是最大
  的一块缺口。绝对定位设备没有"相对位移"，上面三个缺陷会一起消失。

**宿主侧可选调整**

把显示缩放改成整数（100%）：mutter 逻辑空间 = 2560×1440 = 面板原生，
GDK 取 scale=1，guest 画面 1:1 完全清晰、指针仍 1:1，QEMU 窗口
（1440×900）也能放下。维持 125% 则画面被放大 1.25×、略软，但指针依旧精确。

## 验证边界（本次未覆盖的部分）

- 端到端逐包配对在 **X11 后端**实测（`GDK_BACKEND=x11`）。**原生 Wayland
  后端**只验证了 GDK 报告的坐标系与缩放因子（同样为 2048×1152 @ scale 2），
  没有重复跑完整的逐包配对。
- "从画布内边（非屏幕边缘）离开 → 陈旧位移真的发给 guest"这一条**只做了
  源码级论证**（`gd_leave_event()` 不重置 `s->last_set` + 边缘分支只在屏幕
  边缘触发），**没有跑出逐包证据**：实测环境里聊天窗口叠在 QEMU 窗口之上，
  合成器把指针路由给了 Wayland 客户端，X 侧收不到 motion（见"复现方法"
  里的抬窗要求）。
- guest 侧用 QEMU 的 `ps2_mouse_send_packet` trace 核对（那就是 guest 收到的
  字节流）；`/bin/mousetest` 的串口输出因本机 guest shell 启动超时未采到，
  串口侧比例未做交叉验证。
- 边缘回卷在 **Wayland 下为 no-op** 这条结论来自 GTK3 源码
  （`gdk_wayland_device_warp()` 空实现），未在本机 Wayland 会话里复现
  "指针被回卷"的可见行为。
- guest 事件环（`MOUSE_RING_CAPACITY = 64`）在一次 −1023 的假位移（拆成
  多包）下未观察到丢包，但快速甩动时是否溢出未专门验证。
