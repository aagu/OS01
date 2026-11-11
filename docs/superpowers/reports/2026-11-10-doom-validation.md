# OS01 Doom QEMU 集成验收报告

- **日期**：2026-11-11
- **目标架构**：x86_64（profile: `x86_64-clang`）
- **执行计划**：[docs/superpowers/plans/2026-11-10-doom-06-e2e.md](../plans/2026-11-10-doom-06-e2e.md)
- **设计规约**：[docs/superpowers/specs/2026-11-10-doom-06-e2e-design.md](../specs/2026-11-10-doom-06-e2e-design.md)

---

## 1. 源码与引擎材料清单

### 1.1 上游引擎 Submodule

- **上游仓库**：`https://github.com/ozkl/doomgeneric`
- **固定提交（Pinned Commit）**：`e257a09527062bfb986b6e5e0780839b6cbd5b71`
- **翻译单元**：共 80 个显式 C 编译单元，剔除平台入口点与 Zenity GUI 依赖。
- **访客分发路径**：
  - `/usr/share/licenses/doom/LICENSE`：doomgeneric 上游许可协议
  - `/usr/share/doc/doom/SOURCE.txt`：来源仓库与基线提交记录
  - `/usr/share/doc/doom/PATCHES.txt`：有序补丁清单、原因说明及 SHA256 校验
  - `/usr/share/doc/doom/patches/*`：全量补丁原文

### 1.2 有序补丁集（Application Order）

| 序号 | 补丁文件名 | SHA256 校验和 | 补丁功能说明 |
|:---:|:---|:---|:---|
| 01 | `0001-os01-no-zenity.patch` | `7fb62ba138e6a121577ebbe594d9302613d96fe84666d953936a28fffa9b2326` | 移除 Zenity 弹窗依赖，错误输出直接写入标准错误 |
| 02 | `0002-os01-quit.patch` | `b9fcf846fc6b2fe990ff5df8350711aa40be6d8591f4b8cf6b46487ff6819eb7` | 规范 `I_Quit` 退出流程，返回标准状态码 0 |
| 03 | `0003-os01-config.patch` | `085698b67f1396a9d18fa2bf8d132644e5904df2cbafaa4b17bfd751fb4c3600` | 修复符号比较并支持配置文件持久化读取与写入 |
| 04 | `0004-os01-input-budget.patch` | `b1e2ca493b860b06b2e3e57620a232f7b88970868f0cb4d4d8ef84f2c5efc077` | PS/2 独占租约输入批量非阻塞读取与事件投递 |
| 05 | `0005-os01-test-observe.patch` | `82f9d8a39a48f480398687ba1b1b369c766b57d605655755104443907c0ea4eb` | 仅在 `DOOM_TEST=1` 注入只读诊断上报，格式化输出串口帧与世界状态 |

---

## 2. 游戏资源材料（IWAD）

- **测试使用资源**：`~/Downloads/DOOM.WAD`
- **文件大小**：`12,408,292` 字节
- **SHA256 校验和**：`6fdf361847b46228cfebd9f3af09cd844282ac75f3edbb61ca4cb27103ce2e7f`
- **格式**：id Software Doom Registered v1.9 IWAD
- **打包变量**：
  - `DOOM_IWAD=/home/aagu/Downloads/DOOM.WAD`
  - `DOOM_IWAD_SHA256=6fdf361847b46228cfebd9f3af09cd844282ac75f3edbb61ca4cb27103ce2e7f`
  - `DOOM_IWAD_LICENSE=/home/aagu/Downloads/DOOM-LICENSE.txt`
  - `DOOM_IWAD_SOURCE="id Software Doom Registered v1.9"`
- **访客路径**：
  - `/usr/share/games/doom/iwad.wad`
  - `/usr/share/games/doom/IWAD-LICENSE.txt`
  - `/usr/share/games/doom/IWAD-SOURCE.txt`

---

## 3. 验收场景与观测证据

| 场景 | 验收要求与状态证据 | 判定 |
|---|---|:---:|
| **标题与开局** | 成功加载 IWAD；libgfx 独占接管并呈现 320×200（缩放倍数 3，居中黑边正确）；QMP 截图非空；Enter 进入 E1M1 | **PASS** |
| **持续控制** | PS/2 按键通过 QMP make/break 注入；UP 按键释放后 ticcmd 对应位立即清零，世界坐标推进符合物理惯性；组合键解绑单独生效 | **PASS** |
| **组合键控制** | 同时按住方向键与开火键，松开方向键时开火键动作持续，未出现全局提前清零 | **PASS** |
| **菜单与退出** | Esc 唤起菜单，选择 Quit 并按 Y 确认退出；经 `I_Quit` 退出码为 0；终端正确恢复前台并响应唯一 nonce 的 shell 命令 | **PASS** |
| **配置持久化** | 退出前写入按键与设置至 `/doom-data/default.cfg`；重启实例后成功重载绑定（Ctrl 开火、Space 使用均保持生效） | **PASS** |
| **存档与读档** | 关卡中存入第 1 槽位；继续游玩后加载存档，玩家状态与地图位置精确恢复；重启隔离 VM 副本后仍可读回 | **PASS** |
| **错误 IWAD** | 缺少或损坏 IWAD 时安全打印错误并退出，无 Zenity 弹窗，终端控制权完整保留 | **PASS** |
| **EBUSY 冲突** | 前台被预占时，新启动的 Doom 实例无法获得 display 独占租约，以 `EBUSY` 安全退出 | **PASS** |
| **模式切换失效** | testctl 在首帧后调用 `FBIOSET_MODE` 切换分辨率，Doom 检测到 generation 变化后立即放弃渲染并安全退出，不抢回前台 | **PASS** |
| **SIGKILL 强杀回收** | testctl 启动子进程并发送 `SIGKILL`，内核自动撤销图形与键盘独占租约，终端正常可用 | **PASS** |

---

## 4. 多核（SMP=1 / SMP=2）矩阵验证

- **SMP=1**：单核全场景单实例执行，无死锁、无时钟抖动异常，平均帧呈现平稳。
- **SMP=2**：双核调度下，后台守护进程与终端并发运行，键盘事件投递与帧缓冲互斥稳定，无并发抢占异常。
- **镜像隔离**：构建在 `build/x86_64-clang/image/doom-test/disk.img` 独立 variant，普通镜像哈希在前后完全一致（`normal.before` == `normal.after`）。

---

## 5. 自动化测试套件执行证据

| 测试套件 / 命令 | 覆盖范围 | 结果 |
|---|---|:---:|
| `python3 -m unittest qemutests.test_doom_runner` | 18 项 runner 状态断言、坏数据拒绝、键位与存档比对 | **18/18 PASS** |
| `python3 -m unittest qemutests.test_doom_build_contract` | 18 项构建契约、维度匹配、80 单元固化、无系统调用泄漏 | **18/18 PASS** |
| `python3 -m unittest qemutests.test_doom_engine_notice` | 4 项引擎许可证打包、补丁顺序与哈希防篡改、无 WAD 镜像安装 | **4/4 PASS** |
| `make test-doom-host DOOM_CASE=all` | 6 组宿主单元测试（build, config, video, input, runtime, diag） | **6/6 PASS** |
| `make test-harness` | 47 个全量测试框架模块（含 runner 与 notice 模块） | **47/47 PASS** |
| `make test-host` | 全量宿主单元测试集与 PMM/内核预留检查 | **PASS** |
| `make test-static` | 14 项内核布局、栈保护、系统调用边界与驱动模型静态审计 | **14/14 PASS** |

---

## 6. 未验证事项与后续规划

1. **Freedoom 全关卡分发**：本次验证使用 Shareware / Registered IWAD 闭环验证引擎核心；Freedoom Phase 1 / Phase 2 开源资源包实测可作为后续扩展，不影响本 D01–D06 交付。
2. **音频输出**：OS01 当前尚未实现声卡驱动与音频混音子系统，音频输出处于禁用静音桩状态。
