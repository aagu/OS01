# GUI-2b 公共控件句柄与统一焦点实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** libgui 创建并拥有原生控件，使用不暴露后端的值句柄统一焦点、导航和销毁；桌面只依赖公共 API。

**Architecture:** 每平台一张有界控件表，每 input 一套焦点登记与选择状态。LVGL 组是该状态的投影，不是另一套决策者。公共 API 完全隔离后端类型；raw 诊断登记复用同一引擎，但不能与原生登记混用。

**Tech Stack:** C11、LVGL 9.5.0、Make、真实 LVGL 离屏宿主测试、Python/QMP 集成门禁。

**Spec:** `docs/superpowers/specs/2026-10-25-gui-2b-public-widget-focus-design.md`。

**状态：** 2026-10-25 完成本次两轮独立子 agent 评审，R2明确 **APPROVE**；本计划不授权开始产品实现。替换 v1–v3 的重复实现正文，保留接口、算法约束、测试判据和任务依赖。旧稿备份 `/tmp/gui-2b-plan-v3-before-revision.md` 仅供本次修订追溯，不是实施输入。

## Global Constraints

- 公共头只在 `libgui/include/gui/`，不出现后端符号、类型或宏；不安装私有头，不提供 adopt/unwrap/get-native，不用指针转换伪装身份。
- `gui_widget_t` 为仅含 `uint64_t id` 的值类型；零为 NONE。身份号进程全局递增，平台销毁/重建不重置，不复用、不回绕，耗尽报 EOVERFLOW。
- 原生控件上限 **1024**（根及标签都计入，私有光标不计）；焦点登记上限 **128**（CLICK 也计入）；文件96项加返回按钮共97项须全部可达。
- 平台仍为单例、单 UI 线程。平台/input 指针销毁后不得使用；值句柄不延长对象寿命，不是 IPC 能力。
- 错误：无效参数/enum/未知或失效及外平台身份 EINVAL；不支持的控件操作 EOPNOTSUPP；不可用目标设焦点 EACCES；容量 ENOSPC；原生分配 ENOMEM；禁止的回调重入 EBUSY。int API 为0/-1，错误不部分修改，输出身份清零。
- 原生表与桥接分配失败回滚；后端 fatal OOM 保留已有独立子进程退出契约，不能承诺所有 LVGL OOM 可恢复。
- 父拥有子；destroy 递归销毁、clear 保留容器。对象销毁先使身份不可被公共查询使用，再摘焦点并取消旧键；删除回调结束前不得复用槽位。
- 回调可更新文字/外观/可见性/使能并投递意图；不得创建/删除树、修改布局结构/焦点策略或选择、嵌套 step。平台/input 销毁仍只能在外层主循环。
- UI 销毁只在外层业务意图执行。主循环保留终端刷新，时钟单独节流；终端仍优先消费字符、Enter、历史键，ESC 返回。
- Material 是焦点视觉唯一来源，删除绿色局部覆盖；保留桌面原像素样本、gui-desktop 原断言、光标颜色、raw slider/textarea 交互和 GUI-2 取消协议强度。
- 结构改变后先 `make PROFILE=x86_64-clang clean`；运行 QEMU 前重新解析 `print-run-paths`。所有测试从仓库根 make 入口调用。
- 本期不增加 Window Server、多显示平台、IPC、模态 scope、中文输入或文本编辑器。只补齐现有桌面需要的三类控件与布局。

## Review Focus

1. 旧控件/槽位/业务 context 被复用：旧身份和旧 handler 都不能指向新对象（Tasks 2/3/6）。
2. 按住键时清焦点、删当前项或非当前项：真实接收者、取消状态与公开身份一致，稳定维护不取消键（Tasks 7–9）。
3. MANUAL 首次鼠标选择及 CLICK-only/raw 控件：跨 step、隐藏与恢复不改变声明的策略（Tasks 7–9）。
4. 96项目录重建：容量峰值、版本与行 context 生命周期，不把97项伪装成一次非零焦点检查（Tasks 11/13）。
5. 失败和重入：无效设置不能解除显式清空；回调不能嵌套输入；失败测试也必须释放单例（Tasks 1/6/8/11）。

## 文件与依赖

| 文件 | 责任 |
| --- | --- |
| 新 `libgui/include/gui/gui_widget.h` | 句柄、kind、生命周期、文字/可见/使能、bounds查询 |
| 新 `gui_layout.h` / `gui_event.h` / `gui_focus.h`（同目录） | 原生布局/外观结构、事件、焦点API |
| 新 `libgui/internal/gui_widget_private.h` | 固定容量表、槽状态、跨模块私有入口 |
| 新 `libgui/src/widget.c` / `layout.c` / `event.c` / `focus.c` | 原生对象、布局、事件桥接、公共焦点封装 |
| 改 `gui.h` / `gui_platform.h` / `gui_platform_private.h` / `platform.c` | 汇总头、统计、平台所有权与注册表 |
| 改 `gui_input_private.h` / `input_focus.c` / `input_lvgl.c` | 唯一焦点引擎、模式、投影和取消；保持解码/FIFO路径 |
| 改 `libgui/Makefile` / `hosttests/Makefile` / `mk/components/run.mk` | 目标编译、安装、宿主测试与CASE注册 |
| 新 `hosttests/cases/test_gui_widget.c` / `test_gui_focus.c` | 公共契约；只通过宿主私有fixture接入合成输入 |
| 新 `hosttests/mock/gui_native_fixture.h` / `.c` | 单例fixture、时钟/输入驱动、故障与归属测试辅助 |
| 改现有 GUI input/theme/platform/desktop 测试、运行时mock | 真实后端回归和新模块链接依赖 |
| 改 `user/desktop/*` / `app.mk` | 业务状态保留，控件/布局/事件迁移，删除第二套焦点模型 |
| 改 `user/test_gui_input/test_gui_input.c` | 主题焦点环预期；保留raw诊断例外 |
| 改 `qemutests/gui_build_audit.py` / `gui_desktop_test.py` / `test_gui_desktop_runner.py` | 发布、增量、实际PS/2输入、runner正负控制 |
| 改 `docs/gui/gui.md` / `docs/roadmap.md` / GUI-2验证报告 | 新接口与真实验证证据 |

任务依赖：1→2→3→4→5→6→7→8→9→10→11→12→13。相互依赖的接口在下一节一次定义，任务不重复函数体。每任务结束以列明测试通过为提交条件；不能靠未实现的占位函数或空mock让中间阶段通过。新模块的链接依赖、发布清单在引入它的任务同步更新，Task12负责补充负控制和全链验证。

## 公共接口合同（Tasks 1/4/5/6/7分别落地）

`gui_widget.h` 只依赖平台头及标准整数/布尔类型；`gui_event.h` 引用 widget 头，handler声明只在 event 头，避免循环。所有新头保留现有 `extern "C"` 约定；NONE与默认构造在C11/C++17下均须可编译。

```c
typedef struct { uint64_t id; } gui_widget_t;
typedef enum {
    GUI_WIDGET_CONTAINER = 0, GUI_WIDGET_LABEL, GUI_WIDGET_BUTTON,
    GUI_WIDGET_KIND_COUNT
} gui_widget_kind_t;
typedef struct { int32_t x, y, width, height; } gui_rect_t;

int gui_widget_create(gui_platform_t *, gui_widget_t parent,
                      gui_widget_kind_t, gui_widget_t *out);
int gui_widget_destroy(gui_platform_t *, gui_widget_t);
int gui_widget_clear(gui_platform_t *, gui_widget_t container);
bool gui_widget_is_valid(gui_platform_t *, gui_widget_t);
int gui_widget_get_kind(gui_platform_t *, gui_widget_t, gui_widget_kind_t *out);
int gui_widget_get_bounds(gui_platform_t *, gui_widget_t, gui_rect_t *out);
int gui_widget_set_visible(gui_platform_t *, gui_widget_t, bool);
int gui_widget_set_enabled(gui_platform_t *, gui_widget_t, bool);
int gui_widget_set_text(gui_platform_t *, gui_widget_t label, const char *text);
```

- `GUI_WIDGET_NONE` 为零值构造表达式，不是指针；kind的COUNT哨兵不得省略。`is_valid` 无效身份返回false，不产生结构副作用。
- 只有CONTAINER允许NONE parent，表示该平台active screen上的全屏根；其他kind必须有本平台有效父对象。clear仅CONTAINER，set_text仅LABEL，类型不符EOPNOTSUPP。NULL文字按空串，复制字节串，不借用其内存。
- `get_bounds` 输出当前布局完成后的视口像素矩形，失败输出清零；不暴露对象指针。测试通过它验证几何和滚动布局，不删除原尺寸断言。
- 无效int输出参数先校验指针，再清输出；所有验证完成后才能修改状态。对已销毁平台指针不承诺错误恢复。

### 布局与外观合同

```c
typedef enum {
    GUI_SIZE_MODE_PX = 0, GUI_SIZE_MODE_CONTENT, GUI_SIZE_MODE_FILL
} gui_size_mode_t;
typedef struct { gui_size_mode_t mode; int32_t value; } gui_size_t;
typedef struct {
    int32_t x, y;
    gui_size_t width, height;
} gui_geometry_t;
```

构造使用 `GUI_SIZE_PX(v)`、`GUI_SIZE_CONTENT`、`GUI_SIZE_FILL`，模式枚举必须用MODE前缀，避免宏展开破坏case标签。PX非负，坐标及加减用宽中间类型检查；拒绝任何非法enum及后端无法表达的值，不先截断再验证。

| 类型/字段 | 值与规则 |
| --- | --- |
| `gui_align_t` | TOP_LEFT/TOP_MID/TOP_RIGHT/MID_LEFT/MID/MID_RIGHT/BOTTOM_LEFT/BOTTOM_MID/BOTTOM_RIGHT，0起顺序；COUNT为非法哨兵 |
| `gui_alignment_t` | `anchor`、`int32_t x_offset,y_offset`；相对父内容区 |
| `gui_flow_t` | NONE/ROW/COLUMN；`gui_flow_align_t`为START/CENTER/END |
| `gui_layout_t` | flow、main_align、cross_align、`uint32_t grow`、`int32_t gap,pad_top,pad_bottom,pad_left,pad_right`；间距非负；grow是子项权重（0不增长），须适配后端范围或拒绝 |
| `gui_text_mode_t` | WRAP/CLIP；`gui_text_align_t`为LEFT/CENTER/RIGHT |
| `gui_scroll_t` | `bool horizontal,vertical`、text_mode、text_align；LABEL只接受滚动false，容器/按钮只使用滚动字段，文本字段必须为默认；不使用互相矛盾的wrap/clip布尔对 |
| `gui_variant_t` | ROOT/PANEL/CARD/ACTION/PLAIN；ROOT限根，LABEL限PLAIN，CONTAINER为PANEL/CARD/PLAIN，BUTTON为ACTION/CARD/PLAIN |
| `gui_color_role_t` | 对当前 `gui_theme_colors_t` 每一字段给一个 `GUI_COLOR_<大写字段>`，另有COUNT；完整switch映射，不能靠结构偏移或裸颜色 |
| `gui_appearance_t` | variant、bg/text/border色彩角色、font、radius、elevation主题角色、`bool filled,rounded,bordered`；rounded=false为零圆角，bordered=false为零普通边框；不关闭主题焦点环 |

```c
int gui_widget_set_geometry(gui_platform_t *, gui_widget_t, const gui_geometry_t *);
int gui_widget_set_alignment(gui_platform_t *, gui_widget_t, const gui_alignment_t *);
int gui_widget_set_layout(gui_platform_t *, gui_widget_t, const gui_layout_t *);
int gui_widget_set_scroll(gui_platform_t *, gui_widget_t, const gui_scroll_t *);
int gui_widget_set_appearance(gui_platform_t *, gui_widget_t, const gui_appearance_t *);
```

- DEFAULT构造：layout为NONE/START/START、grow与间距0；scroll为false/false/WRAP/LEFT；appearance为PANEL、SURFACE_CONTAINER/ON_SURFACE/OUTLINE_VARIANT、BODY/MD/ELEVATION_0、filled=true、rounded=true、bordered=false。根创建时覆盖为ROOT、透明、零padding/边框/圆角；标签为PLAIN且透明；按钮为ACTION并取现有主题默认。
- 布局setter独立于外观setter：只有layout控制padding/gap，不让appearance重写布局。CARD普通边框1px，焦点边框2px由主题状态控制。PLAIN+filled=false满足透明顶栏/返回按钮；elevation使用现有shadow width/offset/opa函数，不臆造令牌。
- **FILL为每一轴的父内容区全长**（扣父padding，不扣兄弟占用；共享剩余空间用grow）。PX/CONTENT/FILL独立，FILL不可重设另一轴。普通父、ROW交叉轴、COLUMN交叉轴均有效。
- 后端按轴映射FILL到父内容区百分比尺寸（例如已锁定版本的 `lv_pct(100)`），保存原生尺寸来源；grow另行映射flex权重，不拿grow冒充FILL。父尺寸变化重新布局；先设layout或先设geometry结果相同；FILL→PX/CONTENT清除该轴旧来源。
- CONTENT父与同轴FILL子构成尺寸循环时，相关setter必须在改变任一状态前返回EINVAL；先FILL后把父改CONTENT同样拒绝。根默认PX视口，不受该循环影响。
- `set_geometry`把该控件定位模式设为绝对x/y，`set_alignment`切换为锚点+偏移；两者尺寸来源不互相抹除。在ROW/COLUMN中，子项x/y锚点不控制流位置；grow有效。LABEL可设grow以适配流父；设置其非默认容器flow/padding/gap返回EOPNOTSUPP。
- set_appearance只设普通状态，不能用更高优先级局部样式覆盖FOCUSED/FOCUS_KEY的主题环；Task5用像素观测证明。文字对齐与换行通过set_scroll表达，覆盖桌面现有调用。

### 事件与统计合同

```c
typedef enum { GUI_EVENT_ACTIVATE = 0 } gui_event_kind_t;
typedef struct { gui_event_kind_t kind; gui_widget_t target; } gui_event_t;
typedef void (*gui_event_handler_t)(const gui_event_t *, void *userdata);
int gui_widget_set_event_handler(gui_platform_t *, gui_widget_t,
                                 gui_event_handler_t, void *userdata);
```

ACTIVATE仅BUTTON；LABEL不拦截按钮命中，CONTAINER图标装饰默认不拦截。桥接只有按钮自己的一次CLICKED回调负责投递，原生event.target是按钮身份，不能因子对象冒泡重复投递。userdata只接受业务context地址（测试用`&context`，不能传值句柄）；每槽handler可替换/注销，删除后不投递。

`gui_platform_stats_t` 字段：原生 `format`（XRGB8888/RGB565/ARGB8888/I8/UNKNOWN枚举）、`uint32_t width,height,widget_count`、`uint64_t pool_used,pool_total,pool_max_used,registry_bytes`、`char renderer_name[16]`及三个`uint16_t renderer_version_*`。`int gui_platform_get_stats(gui_platform_t *, gui_platform_stats_t *out)`失败清零。

format读取实际display映射，不硬编码；池已用字节=`lv_mem_monitor_t.total_size-free_size`，峰值=`max_used`，不使用不存在的`used`字段或把`used_cnt`当字节。名字/版本由后端实现填入，不在公共头定义后端宏。注册表预算与4MiB渲染池分别报告。

### 焦点合同

```c
typedef enum { GUI_FOCUS_TAB = 0, GUI_FOCUS_CLICK, GUI_FOCUS_STRONG } gui_focus_policy_t;
typedef enum { GUI_FOCUS_INITIAL_AUTO_FIRST = 0, GUI_FOCUS_INITIAL_MANUAL } gui_focus_initial_t;
typedef enum { GUI_FOCUS_FORWARD = 0, GUI_FOCUS_BACKWARD } gui_focus_direction_t;
int gui_focus_register(gui_input_t *, gui_widget_t, gui_focus_policy_t);
int gui_focus_unregister(gui_input_t *, gui_widget_t);
int gui_focus_set(gui_input_t *, gui_widget_t); /* NONE = 显式清空 */
int gui_focus_get(gui_input_t *, gui_widget_t *out);
int gui_focus_move(gui_input_t *, gui_focus_direction_t);
int gui_focus_set_initial_policy(gui_input_t *, gui_focus_initial_t);
```

- 登记仅BUTTON；重复更新policy保留位置，不占新容量。初始策略只控制自动建立首次焦点，不因为配置策略而偷偷移动已有选择。
- input首次成功登记锁定UNSET→NATIVE或RAW_DIAGNOSTIC；失败不锁定，全注销不解锁。跨模式登记EBUSY；RAW中其他公共焦点API（含initial_policy/get）EOPNOTSUPP、输出NONE。UNSET get为NONE、move/set(NONE)成功，set其他EINVAL。
- raw登记保持原有点击聚焦能力，不把原生TAB-only的flag变换应用到raw textarea/slider；raw仍按登记顺序参与Tab。
- 当前焦点可用性=`在册、存活、自身/祖先可见且未禁用`；Tab候选额外排除原生CLICK。保活、显式set和点击使用可用性，不能用Tab谓词。
- MANUAL未选择时保持NONE；正/反首次Tab分别首/末可用Tab项。AUTO_FIRST首次可用Tab登记建立选择。实际成功的程序、键盘、鼠标选择都设置`ever_selected`；组回调先执行的鼠标路径也必须设置。
- 显式NONE保持到下一次成功选择；后续登记或refresh不复活。所有失败的set（包括有效但未登记的目标）必须保留该标志与原焦点。
- 当前项隐藏/禁用选它之后第一个可用Tab项；注销/删除按**移除前的位置**选择后继（移除后同下标即原后继），无候选NONE。焦点失效后的自动修复找不到候选时保留修复位置，之后候选恢复可继续修复；这不同于主动清空。
- 在CLICK-only组按Tab没有候选，按spec返回成功并清为空；get为0/NONE，不能把“无候选”当API错误。

## 焦点引擎的实施算法边界（Tasks 7–9共用）

1. **唯一选择状态。** 记录条目的原生身份（raw为NONE）、登记时obj、policy、顺序和观察者归属；选择记录含目标、ever_selected、explicit_clear、repair_pending/起点。对象只在仍存活时解引用，删除事件中的目标在该事件结束前仍有效；不能把“注册表已失效”误写成“LVGL对象及回调已经释放”。
2. **一次提交选择。** 统一内部选择入口验证目标、比较旧/新身份、更新选择及历史状态并投影。目标真的变化（含→NONE）或活动键目标被删时，显式调用现有`gui_input_lvgl_mark_cancel`，不依赖LVGL是否发focus callback。停止重复并保证取消完成前不接受新键；实际indev read只在回调外执行。外层API可尝试取消，indev阻塞则保留pending，由pump重试。
3. **投影回调保护。** 内部重建用深度标志屏蔽组回调的中间选择；冻结+remove_all+add可建立真实NONE，再应用最终目标。`lv_group_remove_obj`会在删除当前项时**解除冻结并refocus**，不能靠freeze阻止它。受控删除先将组整体detach到真实NONE，再修改表并重建；不对当前项调用remove_obj后假定后端没有转焦。
4. **稳定维护。** 提前返回须同时满足成员集合正确、选择有效/有意NONE、真实接收者与选择一致、无待修复标志。仅检查成员集合不够。恢复同一选择不得重发取消；不反复清选择记录来恢复后端。组重建、隐藏非当前项、注销非当前项都保持活动键。
5. **删除事务。** 公共destroy/clear在删树前将整棵待删子树标为DESTROYING（公共查验即失效），开始batch；记录旧选择与原顺序起点，组detach。在各DELETE回调中按捕获obj**比较**摘除条目；输入存活时其观察者必须完成摘除。批次结束后选跳过整棵待删子树的后继，提交一次最终投影；槽位在DELETE回调返回后才FREE。注销当前项也必须修复，即使剩余成员已完全匹配。
6. **raw外部删除。** 删除观察者立即摘登记、失效旧选择并标记取消/修复；不递归read indev。若后端在析构中自行refocus，组回调只标脏，不采纳无效/中间目标。下一个外层维护在输入投递前完成投影；不能解引用已从表移除的obj。原生公共删除/注销在返回前提交一致选择；后端回调内只允许延迟实际read。
7. **观察者所有权。** 显式注销必须移除native/raw两类删除观察者。input析构卸载所有仍在册条目的观察者；已注销的不得残留。DESTROYING对象的事件还在执行时可用事件context保存的obj处理卸载，不能只因公开lookup失败就跳过必要清理。平台关闭先停input、拆其组/观察者，再删display，注册表保活到全部DELETE结束。
8. **指针选择。** 原生登记/更新policy即同步CLICK_FOCUSABLE：TAB关闭、CLICK/STRONG开启；未登记原生对象关闭。raw保存既有flag，不重写。LVGL直接组聚焦回调在非投影事务中只接受在册可用目标，经过同一选择提交并记录ever_selected；回调不递归设置同一个group焦点。不把CLICKED兜底作为阻止默认鼠标夺焦的手段。
9. **视觉/输入边界。** 输入层仅设置状态，主题绘制。set_visible/set_enabled在事件回调中可标记dirty，回调结束后的外层维护必须先修复/取消再投递下一个键；外层调用则返回前完成选择修复。只读query可用于业务回调，返回最后已提交状态；禁止的修改一律无部分修改。不能用“一切focus API都EBUSY”拦截get。

以下任务按此合同实现；任何代码不能重新引入依赖LVGL自动refocus的第二套决策。

## 宿主测试基座（Task 1建立，后续任务复用）

- 框架用现有`test_framework.h`，真实平台、主题、输入、PS/2解码、LVGL host archive；复用`GUI_HOST_CFLAGS`、`GUI_THEME_OBJS`、`GUI_INPUT_WRAPS_COMMON`、`GUI_LVGL_ARCHIVE`。先读hosttests现有input-lvgl链接规则，再扩展新源集合，不复制一份可能缺依赖的归档。
- 新 `gui_native_fixture_t` 包含platform、input、root、`uint32_t delay`；`int gui_native_fixture_create(gui_native_fixture_t *, bool with_input)`先`gui_test_reset()`，明确OFFSCREEN 640×480，input为SYNTHETIC；root初始化NONE，由各测试显式创建，空注册表测试不会偷偷多一个根。失败反向清理并保留errno。`void gui_native_fixture_destroy(gui_native_fixture_t *)`幂等释放input→platform，清指针。
- `int gui_native_fixture_step(gui_native_fixture_t *, uint64_t advance_ms)`使用假时钟推进、调用真实`gui_platform_step(platform, &delay)`，要求OK；无sleep。`int gui_native_fixture_keys(gui_native_fixture_t *, const uint8_t *, size_t)`及`int gui_native_fixture_mouse(gui_native_fixture_t *, const mouse_event_t *, size_t)`包含真实私有注入声明，不自行写错误extern，0表示成功、-1传播错误；注入后由测试显式step。mouse_event_t只存在于宿主私有fixture头，不进入新公共头。
- fixture私有头允许后端类型，仅宿主使用；公共契约测试自身只包含`gui/*.h`、标准库、框架和该fixture头，不据此声称整个TU完全无后端依赖。独立纯公共consumer编译/链接探针在Task12证明发布边界。
- 另提供宿主私有故障/计数/表查询fixture；所有新测试注入及身份耗尽设置仅在`OS01_HOST_TEST`编译，**不安装、不导出到目标库**。
- 每个TEST_FUNC使用fixture并保证结束释放；新增连续执行两轮整个widget/focus二进制的验证，发现singleton泄漏。活动键测试必须有真实DOWN→跨step→UP，不能仅检查焦点句柄。

---

## Phase 1：对象基础

### Task 1：公共类型、注册表和可复用fixture

**Files:** 新widget/event公共头、widget私有头、widget.c、native fixture与widget测试；改gui.h、platform私有头、platform.c、libgui/Makefile、hosttests/Makefile、run.mk及发布清单。

**Interfaces:** 落地上文widget类型、`gui_widget_is_valid/get_kind`；私有表有FREE/RESERVED/LIVE/DESTROYING四态、1024槽、count/peak；每槽身份、obj、parent身份、kind、handler/userdata、几何/布局缓存、flags。`gui_widget_registry_init/fini(gui_platform_t *)`仅libgui内部。事件类型先存在供槽声明使用，事件行为到Task6落地。

- [ ] 写 `test_public_widget_types_compile`：COUNT==3；零句柄；C11/C++17 consumer可包含gui.h；无值句柄到void*的隐式传参。写`test_empty_registry_rejects_unknown`：NONE/随机身份false，get_kind返回EINVAL。
- [ ] `make PROFILE=x86_64-clang test-gui-host CASE=widget`，确认失败是缺少新接口/规则而非现有基线失败。
- [ ] 实现固定表与进程全局计数；槽复用必须全字段初始化。平台calloc的注册表随平台存在，不依赖LVGL池；此阶段不得添加未定义的create/event/focus符号调用。补源、CASE=widget、头安装/审计清单及头依赖。
- [ ] 同命令PASS；纯类型consumer用目标编译参数及宿主C/C++分别syntax-check。此阶段编译Task1实际定义的符号，不能提前要求尚未实现的Task2/6导出。
- [ ] 提交：`feat(gui): add native widget identity and registry foundation`；只暂存本任务文件。

### Task 2：创建、父子关系与完整槽初始化

**Files:** widget.c、widget私有头、platform.c、widget测试及对应构建依赖。

**Interfaces:** `gui_widget_create/destroy/clear/set_text/set_visible/set_enabled/get_bounds`；根选择其platform的disp。先预留槽，再建对象、挂注册表观察者，最后发布LIVE/out身份；handler桥到Task6添加。

- [ ] 先写`test_widget_tree_lifecycle`：根填640×480，LABEL/BUTTON子项归属正确，删父使全部旧身份失效，clear保留根；TYPE/非法kind/NULL输出先验证，不创建半对象。
- [ ] 写`test_text_copies_input`（修改传入缓冲后文字像素仍对应旧副本）、`test_slot_reuse_resets_metadata`（旧handler字段、布局和flags全清）、`test_foreign_parent_rejected`。
- [ ] 运行CASE=widget确认这些新增断言失败；实现上述接口。内部DELETE观察者在回调期间标记不可用，回调结束后才放回空闲表；不得提前清光观察者还需要的context。
- [ ] 公开destroy/clear守卫业务回调；LABEL默认不抢命中，装饰CONTAINER默认不可点击；BUTTON可点击。原生创建不使用默认display/global group。
- [ ] CASE=widget及CASE=platform、CASE=lvgl PASS。结构已改变，首次运行前clean；保留原布局断言，新增模块改变实际分配次数时明确更新对应资源计数预期。
- [ ] 提交：`feat(gui): own native widget trees and lifecycle`。

### Task 3：失效、容量、故障与平台关闭

**Files:** widget.c、platform.c、native fixture、widget/平台测试、宿主私有故障桥及审计拒绝列表。

**Interfaces:** 无新公共函数；强化Task2合同。后端删除fixture仅内部执行；公开测试仍通过值句柄判断。

- [ ] 写`test_ids_survive_platform_recreation`、`test_old_id_never_revives_after_slot_reuse`、`test_identity_exhaustion_does_not_wrap`（宿主独立子进程设置末值，不污染后续测试）。
- [ ] 写`test_1024_widgets_and_1025th_enospc`、`test_create_failure_rolls_back_reserved_slot`、`test_invalid_call_leaves_registry_unchanged`；fault覆盖平台表/对象bridge原生分配、观察者安装失败及后端fatal OOM，后者在子进程验证已有fatal退出行为。
- [ ] 运行CASE=widget确认新增断言FAIL；实现回滚、错误输出、DESTROYING→FREE、peak计数。先校验parent/kind/out/重入再消耗资源；失败可以消耗身份号但不可复用它。
- [ ] 平台关闭按spec：取消/停止输入→拆input/观察者/组→删display/原生树→释放表→拆主题→lv_deinit。input单独关闭不删widget，新input不继承登记；焦点行为在Task7补验证。
- [ ] CASE=widget/platform/lvgl PASS，50轮树创建/销毁后native count、LVGL池与分配释放配对恢复基线。
- [ ] 提交：`test(gui): enforce widget identity limits and rollback`。

## Phase 2：桌面所需界面能力

### Task 4：完整的原生几何、布局与文本模式

**Files:** 新gui_layout.h、layout.c；widget槽缓存、gui.h、Makefile、widget测试及同步发布清单。

**Interfaces:** 实现布局合同中的全部枚举/结构及五个setter中的geometry/alignment/layout/scroll；bounds已由Task2定义，布局完成后读回真实矩形。

- [ ] 写`test_size_constructors_compile`：MODE枚举可作为case常量，构造是gui_size_t，两个命名空间不冲突。
- [ ] 写`test_fill_per_axis_and_update_order`：300×200内容区，FILL宽=300/固定高=20；交换设置顺序等价；父改宽到400后子宽=400；ROW/COLUMN主/交叉轴，FILL→PX50，另一轴不变。
- [ ] 写`test_content_fill_cycle_rejected_atomically`：先CONTENT父后FILL子、反向设置都EINVAL；旧bounds和来源不变。写`test_geometry_range_and_enum_validation`：负尺寸、非法enum、整数边界、bounds失败清零。
- [ ] 写`test_flow_grow_padding_and_alignment`：显式PX结果与现有桌面一致；grow分剩余空间，不能与FILL混同。写`test_wrap_clip_and_text_alignment`，通过真实像素/边界判定，而不只断言返回0。
- [ ] CASE=widget先FAIL再实现；使用按轴百分比/原生缓存落地FILL，按上文规则处理布局循环及调用顺序，不给桌面后端逃生口。
- [ ] CASE=widget/lvgl PASS，公共头consumer syntax-check PASS；安装头与目标编译依赖同任务更新。
- [ ] 提交：`feat(gui): add typed geometry layout and text modes`。

### Task 5：外观、角色与主题状态

**Files:** gui_layout.h、layout.c、theme_apply.c（仅必要变体接缝）、widget/theme测试。

**Interfaces:** `gui_color_role_t/gui_variant_t/gui_appearance_t/gui_widget_set_appearance`全部落地，沿用现有gui_theme.h而非复制令牌值。

- [ ] 写`test_all_color_roles_map_to_tokens`，枚举覆盖当前颜色结构每一字段；`test_bad_appearance_is_atomic`覆盖所有非法角色/变体。
- [ ] 写`test_card_plain_root_and_action_pixels`：透明根/面板、1px卡片边框、elevation、正文角色和零圆角都能表达原桌面取样；`test_appearance_preserves_layout_padding`。
- [ ] 写`test_material_focus_style_overrides_base_border`：ACTION与CARD BUTTON的普通/hover/pressed/focused/disabled状态来自同一主题，primary 2px焦点环不能被普通局部边框遮掉。
- [ ] 本任务的状态像素测试使用新增私有`int gui_native_fixture_set_focus_visual(gui_native_fixture_t *, gui_widget_t, bool)`，仅在宿主fixture内解析真实对象并设置/清除后端视觉状态，不创建输入登记，也不调用Task7尚未实现的公共焦点API。Task9再以真实输入验证相同视觉；该helper不安装、不编译进目标库。
- [ ] CASE=widget/theme/theme-apply先FAIL再实现；选择器优先级由真实像素测试决定，不以删断言方式接受状态覆盖。
- [ ] 同case全部PASS。回调内set_appearance允许；结构布局修改仍由guard拒绝。
- [ ] 提交：`feat(gui): express native appearance through Material roles`。

### Task 6：一次事件桥接、回调guard与真实统计

**Files:** event.c、widget.c、platform.c、gui_platform.h、native fixture、widget测试、已有所有链接真实platform的宿主目标及发布审计。

**Interfaces:** 实现事件与统计合同；内部回调深度guard为计数而非易被嵌套覆盖的单个slot指针。每个新对象发布前装一次bridge，不占后端对象user data。

- [ ] 写`test_event_handler_replace_and_unset`：使用有效业务context地址，替换只调用新handler，注销不删widget；真实pointer down/up只产生一次ACTIVATE且target为BUTTON。
- [ ] 写`test_child_label_and_icon_activate_parent_once`与`test_reused_slot_never_calls_old_handler`；后者毁旧按钮/context后复用槽再点击，不设置新handler时旧计数不变。
- [ ] 写`test_callback_mutation_guard`：ACTIVATE中create/destroy/clear/layout/嵌套step返回EBUSY且不部分改变；文字/外观/可见/使能及只读统计允许。公共焦点API尚未实现，其guard断言在Task7添加，不用占位函数凑齐本任务。不使用无差别guard阻断合法外观更新。
- [ ] 写`test_stats_report_real_format_and_pool_bytes`：OFFSCREEN实际XRGB8888、used=total-free、版本/renderer非空、native count准确；失败清输出。不要复制旧稿不存在的mon.used字段。
- [ ] CASE=widget先FAIL再实现；gui_platform_step增加嵌套调用的显式返回GUI_STATUS_ERROR/errno=EBUSY且不锁存永久错误；输入推进不能发生重入。
- [ ] CASE=widget/platform/lvgl/theme-apply PASS；新源加入真实platform依赖的**所有**宿主链接规则（含input-lvgl、desktop-shell），不能只更新widget二进制。
- [ ] 提交：`feat(gui): bridge native events and platform statistics`。

## Phase 3：唯一焦点引擎

### Task 7：登记模式、策略与生命周期事务

**Files:** 新gui_focus.h、focus.c；input_focus.c、gui_input_private.h、widget删除batch接缝、fixture、新test_gui_focus.c及Makefile/run.mk/发布清单。

**Interfaces:** 六个公共焦点函数；私有输入容量128、模式锁定、选择提交、投影深度、删除batch；沿用既有gui_input_focus_*作为内部hooks供raw及native共用，不再复制一套导航数组。

- [ ] 写`test_focus_registration_modes`：两方向混用拒绝；失败登记不锁定，全注销仍锁定；RAW中公共initial_policy/get等EOPNOTSUPP，get输出NONE；UNSET行为符合合同。
- [ ] 写`test_callback_focus_mutation_guard`：已建原生登记的ACTIVATE回调内register/unregister/set/move/initial_policy均EBUSY且选择/登记/策略不变，get允许并返回该已提交按钮身份；此时才依赖本任务新增的公共焦点函数。
- [ ] 写`test_register_duplicate_updates_policy_at_capacity`：128成功、第129项ENOSPC、满表重复成功且位置不变；非BUTTON EOPNOTSUPP、外display/外platform/失效身份EINVAL。
- [ ] 写`test_unregister_current_uses_original_successor`：A/B/C焦点B，注销B→C；同场加入CLICK-only和禁用项验证跳过；只剩一项/零项也一致。fixture检查公开句柄与真实group接收者，不能只看表成员。
- [ ] 写`test_unregister_noncurrent_preserves_active_receiver`：B按下Enter，注销A不取消B，UP只激活B一次；先验证target，再验证PRESSED/取消状态和真实激活。
- [ ] 写`test_native_unregister_input_destroy_widget_destroy`、`test_delete_subtree_skips_all_doomed_descendants`、`test_input_destroy_detaches_all_observers`；单独销毁input后widget仍有效，第二个input无旧登记。
- [ ] CASE=focus先FAIL，再按“焦点引擎算法边界”实现。明确成员匹配不跳过pending修复；不用会自行refocus的remove_obj模拟冻结事务；故障注入登记观察者失败时回滚且不锁模式。
- [ ] 跨display宿主fixture建立第二个私有display/registry归属，测试解析与display双层拒绝；生产第二platform仍EBUSY。测试入口不得变为public adopt。
- [ ] CASE=focus/widget/input-lvgl PASS；raw路径原有删除回调和get_current内部语义保持；目标库不包含新增宿主注入。
- [ ] 提交：`feat(gui): unify native and diagnostic focus registration`。

### Task 8：选择历史、MANUAL、清空与取消协议

**Files:** input_focus.c、focus.c、input_lvgl.c、test_gui_focus.c、test_gui_input_lvgl.c、fixture。

**Interfaces:** 不新增公共API；补齐所有选择来源和取消时序，使用现有400ms/40ms重复及重新就绪协议。

- [ ] 写`test_manual_first_tab_and_shift_tab`、`test_auto_first_and_initial_policy_change`：MANUAL跨step仍NONE，首次正向A/反向C；AUTO首次可用Tab项；改变策略不偷换已有焦点。
- [ ] 写`test_stable_refresh_keeps_active_key`：真实DOWN→三次step→UP；按住期间无ACTIVATE，释放后一次；后台非当前项增删不取消当前键。raw textarea/slider继续验证重复/编辑/拖动。
- [ ] 写`test_clear_cancels_pressed_and_repeat`：Enter DOWN后set(NONE)，公开/后端都NONE；pump后active key/重复/旧PRESSED清除，迟到UP不激活；重新选对象再完整新键能激活。补indev读取阻塞时不允许新键直通的原有测试。
- [ ] 写`test_manual_mouse_selection_repairs_when_hidden`：MANUAL→真实鼠标选B→隐藏/禁用B→C；不是先调用公共set代替点击。组回调先执行时也必须ever_selected=true。
- [ ] 写`test_click_only_focus_survives_step`、`test_hidden_ancestor_and_empty_repair`、`test_failed_set_preserves_explicit_clear`：有效但未登记、隐藏、外display的失败不修改原选择/clear标志。
- [ ] 写`test_destroy_current_during_backend_callback_cancels`：raw callback删除/换焦点，观察者不递归read，不把释放投给新对象。业务ACTIVATE删除仍应被guard拒绝，此fixture不得混淆两种回调。
- [ ] 每个新增测试的清理使用统一fixture；运行CASE=focus/input-lvgl确认FAIL，再实现选择提交显式标记取消与安全边界pump。
- [ ] 同case PASS；每个二进制连续跑两轮PASS，并验证首轮所有测试均执行，不能用单独子进程掩盖遗漏teardown。
- [ ] 提交：`fix(gui): preserve focus selection and key cancellation contracts`。

### Task 9：指针策略、激活与主题色迁移

**Files:** input_lvgl.c、input_focus.c、event.c、相关测试、user/test_gui_input/test_gui_input.c、qemutests/gui_test.py（所有焦点色采样使用点）、构建runner自测。

**Interfaces:** 原生pointer/Enter/Space→同一ACTIVATE；raw保持既有click focus。未登记按钮可点击激活，但不取得键盘焦点。

- [ ] 写`test_tab_only_click_activates_without_focus_steal`：B为键盘焦点，点TAB-only A→A激活一次但B仍接收随后键；原生CLICK/STRONG点击改变焦点，更新policy后flag立即同步；标签/图标点击目标一致。
- [ ] 写`test_raw_textarea_click_focus_is_unchanged`：两raw文本框，点击非当前框再输入只进入新框；raw登记不被TAB-only flag转换影响。
- [ ] 写`test_enter_space_activate_on_release_once`：DOWN及重复make不激活，UP一次；`test_late_up_after_clear_move_delete_is_ignored`真实发送0x9c；随后新DOWN/UP仍正常。
- [ ] 写`test_material_ring_tracks_focus`：用非primary底色的BUTTON（raw诊断可用显式surface容器），先真实渲染再采样primary边框区域，焦点迁移后旧处0、新处>0。保留guest对应像素观测和光标观测，不靠填充色冒充环。
- [ ] CASE=focus/input-lvgl/theme-apply先FAIL；实现同步flag、一次bridge和状态更新。保持已有lv_indev_read按下/释放路径，不用lv_group_send_data替换它来绕开取消协议。
- [ ] 删除输入层绿色宽度/颜色覆盖；host及guest从gui_theme_colors()->primary取预期，扫描runner所有outline采样处同步，不保留旧绿色例外，不放宽像素/行为断言。
- [ ] CASE=focus/input-lvgl/theme-apply PASS；阶段结束执行test-gui-host及SUITE=gui，归档SMP1/2结果。
- [ ] 提交：`fix(gui): unify pointer activation and Material focus feedback`。

## Phase 4：桌面迁移与验收

### Task 10：桌面完整公共API迁移

**Files:** user/desktop/desktop.c、desktop_shell.{c,h}、desktop_apps.{c,h}、app.mk；desktop-shell及desktop-audit测试。

**Interfaces:** `desktop_shell_t *desktop_shell_create(gui_platform_t *, gui_input_t *)`必须两个有效且关联的参数；root/设置body返回gui_widget_t。删除attach_input兼容入口和font指针helper，保留只读desktop_shell_input/platform。保留现有state/registry纯C层，不为它们增加后端依赖。

- [ ] 先改fixture创建合成input，再调用新create；新增`test_shell_requires_matching_input`。MANUAL在任何HOME控件登记前设置；构造失败销毁整棵未完成树并保留errno。
- [ ] 依下表迁移所有后台调用；动作容器用BUTTON，按原构建顺序STRONG登记；删desktop_shell_focus_*、数组、索引和手工CLICKED/FOCUSED。

| 原用法 | 公共替代 |
| --- | --- |
| 默认display/active screen、裸obj/label/button创建 | platform显式root、三种widget_create |
| 绝对位置/尺寸、align/center、flex/grow/padding | geometry/alignment/layout |
| label长模式、text_align、滚动容器 | set_scroll |
| 颜色、字体、卡片阴影/圆角/边框 | appearance角色/variant；根直接绘制wallpaper背景，不再修改后端screen |
| 对象userdata索引与LVGL事件 | 稳定业务context、ACTIVATE回调 |
| label_get_text去重 | 业务dirty或最近提交文本副本 |
| lv_timer时钟 | tick先无条件desktop_app_refresh，再单独节流时钟 |
| display尺寸/格式、内存、后端版本宏 | platform width/height/stats |
| 测试后端get_width/get_height/get_coords | 公共get_bounds，保持原尺寸/像素断言 |

- [ ] 终端原生按键消费优先及状态保留；每轮更新输入/输出，不能随时钟提前return。新增`test_terminal_refreshes_within_clock_throttle`，覆盖输入、Enter输出、ESC再进入历史保留。
- [ ] 保留状态机、四应用、settings状态及所有像素取样。原本卡片容器换BUTTON后使用CARD/PLAIN精确表达原普通态；焦点环由主题接管。
- [ ] 每次整表面切换先在外层成功调用`gui_focus_set(input, GUI_WIDGET_NONE)`，再删除旧根并构建新表面；显式clear状态跨新登记保留，避免上一表面的ever_selected/repair_pending使MANUAL在重建HOME时自动聚焦。新增`test_rebuilt_surface_starts_without_focus`覆盖HOME→APP→HOME及settings重建，首次正/反Tab仍命中该表面首/末项。文件列表的局部替换不做这次表面级clear，仍遵守删除当前位置的后继修复。
- [ ] 移除桌面第三方include与后端头，链接仍为`-lgui -lgfx`（静态库依赖不能因为去LVGL直用而删除gfx）。所有公开控件身份字段清零用NONE而非NULL。
- [ ] CASE=desktop-shell/desktop-state/desktop-audit/widget/focus先FAIL再实现后PASS。迁移成功路径用合成输入走decoder/FIFO/step，不用手调桌面键函数假装完整集成。
- [ ] 提交：`refactor(desktop): use native widgets and the shared focus engine`。

### Task 11：文件导航与97项可达性

**Files:** desktop_shell.{c,h}、desktop_apps.{c,h}、test_desktop_shell.c、fixture。

**Interfaces:** shell私有导航结构含pending、完整绝对路径副本、表面/目录版本。`int desktop_shell_request_file_nav(desktop_shell_t *, const char *absolute_path, uint64_t version)`只复制意图（0接受/忽略后到请求，-1校验失败）；根Back直接投递既有返回HOME意图。`int desktop_shell_apply_pending(desktop_shell_t *)`变为可传播构建失败；主循环错误时清理退出。

- [ ] 回调context含shell、创建时version与目标路径副本；Back只借shell，激活时从当前cwd/version计算父路径。根用`strcmp(cwd,"/")==0`（空串防御性按根），不是只判断空串。字符串拼接截断即拒绝。
- [ ] 第一导航意图占槽，同轮后到不覆盖；既有/后来ESC或表面切换优先并清导航槽。外层验证仍是同一文件表面及版本后才执行。成功提交的版本单调增加，旧行不能引用新条目索引。
- [ ] **分期导航，不要求双份焦点表。** 外层先把目标目录读入暂存纯C条目数组（至多96、目录优先排序），不改变当前cwd/控件/context/焦点；打不开或路径错误则完整保留旧视图。确定new_version后注销/销毁旧列表子树，释放旧行context，再按new_version创建新列表和行并登记；不会同时登记旧97项与新97项。此提交阶段创建/登记失败按spec清理未完成树、返回错误并由主入口退出，不假装回滚到已删除旧树。
- [ ] `int desktop_files_read_entries(const char *path, desktop_files_snapshot_t *out)`纯C暂存；snapshot含路径、条目数及现有entry数组。`int desktop_files_replace_list(desktop_shell_t *, const desktop_files_snapshot_t *, uint64_t new_version)`处理不可逆UI提交。读取目标通过显式snapshot传递，不能重建时仍读取旧cwd。
- [ ] 成功后统一发布新行version、cwd、顶栏副标题和shell目录版本；不可逆阶段不处理新输入，原焦点修复由libgui负责。行context存储在旧widget及handler全部销毁后才能复用；pending意图按值复制，所以不延长旧context寿命。
- [ ] 宿主用`mkdtemp`和标准目录操作建立96个子目录的真实临时树（不是guest FS，也不需要guest FS）；每子目录再含一子目录用于两层返回。只用测试拥有的tmp路径，结束删除fixture文件并销毁平台。
- [ ] 写`test_files_97_items_exact_focus_cycle`：保存业务按钮身份，Back=h[0]、按排序96行=h[1..96]。显式set(NONE)后依真实Tab键流逐项断言h[0]…h[96]，下一Tab为h[0]；清空后反向h[96]…h[0]，下一反向h[96]。统计97个不同非零身份并用Enter进入末项对应目录，不能只检查非零或误把Back称第97项。
- [ ] 写`test_file_navigation_mouse_and_keyboard_two_levels`、`test_stale_nav_and_esc_priority`、`test_nav_long_path_and_open_failure_leave_view`、`test_nav_build_failure_exits_cleanly`。比对cwd与副标题；从子目录Back逐层返回，根Back HOME；新增行立即可用；fault覆盖所有创建/登记步骤。
- [ ] CASE=desktop-shell/focus PASS；测最大页面widget count/registry bytes/LVGL池，97登记成功且表面反复切换不泄漏；提交`fix(desktop): defer file navigation with versioned contexts`。

### Task 12：发布、目标链接、静态和增量门禁

**Files:** gui_build_audit.py、hosttests/cases/test_desktop_audit.c、libgui/Makefile、mk/components/sysroot.mk（仅实际依赖需要处）、hosttests/Makefile、run.mk及harness测试。

**Interfaces:** 公共头精确集合为原四头gui.h/gui_platform.h/gui_input.h/gui_theme.h加widget/layout/event/focus四头。目标归档新增widget.o/layout.o/event.o/focus.o；本计划合同列出的非inline公共函数均须导出。

- [ ] 新源每次引入时已更新安装/manifest；本任务检查sysroot、staging、发布代及依赖闭包每一处清单，不能只更新GUI_PUBLIC_HEADERS。私有头名单加widget_private，新增宿主注入名都列入禁止目标导出规则。
- [ ] 增量门禁对**每个新公共头**逐个touch，证明真实生产/消费者依赖更新、无无关对象全量重编；重复构建不重编；发布精确头集合。沿用编译器依赖观测，不信任可伪造.d文件。
- [ ] 纯公共C/C++consumer包含gui.h，C目标链接引用每个新公共函数并以真实静态库顺序`-lgui -lgfx`解析；负控制缺一个必需定义就失败。归档可有正常后端定义，不要求消灭lv_*。
- [ ] 桌面审计覆盖所有.c/.h与app.mk：禁止后端include/符号、私有头、旧desktop_shell_focus_*、手工聚焦投递和后端地址转换；不使用会误伤gui_focus_move的裸substring `focus_move`。保留颜色/字号/primitive/设备路径/窗口装饰既有审计。
- [ ] 写审计负fixture：遗漏新头、泄漏私有头、缺导出、目标宿主注入、桌面偷偷引入后端、漏单个增量依赖，均确实FAIL。CASE注册同时加widget/focus两个明确runner目标，不只有枚举名。
- [ ] `test-static`、`test-gui-incremental`、CASE=desktop-audit PASS，所有新头C/C++syntax-check通过；提交`test(gui): audit native API publication and incremental dependencies`。

### Task 13：真实键盘门禁与证据

**Files:** gui_desktop_test.py、test_gui_desktop_runner.py、desktop.c/shell（只读诊断模式）、docs/gui/gui.md、docs/roadmap.md、GUI-2验证报告。

**Interfaces:** 原gui-desktop正常启动/画面/Tab+Enter/ESC/退出场景原断言保留。额外诊断启动`/bin/desktop probe <nonce>`，属于应用测试观测，不是公共libgui API；默认正常启动无诊断I/O。

- [ ] 为能断言具体业务焦点而非猜画面，probe只在外层step/业务意图结束后发布只读状态：nonce、单调seq、表面/app id、cwd/副标题、当前gui_focus_get对应业务tag（back、entry绝对路径、launcher app id）及条目数。保存到唯一`/tmp/gui-desktop-<nonce>.state`；严格校验nonce字符/长度，单次写临时文件再原子替换。tag由现有业务context匹配身份得到，不维护另一套导航序列，不调用私有头，不接受输入/控制命令。
- [ ] 每轮外层step递增seq并发布测试状态；runner等待nonce匹配且seq推进后断言。probe不是控件成功输入的替代证据：输入仍是QMP真实PS/2，状态查询只报告公共焦点，原像素门禁依旧验证真实呈现。
- [ ] 新runner `_tap_combo_key(modifier,qcode)`按下modifier→按下目标→释放目标→释放modifier，每边沿沿用KEY_PACE_SECONDS；异常时释放已按键。使用合法`shift`，不造`shift_up`。
- [ ] 新`desktop_keyboard_reverse`：回HOME且确认NONE后，真实Shift+Tab→末launcher业务tag，再Enter→精确末app id；重建HOME后Tab→首tag/首app id。保留像素差、panic与原正常场景断言，不能仅以“两张图不同”认定正确应用。
- [ ] 新`desktop_files_97_focus_items`：先用serial shell创建唯一96子目录fixture，再启动probe桌面；沿真实Tab/Enter走根→tmp→fixture（按tag搜索，最多当前项数+1步）。先复位到可确认的起点Back：导航进入后若非Back，用Tab有界找到Back；从Back起连续96次Tab每次比对排序行路径，下一次回Back；97次Shift+Tab应回原起点Back，而非错误声称回另一项。逐项归档tag/seq、像素截图及QMP转录。
- [ ] 新`desktop_files_two_levels`与`desktop_terminal_enter`：Enter进入确定子目录再下一层，Back逐层且cwd/副标题正确；终端真实键输入及Enter执行，观察输出/截图，ESC回HOME后状态保留。
- [ ] runner单元测试正/负模型：错误组合键、永不移焦、跳过中间一项、始终同一app、旧nonce/seq、错误cwd、无render/进程退出分别FAIL；更新预期场景集合、SMP1/2报告，不让mock为新case无条件PASS。
- [ ] 每次启动记录本次PID，复用现有runner finally/QEMU硬杀/PDEATHSIG与镜像hash保护；原正常场景完成后再运行probe场景，失败同样清理。probe输入设备和独占租约与正常路径一致，不增加libgui宿主注入到guest。
- [ ] 最终依次执行以下命令，读取归档result.json和退出码，不只看末行PASS：

```bash
make PROFILE=x86_64-clang clean
make PROFILE=x86_64-clang test-gui-host
make PROFILE=x86_64-clang test-static
make PROFILE=x86_64-clang test-gui-incremental
make PROFILE=x86_64-clang test-qemu SUITE=gui
make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop
```

- [ ] docs/gui写接口/所有权/容量/焦点及桌面迁移；GUI-2旧报告追加GUI-2b实际执行日期、命令、归档、SMP、资源实测，不覆盖历史；只有真实门禁全部通过才将roadmap标完成。不得预填PASS或沿用计划日期冒充执行日期。
- [ ] 提交`docs(gui): record native widget focus validation and roadmap completion`，完成时报告真实提交与证据；不在未测实现上宣布GUI-2b完成。

## 自查与本轮修订记录

| spec部分 | 对应任务 |
| --- | --- |
| §1–4身份/父子/错误/关闭 | Tasks1–3、7 |
| §5原生控件、布局、事件、统计、回调 | Tasks1/2/4/5/6/10 |
| §6唯一焦点、初始策略、点击/键盘、raw隔离、主题与取消 | Tasks7–9 |
| §7桌面及版本导航 | Tasks10/11 |
| §8–10发布、故障、宿主与QEMU门禁 | Tasks1–13尤其12/13 |
| §11非目标、§12预算/映射/顺序 | 全局约束、合同、任务依赖、Tasks10/11/13 |

v1–v3评审记录均为REQUEST_CHANGES，不能当成批准。已将意见归入可验证规则：尺寸宏分名、槽完整初始化、native/raw观察者卸载、删除当前项强制修复、显式clear标记取消、稳定投影不假取消、CLICK保活、鼠标ever_selected、raw点击flag保留、分轴FILL、fixture统一清理、真实DOWN/UP、97身份遍历、文件版本分期、根路径判断、终端独立刷新、真实组合键与精确业务观测。

本次修订已获得明确APPROVE，计划评审通过。评审通过不等于产品实现或运行时测试已通过。

| 本次修订轮次 | 评审者 | 结论与处理 |
| --- | --- | --- |
| R1 | `gui_plan_revision_r1` | REQUEST_CHANGES：Task6提前依赖Task7焦点API；已把公共焦点guard测试移至Task7，并明确Task5使用私有视觉fixture。主agent另补表面重建显式clear及fixture精确签名。 |
| R2 | `gui_plan_revision_r2` | **APPROVE**：完整核查计划、spec及相关源码，确认R1依赖已解除；未发现需修订的实质P1/P2，任务与验证方案可执行。 |
