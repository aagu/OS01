# fbtest 私有头发布边界调查

日期：2026-10-24。对应 roadmap 分辨率切换器 parked 的测试 sysroot 私有头项。

## 根因与修改

`kernel/Makefile` 的 `install-headers` 先复制 `kernel/include/`，再删掉不应
发布的头。旧规则仅在 `FB_RESOLUTION_TEST != 1` 时同时移除
`driver/fb_test.h` 与 `uapi/fb_test.h`，导致测试配置把内核注入钩子也发布
给用户程序。

调用方审计确认：`driver/fb_test.h` 的消费者是内核实现与宿主内核 fixture。
测试用户程序 `test_resolution.c` 和 `terminal_display.c` 使用
`uapi/fb_test.h` 的 ioctl ABI，不需要私有钩子声明。宿主 fixture 直接使用
内核源码头路径，不依赖 sysroot 发布。

将私有头移除操作放到无条件路径；UAPI 仍仅在测试配置发布。修正私有头
顶部原先声称加入 sysroot 的注释。没有改 ioctl、结构布局或运行时行为。
发布器继续依据 staging manifest 生成新 sysroot；历史不可变 generation
保留原样，当前 generation 不再包含私有头。

## 回归

新增 `tools/tests/test_kernel_headers.py`，直接调用真实 kernel Makefile 的
安装规则，使用临时 profile 和目录，不复制或模拟安装逻辑：

- 独立的干净生产/测试 staging，检查私有头始终不存在。
- 生产不发布测试 UAPI；测试发布且字节内容与源文件一致。
- 预置旧私有头，再按测试→生产→测试顺序重装，确保旧文件被清除。
- 同时检查文件和 manifest，普通 `uapi/fb.h` 继续存在。

测试接入根 `make test-host`。旧规则在测试配置的三个 subtest 上失败；
修复后两项测试全部通过。

## 验证记录

- `python3 tools/tests/test_kernel_headers.py`：红绿验证通过。
- `make PROFILE=x86_64-clang FB_RESOLUTION_TEST=1 image`：测试镜像构建通过；
  真实 published sysroot 与 kernel staging manifest 均无私有头，仍含测试
  UAPI，且 UAPI 与源文件内容一致。
- `make PROFILE=x86_64-clang test-resolution-host`：完整九组通过，确认内核
  hooks 与用户终端 fault hook 的编译和回归未受影响。
- `make PROFILE=x86_64-clang image`：生产镜像构建通过；当前 published
  sysroot 和 staging manifest 均不含私有头及测试 UAPI，普通 framebuffer
  UAPI 继续存在。
- `make PROFILE=x86_64-clang test-host`：全部宿主二进制、PMM reservation、
  43 项 kernel selftest runner、7 项 user build 和新增 2 项 header staging
  回归全部通过。此次仅修改发布规则，未另跑 QEMU 运行时套件。

日志前缀：`/tmp/os01-parked-fbtest-headers-`，包括 `red.log`、`green.log`、
`test-image.log`、`test-publish.log`、`resolution.log`、
`production-image.log`、`production-publish.log` 和 `host.log`。
