#!/usr/bin/env python3
"""PL011 RX -> GIC SPI 通路注入测试 (spec sec 7.4).

-chardev socket 起 QEMU serial: read side scans for '[gic] spi-test armed
intid=<N>', then injects 1 byte to trigger the PL011 RX IRQ and asserts
'[gic-spi] intid=<N> handled count=1' eventually appears.

DTB 机制与 SMP 套件一致: --diagnostic-dtb auto 逐 case 生成
(复用 aarch64_uefi_smp.generate_diagnostic_dtb, 同目录 import). UEFI
固件自身的输出也走这条 PL011, 读端只做行扫描不受影响.

RED 现状: PL011 RX IRQ handler 表里没注册 SPI 33 (handler 由 Task 2.3b
GREEN 注入), 注入字节后 IRQn 不会变 handled, harness 会超时 FAIL.
"""

import argparse
import os
import re
import select
import socket
import subprocess
import sys
import time
from pathlib import Path

ARMED_RE = re.compile(r"^\[gic\] spi-test armed intid=(\d+)$", re.MULTILINE)
HANDLED_RE = re.compile(r"^\[gic-spi\] intid=(\d+) handled count=(\d+)$", re.MULTILINE)
# \r 在 PL011 字节流里很常见 (LF+CR). spi_verdict() 会统一去掉再匹配,
# 同时 self-test 显式测一条 LF+CR 行确认容错.

# R2-1 fixture 命名 (8 个): 每个都是 raw serial buffer 的最小有效样例.
ARMED_ONLY = "[gic] spi-test armed intid=33\n"
HANDLED_ONLY = "[gic-spi] intid=33 handled count=1\n"
ARMED_HANDLED = ARMED_ONLY + HANDLED_ONLY
# handled 行在 armed 行之前: 模拟某种 out-of-order 行序; 当前 verdict
# 逻辑对两种 intid 都出现的情形直接判定 pass, 这是有意为之的宽松
# (QEMU serial 行序没保证, 不应因行序而误判 FAIL).
INJ_AFTER_HANDLE = HANDLED_ONLY + ARMED_ONLY
TIMEOUT_ONLY = "[gic-spi] timeout intid=33\n"
NONE_FIXTURE = "fjdkslajflk random serial noise\n[other] foo bar baz\n"
EMPTY_FIXTURE = ""
GARBAGE_FIXTURE = "### bogus markers ###\n[noise] no gic here\n"


def spi_verdict(text: str, injected: bool):
    """状态机: 返回 (verdict, inject_now).

    verdict: 'pass' / 'fail' / None (继续等); inject_now 仅在 armed 已
    出现且尚未注入时为真 (注入窗口). PL011 输出是 LF+CR, 统一去 \\r
    后匹配 (镜像 SMP harness 的做法).
    """
    text = text.replace("\r", "")
    armed = ARMED_RE.search(text)
    handled = HANDLED_RE.search(text)
    if armed and handled:
        if int(handled.group(1)) != int(armed.group(1)):
            return "fail", False
        if int(handled.group(2)) >= 1:
            return "pass", False
        # handled 行打到了但 count 还没递增: 算 partial, 继续等.
        return None, False
    if armed and not injected:
        return None, True
    return None, False


def self_test() -> None:
    """R2-1 fixture: 8 个具名 fixture + 11 个断言, 把 verdict 状态机锁住."""
    # armed_handled: 两条 marker 都到位, intid 一致, count >= 1 -> pass.
    assert spi_verdict(ARMED_HANDLED, injected=True)[0] == "pass", \
        "armed_handled (injected=True) must pass"
    # R2-1: 断言完整元组. armed+handled 已齐, 注入与否不再影响 verdict.
    assert spi_verdict(ARMED_HANDLED, injected=False) == ("pass", False), \
        "armed_handled (injected=False) must return ('pass', False)"
    # armed_only: armed 行到位, 尚未注入 -> 进入注入窗口.
    assert spi_verdict(ARMED_ONLY, injected=False) == (None, True), \
        "armed_only (injected=False) must return (None, True) — injection window"
    # armed_only: 已注入, 等待 handled 行.
    assert spi_verdict(ARMED_ONLY, injected=True) == (None, False), \
        "armed_only (injected=True) must return (None, False) — wait for handled"
    # handled_only: 只看到 handled, 没 armed 上下文 -> 拒判, 继续等.
    # 这条防线避免 kernel handler 抢跑 (例如先打 handled 后打 armed) 误判.
    assert spi_verdict(HANDLED_ONLY, injected=False) == (None, False), \
        "handled_only (injected=False) must return (None, False) — no armed anchor"
    # inj_after_handle: 行序颠倒; 当前 verdict 逻辑对两条都在的情形直接
    # 判定 pass. 这条断言锁住有意为之的宽松行为, 后续若要严格改顺序可
    # 单独打破这条 self-test.
    assert spi_verdict(INJ_AFTER_HANDLE, injected=False) == ("pass", False), \
        "inj_after_handle (out-of-order) must still return ('pass', False)"
    # timeout_only: 不被任一 regex 捕获 -> 继续等 (或最终超时由 main() 判).
    assert spi_verdict(TIMEOUT_ONLY, injected=False) == (None, False), \
        "timeout_only (no matching marker) must return (None, False)"
    # none: 纯噪声行 -> 继续等.
    assert spi_verdict(NONE_FIXTURE, injected=False) == (None, False), \
        "none (random noise) must return (None, False)"
    # empty: 完全无输入 -> 继续等.
    assert spi_verdict(EMPTY_FIXTURE, injected=False) == (None, False), \
        "empty must return (None, False)"
    # garbage: 形似但实际不匹配的乱码 -> 继续等, 不误判 pass.
    assert spi_verdict(GARBAGE_FIXTURE, injected=False) == (None, False), \
        "garbage must return (None, False)"
    # wrong intid: armed=33 handled=40 -> fail.
    wrong = ARMED_ONLY + "[gic-spi] intid=40 handled count=1\n"
    assert spi_verdict(wrong, injected=True)[0] == "fail", \
        "wrong intid must return 'fail'"
    # CR 容错: 同一 ARMED_HANDLED 内容但 LF 全部变 LF+CR, 仍应 pass.
    crlf = ARMED_HANDLED.replace("\n", "\n\r")
    assert spi_verdict(crlf, injected=True)[0] == "pass", \
        "CR/LF line endings must not affect verdict"


def qemu_command(args, dtb: str, sock: str) -> list:
    return [args.qemu, "-M", "virt,gic-version=2,acpi=off", "-cpu", "cortex-a53",
            "-smp", str(args.cpus), "-m", "512",
            "-drive", "if=pflash,format=raw,file=" + args.firmware,
            "-drive", "if=none,file=" + args.image +
                      ",format=raw,readonly=on,id=disk",
            "-device", "virtio-blk-device,drive=disk",
            "-chardev", "socket,id=ser0,path=" + sock + ",server=on,wait=off",
            "-serial", "chardev:ser0", "-display", "none",
            "-no-reboot", "-no-shutdown", "-dtb", dtb]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true",
                        help="run 8-fixture verdict self-test and exit")
    parser.add_argument("--firmware",
                        help="QEMU_EFI.fd (UEFI pflash image)")
    parser.add_argument("--image",
                        help="aarch64-uefi.img (virtio-blk raw image)")
    parser.add_argument("--qemu",
                        help="path to qemu-system-aarch64 binary")
    parser.add_argument("--log-dir",
                        help="directory to write serial.log and metadata")
    parser.add_argument("--cpus", type=int, default=1,
                        help="SMP CPU count forwarded to -smp "
                             "(must match the diagnostic DTB's /cpus)")
    parser.add_argument("--timeout", type=float, default=90.0,
                        help="seconds to wait for armed + handled markers")
    parser.add_argument("--diagnostic-dtb", metavar="PATH_OR_AUTO",
                        help="'auto' to materialize a QEMU-emitted DTB for "
                             "the current --cpus value into --log-dir; or "
                             "an explicit PATH to a prebuilt DTB. R2-1: "
                             "required (firmwares without EFI handoff DTB "
                             "will otherwise FATAL at boot)")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("aarch64_gic_spi: self-test passed")
        return 0
    if not all((args.firmware, args.image, args.qemu, args.log_dir)):
        parser.error("--firmware, --image, --qemu, and --log-dir are required "
                     "outside --self-test")
    if not args.diagnostic_dtb:
        parser.error("--diagnostic-dtb is required (use 'auto' or an explicit path)")
    if args.cpus < 1 or args.timeout <= 0:
        parser.error("--cpus must be >= 1 and --timeout must be > 0")

    Path(args.log_dir).mkdir(parents=True, exist_ok=True)
    if args.diagnostic_dtb == "auto":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from aarch64_uefi_smp import generate_diagnostic_dtb
        dtb = generate_diagnostic_dtb(args.qemu, args.log_dir, args.cpus)
    else:
        dtb = args.diagnostic_dtb

    sock = os.path.join(args.log_dir, "pl011.sock")
    proc = subprocess.Popen(qemu_command(args, dtb, sock),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = bytearray()
    verdict = None
    timed_out = False
    injected = False
    try:
        client = None
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and proc.poll() is None:
            if client is None:
                try:
                    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    client.connect(sock)
                    client.setblocking(False)
                except OSError:
                    try:
                        client.close()
                    except OSError:
                        pass
                    client = None
                    time.sleep(0.2)
                    continue
            # 200ms 切片 select: 让 deadline 检查 + inject 触发都有机会
            # 在 idle 串口上及时推进, 不会无限挂在内核 recv().
            readable, _, _ = select.select([client], [], [], 0.2)
            if not readable:
                continue
            try:
                chunk = client.recv(4096)
            except BlockingIOError:
                continue
            if not chunk:
                # 远端关闭 (QEMU 退出); 跳出等 verdict 收敛.
                break
            log.extend(chunk)
            verdict, inject_now = spi_verdict(log.decode("utf-8", "replace"),
                                              injected)
            if inject_now and not injected:
                # 注入 1 字节 -> PL011 RX IRQ. 现状 kernel 没 handler,
                # IRQn 不变 handled, harness 继续等 handled marker,
                # 最终由 deadline -> timed_out -> exit 2 (RED 证据).
                try:
                    client.send(b"G")
                    injected = True
                except OSError:
                    pass
            if verdict is not None:
                break
        # 跳出 while 时若还没出 verdict, 就是 deadline 到了.
        if verdict is None and time.monotonic() >= deadline:
            timed_out = True
    finally:
        (Path(args.log_dir) / "serial.log").write_bytes(log)
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    serial_path = Path(args.log_dir) / "serial.log"
    log_text = log.decode("utf-8", "replace")
    # 写出诊断, 方便 RED 阶段肉眼对照: 是否真出现过 armed 行? 出现过
    # 但没 handled (handler 缺失) 还是压根没 armed (fixture 没跑到)?
    armed_count = len(ARMED_RE.findall(log_text.replace("\r", "")))
    handled_count = len(HANDLED_RE.findall(log_text.replace("\r", "")))
    summary = {
        "event": "spi-case",
        "cpus": args.cpus,
        "result": "PASS" if verdict == "pass" else
                  ("TIMEOUT" if timed_out else "FAIL"),
        "injected": injected,
        "armed_count": armed_count,
        "handled_count": handled_count,
        "serial": str(serial_path),
    }
    print(json.dumps(summary))

    if verdict == "pass":
        return 0
    if timed_out:
        return 2
    return 1


# 延迟 import: 仅 main() 路径需要 json, self_test 路径不需要, 提前 import
# 会让 --self-test 的启动稍慢; 但 json 是 stdlib 且启动开销可忽略, 直接
# 顶层 import 即可, 与其它 qemutests 一致.
import json  # noqa: E402


if __name__ == "__main__":
    sys.exit(main())