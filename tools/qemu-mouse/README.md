# tools/qemu-mouse — 宿主侧指针取证工具

QEMU 窗口化运行时，"宿主指针位移 → guest 鼠标计数"这条链跨越了
GDK/Qt、QEMU ui/gtk.c、QEMU 的 PS/2 打包、guest 的 i8042 驱动四层。
本目录提供一组**宿主侧**工具，把每一层的中间量都抓出来，使结论可复算。

调查背景与全部实测数据见
[`docs/debug/2026-10-25-qemu-gtk-mouse-pointer.md`](../../docs/debug/2026-10-25-qemu-gtk-mouse-pointer.md)。

这些工具**不属于 OS01 构建**：不挂进根 Makefile、不进 profile 依赖、
不产生 guest 侧产物，只在开发机上按需编译。

## 依赖

| 工具 | 依赖 | 缺失时 |
|------|------|--------|
| `gdk_probe` | `gtk+-3.0` 开发包 | `make` 报错提示 gtk3-devel |
| `xdrive` | `libX11`、`libXtst` | `make` 报错 |
| `measure_mouse.sh` | `qemu-system-x86_64`、构建好的 OS01 镜像 | 脚本内自检 |
| `collect_host_display.sh` | `edid-decode`、`gdbus`、`xrandr` | 分节跳过 |
| `analyse_mouse.py` | `python3` | — |

```bash
cd tools/qemu-mouse
make check-deps      # 检查依赖
make                 # 构建 gdk_probe + xdrive
```

## 工具

### `gdk_probe` — GDK 坐标系探针

打印 GDK3 报告的所有坐标系，并把 QEMU `ui/gtk.c` 的换算原样跑一遍：

```bash
GDK_BACKEND=x11 ./gdk_probe          # X11 后端
GDK_BACKEND=wayland ./gdk_probe      # 原生 Wayland 后端
```

每个 motion 事件输出一行 `MOTION local=… root=… fbx=… delta=…` 与
`monitor_geo=… edge_warp=…`。`edge_warp` 复刻的是 QEMU 的"指针撞到
显示器边缘就回卷"判定条件，用来确认 `motion->x_root` 与
`gdk_monitor_get_geometry()` 是否同一单位。

### `xdrive` — X11 指针驱动

```bash
./xdrive --probe              # 打印 QEMU 窗口几何与当前指针位置
./xdrive --center             # 指针移到 QEMU 窗口中心
./xdrive --abs X Y            # 绝对移动（X 根坐标，设备像素）
./xdrive --dx N --dy M        # 相对移动（N/M 为逻辑像素）
./xdrive --click              # 左键按下+抬起（触发 QEMU 的隐式抓取）
```

要点：Xwayland 下 `XTestFakeMotionEvent` 会被合成器忽略（指针不跟随），
`XWarpPointer` 才有效，所以本工具用 warp 驱动。`--dx/--dy` 以**逻辑
像素**为单位，内部按 `--scale`（默认 2）换算成 X 设备像素。

> **必须把 X 窗口抬到最上层**：GNOME 下若某个 Wayland 窗口（常见于全屏的
> Electron 应用）盖在 QEMU 窗口之上，合成器会把指针路由给那个 Wayland
> 窗口，**X 客户端一个 motion 事件都收不到** —— 表现为"warp 成功、指针
> 位置也变了，但 QEMU 里一条事件都没有"。本工具在 `--center` / `--click`
> / `--abs` / `--raise` 时会自动 `XRaiseWindow` + `XSetInputFocus`。
>
> 同理，**原生 Wayland 客户端（GDK_BACKEND=wayland）不受这个影响**：
> 实测 Wayland 客户端在同样的状态下照常收到 motion 事件。
>
> 若桌面上有聚焦的 Wayland 窗口压着 X 窗口，抬窗可能仍然无效（合成器会
> 把聚焦窗口重新置顶）。这时把那个窗口最小化，或用 `--no-guest` 之外的
> 方式腾出屏幕即可。

### `measure_mouse.sh` — 端到端比例实测

```bash
./measure_mouse.sh                     # 用默认 profile 跑
./measure_mouse.sh --work-dir /tmp/mm  # 指定产物目录
./measure_mouse.sh --no-guest          # 只跑宿主侧（不启动 guest）
```

流程：以 `--trace gd_motion_event --trace ps2_mouse_send_packet` 启动
QEMU（真实 OS01 镜像）→ 在窗口内点击一次以开启相对模式的抓取 → 指针
移出窗口再移回（复现假位移）→ 在窗口内移动已知距离 → 解析并打印比例。

产物：`qemu-trace.log`（宿主侧 + guest 侧包）、`guest-serial.log`
（guest 串口，含 `/bin/mousetest` 的 dx/dy 行）。

> guest shell 起来约需 60 s；超时后 trace 依然保留，宿主侧比例不依赖 guest。

### `analyse_mouse.py` — trace 解析

```bash
python3 analyse_mouse.py qemu-trace.log guest-serial.log
```

输出宿主侧 fbx 序列、guest ps2 包 dx/dy、符号位字节、**单事件位移 >127
的假位移清单**，以及 `guest 实收计数 / 宿主位移` 的比例（1.0 = 1:1）。

### `collect_host_display.sh` — 宿主显示链取证

```bash
./collect_host_display.sh
```

一次性打印并区分三个容易混淆的数字：面板**物理分辨率**（EDID/DRM）、
合成器**逻辑空间**（mutter 分数缩放）、**X11 虚拟屏幕**（xrandr），
最后用一段算式说明三者的关系。
