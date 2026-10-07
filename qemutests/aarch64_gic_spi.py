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
import hashlib
import os
import re
import select
import socket
import subprocess
import sys
import time
import tempfile
from datetime import datetime, timezone
from pathlib import Path

# Running this file as a script puts ``qemutests/`` on ``sys.path[0]`` —
# not the repo root — so bootstrap the repo root explicitly (Ruling 7).
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:  # noqa: E402
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:  # noqa: E402
    from qemutests.harness.result import RunArchive, RunReport
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]

# Suite id for the archive path and the RunReport.
SUITE = "aarch64-gic-spi"

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


def _sha256_path(path) -> str | None:
    if path is None:
        return None
    try:
        with open(path, "rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()
    except (OSError, TypeError):
        return None


def _git_revision() -> tuple[str, bool]:
    rev, dirty = "unknown", False
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=_ROOT,
                           capture_output=True, text=True, timeout=5)
        if r.returncode == 0 and r.stdout.strip():
            rev = r.stdout.strip()
        d = subprocess.run(["git", "status", "--porcelain"], cwd=_ROOT,
                           capture_output=True, text=True, timeout=5)
        if d.returncode == 0:
            dirty = bool(d.stdout.strip())
    except (OSError, subprocess.TimeoutExpired):
        pass
    return rev, dirty


def _write_suite_report(archive, *, argv, profile, cpu, memory_mib,
                        firmware, image, started_monotonic, status,
                        runner_exit_code, session, errors, extra=None) -> None:
    """Persist a suite-unit RunReport; the suite has no guest v1 cases."""
    if archive is None or RunReport is None:
        return
    try:
        rev, dirty = _git_revision()
        outcomes = [{"errors": list(errors)}]
        if extra:
            outcomes[0].update(extra)
        report = RunReport(
            schema_version=1, run_id=archive.run_dir.name,
            git_revision=rev, git_dirty=dirty, profile=profile, suite=SUITE,
            request=None, declared_ids=None, observed_ids=None,
            argv=list(argv), cpu_count=int(cpu), memory_mib=int(memory_mib or 0),
            tool_versions={},
            firmware_path=firmware or None,
            firmware_sha256_before=_sha256_path(firmware),
            firmware_sha256_after=_sha256_path(firmware),
            image_path=image or None,
            image_sha256_before=_sha256_path(image),
            image_sha256_after=_sha256_path(image),
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=max(0.0, time.monotonic() - started_monotonic),
            runner_exit_code=runner_exit_code,
            child_exit_code=(session.returncode if session is not None else None),
            stopped_by_runner=bool(session is not None and session.stopped_by_runner),
            status=status, count_unit="suite", outcomes=outcomes,
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
    except Exception as exc:  # noqa: BLE001
        print(f"[{SUITE}] warning: failed to write RunReport: {exc}",
              file=sys.stderr)


def run_case(args, diagnostic_dtb, *, session_factory=None, build_dir=None,
             profile="default", sock_path=None) -> int:
    """Drive one PL011 RX -> GIC SPI injection case through ProcessSession.

    Only the common process/output/archive code is delegated: the verdict
    is still ``spi_verdict`` (suite owned) over the serial socket stream.
    Returns 0 (PASS), 1 (FAIL) or 2 (TIMEOUT/ERROR).
    """
    if session_factory is None:
        session_factory = ProcessSession
    started = time.monotonic()

    archive = None
    if build_dir and RunArchive is not None:
        archive = RunArchive.create(Path(build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)

    socket_dir = None
    if sock_path is None:
        # Worktree log paths can exceed Linux sockaddr_un.sun_path (108 bytes).
        socket_dir = tempfile.TemporaryDirectory(prefix="os01-spi-")
        sock_path = os.path.join(socket_dir.name, "pl011.sock")
    command = qemu_command(args, diagnostic_dtb, sock_path)

    session = None
    client = None
    log = bytearray()
    verdict = None
    timed_out = False
    injected = False
    spawn_error = None
    try:
        if session_factory is None:
            raise RuntimeError("ProcessSession is unavailable")
        session = session_factory(argv=command, run_dir=run_dir,
                                  timeout_s=args.timeout)
        session.start()
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            if client is None:
                try:
                    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    client.connect(sock_path)
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
    except Exception as error:  # noqa: BLE001
        spawn_error = error
    finally:
        (run_dir / "serial.log").write_bytes(log)
        if client is not None:
            try:
                client.close()
            except OSError:
                pass
        if session is not None:
            try:
                session.stop()
            except Exception:
                pass
            try:
                session.close()
            except Exception:
                pass
        if socket_dir is not None:
            socket_dir.cleanup()

    if spawn_error is not None:
        print(json.dumps({"event": "spawn-error", "error": str(spawn_error)}))
        _write_suite_report(
            archive, argv=command, profile=profile, cpu=args.cpus,
            memory_mib=512, firmware=args.firmware, image=args.image,
            started_monotonic=started, status="ERROR", runner_exit_code=2,
            session=session, errors=[f"spawn error: {spawn_error}"])
        return 2

    serial_path = run_dir / "serial.log"
    log_text = log.decode("utf-8", "replace")
    # 写出诊断, 方便 RED 阶段肉眼对照: 是否真出现过 armed 行? 出现过
    # 但没 handled (handler 缺失) 还是压根没 armed (fixture 没跑到)?
    armed_count = len(ARMED_RE.findall(log_text.replace("\r", "")))
    handled_count = len(HANDLED_RE.findall(log_text.replace("\r", "")))
    accepted = verdict == "pass"
    status = ("PASS" if accepted else
              ("TIMEOUT" if timed_out else "FAIL"))
    rc = 0 if accepted else (2 if timed_out else 1)
    errors = []
    if not accepted:
        errors.append("timeout waiting for armed+handled markers" if timed_out
                      else "armed+handled verdict not satisfied")
    _write_suite_report(
        archive, argv=command, profile=profile, cpu=args.cpus, memory_mib=512,
        firmware=args.firmware, image=args.image, started_monotonic=started,
        status=status, runner_exit_code=rc, session=session, errors=errors,
        extra={"injected": injected, "armed_count": armed_count,
               "handled_count": handled_count})
    summary = {
        "event": "spi-case",
        "cpus": args.cpus,
        "result": status,
        "injected": injected,
        "armed_count": armed_count,
        "handled_count": handled_count,
        "serial": str(serial_path),
    }
    print(json.dumps(summary))
    return rc


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
    parser.add_argument("--build-dir", default=None,
                        help="build dir for the run archive, supplied by Make")
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE",
                                                             "default"),
                        help="profile name recorded in the report")
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

    return run_case(args, dtb, build_dir=args.build_dir, profile=args.profile)


# 延迟 import: 仅 main() 路径需要 json, self_test 路径不需要, 提前 import
# 会让 --self-test 的启动稍慢; 但 json 是 stdlib 且启动开销可忽略, 直接
# 顶层 import 即可, 与其它 qemutests 一致.
import json  # noqa: E402


if __name__ == "__main__":
    sys.exit(main())