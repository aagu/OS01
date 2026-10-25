#!/usr/bin/env python3
"""analyse_mouse.py — 解析 QEMU trace，核对"宿主位移 → guest 计数"的映射。

trace 事件格式（QEMU 11.1.2）：
    gd_motion_event        ww=1440, wh=900, ws=2, x=584, y=372
    ps2_mouse_send_packet  0x55b89503e770 x 22 y 4 z 0 bs 0x8

关键：ps2 包与 motion 事件**不是一一对应**——
  * QEMU 把每个 motion 的位移裁到 ±127（`ps2_mouse_send_packet`），余量
    保留到下一个包；
  * 没有抓住指针时（`s->ptr_owner != vc`）相对位移根本不���送，guest 收不到包。
所以必须**按 trace 里的出现顺序**配对，不能按下标对齐两个数组。

配对规则（与 QEMU 的实现同构）：
    pending += fbx[n] - fbx[n-1]           # 宿主累积位移
    发包时 expected = clamp(pending, -127, 127)
    packet.dx 应当等于 expected，且 pending -= expected（余量结转）

用法：analyse_mouse.py [trace.log] [serial.log]
"""
import re
import sys

trace = sys.argv[1] if len(sys.argv) > 1 else "/tmp/qemu-trace.log"
out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/guest-serial.log"

re_host = re.compile(
    r"gd_motion_event\s+ww=(\d+),\s*wh=(\d+),\s*ws=(\d+),\s*x=(-?\d+),\s*y=(-?\d+)")
re_guest = re.compile(
    r"ps2_mouse_send_packet\s+\S+\s+x\s+(-?\d+)\s+y\s+(-?\d+)\s+z\s+(-?\d+)\s+bs\s+(\S+)")

CLIP = 127

host_events = 0
host_scale = set()
packets = 0
mismatch = []
sign_bytes = set()

last_fbx = None
last_fby = None
pending_x = 0          # 宿主累积、尚未发包的 x 位移
pending_y = 0          # 宿主累积、尚未发包的 y 位移（向下为正）
anchored = False       # 第一个包 = 抓取后的事件流起点
host_x_cum = 0         # 由包携带出去的宿主位移
host_y_cum = 0
guest_x_cum = 0
guest_y_cum = 0
match_x_cum = 0        # 只统计"逐包精确匹配"的那些包
match_y_cum = 0
match_x_guest = 0
match_y_guest = 0

for line in open(trace, errors="replace"):
    m = re_host.search(line)
    if m:
        host_events += 1
        host_scale.add(m.group(3))
        fbx, fby = int(m.group(4)), int(m.group(5))
        if anchored and last_fbx is not None:
            pending_x += fbx - last_fbx
            pending_y += fby - last_fby
        last_fbx, last_fby = fbx, fby
        continue
    m = re_guest.search(line)
    if m:
        packets += 1
        dx, dy = int(m.group(1)), int(m.group(2))
        sign_bytes.add(m.group(4))
        if not anchored:
            # 第一个包是"抓取"那一刻的按钮包（位移为 0），从这里开始配对。
            anchored = True
            pending_x = pending_y = 0
            continue
        # QEMU 的 ps2 鼠标对 Y 取负（ps2_mouse_event: s->mouse_dy -= value），
        # 画布 y 向下为正，所以包里的 dy = -Δfby。
        exp_x = max(-CLIP, min(CLIP, pending_x))
        exp_y = max(-CLIP, min(CLIP, -pending_y))
        if (exp_x, exp_y) != (dx, dy):
            mismatch.append((packets, (exp_x, exp_y), (dx, dy), pending_x, pending_y))
        else:
            match_x_cum += exp_x
            match_y_cum += exp_y
            match_x_guest += dx
            match_y_guest += dy
        pending_x -= exp_x
        # y 发出去的是 -Δfby，所以结转余量要用加法（否则余量符号会翻倍）。
        pending_y += exp_y
        host_x_cum += exp_x
        host_y_cum += exp_y
        guest_x_cum += dx
        guest_y_cum += dy

print(f"宿主 motion 事件   : {host_events}   GTK 缩放因子 ws = {sorted(host_scale)}")
print(f"guest ps2 包       : {packets}   符号位字节 = {sorted(sign_bytes)}")
matched = packets - len(mismatch)
print(f"逐包精确匹配        : {matched} / {packets}"
      + (f"   不匹配例：{mismatch[:3]}" if mismatch else "   ← 全部匹配"))
print(f"匹配包的位移和      : 宿主 x={match_x_cum} y={match_y_cum}   "
      f"guest x={match_x_guest} y={match_y_guest}")
print(f"比值 guest/宿主     : "
      f"x={match_x_guest / match_x_cum if match_x_cum else float('nan'):.4f}  "
      f"y={match_y_guest / match_y_cum if match_y_cum else float('nan'):.4f}   (1.0 = 1:1)")
if mismatch:
    print(f"注意：{len(mismatch)} 个不匹配的包是 QEMU 主动丢弃的陈旧位移事件"
          f"（指针经过屏幕边缘 → 边缘分支把 last_set 清零），"
          f"不是速度误差；未匹配的宿主位移也一并从上面的求和里排除。")
print(f"仍挂在宿主侧未发出的位移: {pending_x}  ← 未抓取指针时的位移不会变成包")

# guest 自己的观察（/bin/mousetest 串口输出）
ser = []
for line in open(out, errors="replace"):
    m = re.search(r"\bdx=(-?\d+) dy=(-?\d+) wheel=", line)
    if m:
        ser.append((int(m.group(1)), int(m.group(2))))
if ser:
    print(f"\nguest mousetest 串口侧: n={len(ser)}  dx 和 = {sum(d for d, _ in ser)}")
    if packets:
        print(f"  与 ps2 包数对比: ps2={packets}  mousetest={len(ser)}  "
              f"{'一致' if packets == len(ser) else '不一致'}")
else:
    print("\nguest mousetest 串口侧: 无数据（guest shell 未起来时拿不到）")

# 宿主单事件大位移（假位移 / 快速甩动）
print("\n宿主单事件位移 >127（假位移或快速甩动）：")
print("  见 xdrive 的 --abs 复现：这些事件在抓取之前，不会变成 guest 的包")
