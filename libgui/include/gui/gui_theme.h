#ifndef _GUI_THEME_H
#define _GUI_THEME_H

/*
 * gui_theme.h — Material 设计令牌表（GUI-3）
 *
 * 本头是 OS01 用户态 GUI 的**唯一**视觉真源：颜色角色、字号层级、间距、
 * 圆角与 elevation 全部由这里定义，界面代码不得出现裸颜色字面量与裸字号。
 *
 * 设计约束：
 *   - **不包含任何 LVGL 头文件**。公共 sysroot 不发布 LVGL 头，libgui 的
 *     公共 ABI 因此必须与 LVGL 解耦；需要 LVGL 类型时只做前置声明
 *     （见 gui_lvgl.h）。
 *   - 只依赖 <stdint.h>，因此可以被宿主测试直接编译。
 *   - 所有取值都是**只读全局常量**：调用方不得修改。
 *
 * 颜色一律以 8 位十六进制的 0xAARRGGBB 表示，且 A（最高字节）恒为 0，
 * 即 0x00RRGGBB 的写法；这与 gui_platform_get_pixel() 的返回值
 * （0x00FFFFFFu 掩码后的结果）可直接比较。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Material 3 浅色色彩角色表
 *
 * 角色名沿用 Material 3；success 是 OS01 扩展角色（Material 无对应项），
 * 用于「正常 / 就绪」类语义。on_* 角色表示「该底色之上应使用的颜色」。
 */
typedef struct {
    uint32_t primary;                  /* #0B57D0 主色 */
    uint32_t on_primary;               /* #FFFFFF 主色之上的文字 */
    uint32_t primary_container;        /* #D3E3FD 选中容器 */
    uint32_t on_primary_container;     /* #041E49 选中容器之上的文字 */
    uint32_t secondary;                /* #00639B 次强调 */
    uint32_t on_secondary;             /* #FFFFFF 次强调之上的文字 */
    uint32_t secondary_container;      /* #CDE5FF 次强调容器 */
    uint32_t on_secondary_container;   /* #001D33 次强调容器之上的文字 */
    uint32_t tertiary;                 /* #8B5000 提示强调 */
    uint32_t on_tertiary;              /* #FFFFFF 提示强调之上的文字 */
    uint32_t tertiary_container;       /* #FFDDB6 提示容器 */
    uint32_t on_tertiary_container;    /* #2C1600 提示容器之上的文字 */
    uint32_t error;                    /* #BA1A1A 错误 */
    uint32_t on_error;                 /* #FFFFFF 错误之上的文字 */
    uint32_t error_container;          /* #FFDAD6 错误容器 */
    uint32_t on_error_container;       /* #410002 错误容器之上的文字 */
    uint32_t success;                  /* #146C2E 正常 */
    uint32_t on_success;               /* #FFFFFF 正常之上的文字 */
    uint32_t success_container;        /* #C4EED0 正常容器 */
    uint32_t on_success_container;     /* #00210B 正常容器之上的文字 */
    uint32_t surface;                  /* #FDFCFF 页面底色 */
    uint32_t on_surface;               /* #1A1C1E 页面底色之上的主要文字 */
    uint32_t surface_variant;          /* #E0E2EC 芯片 / 次级填充 */
    uint32_t on_surface_variant;       /* #44474F 次要文字 */
    uint32_t surface_container_low;    /* #F7F2FA 低层卡片表面 */
    uint32_t surface_container;        /* #F3EDF7 默认卡片表面 */
    uint32_t surface_container_high;   /* #ECE6F0 高层卡片表面 */
    uint32_t surface_container_highest;/* #E6E0E9 最高层卡片表面 */
    uint32_t outline;                  /* #74777F 轮廓 */
    uint32_t outline_variant;          /* #C4C6D0 细分割线 */
    uint32_t inverse_surface;          /* #2F3033 反色表面 */
    uint32_t inverse_on_surface;       /* #F1F0F4 反色表面之上的文字 */
    uint32_t scrim;                    /* #000000 遮罩 */
    uint32_t shadow;                   /* #000000 阴影基色 */
} gui_theme_colors_t;

/**
 * @brief 字体层级（字号自大到小）
 */
typedef enum {
    GUI_THEME_FONT_DISPLAY  = 0,  /* 24：应用大标题、数值 */
    GUI_THEME_FONT_HEADLINE = 1,  /* 20：表面标题 */
    GUI_THEME_FONT_TITLE    = 2,  /* 16：卡片标题、列表项标题 */
    GUI_THEME_FONT_BODY     = 3,  /* 14：正文 */
    GUI_THEME_FONT_LABEL    = 4   /* 12：状态栏、辅助说明 */
} gui_theme_font_t;

/**
 * @brief 间距刻度（4 的倍数）
 */
typedef enum {
    GUI_THEME_SPACE_XS = 0,  /* 4  */
    GUI_THEME_SPACE_SM = 1,  /* 8  */
    GUI_THEME_SPACE_MD = 2,  /* 16 */
    GUI_THEME_SPACE_LG = 3,  /* 24 */
    GUI_THEME_SPACE_XL = 4   /* 32 */
} gui_theme_space_t;

/**
 * @brief 圆角刻度
 */
typedef enum {
    GUI_THEME_RADIUS_SM   = 0,  /* 4  */
    GUI_THEME_RADIUS_MD   = 1,  /* 8  */
    GUI_THEME_RADIUS_LG   = 2,  /* 12 */
    GUI_THEME_RADIUS_XL   = 3,  /* 16 */
    GUI_THEME_RADIUS_FULL = 4   /* 28 */
} gui_theme_radius_t;

/**
 * @brief elevation 层级（0 = 无阴影，最高 5）
 */
typedef enum {
    GUI_THEME_ELEVATION_0 = 0,  /* 页面底 */
    GUI_THEME_ELEVATION_1 = 1,  /* 启动器条 */
    GUI_THEME_ELEVATION_2 = 2,  /* 卡片 */
    GUI_THEME_ELEVATION_3 = 3,  /* 抽屉、浮层 */
    GUI_THEME_ELEVATION_4 = 4,  /* 对话框 */
    GUI_THEME_ELEVATION_5 = 5   /* 最高层浮层 */
} gui_theme_elevation_t;

/**
 * @brief 取得只读的色彩角色表
 * @return 指向静态常量的指针；永不返回 NULL
 */
const gui_theme_colors_t *gui_theme_colors(void);

/**
 * @brief 取得某层级字体的字号（像素）
 * @param role 字体层级
 * @return 字号；非法层级返回 -1
 */
int gui_theme_font_size(gui_theme_font_t role);

/**
 * @brief 取得某刻度的间距值（像素）
 * @param role 间距刻度
 * @return 间距；非法刻度返回 -1
 */
int gui_theme_space(gui_theme_space_t role);

/**
 * @brief 取得某刻度的圆角值（像素）
 * @param role 圆角刻度
 * @return 圆角；非法刻度返回 -1
 */
int gui_theme_radius(gui_theme_radius_t role);

/**
 * @brief 取得某 elevation 的阴影宽度（像素）
 * @param role elevation 层级
 * @return 宽度；非法层级返回 -1
 */
int gui_theme_elevation_shadow_width(gui_theme_elevation_t role);

/**
 * @brief 取得某 elevation 的阴影垂直偏移（像素）
 * @param role elevation 层级
 * @return 偏移；非法层级返回 -1
 */
int gui_theme_elevation_shadow_offset_y(gui_theme_elevation_t role);

/**
 * @brief 取得某 elevation 的阴影不透明度（0..255）
 * @param role elevation 层级
 * @return 不透明度；非法层级返回 -1
 */
int gui_theme_elevation_shadow_opa(gui_theme_elevation_t role);

/**
 * @brief 取得某 elevation 对应的表面色
 * @param role elevation 层级
 * @return 0x00RRGGBB 颜色；非法层级返回 0xFFFFFFFFu
 */
uint32_t gui_theme_elevation_surface(gui_theme_elevation_t role);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_THEME_H */
