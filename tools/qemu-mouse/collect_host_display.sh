#!/bin/bash
# collect_host_display.sh — 采集宿主的显示链事实，用来区分"物理分辨率"、
# "合成器逻辑空间"和"X11 虚拟屏幕"三个容易混淆的数字。
#
# 用法：./collect_host_display.sh
#
# 结论（PHL 275E9 + GNOME Wayland + mutter 51.1 的实测）：
#   物理分辨率     = 2560x1440   （EDID DTD / DRM fb0，唯一真实值）
#   合成器逻辑空间 = 2048x1152   = 2560 / 1.25（mutter 分数缩放）
#   X11 虚拟屏幕   = 4096x2304   = 逻辑空间 × 2（只支持整数缩放的客户端拿到的 scale）
set -u

echo "════ 1. 面板 EDID 的首选时序（物理分辨率的权威来源）════"
for edid in /sys/class/drm/card*-*/edid; do
    [ -r "$edid" ] || continue
    name=$(edid-decode "$edid" 2>/dev/null | grep -m1 "Display Product Name" || true)
    case "$name" in
        *'PHL'*|*'DELL'*|*'LG'*|*'SAM'*|*'ASUS'*|*'Acer'*)
            echo "-- $edid --"
            edid-decode "$edid" 2>/dev/null | sed -n '/Detailed Timing Descriptors/,/^$/p' | head -8
            ;;
    esac
done

echo
echo "════ 2. DRM 连接器与当前 framebuffer ════"
for f in /sys/class/drm/card*-*/status; do
    printf "  %-42s %s\n" "$f" "$(cat "$f" 2>/dev/null)"
done
for fb in /sys/class/graphics/fb*/; do
    [ -r "$fb/virtual_size" ] || continue
    echo "  fb$(basename "$fb" | tr -d 'fb') name=$(cat "$fb/name" 2>/dev/null) " \
         "virtual_size=$(cat "$fb/virtual_size" 2>/dev/null) " \
         "modes=$(head -1 "$fb/modes" 2>/dev/null)"
done

echo
echo "════ 3. mutter 的 logical monitor（合成器逻辑空间 + 分数缩放）════"
gdbus call --session --dest org.gnome.Mutter.DisplayConfig \
          --object-path /org/gnome/Mutter/DisplayConfig \
          --method org.gnome.Mutter.DisplayConfig.GetCurrentState 2>/dev/null \
  | python3 -c "
import re, sys
s = sys.stdin.read()
m = re.search(r'\[\(0, 0, ([0-9.]+), uint32 (\d+), (\w+),', s)
if m:
    print('  logical monitor: x=0 y=0  scale=%s  transform=%s  primary=%s'
          % (m.group(1), m.group(2), m.group(3)))
else:
    print('  (解析失败)')
m = re.search(r\"'is-current': <true>\", s)
print('  当前模式带 is-current 标记:', bool(m))
"
echo "  分数缩放开关: $(gsettings get org.gnome.mutter experimental-features 2>/dev/null)"
if ! gsettings get org.gnome.mutter experimental-features >/dev/null 2>&1; then
    echo "  分数缩放开关(dconf): $(dconf dump /org/gnome/mutter/ 2>/dev/null | grep -i experimental || echo '(未找到)')"
fi

echo
echo "════ 4. X11 虚拟屏幕（xrandr 看到的尺寸）════"
xrandr --listmonitors 2>/dev/null | sed 's/^/  /'
xrandr 2>/dev/null | grep -E "^Screen|^DP-|^HDMI|^eDP" | head -6 | sed 's/^/  /'

echo
echo "════ 5. 三个数字的算术关系 ════"
python3 - <<'PY'
panel_w, panel_h = 2560, 1440
frac = 1.25                      # mutter 报告的分数缩放
logical_w, logical_h = panel_w / frac, panel_h / frac
int_scale = 2                    # 整数缩放客户端（含 GTK3）拿到的 scale
print(f"  物理分辨率      = {panel_w}x{panel_h}")
print(f"  合成器逻辑空间  = {panel_w:.0f}/{frac} x {panel_h:.0f}/{frac} = {logical_w:.0f}x{logical_h:.0f}")
print(f"  X11 虚拟屏幕    = 逻辑 × {int_scale} = {logical_w*int_scale:.0f}x{logical_h*int_scale:.0f}")
print(f"  guest 1 像素在面板上的实际大小 = {frac} 物理像素")
PY

echo
echo "════ 6. GDK3 自己看到的坐标系（需要先 make gdk_probe）════"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -x "$HERE/gdk_probe" ]; then
    timeout 4 "$HERE/gdk_probe" 2>/dev/null | sed 's/^/  /'
else
    echo "  (未构建 gdk_probe：cd $HERE && make gdk_probe)"
fi
