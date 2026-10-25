# GUI-2b：公共控件句柄与统一焦点设计

日期：2026-10-25。状态：已完成三轮独立子 agent 评审，第三轮 **APPROVE**；待用户审阅，不代表已授权实现。

## 1. 目标与决策来源

roadmap 要求公共焦点 API 完全不含 LVGL 符号，桌面删除第二套焦点序列，保留 Material 焦点环及既有键盘门禁。用户进一步选择按长期控件句柄体系考虑，而非名称查询的临时桥梁。

本设计据此提出：**libgui 创建并拥有控件，以原生值句柄公开身份；应用声明焦点策略，libgui 统一管理焦点、导航和销毁。** 控件身份先于焦点登记建立，不能将对象地址转换为 `void *` 或整数来充当句柄。

这是控件基础层加桌面迁移项目，超出“给私有函数加公共声明”的工作量。下文的容量、接口划分、迁移范围和兼容策略是设计者建议，待本草案批准后确定。

完成后应用可仅使用已发布的 `gui/` 头构建桌面、注册焦点、处理激活；LVGL 留在 libgui 后端。首期只实现现有桌面需要的控件与布局能力，不建立完整通用 GUI 框架。

## 2. 现状与约束

- `libgui/src/input_focus.c` 已有 64 项私有焦点记录、注册顺序导航、祖先隐藏/禁用排除、删除回调和按键取消协议。复用它，不引入另一套导航状态。
- `user/desktop/desktop_shell.c` 另有 32 项数组、索引及手工 CLICKED 投递；首个 Tab 才聚焦第一项。现有 libgui 登记首项即自动聚焦，两者不等价。
- `input_focus.c` 用局部样式设置绿色 outline；Material 主题已有 FOCUSED / FOCUS_KEY 样式，需删除输入层的颜色与轮廓样式覆盖。
- 桌面普遍直接创建 LVGL container、label、button，使用对象 user data 存业务标识。只增加一个句柄 typedef 无法解决首次绑定问题。
- 公共 API 位于 `libgui/include/gui/`；所有已发布头均不得含 LVGL 符号。不得给桌面开放 `libgui/internal/`，不得通过新安装的“适配头”绕过审计。
- `gui_platform_create()` 当前只允许一个平台实例，第二次返回 EBUSY。本任务保留该约束；跨 display 防御用宿主私有 fixture 构造，不为测试扩展生产多显示支持。
- 单 UI 线程。原生句柄是进程内对象身份，不是内核能力、IPC 对象或 GUI-5 的 surface 标识。

## 3. 选型

| 路线 | 优点 | 代价与结论 |
| --- | --- | --- |
| 对象名称查询后登记 | 迁移较小 | 需要额外命名、查重和重建规则；名称不是长期控件身份；不采用 |
| 扫描 LVGL 对象树自动收集 | 接入少 | 可点击不等于可键盘聚焦；对象树变化影响顺序；不采用 |
| 原生控件创建与值句柄 | 身份、生命周期、焦点和后端边界一致 | 要补齐桌面所需的公共控件接口并迁移；采用 |

不提供任意现存 LVGL 对象的 public adopt/unwrap/get-native 接口。已有 LVGL 诊断程序仍可使用其既有测试边界，业务桌面逐步迁移到原生接口。原生登记与诊断程序的后端原始对象（下称 raw）登记在同一个 live input 上不得混用，具体边界见 §6.1；它们复用同一焦点引擎，不是两套并行焦点管理器。

## 4. 对象模型与生命周期

### 4.1 原生身份

新增 `gui_widget_t`，公开表示为一个含 `uint64_t id` 的值类型，零为无效值。应用只可保存、复制和比较它，不解释编码。

后端为每个平台注册控件表：`id → 平台归属、对象类型、后端对象、回调及状态`。所有公开控件操作都显式接受仍存活的 `gui_platform_t *`，先查表验证句柄，再解引用后端对象；不接受调用方对象地址。无效/已销毁/其他平台的身份均失败，不能碰目标对象。

首期采用进程全局单调递增的 64 位非零身份号，平台销毁、LVGL deinit 和新平台创建都不重置计数。槽位可复用，身份号永不复用，因此旧句柄不能误指向同地址或同槽位的新对象。这是代次失效语义的一种实现，公共接口不承诺槽位和代次的位布局。计数耗尽返回 EOVERFLOW，不回绕。

初始每平台最多 1024 个原生控件（含 libgui 创建的桌面根容器及所有标签；后端私有光标不占表）；统一焦点表从现有 64 项提高到 **128 项**。桌面文件浏览器已有 `DESKTOP_FILES_MAX_ENTRIES=96`，再加返回按钮至少需要 97 项；128 容量使这 97 项全部可通过键盘访问，不新增分页或减少目录项。达到容量返回 ENOSPC，分配失败返回 ENOMEM。1024/128 是首期资源预算，不是永久 ABI 上限；实施需记录最大目录页面的同时存活控件量、焦点项数与注册表内存开销，并证明未触及任一上限。采用预分配注册表或等价有界分块，查找首期允许有界线性扫描，不引入哈希基础设施。

平台指针沿用既有生命周期约定：平台被销毁后不得再次传入；值句柄的失效检查不使已释放的 C 上下文指针变安全。

### 4.2 创建、父子关系与删除

- 原生根容器由 `gui_widget_create(platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &out)` 创建于该平台 active screen，默认填满视口；不依赖 LVGL 默认 display。
- 非零 parent 必须属于同一平台。父拥有子，删除父递归销毁子；`gui_widget_clear()` 删除子树但保留容器。
- 句柄只是借用身份，不持有引用。复制句柄不会延长对象生命。
- 创建过程先预留表项，再创建后端对象、安装桥接回调，最后发布句柄。失败回滚所有已获得资源，输出清零；不得遗留半注册对象。
- 每个对象挂接 libgui 私有删除观察者。任何后端删除路径都先使原生身份失效，摘除焦点并标记取消活动键；然后才能复用表项。不得向失效的事件目标投递业务回调。
- 普通控件事件使用桥接回调自己的 context，不占用 LVGL 对象 user data，避免与内部机制耦合。
- 平台销毁顺序：停止输入与活动手势 → 拆 input/焦点组及其删除观察者 → 销毁 display/控件树并使全部身份失效 → 释放注册表 → 拆主题 → `lv_deinit()`。注册表在控件删除期间仍有效。
- 单独销毁 input 仅删除焦点登记，不删除平台上的控件；新 input 不继承旧登记。

## 5. 公共控件层：桌面所需的最小完整接口

新增 `gui_widget.h`、`gui_layout.h`、`gui_event.h`、`gui_focus.h`；由 `gui.h` 汇总。公开类型、枚举及结构都由 libgui 定义，数值不复用 LVGL 常量。

核心接口形状如下；返回 int 的操作均为 0 成功、-1 失败并设置 errno：

```c
typedef struct { uint64_t id; } gui_widget_t;
#define GUI_WIDGET_NONE ((gui_widget_t){0})

int gui_widget_create(gui_platform_t *, gui_widget_t parent,
                      gui_widget_kind_t kind, gui_widget_t *out);
int gui_widget_destroy(gui_platform_t *, gui_widget_t);
int gui_widget_clear(gui_platform_t *, gui_widget_t container);
bool gui_widget_is_valid(gui_platform_t *, gui_widget_t);
int gui_widget_set_visible(gui_platform_t *, gui_widget_t, bool);
int gui_widget_set_enabled(gui_platform_t *, gui_widget_t, bool);
int gui_widget_set_text(gui_platform_t *, gui_widget_t, const char *);
int gui_widget_set_event_handler(gui_platform_t *, gui_widget_t,
                                 gui_event_handler_t, void *userdata);
```

首期 kind 为 CONTAINER、LABEL、BUTTON。桌面现有启动器卡片、抽屉行、设置行和返回按钮都用 BUTTON 承载动作，其标签为子对象；非交互面板用 CONTAINER。不依赖 `lv_obj_create()` 的容器恰好能响应 Enter。暂不引入 textarea、原生终端、窗口或可插拔后端注册系统。

业务文字仍存外壳的 C 状态。`set_text` 复制 UTF-8 字节串，首期字形能力不变；不公开后端字符串指针。文件项身份从现有对象 user data 的条目索引迁移到回调 context 中的路径与版本（§7.1）。终端现有 `lv_label_get_text()` 比较改为业务状态持有的 dirty 标志或最近提交文本副本，避免为刷新去重而引入后端字符串读取接口。

布局接口以 libgui 自有结构表达：

| 能力 | 首期契约 |
| --- | --- |
| 几何 | 有符号坐标；宽高为 PX / CONTENT / FILL 显式标签值，不沿用负数魔数；像素尺寸非负，溢出拒绝 |
| 对齐 | 父区域九宫格锚点加偏移；保留现有桌面绝对定位能力 |
| 流布局 | NONE / ROW / COLUMN，grow、主轴/交叉轴对齐，padding 与 gap |
| 滚动 | 独立设置横/纵可滚动性；标签支持 wrap / clip |
| 视觉 | theme role / variant、字号角色、色彩角色、间距/圆角/elevation 令牌；无任意后端样式指针 |

用 `gui_widget_set_layout()` / `set_geometry()` / `set_appearance()` 等有类型的结构接口覆盖这些能力，不逐个转发几十个 `lv_obj_set_style_*`，不做无类型 property bag。结构在公共头给出默认初始化方式；首期完整字段和枚举在实施计划中按桌面调用清单逐项列明，不留任何必须取回后端指针的缺口。桌面像素级几何可以保留原数值；主题色、字体和视觉状态继续由统一主题映射。

公共事件首期为 ACTIVATE（按钮点击或键盘激活）。事件携带 target 值句柄，回调 userdata 仅承载业务 context，不承载后端对象。每控件一个可替换 handler，注销 handler 不删除控件。应用负责 context 活至控件或 handler 注销；删除控件不会再发业务事件。

业务回调执行于 step 中。回调可更新文字/视觉并投递业务意图，但不得创建/删除子树、改变焦点策略、切换焦点或嵌套 step；此类原生操作返回 EBUSY，销毁平台/input 仍遵循既有“只在外层主循环执行”的前置条件。破坏性操作由主循环串行完成。无需为本任务增加通用 deferred-command 队列。

桌面的每秒时钟由现有主循环 `desktop_shell_tick()` 按单调时间节流，再查询 wall clock 更新文本，移除其 LVGL timer 依赖。系统应用从 platform 查询分辨率；新增纯数据的 `gui_platform_get_stats()` 提供渲染格式（原生枚举）及 UI 池使用统计，替代 `lv_mem_monitor()` 等业务侧后端调用。只提供现有诊断需求，不公开 display 或 allocator。

## 6. 焦点 API 与行为

### 6.1 登记和策略

```c
int gui_focus_register(gui_input_t *, gui_widget_t, gui_focus_policy_t);
int gui_focus_unregister(gui_input_t *, gui_widget_t);
int gui_focus_set(gui_input_t *, gui_widget_t); /* NONE = 显式无焦点 */
int gui_focus_get(gui_input_t *, gui_widget_t *out);
int gui_focus_move(gui_input_t *, gui_focus_direction_t);
int gui_focus_set_initial_policy(gui_input_t *, gui_focus_initial_t);
```

policy 为 TAB / CLICK / STRONG（两者），显式登记顺序为 Tab 顺序；CLICK 项不占 Tab 位置但占 128 项登记容量。未登记项没有键盘焦点，LABEL/普通 CONTAINER 首期不可登记，返回 EOPNOTSUPP；容器动作应使用 BUTTON。不存在“所有可点击对象自动登记”。同一对象重复登记更新策略、保持位置且不增加容量。

所有登记/设焦点都以 input 所属 platform 的对象表查验归属，再核对后端 display。禁止从 LVGL 全局默认 display 推断归属。未知、已删除、外平台句柄返回 EINVAL；在 live input 上注销已不在焦点组但仍有效的控件为幂等成功。删除后再注销返回 EINVAL。无效 input 指针仍不在可恢复错误的承诺范围内。

**诊断兼容模式。** input 创建时登记模式为 UNSET，首次成功的公共登记锁定为 NATIVE，首次成功的私有 raw 登记锁定为 RAW_DIAGNOSTIC。失败的登记不锁定模式；注销全部成员也不解锁，只有销毁 input 后新建才能重新选择。NATIVE 中私有 raw 登记失败为 EBUSY；RAW_DIAGNOSTIC 中公共登记失败为 EBUSY，其他公共 `gui_focus_*` 操作失败为 EOPNOTSUPP（带输出句柄的清为 NONE），不能把 raw 焦点伪装成 NONE 或分配原生身份。UNSET 中公共 get 返回 NONE、move 为成功无操作、set(NONE) 成功，其他未登记目标的 set 返回 EINVAL；初始策略可在登记前配置。实现需区分原生入口与私有诊断登记入口，内部共享的导航、current、取消和删除 hook 继续服务两种模式，不把它们误当成 raw-only API。测试保留 raw slider/textarea 交互，并增加两个方向的混用拒绝及注销后仍锁定的用例。

初始策略 AUTO_FIRST 为已有 GUI-2 路径保留默认行为；桌面在构建任何控件之前设置 MANUAL。MANUAL 下登记不自动聚焦，首次 Tab 到首个可用 TAB 项，首次 Shift+Tab 到末项。显式清空也保持无焦点，不能被下一轮 refresh 自动补回。实际焦点因隐藏、禁用、注销、删除而失效时才触发修复：隐藏/禁用选其后第一可用项；注销/删除同样按原登记位置选择其后项，循环寻找；无候选则无焦点。删除批次结束前不向中间选中的对象投递新按键。

NONE 必须同时表示原生逻辑焦点与后端组的真实接收者均为空，且旧焦点视觉与活动键被取消；仅清空 `input->focus.focus` 不满足契约。现有 `focus_apply(NULL)` 没有清除后端组的焦点，不能原样复用；LVGL 的 `lv_group_focus_obj(NULL)` 也是 no-op。实施计划须用后端公开组操作建立真实无焦点状态，并验证显式清空后 Enter/Space 不产生任何 ACTIVATE。

首期不公开重排函数；需要重排可在外层主循环注销后重新登记到尾部。平台根、子树和焦点策略彼此独立，不新增 public focus-scope 对象；每 input 仍只有一组，未来模态 scope 可在此基础上独立设计。

### 6.2 输入与视觉

- libgui 唯一处理未被应用消费的 Tab / Shift+Tab，识别左右 Shift；按原登记顺序循环并跳过自身或祖先隐藏/禁用的项。
- CLICK / STRONG 项被指针点击时，统一更新同一个焦点状态；TAB-only 的点击可激活动作但不夺取键盘焦点。点击子标签按所属按钮处理。焦点视觉和实际 keypad 接收者不得分离。
- Enter / 非文本控件上的 Space 沿既有 keypad 按下/释放路径激活 BUTTON，桥接为一次 ACTIVATE；不由桌面手工发 CLICKED，不把键盘和鼠标路径分别调用业务 handler。按住不重复激活。
- 焦点变化、控件删除与隐藏/禁用继续使用 GUI-2 的取消/重新就绪协议，不在 LVGL 回调里递归读 indev，不把旧键的释放变成新控件的 click。
- 输入层仅维护焦点状态与来源；删除绿色 outline 常量及局部样式设置。主题继续统一给出 Material primary 轮廓。焦点原因与是否绘制键盘环保存在输入层；首期维持桌面现有可见焦点效果，新增状态不改变令牌值。
- 主题色迁移必须同时修改 `hosttests/cases/test_gui_input_lvgl.c` 的 `FOCUS_OUTLINE_COLOR` 与 `user/test_gui_input/test_gui_input.c` 的 `PROBE_OUTLINE_COLOR` 及其私有常量一致性检查。两者从公共主题令牌取得 primary 颜色，不保留绿色输入层例外。宿主仍须证明“有焦点时有轮廓、无焦点时无轮廓、转移后旧轮廓消失而新轮廓出现”；guest 保留对应像素观测及取消/交互断言。此次只更新 GUI-2 焦点环的预期颜色，桌面既有像素取样预期与 gui-desktop 原断言不改变，光标资源颜色契约不改变。
- 不公开 `activate_current()`：真实激活来自输入，应用若主动执行业务动作直接调用自己的命令函数。
- 终端壳内应用现有原生回调仍先消费 Enter、字符与历史键；ESC 仍投递返回意图。通用按钮焦点不能破坏这条业务优先契约。

## 7. 桌面迁移

1. 调整创建依赖：`desktop_shell_create(platform, input)` 在构建 HOME 前持有借用 input 并设置 MANUAL，删除后挂接时机。测试同步创建合成 input。
2. 把 shell/apps 持有的 `lv_obj_t *` 全部换为 `gui_widget_t`，控件创建、布局、主题、文字和事件均走公共 API。应用业务状态、文件遍历、命令执行不改变。
3. 动作容器改为原生 BUTTON；创建后按原来的顺序 STRONG 登记。失败必须向上传播并销毁未完成子树，不能静默失去键盘可用性。
4. 删除 `desktop_shell_focus_*`、focus 数组/索引、focus_clear/move/activate 及手工 FOCUSED 状态。原测试直接使用公共焦点 API 查询目标身份，不保留同名兼容包装。
5. 表面切换和文件列表清空依靠父子销毁自动摘除焦点；外壳只清理保存的业务控件身份为 NONE，不维护另一份存活清单。
6. 移除桌面 LVGL 头、APP_CFLAGS 中的第三方 include、字体指针 helper 和 timer；主循环、系统信息、测试探针按 §5 改用原生能力。
7. 保留既有 HOME/DRAWER/SETTINGS/APP 状态机、应用注册表和跨表面业务状态。原像素样本和 QEMU 键盘断言保持原值与强度。

### 7.1 文件导航必须经过外层业务意图

现有 `files_row_cb → desktop_files_enter → files_refresh` 与文件应用的顶栏返回会同步重建列表，必须一并迁移。ACTIVATE 回调不再直接调用会读目录、删除列表或创建新行的函数。

- 每行的业务 context 保存所属 shell、创建时的表面/目录版本及目标绝对路径副本，不能只保存可在刷新后复用的条目索引。
- 顶栏 Back 在列表重建时继续存活，其 handler 只借用 shell，不保存创建时的目录版本或父路径。每次 ACTIVATE 都读取 shell **当前** cwd 与业务版本，当场计算并复制父目录导航意图；当前处于根目录时则投递回 HOME。这样连续进入多层目录后不会沿用最初的父路径。
- 回调把完整路径和版本**按值复制**到 shell 持有的单槽待处理文件导航意图，不把行 context 指针或控件句柄作为稍后导航依据。第一条意图占用待处理槽，同一轮后来的导航激活不覆盖它；已有表面切换/ESC 意图优先，执行表面切换时清除文件导航意图。
- 每次表面切换或文件列表成功更新都会推进业务版本。主循环在 `gui_platform_step()` 返回后，确认仍是同一文件应用表面及目录版本，再验证目标路径能否打开，执行读取/排序/列表重建。成功导航还需在同一个外层更新中同步 cwd 和顶栏路径副标题，再使新列表 context 的版本生效；Back 下次激活读取的就是该新状态。过期意图丢弃；不能用旧索引访问新 `files_entries`。业务版本只在外层推进，不在输入投递中修改条目集合。
- 路径缓冲区复用现有 `DESKTOP_FILES_PATH_MAX`；拼接或复制会截断则拒绝导航并保留当前目录，不尝试打开截断路径。目标不可打开时保留当前目录与列表。控件构建/焦点登记失败按 §7 第3项销毁未完成树并向主循环返回错误，主入口清理退出，不留半个可交互列表。
- 行 context 由 shell 的当前列表存储持有，直到旧列表的 handler 被注销/控件被销毁后才可释放或复用；待处理意图已持有副本，因此无需延长旧行 context 的生命周期。

读取上限仍为 96，保持现有目录优先排序及跨表面 cwd 状态；不引入分页。以 96 项目录验证 97 个焦点项全部成功登记，最后一项仍可经 Tab/Shift+Tab 到达。

## 8. 内部实现边界与分阶段交付

新增源与内部头对称放置于 `libgui/src/widget*.c`、`layout.c`、`event.c` 及 `libgui/internal/gui_widget_private.h` 等位置；公开头只在 `libgui/include/gui/`。后端对象解析仅供 libgui 内部模块使用。

建议四个可独立验证的交付阶段，全部完成才关闭 GUI-2b：

1. **对象基础**：值句柄、表、原生创建/删除、父子生命周期及归属校验。
2. **桌面所需界面能力**：三类控件、布局/主题/事件、原生平台统计；完成无需后端头的示例测试。
3. **统一焦点**：公共登记/策略、128 项容量、初始策略、鼠标与键盘统一、主题接管视觉；GUI-2 宿主与 guest 探针的颜色预期同步迁移，取消协议与像素观测强度保留。
4. **桌面切换与门禁**：迁移桌面及测试、删除第二套模型、审计/staging/增量构建、文档与真实 guest 验证。

更新 libgui Makefile 的源、安装头及 manifest；同步 sysroot 单写者依赖与 `qemutests/gui_build_audit.py` 的精确头清单、必需导出符号、增量检查。宿主测试注入不进入目标库。现有 `libgui.a` 仍包含后端实现，审计区分“禁止公共头暴露”与“归档内正常存在后端符号”，不错误要求全部 `lv_*` 定义消失。

## 9. 错误与重入契约

| 情形 | 处理 |
| --- | --- |
| NULL 必填参数、非法 enum、未知/失效/外平台句柄 | -1 / EINVAL，输出句柄清零，已有状态不改变 |
| 操作与控件类型不匹配 | -1 / EOPNOTSUPP（使用目标 libc 已有定义，不新增 ENOTSUP 别名） |
| 不可用项请求 focus_set | -1 / EACCES，不改变当前有效焦点 |
| 焦点/控件容量耗尽 | -1 / ENOSPC；重复登记先判定，不误报容量 |
| 注册表/业务桥接分配失败 | -1 / ENOMEM，原子回滚 |
| 身份号耗尽 | -1 / EOVERFLOW，旧身份绝不复用 |
| 回调中请求禁止的结构/焦点修改 | -1 / EBUSY，无部分修改 |
| input 已锁定另一种焦点登记模式 | 另一种登记 -1 / EBUSY；RAW_DIAGNOSTIC 中其他公共焦点操作 -1 / EOPNOTSUPP，输出 NONE |
| 无可导航对象 | focus_move 返回 0，当前焦点为 NONE；focus_get 成功输出 NONE |

由于 LVGL 本身的内存分配可能触发既有 fatal assertion，不承诺整个后端所有 OOM 都可恢复。原生表/桥接的分配失败必须可回滚，后端 fatal OOM 保留既有进程退出契约并单独测试，不能把它写成可恢复 ENOMEM。

## 10. 验证与完成条件

使用真实 LVGL 的离屏宿主测试验证可观察结果；表和错误分支可以纯 C fixture 测试。私有 fixture 仅供测试，不能新增 public adopt/native-object 入口。

- 身份：创建/查询/销毁，父子递归删除、clear 容器保留、平台重建旧身份失效、槽位/地址复用不复活、伪造/零/外平台身份拒绝、容量和计数耗尽。
- 故障：注册表预留、桥接 context、后端创建各阶段失败；无半对象、无残留回调；后端 fatal OOM 独立子进程测试。
- 焦点：重复登记/更新策略/注销；AUTO_FIRST 与 MANUAL；首次正反 Tab；隐藏/禁用祖先；注销/删除当前项；128 项成功、第129项 ENOSPC、满表重复登记仍成功；点击子标签、策略差异、焦点实际接收者一致；显式清空后组内真实无焦点且 Enter/Space 不激活；UNSET/NATIVE/RAW_DIAGNOSTIC 模式及失败登记/全注销后的边界。原 GUI-2 的容量测试同步使用新统一上限，保留超限失败断言。
- 激活：真实合成键流及指针流各激活一次；按住 Enter、释放前切焦点、删除目标、整个表面替换不能产生幽灵激活；原生事件 target 仍为有效按钮身份。
- 生命周期：先删除控件/先删除 input/平台兜底销毁；业务回调禁止操作返回 EBUSY；多轮表面和文件列表重建后控件、焦点及池占用稳定。
- 跨 display：宿主内部建立两个后端 display/归属 fixture，覆盖注册阶段与后端解析阶段的拒绝；另保留生产 singleton EBUSY 测试，不能声称已有多显示支持。
- 桌面：原状态机和像素契约继续成立；测试只从公共 API 取得焦点与布局信息。键盘集成通过合成输入进入现有 decoder/FIFO/step，禁止直接调用桌面按键 helper 冒充完整输入链。覆盖 96 项目录的全部97项焦点、鼠标/键盘连续进入至少两层子目录再逐层返回、根目录返回 HOME，以及同轮重复激活、待执行前 ESC、旧版本意图、超长路径和打不开的目标；同时断言 cwd 与路径副标题一致，回调期间没有结构修改，旧 context 不被稍后引用。
- 静态门禁：公共头无 LVGL 符号；目标归档有新必需 API、无 host-only 注入；桌面无 LVGL/私有头、无 `desktop_shell_focus_*`、无后端指针转换；增量 staging 发布精确头清单。

实施后的验证入口：`make PROFILE=x86_64-clang test-gui-host`、`test-static`、`test-gui-incremental`，以及 `make PROFILE=x86_64-clang test-qemu SUITE=gui` 和 `SUITE=gui-desktop`（SMP 1/2）。既有 gui-desktop 键盘场景的原断言不放宽，增加 Shift+Tab/切换表面/终端 Enter 场景。结构变动后按仓库要求先 `make clean`，再解析新的 run paths。

完成时更新 `docs/gui/gui.md`、roadmap 及 `docs/superpowers/reports/2026-10-22-gui-2-validation.md`，以新增 GUI-2b 节记录新执行日期、命令、日志与结果，不把原 GUI-2 历史证据改写为新结果。

## 11. 不在本期的内容与后续扩展

本期不增加 Window Server、IPC、共享 surface、多显示平台、模态 scope、任意控件库、原生文本编辑器、中文输入或后端插件系统。句柄可作为以后这些能力的本地控件身份，但不能跨进程传输或冒充权限凭证。

后续扩展通过新增 kind、类型安全接口或独立组件推进；不依赖名称作为身份，也不要求将来继续暴露 LVGL 类型。当前 1024/128 限额将来可在实现侧调整，但修改时必须保留桌面97项的最低需求、显式容量失败和边界测试。

## 12. 实施计划需细化的事项

在本设计批准后，实施计划逐项列出桌面现有后端调用到原生能力的映射、布局/appearance 结构字段、widget 内存预算、源/头/测试/审计触点、提交顺序与每阶段验证命令。内部表查找算法、文件进一步拆分及测试 fixture 组织由实施者决定，不改变本文外部行为。不得开始实现后才用后端逃生接口填补缺失能力。

## 13. 子 agent 评审记录

评审日期均为 2026-10-25。每轮由不同的子 agent 只读核对 spec 与相关源码，主 agent 修订后再进入下一轮。

| 轮次 / 评审者 | 结论 | 意见与处理 |
| --- | --- | --- |
| 1 / `gui_spec_review_r1` | REQUEST_CHANGES | 解决文件97项需求与焦点容量冲突（改128）；补文件导航意图和context生命周期；错误码采用现有EOPNOTSUPP；明确宿主及guest的主题色迁移，不放宽行为观测 |
| 2 / `gui_spec_review_r2` | REQUEST_CHANGES | 确认前四项解决；进一步明确存活Back按钮读取当前cwd/版本、更新路径副标题，以及原生/raw登记模式锁定与混用错误；补真实无焦点约束并修正终端文本读取说明 |
| 3 / `gui_spec_review_r3` | **APPROVE** | 独立核查上述修订及当前代码，未发现阻碍制定实施计划的实质问题；§12所列布局字段、调用映射、预算与具体组操作可在实施计划中细化 |

评审通过仅针对设计可实施性，不代表运行时测试已经通过，也不替代用户对设计和后续实现的批准。
