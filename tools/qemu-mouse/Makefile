# tools/qemu-mouse/Makefile — 宿主侧指针取证工具（host-only）
#
# 这些工具只在**宿主机**上运行，用来测量 QEMU 窗口化运行时"宿主指针位移
# → guest 鼠标计数"的链路。它们不是 OS01 构建的一部分，因此不挂进根
# Makefile，也不参与 profile 依赖。
#
# 可选依赖：gtk+-3.0 开发包（gdk_probe）、libX11 + libXtst（xdrive）。
# 缺依赖时 make 会给出明确提示，而不是硬失败。
HOST_CC ?= cc
CFLAGS  = -Wall -Wextra -O2 -std=c11
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)
X_LIBS     := -lX11 -lXtst

.PHONY: all gdk_probe xdrive clean check-deps

all: gdk_probe xdrive

gdk_probe: gdk_probe.c
	@if [ -z "$(GTK_LIBS)" ]; then \
	    echo "ERROR: missing gtk+-3.0 development files (install gtk3-devel)"; exit 1; fi
	$(HOST_CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ $< $(GTK_LIBS)

xdrive: xdrive.c
	$(HOST_CC) $(CFLAGS) -o $@ $< $(X_LIBS)

check-deps:
	@missing=0; \
	for cmd in qemu-system-x86_64 edid-decode gdbus xrandr python3; do \
	    command -v $$cmd >/dev/null 2>&1 || { \
	        echo "WARN: missing $$cmd"; missing=1; }; \
	done; \
	pkg-config --exists gtk+-3.0 || { echo "WARN: missing gtk+-3.0 development files"; missing=1; }; \
	ls /usr/lib*/libXtst.so* >/dev/null 2>&1 || { echo "WARN: missing libXtst"; missing=1; }; \
	test $$missing -eq 0 && echo "deps ok"

clean:
	rm -f gdk_probe xdrive
