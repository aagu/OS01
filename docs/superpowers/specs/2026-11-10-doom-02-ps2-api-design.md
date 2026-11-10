# D02 — 纯 PS/2 解码器公开接口

日期：2026-11-10。状态：文档评审通过；仅设计，不表示已实施。

[总览与公共约束](2026-11-10-doom-port-design.md) · [配套实施计划](../plans/2026-11-10-doom-02-ps2-api.md)

## 目标、依赖与交付边界

把现有无 IO、无 LVGL 调用的用户态 PS/2 解码器公开给独立图形应用使用。不修改状态机行为，不建立新的输入框架，不依赖 D01 或 Doom 引擎；可单独合入。

## 公共契约

将 `gui_input_ps2.h` 的状态/接口提升到 `libgui/include/gui/`，保留 `gui_ps2_*` 名称与语义。`input_ps2.c` 改为包含公开头。类型继续复用 `gui_key_event_t`，它的公开头只依赖 GUI 公共声明，不引入 LVGL 类型；首版无需为此拆分新输入库。

公开头安装、staging 输入摘要、sysroot 内容校验与 hosttests 包含路径一起更新。Doom 只包含 `<gui/gui_input_ps2.h>` 等安装后的公开头，不加 `-Ilibgui/internal`。静态链接 map 若出现不必要的 LVGL 对象，应先查引用边界。

公开接口保留原签名：

```c
void gui_ps2_init(gui_ps2_state_t *st, uint16_t initial_modifiers);
int gui_ps2_feed(gui_ps2_state_t *st, uint8_t byte, uint64_t now_ms,
                 gui_key_event_t *out);
void gui_ps2_expire(gui_ps2_state_t *st, uint64_t now_ms);
```

`gui_ps2_state_t`、槽位常量及 `gui_ps2_key_slot()` 随公开头一起交付。feed 返回 1 代表真实边沿、0 代表无事件、-1 代表参数错误。E0 物理身份为 `0x100|scan`，真实 UP 沿用 DOWN 身份，重复 make 不生成 DOWN，前缀超时仍为 100 ms。`initial_modifiers` 的调用方前置条件是只传 0 或 GUI_MOD_CAPS，不植入已按住的 Shift/Ctrl/Alt；现有 init 原样保存参数，本次不增加主动过滤行为。公开头必须说明该前置条件，D04 负责把租约 Caps bit0 转换为 GUI_MOD_CAPS 后传入。

## 文件与验收

修改 `libgui/src/input_ps2.c`、内部引用该头的实现/测试、`libgui/Makefile` 和 `mk/components/sysroot.mk` 的实际输入摘要。旧内部头若保留，只转发 `<gui/gui_input_ps2.h>`。不在两处维护类型定义。

沿用 `hosttests/cases/test_gui_input_decode.c` 和 `hosttests/mk/gui.mk` 的生产对象测试；新增仅依赖安装头的独立 TU 编译/链接探针。`make sysroot` 后新头出现在 generation 中；只改新头会刷新 generation 并重编消费者。探针业务调用仅限 gui_ps2_*，链接采用现有目标 crt0、sigreturn、消费者对象、`-lgui -lgfx -lc` 的完整顺序；map 允许目标启动对象与 libc，不能抽取 platform、完整 input 适配层或 LVGL 对象。

完成条件：`make test-gui-host CASE=input-decode`、`make test-gui-build-audit`、`make test-gui-incremental` 通过；公开头探针从 sysroot 编译链接成功。无需硬件/WAD，也不把完整 GUI 平台作为前置依赖。
