#!/bin/bash
# measure_mouse.sh — 端到端测量"宿主指针位移 → guest 鼠标计数"的比例。
#
# QEMU 侧同时开两个 trace 事件：
#   gd_motion_event        (ww, wh, ws=gtk 缩放因子, x=fbx, y=fby)   [宿主侧]
#   ps2_mouse_send_packet  (ptr, x=dx1, y=dy1, z=dz1, bs=符号位字节)  [guest 侧]
# 二者相除就是 guest 实收计数 / 宿主逻辑位移。
#
# 用法：
#   ./measure_mouse.sh [--work-dir DIR] [--profile PROFILE] [--no-guest]
#
#   --work-dir  产物目录（默认 $TMPDIR/os01-mouse）
#   --profile   构建 profile（默认 x86_64-clang）
#   --no-guest  不启动 guest，只跑 QEMU + 指针驱动（用于只看宿主侧）
#
# 说明：
#   * Xwayland 下 XTest 的 motion 会被合成器忽略，xdrive 用 XWarpPointer 驱动。
#   * QEMU 在相对模式下要**抓住指针**才会把位移送给 guest；抓取由窗口内
#     第一次左键点击隐式开启，因此脚本会先 click 一次。
#   * guest 侧确认需要 shell 起来（/bin/mousetest），本机启动约 60s；
#     宿主侧比例不依赖 guest，脚本超时后仍会保留 trace。
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
OS01_ROOT="$(cd "$HERE/../.." && pwd)"
PROFILE=x86_64-clang
WORK="$(mktemp -d "${TMPDIR:-/tmp}/os01-mouse-XXXXXX")"
WITH_GUEST=1

while [ $# -gt 0 ]; do
    case "$1" in
        --work-dir) WORK="$2"; shift 2 ;;
        --profile)  PROFILE="$2"; shift 2 ;;
        --no-guest) WITH_GUEST=0; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
mkdir -p "$WORK"

TRACE="$WORK/qemu-trace.log"
SERIAL="$WORK/guest-serial.log"
FIFO="$WORK/serial-in"
rm -f "$TRACE" "$SERIAL" "$FIFO"; mkfifo "$FIFO"
# 读写方式打开，避免在 QEMU 挂上读端之前阻塞。
exec 3<>"$FIFO"

if [ "$WITH_GUEST" = 1 ]; then
    FW=$(cd "$OS01_ROOT" && make -s PROFILE="$PROFILE" print-run-paths | sed -n 's/^firmware=//p')
    IMG=$(cd "$OS01_ROOT" && make -s PROFILE="$PROFILE" print-run-paths | sed -n 's/^image=//p')
    [ -r "$FW" ]  || { echo "ERROR: firmware not found: $FW" >&2; exit 1; }
    [ -r "$IMG" ] || { echo "ERROR: image not found: $IMG" >&2; exit 1; }
    echo "firmware=$FW"
    echo "image   =$IMG"
fi

# X11 后端：Xwayland 的 X 屏幕尺寸与 GDK 的缩放因子都由它决定，
# 两后端在逻辑空间上是一致的（见 docs/debug/2026-10-25-qemu-gtk-mouse-pointer.md）。
export GDK_BACKEND=x11

QEMU_ARGS=(-M q35 -smp 2 -m 512 -display gtk -serial stdio -no-reboot
           --trace 'gd_motion_event' --trace 'ps2_mouse_send_packet')
if [ "$WITH_GUEST" = 1 ]; then
    QEMU_ARGS+=(-drive "if=pflash,format=raw,readonly=on,file=$FW"
                -drive "file=$IMG,format=raw,if=none,id=disk" -device ahci,id=ahci
                -device ide-hd,drive=disk,bus=ahci.0
                -netdev user,id=net0 -device e1000,netdev=net0)
fi

qemu-system-x86_64 "${QEMU_ARGS[@]}" < "$FIFO" > "$SERIAL" 2> "$TRACE" &
QPID=$!
echo "qemu pid=$QPID   work=$WORK"

cleanup() {
    kill -TERM $QPID 2>/dev/null
    sleep 1
    kill -9 $QPID 2>/dev/null
    exec 3>&-
}
trap cleanup EXIT

if [ "$WITH_GUEST" = 1 ]; then
    echo "--- 等待 guest shell（启动约 60s）---"
    for _ in $(seq 1 45); do
        sleep 2
        grep -qa "Move mouse" "$SERIAL" && break
        printf '/bin/mousetest\n' >&3
        kill -0 $QPID 2>/dev/null || break
    done
    sleep 3
    printf '/bin/mousetest\n' >&3
    sleep 4
    grep -a "mousetest\|Move mouse" "$SERIAL" | tail -4
fi

sleep 2
"$HERE/xdrive" --probe || true

echo "--- 步骤 1（尚未抓取）：指针移出窗口，再移回，复现假位移 ---"
"$HERE/xdrive" --center || true
sleep 1
"$HERE/xdrive" --abs 60 60 || true
sleep 2
"$HERE/xdrive" --abs 3900 1338 || true
sleep 2

echo "--- 步骤 2：窗口内点击一次（隐式开启相对模式的抓取）---"
"$HERE/xdrive" --center || true
sleep 1
"$HERE/xdrive" --click || true
sleep 1

echo "--- 步骤 3（已抓取）：画布内移动已知距离 +400 逻辑像素 ---"
"$HERE/xdrive" --center || true
sleep 1
"$HERE/xdrive" --dx 400 --dy 0 || true
sleep 2
"$HERE/xdrive" --dx -400 --dy 0 || true
sleep 2

echo "--- 步骤 4（已抓取）：从画布上边界离开再回来（不是屏幕边缘）---"
"$HERE/xdrive" --center || true
sleep 1
"$HERE/xdrive" --abs 2158 250 || true
sleep 2
"$HERE/xdrive" --center || true
sleep 2

echo "--- 解析 ---"
python3 "$HERE/analyse_mouse.py" "$TRACE" "$SERIAL" || true
echo "产物：$TRACE  $SERIAL"
