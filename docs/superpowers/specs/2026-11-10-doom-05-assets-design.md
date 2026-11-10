# D05 — Doom IWAD 打包

日期：2026-11-10。状态：文档评审通过；仅设计，不表示已实施。

[总览与公共约束](2026-11-10-doom-port-design.md) · [配套实施计划](../plans/2026-11-10-doom-05-assets.md)

## 目标、依赖与交付边界

独立实现资源选择/校验/manifest 增量语义，不要求游戏已可玩。根构建入口为前置；与 D03/D04 无代码接口依赖，但完整游戏镜像需要 D04。交付有/无资源两种构建及其测试；不自动下载 WAD，不选择未经运行验证的 Freedoom 版本。

## 数据契约

默认构建 Doom ELF，但不自动下载或打包 IWAD；缺少游戏数据不应使日常 `make` 联网。提供一组明确的可选根构建输入（本文建议命名）：

- `DOOM_IWAD=/absolute/path/to/file.wad`。
- `DOOM_IWAD_SHA256=<64 位十六进制>`，与路径同时提供，打包前逐字节验证。
- `DOOM_IWAD_LICENSE=/absolute/path/to/license.txt` 和 `DOOM_IWAD_SOURCE=<来源说明或 URL>`，用于可分发镜像的可追溯记录；测试私有数据也必须记录其来源与用途。

四项作为一组校验，空组表示无 WAD；半组、校验不符、不是普通文件均失败。路径含空白/分隔符时应明确拒绝，因为当前 rootfs 的 `dest=source:mode` 编码不支持任意路径，不能让 shell 静默拆词。DOOM_IWAD 的宿主路径不直接写入访客 argv。

四变量只由根镜像层消费，不加入 `OS01_SUBMAKE_ALLOWED/OS01_SUBMAKE_ARGS`，不经递归 make 参数字符串传播。根层使用简单展开的专用环境变量 `OS01_DOOM_ASSET_IWAD/SHA256/LICENSE/SOURCE` 导出原始值（各自以 `$(value DOOM_...)` 读取），脚本用 `--from-env` 从进程环境读取；禁止把来源或路径插入 recipe shell 文本、eval 或二次 Make 展开。来源为单行文本，空格、引号、美元符、反引号和分号均保留原文，嵌入换行/NUL 拒绝。半组/非法输入必须在资源 stamp、rootfs manifest 与镜像发布前失败。单独 CLI 测试不能代替根 Make 路径验收。

资源经 rootfs manifest 安装到 `/usr/share/games/doom/iwad.wad`、对应许可证和来源记录。镜像输入摘要包含 **WAD 内容哈希、许可证内容与来源**；同一路径替换文件也必须触发重打包，去掉变量后旧 WAD 不得残留于新镜像。给 `ROOTFS_MANIFEST` 补充真实资源 prerequisites/校验 stamp；只修改 `ROOTFS_FILES` 不足以跟踪原地内容变化。

开发基准采用本地提供的 doom1.wad v1.9；可分发资源候选为 Freedoom Phase 1。首版不预填未经验证的下载 URL、版本或 SHA，不提交 WAD 二进制进 Git。未来若加下载目标，必须作为独立显式操作，锁定版本、URL、完整哈希和许可证后再接入，不能在默认构建时追逐 latest。

需要随镜像分发 Freedoom 时，实施阶段必须把实际测试通过的版本、SHA 和许可原文记入资源 manifest；在此之前只交付“不含 WAD 的默认镜像 + 显式本地资源构建”，不宣称已验证某版 Freedoom。原版商业 WAD 仅由用户自行提供，不进入默认分发。

D05 只拥有 IWAD 的许可/来源记录，独立交付不依赖引擎存在。引擎 GPL 许可证、固定源码来源和本地修改说明由 [D06 的镜像材料任务](2026-11-10-doom-06-e2e-design.md) 在 D03/D04 完成后生成并安装，不能遗漏。需要发布二进制时另行核对对应源码交付材料；不能由已有 BusyBox 推导 Doom 与所有资源可任意再分发，不替整个仓库决定许可证。

## 文件与验收

拥有 `config/rootfs.mk`、`mk/components/image.mk`、新增 `scripts/prepare_doom_assets.py` 和 `qemutests/test_doom_assets.py`；`mk/project.mk` 如需修改，仅限根层变量校验，不把四变量加入递归白名单。统一访客目标：`/usr/share/games/doom/iwad.wad`、`IWAD-LICENSE.txt`、`IWAD-SOURCE.txt`。脚本生成 profile 私有资源制品/校验 stamp，image 只消费制品。

宿主测试可使用微型假 WAD 验证打包字节/摘要，不代表引擎兼容性。四变量空组不联网；半组、错误哈希、目录输入和非法路径失败。WAD 内容或许可证原地改变触发更新，移除变量清除新镜像的旧资源；摘要未变时保持 stamp 稳定。来源允许普通空格文本，但必须结构化写入，不经不安全 shell 插值。

完成条件：`python3 -m unittest qemutests.test_doom_assets` 和独立根入口 `make test-doom-assets` 通过（后者路由到同一套测试）；实际 rootfs.manifest/磁盘内容与三条访客路径一致。D06 才负责 suite 的隔离 variant 和游戏实测，资源校验通过不等于数据可玩。
