/*
 * xdrive.c — X11 指针驱动，用于复现 QEMU 的宿主→guest 鼠标链路。
 *
 * Xwayland 下 XTestFakeMotionEvent 会被合成器忽略（指针位置不跟随），
 * 但 XWarpPointer 真实生效，所以这里用 warp 驱动指针。
 *
 * 用法：
 *   xdrive --probe              打印 QEMU 窗口几何与当前指针位置
 *   xdrive --center             把指针移到 QEMU 窗口中心
 *   xdrive --abs X Y            绝对移动（X 根坐标，设备像素）
 *   xdrive --dx N --dy M        相对移动（N/M 为**逻辑**像素，内部按 scale 换算）
 *   xdrive --click              在当前位置发一次左键按下+抬起
 *
 * 注：QEMU 在相对模式下会在窗口内第一次左键点击时隐式开启抓取
 * （ui/gtk.c "relative-mode-click"），抓取之后相对位移才会送到 guest。
 */
/* usleep() 在严格 c11 下被隐藏，需要 _DEFAULT_SOURCE 才暴露。 */
#define _DEFAULT_SOURCE

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static Display *dpy;

/* 逻辑像素 → X 设备像素的换算：X 屏幕 4096x2304 对应逻辑 2048x1152。 */
static int device_scale = 2;

static Window find_qemu_window(Window root, int depth)
{
    char *name = NULL;
    if (XFetchName(dpy, root, &name) && name) {
        int hit = strstr(name, "QEMU") != NULL;
        XFree(name);
        if (hit) {
            return root;
        }
    }
    Window r, parent, *kids = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, root, &r, &parent, &kids, &n)) {
        return 0;
    }
    Window found = 0;
    for (unsigned int i = 0; i < n && !found; i++) {
        found = find_qemu_window(kids[i], depth + 1);
    }
    if (kids) {
        XFree(kids);
    }
    return found;
}

static void report_pointer(Window root, const char *tag)
{
    Window r, ch;
    int rx, ry, wx, wy;
    unsigned mask;
    XQueryPointer(dpy, root, &r, &ch, &rx, &ry, &wx, &wy, &mask);
    printf("%s pointer_root=(%d,%d) 逻辑=(%d,%d)\n", tag, rx, ry,
           rx / device_scale, ry / device_scale);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int dx = 0, dy = 0, abs_x = -1, abs_y = -1;
    int do_probe = 0, do_center = 0, do_click = 0, do_raise = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dx")) {
            dx = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--dy")) {
            dy = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--abs")) {
            abs_x = atoi(argv[++i]);
            abs_y = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--scale")) {
            device_scale = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--probe")) {
            do_probe = 1;
        } else if (!strcmp(argv[i], "--center")) {
            do_center = 1;
        } else if (!strcmp(argv[i], "--click")) {
            do_click = 1;
        } else if (!strcmp(argv[i], "--raise")) {
            do_raise = 1;
        } else {
            fprintf(stderr, "usage: xdrive [--probe|--center|--click|"
                            "--abs X Y|--dx N --dy M] [--scale S]\n");
            return 2;
        }
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xdrive: cannot open display\n");
        return 1;
    }
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);

    Window win = find_qemu_window(root, 0);
    if (!win) {
        fprintf(stderr, "xdrive: QEMU window not found\n");
        return 2;
    }

    XWindowAttributes wa;
    XGetWindowAttributes(dpy, win, &wa);
    printf("qemu_window=0x%lx geom=%dx%d+%d+%d screen=%dx%d\n", win,
           wa.width, wa.height, wa.x, wa.y,
           DisplayWidth(dpy, scr), DisplayHeight(dpy, scr));

    /*
     * 必须把 X 窗口抬到最上层：GNOME 下若某个 Wayland 窗口（常见于
     * 全屏的 Electron 应用）盖在上面，合成器会把指针路由给它，
     * **X 客户端一个 motion 事件都收不到** —— 表现为"warp 成功、
     * 指针位置也变了，但 QEMU 里没有任何事件"。
     */
    if (do_raise || do_center || do_click || abs_x >= 0) {
        XRaiseWindow(dpy, win);
        XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
        XSync(dpy, False);
        usleep(150000);
        printf("raised qemu window 0x%lx\n", win);
    }

    if (abs_x >= 0) {
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, abs_x, abs_y);
        XFlush(dpy);
        usleep(200000);
        report_pointer(root, "abs");
        return 0;
    }

    if (do_probe) {
        report_pointer(root, "probe");
        return 0;
    }

    if (do_center) {
        int tx = wa.x + wa.width / 2, ty = wa.y + wa.height / 2;
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, tx, ty);
        XFlush(dpy);
        usleep(200000);
        report_pointer(root, "center");
        return 0;
    }

    if (do_click) {
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XFlush(dpy);
        usleep(80000);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XFlush(dpy);
        usleep(80000);
        report_pointer(root, "click");
        return 0;
    }

    /* 相对移动：dx/dy 是逻辑像素，按 device_scale 换算成 X 设备像素。 */
    Window r, ch;
    int rx, ry, wx, wy;
    unsigned mask;
    XQueryPointer(dpy, root, &r, &ch, &rx, &ry, &wx, &wy, &mask);
    printf("start_root=(%d,%d)\n", rx, ry);

    int step = 4;                       /* 小步长，保证产生一串真实 motion 事件 */
    int moved_x = 0, moved_y = 0;
    while (moved_x < dx || moved_y < dy) {
        int sx = dx - moved_x; if (sx > step) sx = step;
        int sy = dy - moved_y; if (sy > step) sy = step;
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx == 0 && sy == 0) break;
        XWarpPointer(dpy, None, root, 0, 0, 0, 0,
                     rx + sx * device_scale, ry + sy * device_scale);
        XFlush(dpy);
        rx += sx * device_scale;
        ry += sy * device_scale;
        moved_x += sx;
        moved_y += sy;
        usleep(12000);
    }
    report_pointer(root, "end");
    return 0;
}
