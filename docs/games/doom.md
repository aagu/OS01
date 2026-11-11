# OS01 Doom

OS01 ports Doom using the `doomgeneric` engine adapter directly rendered to the exclusive `libgfx` surface with leased PS/2 keyboard input.

## 默认构建行为

默认情况下，`make image`（或 `make user`）编译并发布 `/bin/doom` 可执行文件到根文件系统，但**不会自动联网下载或打包任何 IWAD 游戏数据**，保持纯离线构建。

当未打包 IWAD 时，在 OS01 中运行 `/bin/doom` 会提示缺少 IWAD 文件并安全退出，不会夺取图形所有权。

## 本地 IWAD 资源打包（D05）

若需将本地已有的 IWAD 资源打包进根文件系统镜像，需在根构建时**成组提供**以下四个变量：

- `DOOM_IWAD`：本地 IWAD 文件绝对路径（例如 `/path/to/doom1.wad`）
- `DOOM_IWAD_SHA256`：IWAD 文件的 64 位十六进制 SHA256 校验和
- `DOOM_IWAD_LICENSE`：IWAD 对应许可证文本文件的绝对路径
- `DOOM_IWAD_SOURCE`：资源来源说明或 URL（单行文本，支持空格、引号、分号等字符）

> [!IMPORTANT]
> 四个变量必须同时提供。若只提供部分变量、哈希不匹配、路径包含空格/冒号或指向目录，构建将立即中止报错。

### 打包命令示例

```bash
make image \
  DOOM_IWAD=/home/user/games/doom1.wad \
  DOOM_IWAD_SHA256=2b58832a833502931a74d28470409a89c4709d0cb539f40eb52c7be0c57d598e \
  DOOM_IWAD_LICENSE=/home/user/games/doom1-license.txt \
  DOOM_IWAD_SOURCE="id Software Doom Shareware v1.9"
```

打包成功后，资源将安装至访客文件系统的以下路径：
- `/usr/share/games/doom/iwad.wad`
- `/usr/share/games/doom/IWAD-LICENSE.txt`
- `/usr/share/games/doom/IWAD-SOURCE.txt`

### 移除资源

在后续构建中若清空或不传入 `DOOM_*` 变量执行 `make image`，构建系统会自动重新生成根文件系统清单，新镜像中将不会残留任何旧的 Doom 资源文件。

## 运行与参数

在 OS01 终端中启动 Doom：

```bash
/bin/doom
```

支持的参数：
- `--os01-scale <1..3>`：显式指定渲染缩放倍数（默认根据屏幕分辨率自动选择居中最大整数倍数，1024×768 下为 3 倍）
- `--os01-data-dir <绝对路径>`：配置与存档保存目录（默认为 `/doom-data`，启动时若不存在会自动创建）
- `-iwad <路径>`：指定特定的 IWAD 文件路径（相对路径在切换到数据目录前解析为绝对路径）
- `--os01-diag-nonce <nonce>`（仅在 `DOOM_TEST=1` 诊断构建生效）：设置串口诊断输出 nonce，开启结构化运行指标
- `--os01-diag-action self-release`（仅在 `DOOM_TEST=1` 诊断构建生效）：首帧呈现后主动释放图形表面并以退出码 0 安全退出

## 按键说明与控制

Doom 在独占输入租约下直接读取 PS/2 键盘：
- **移动**：方向键（上/下前进后退，左/右转身），`,`/`.` 平移（Strafe left/right）
- **开火**：`Ctrl`
- **使用/开门/触发开关**：`Space`
- **加速跑步**：`Shift`
- **菜单**：`Esc` 打开/关闭菜单，方向键移动光标，`Enter` 确认，`Esc` 返回
- **确认/取消**：`Y` / `N`

配置与键位绑定保存在数据目录下的 `default.cfg` 中。游戏退出后终端输入恢复正常。

## 诊断构建与测试工具（DOOM_TEST=1）

当传入 `DOOM_TEST=1` 触发隔离测试构建时：
- 发布测试控制工具 `/bin/doom_testctl`，支持 `ebusy`（预占图形测试冲突退出）、`sigkill <child_pid>`（外部信号强杀安全回收验证）以及 `mode-switch`（运行时切换屏幕分辨率测试所有权校验退出）；
- `/bin/doom` 包含诊断上报桩，格式化输出 `[doom-diag] v=1 ...` 到串口设备 `/dev/serial`；
- 普通生产构建（未设置 `DOOM_TEST=1`）不包含 `/bin/doom_testctl`，`/bin/doom` 中亦不包含任何诊断上报代码。
