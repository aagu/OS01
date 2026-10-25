/*
 * gdk_probe.c — 打印 GDK3 的各坐标系，复刻 QEMU ui/gtk.c 的换算。
 *
 * 用途：判断"宿主指针位移 → QEMU → guest 计数"这条链上，每一环用的是
 * 逻辑像素还是设备像素。QEMU 11.1.2 的 gd_motion_event() 是：
 *
 *     fbx = (motion->x - wx_offset) / vc->gfx.scale_x;
 *     qemu_input_queue_rel(con, INPUT_AXIS_X, fbx - s->last_x);
 *
 * 其中 motion->x 是 GDK 的事件坐标（application/逻辑像素）。本探针打印
 * GDK 报告的所有坐标系，并把同一套算式跑一遍，便于与 QEMU 的 trace
 * （gd_motion_event / ps2_mouse_send_packet）逐条对齐。
 *
 * 编译：make gdk_probe   （需要 gtk+-3.0 开发包）
 * 运行：GDK_BACKEND=x11 ./gdk_probe        # X11 后端
 *       GDK_BACKEND=wayland ./gdk_probe    # 原生 Wayland 后端
 */
#include <gtk/gtk.h>
#include <stdio.h>

/* 与 QEMU 一致的假设：guest 帧缓冲 1440x900（OS01 的 BGA 模式）。 */
#define GUEST_W 1440
#define GUEST_H 900

/* QEMU 的 zoom（gfx.scale_x/y）。窗口化且未缩放时为 1。 */
static double gfx_scale_x = 1.0;
static double gfx_scale_y = 1.0;

static int last_fbx = 0, last_fby = 0, have_last = 0;

static void motion_cb(GtkWidget *w, GdkEventMotion *m, gpointer u)
{
    (void)u;
    int ww_widget = gtk_widget_get_allocated_width(w);
    int wh_widget = gtk_widget_get_allocated_height(w);
    int ww_surface = (int)(GUEST_W * gfx_scale_x);
    int wh_surface = (int)(GUEST_H * gfx_scale_y);
    int wx_offset = 0, wy_offset = 0;

    if (ww_widget > ww_surface) {
        wx_offset = (ww_widget - ww_surface) / 2;
    }
    if (wh_widget > wh_surface) {
        wy_offset = (wh_widget - wh_surface) / 2;
    }

    /* —— 以下与 QEMU ui/gtk.c gd_motion_event() 同构 —— */
    int fbx = (int)((m->x - wx_offset) / gfx_scale_x);
    int fby = (int)((m->y - wy_offset) / gfx_scale_y);

    GdkDisplay *dpy = gtk_widget_get_display(w);
    GdkWindow *win = gtk_widget_get_window(w);
    GdkMonitor *mon = gdk_display_get_monitor_at_window(dpy, win);
    GdkRectangle geo;
    gdk_monitor_get_geometry(mon, &geo);

    /* 指针的设备像素位置：与事件里的逻辑坐标对照，可直接看出比例。 */
    GdkDevice *pd = gdk_seat_get_pointer(gdk_display_get_default_seat(dpy));
    int dev_x = 0, dev_y = 0;
    gdk_device_get_position(pd, NULL, &dev_x, &dev_y);

    /* QEMU 的"指针撞到显示器边缘"判定：x_root 与显示器几何比较。 */
    int xr = (int)m->x_root, yr = (int)m->y_root;
    int at_edge = (xr <= geo.x || xr - geo.x >= geo.width - 1 ||
                   yr <= geo.y || yr - geo.y >= geo.height - 1);

    printf("MOTION local=(%.1f,%.1f) root=(%d,%d) fbx=%d fby=%d delta=(%d,%d) "
           "[gs=%d alloc=%dx%d device=(%d,%d)]\n",
           m->x, m->y, xr, yr, fbx, fby,
           have_last ? fbx - last_fbx : 0, have_last ? fby - last_fby : 0,
           gdk_window_get_scale_factor(win), ww_widget, wh_widget, dev_x, dev_y);
    printf("        monitor_geo=(%d,%d %dx%d) -> edge_warp=%s\n",
           geo.x, geo.y, geo.width, geo.height,
           at_edge ? "YES (QEMU 会回卷指针并丢弃这一次位移)" : "no");
    fflush(stdout);

    last_fbx = fbx;
    last_fby = fby;
    have_last = 1;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    GdkDisplay *dpy = gdk_display_get_default();

    printf("GDK_BACKEND env = %s\n",
           g_getenv("GDK_BACKEND") ? g_getenv("GDK_BACKEND") : "(unset)");
    printf("display name    = %s\n", gdk_display_get_name(dpy));

    int nmon = gdk_display_get_n_monitors(dpy);
    for (int i = 0; i < nmon; i++) {
        GdkMonitor *mon = gdk_display_get_monitor(dpy, i);
        GdkRectangle geo;
        gdk_monitor_get_geometry(mon, &geo);
        printf("monitor[%d] geo=(%d,%d %dx%d) scale=%d\n", i,
               geo.x, geo.y, geo.width, geo.height,
               gdk_monitor_get_scale_factor(mon));
    }

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), GUEST_W, GUEST_H);
    gtk_widget_add_events(win, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(win, "motion-notify-event", G_CALLBACK(motion_cb), NULL);
    gtk_widget_show_all(win);

    while (gtk_events_pending()) {
        gtk_main_iteration();
    }

    GdkWindow *gw = gtk_widget_get_window(win);
    GdkDevice *pd = gdk_seat_get_pointer(gdk_display_get_default_seat(dpy));
    int ox = 0, oy = 0, sx = 0, sy = 0, px = 0, py = 0;
    GdkModifierType mask = 0;

    gdk_window_get_origin(gw, &ox, &oy);
    gdk_window_get_device_position(gw, pd, &sx, &sy, &mask);
    gdk_device_get_position(pd, NULL, &px, &py);

    printf("window alloc    = %d x %d (逻辑像素)\n",
           gtk_widget_get_allocated_width(win), gtk_widget_get_allocated_height(win));
    printf("gdk scale factor= %d\n", gdk_window_get_scale_factor(gw));
    printf("window origin   = (%d,%d)  逻辑像素\n", ox, oy);
    printf("window device   = (%d,%d)  设备像素\n", sx, sy);
    printf("pointer device  = (%d,%d)  设备像素\n", px, py);
    printf("--- 移动鼠标即可看到每个 motion 事件与 QEMU 换算结果 ---\n");
    fflush(stdout);

    gtk_main();
    return 0;
}
