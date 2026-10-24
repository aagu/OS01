#!/usr/bin/env python3
"""OS01 GUI-3 Material 桌面的 QEMU 端到端门禁。

宿主侧的像素契约在 ``hosttests/cases/test_desktop_shell.c`` 里；那里用
真实 LVGL + 真实离屏平台跑，但跑在宿主机上。这里补上**只能在真机上证伪**
的部分：

1. **冒烟** —— ``/bin/desktop smoke`` 在目标 libc、目标 framebuffer 后端
   上离屏渲染一帧后退出，rc=0 且日志没有 panic。这条把「LVGL 渲染 +
   Material 主题 + 令牌表」在目标平台上真正跑通。
2. **接管画面** —— ``/bin/desktop`` 接管显示后，QMP screendump 拿到的画面
   既不是终端、也不是纯色，而且必须有结构（多种颜色）：即「Material 桌面
   确实画出来了」，而不是一块死掉的 framebuffer。
3. **键盘导航** —— 用 QMP 注入真实的 PS/2 按键（Tab / Enter / Esc），验证
   桌面在**真机输入通道**上能启动应用并回到主屏；画面差异就是证据。
4. **退出** —— 杀掉桌面后 guest 仍然活着，日志没有 panic。

设计约束（与 GUI-1 / GUI-2 的门禁一致）：
- QEMU 一律 ``-snapshot``，磁盘镜像的 sha256 在跑之前与跑之后必须一致。
- 按键要**限速**注入：guest 的 PS/2 路径会丢掉来得太快的按键。
- 每一步的证据都落进 run archive（截图 + QMP 事件转录）。

用法::

    python3 qemutests/gui_desktop_test.py --image <disk.img> --firmware <OVMF>
    make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import sys
import tempfile
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

# ``python3 qemutests/gui_desktop_test.py`` puts ``qemutests/`` on
# sys.path[0]; the repository root is what carries the ``qemutests``
# package.  Mirror what run_test.py does.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from qemutests.gui_test import (  # noqa: E402
    QmpClient,
    Surface,
    _panic_in,
    _qemu_argv,
    _sha256,
    is_fault_pure_color,
    parse_ppm,
    surface_diff,
)

# SMP 矩阵：与 GUI-1 的 gui 门禁一致（1 与 2 核）。
CORE_MATRIX = (1, 2)

# 每个键的按下 / 抬起之间的间隔与两个键之间的间隔。guest 的 PS/2 路径
# 会丢掉来得太快的按键，所以按键必须限速注入（与 GUI-2 门禁同源）。
KEY_PACE_SECONDS = 0.05

# 画面稳定等待：桌面渲染一帧 + LVGL 的 33ms 刷新定时器 + guest 调度。
SETTLE_SECONDS = 1.2

# 等待「桌面接管了显示」的时长上限。
TAKEOVER_TIMEOUT_SECONDS = 20.0

# 桌面上画面的「有结构」判据：抽样到的不同颜色数下限。Material 主屏有
# 壁纸、状态栏、欢迎卡片、启动器与应用图标，颜色数远高于这个下限；
# 一块死掉的 framebuffer 只有 1 种。
MIN_DISTINCT_COLORS = 5

# 两次画面之间的差异下限：低于它就当作「画面没变」。
#
# 不能更小：guest 的控制台（/bin/terminal）一直在刷屏（时钟、探针输出），
# 那些变化只占千分之几的像素。应用表面与主屏之间隔着整条应用顶栏
# （1440x40 / 1440x900 ≈ 4%），2% 干净地把两者分开。
MIN_SURFACE_DIFF = 0.02

# 桌面壁纸底色：令牌表的 surface_dim 角色（#D8D2E4）。桌面接管显示后
# 会把整块屏幕刷成这个颜色 —— 这就是「桌面真的接管了显示」的判据。
WALLPAPER_RGB = (216, 210, 228)

# 画面里壁纸底色的占比下限（桌面接管后应接近 0.918）。
MIN_MATERIAL_FRACTION = 0.5

# 指针光标是 16x24，热点在左上角。门禁从不移动鼠标（只注入按键），所以
# 指针恒在屏幕正中 —— 而正中落在空白壁纸区（欢迎卡片在上方、启动器条
# 在下方）。
CURSOR_BOX_W = 16
CURSOR_BOX_H = 24


def distinct_colors(surface: Surface, step: int = 4) -> int:
    """画面里出现的不同颜色数（抽样统计）。"""
    colors = set()
    for y in range(0, surface.height, step):
        for x in range(0, surface.width, step):
            colors.add(surface.pixel(x, y))
    return len(colors)


def material_fraction(surface: Surface, step: int = 4) -> float:
    """画面里壁纸底色的占比（抽样统计）。"""
    if surface.height == 0 or surface.width == 0:
        return 0.0
    hit = 0
    total = 0
    for y in range(0, surface.height, step):
        for x in range(0, surface.width, step):
            total += 1
            if surface.pixel(x, y) == WALLPAPER_RGB:
                hit += 1
    return hit / total if total else 0.0


def cursor_visible(surface: Surface) -> bool:
    """屏幕中心的窗口里既有比壁纸更暗的像素，也有比壁纸更亮的像素。

    光标是深色箭头 + 浅色描边，所以它的窗口里必须两者都有。旧光标是
    纯白：窗口里没有更暗的像素 —— 这条判据因此能抓到回归。
    """
    x0 = surface.width // 2
    y0 = surface.height // 2
    darker = False
    lighter = False
    for y in range(y0, min(y0 + CURSOR_BOX_H, surface.height)):
        for x in range(x0, min(x0 + CURSOR_BOX_W, surface.width)):
            r, _g, b = surface.pixel(x, y)
            if r < WALLPAPER_RGB[0]:
                darker = True
            if b > WALLPAPER_RGB[2]:
                lighter = True
            if darker and lighter:
                return True
    return False


def qmp_key_event(down: bool, qcode: str) -> dict:
    """一条 QMP ``input-send-event`` 的按键事件。"""
    return {
        "type": "key",
        "data": {"down": down, "key": {"type": "qcode", "data": qcode}},
    }


class DesktopCaseRunner:
    """在单个 SMP 核数上驱动 Material 桌面的 QEMU 场景。"""

    def __init__(
        self,
        smp: int,
        image: Path,
        firmware: Path,
        log: Callable[[str], None] = print,
        session_factory: Optional[Callable] = None,
        qmp_factory: Optional[Callable] = None,
        run_dir: Optional[Path] = None,
        timeout: Optional[float] = None,
    ):
        self.smp = smp
        self.image = image
        self.firmware = firmware
        self.log = log
        self.session_factory = session_factory
        self.qmp_factory = qmp_factory
        self.run_dir = run_dir or Path(tempfile.mkdtemp(prefix=f"os01-gui-desktop-smp{smp}-"))
        self.timeout = timeout if timeout is not None else 240.0
        self.boot_timeout = min(self.timeout, 30.0)
        self.cmd_timeout = min(self.timeout, 15.0)
        self.qmp: Optional[Any] = None
        self.session: Optional[Any] = None

    # ── 底层动作 ──────────────────────────────────────────────────────

    def _wait_shell(self, boot_timeout: Optional[float] = None) -> bool:
        bt = boot_timeout if boot_timeout is not None else self.boot_timeout
        deadline = time.monotonic() + bt
        while time.monotonic() < deadline:
            text = self.session.text
            if text.endswith("# ") or "\n# " in text:
                return True
            try:
                self.session.observe(0.1)
            except Exception:
                pass
        return False

    def _send_canary(self, timeout: float = 5.0) -> bool:
        nonce = f"CANARY_{self.smp}_{time.time_ns()}"
        cmd = f"printf '__SHELL_%s__\\n' {nonce}\n".encode()
        offset_before = len(self.session.text)
        self.session.send(cmd)

        pat = re.compile(rf"(^|\n)__SHELL_{re.escape(nonce)}__(\n|$)")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if pat.search(self.session.text[offset_before:]):
                return True
            try:
                self.session.observe(0.1)
            except Exception:
                pass
        return False

    def _run_cmd_with_rc(self, cmd: str, timeout: float = 10.0) -> Tuple[bool, int, str]:
        nonce = f"CMD_{self.smp}_{time.time_ns()}"
        full_cmd = f"{cmd}\nprintf '__EXIT_%s_%d__\\n' {nonce} $?\n".encode()
        offset_before = len(self.session.text)
        self.session.send(full_cmd)

        pat = re.compile(rf"(^|\n)__EXIT_{re.escape(nonce)}_(-?\d+)__(\n|$)")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            m = pat.search(self.session.text[offset_before:])
            if m:
                return True, int(m.group(2)), self.session.text[offset_before:]
            try:
                self.session.observe(0.1)
            except Exception:
                pass
        return False, -1, self.session.text[offset_before:]

    def _screendump(self, label: str, reasons: Optional[List[str]] = None) -> Optional[Surface]:
        shot_path = self.run_dir / f"{label}.ppm"
        try:
            return self.qmp.screendump(shot_path)
        except Exception as exc:
            if reasons is not None:
                reasons.append(f"QMP screendump failed at {label}: {exc}")
            return None

    def _observe_check_panic(self, label: str, reasons: List[str]) -> bool:
        try:
            self.session.observe(0.2)
        except Exception:
            pass
        text = self.session.text
        if _panic_in(text):
            reasons.append(f"[{label}] kernel panic detected in the serial log")
            return False
        return True

    def _tap_key(self, qcode: str) -> None:
        """注入一次完整的按键（按下 + 抬起），并限速。"""
        self.qmp.input_events([qmp_key_event(True, qcode)])
        time.sleep(KEY_PACE_SECONDS)
        self.qmp.input_events([qmp_key_event(False, qcode)])
        time.sleep(KEY_PACE_SECONDS)

    def _wait_for_desktop_surface(self, label: str, timeout: float) -> Optional[Surface]:
        """等到桌面接管显示为止（画面被 Material 底色铺满）。

        不能拿终端画面当基线做像素差：控制台一直在刷屏，差值会被噪声
        吃掉。判据改成「Material 底色铺满整屏」—— 只有桌面渲染才会这样。
        """
        deadline = time.monotonic() + timeout
        latest: Optional[Surface] = None
        while time.monotonic() < deadline:
            shot = self._screendump(label)
            if shot is not None:
                latest = shot
                if material_fraction(shot) >= MIN_MATERIAL_FRACTION:
                    return shot
            try:
                self.session.observe(0.1)
            except Exception:
                pass
            time.sleep(0.3)
        return latest

    def _wait_for_surface_change(
        self, baseline: Surface, label: str, timeout: float
    ) -> Optional[Surface]:
        """等到画面与基线明显不同为止（返回新的画面）。

        循环里必须泵 serial：否则 guest 的输出（包括桌面自己的错误
        报告）不会被读走，失败时也就看不到原因。
        """
        deadline = time.monotonic() + timeout
        latest: Optional[Surface] = None
        while time.monotonic() < deadline:
            shot = self._screendump(label)
            if shot is not None:
                latest = shot
                if surface_diff(baseline, shot) >= MIN_SURFACE_DIFF:
                    return shot
            try:
                self.session.observe(0.1)
            except Exception:
                pass
            time.sleep(0.3)
        return latest

    def _serial_tail(self, limit: int = 1200) -> str:
        """serial 日志的尾部：失败原因里必须带上它。"""
        try:
            text = self.session.text
        except Exception:
            return ""
        if not text:
            return ""
        return " | serial tail: " + text[-limit:].replace("\n", "\\n")

    def _fail(
        self,
        reasons: List[str],
        outcomes: List[dict],
        case_name: str,
        message: str,
    ) -> Tuple[bool, List[str], List[dict]]:
        reason = f"[{case_name}] {message}{self._serial_tail()}"
        reasons.append(reason)
        outcomes.append({"smp": self.smp, "case": case_name,
                         "status": "FAIL", "reason": reason})
        return False, reasons, outcomes

    # ── 场景 ──────────────────────────────────────────────────────────

    def run_all(self) -> Tuple[bool, List[str], List[dict]]:
        reasons: List[str] = []
        outcomes: List[dict] = []
        self.run_dir.mkdir(parents=True, exist_ok=True)
        # AF_UNIX 的路径长度上限是 107 字节：socket 放 /tmp，证据留 run_dir。
        qmp_sock = f"/tmp/os01-gd-smp{self.smp}-{uuid.uuid4().hex[:8]}.sock"
        argv = _qemu_argv(str(self.image), str(self.firmware), qmp_sock, smp=self.smp)

        try:
            if self.session_factory:
                self.session = self.session_factory(
                    argv, self.run_dir, self.timeout, writable_stdin=True
                )
            else:
                from qemutests.harness.process import ProcessSession

                self.session = ProcessSession(
                    argv=argv,
                    run_dir=self.run_dir,
                    timeout_s=self.timeout,
                    writable_stdin=True,
                )
            self.session.start()
        except Exception as exc:
            reasons.append(f"QEMU launch failed: {exc}")
            # The launch may have got as far as spawning QEMU (the log
            # files are opened AFTER the spawn): close() must still run,
            # or a half-started child becomes an orphan holding the disk
            # image lock.
            self.close()
            return False, reasons, outcomes

        try:
            try:
                if self.qmp_factory:
                    self.qmp = self.qmp_factory(qmp_sock)
                else:
                    self.qmp = QmpClient(qmp_sock, timeout=15.0)
                self.qmp.connect()
            except Exception as exc:
                reasons.append(f"QMP connect failed: {exc}")
                return False, reasons, outcomes

            # QMP 事件转录：注入的每一个按键都留证据。
            set_transcript = getattr(self.qmp, "set_transcript", None)
            if set_transcript is not None:
                try:
                    set_transcript(self.run_dir / "qmp-input-events.jsonl")
                except Exception as exc:
                    reason = f"the QMP transcript could not be enabled: {exc}"
                    reasons.append(reason)
                    return False, reasons, outcomes

            # ── 场景 1：启动与握手 ──
            case_name = "boot/handshake"
            self.log(f"  [gui-desktop smp={self.smp}] starting {case_name}")
            if not self._wait_shell():
                reasons.append(f"[{case_name}] boot timeout before the shell prompt")
                outcomes.append({"smp": self.smp, "case": case_name,
                                 "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            if not self._send_canary(timeout=self.cmd_timeout):
                reasons.append(f"[{case_name}] handshake canary failed")
                outcomes.append({"smp": self.smp, "case": case_name,
                                 "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            terminal = self._screendump("01_terminal", reasons)
            if terminal is None:
                outcomes.append({"smp": self.smp, "case": case_name,
                                 "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            if not self._observe_check_panic(case_name, reasons):
                outcomes.append({"smp": self.smp, "case": case_name,
                                 "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS"})

            # ── 场景 2：冒烟（离屏渲染一帧） ──
            case_name = "desktop_smoke"
            self.log(f"  [gui-desktop smp={self.smp}] starting {case_name}")
            ok, rc, out = self._run_cmd_with_rc(
                "/bin/desktop smoke", timeout=min(self.cmd_timeout, 10.0)
            )
            if not ok or rc != 0:
                return self._fail(
                    reasons, outcomes, case_name,
                    f"/bin/desktop smoke failed (ok={ok}, rc={rc})",
                )
            if not self._observe_check_panic(case_name, reasons):
                return self._fail(reasons, outcomes, case_name,
                                  reasons[-1] if reasons else "panic")
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS"})

            # ── 场景 3：接管显示，画面有结构 ──
            case_name = "desktop_takes_over"
            self.log(f"  [gui-desktop smp={self.smp}] starting {case_name}")
            # 后台启动：桌面接管显示后不退出，shell 仍然可用。
            # ``$!`` 与后台任务在同一条命令行里展开，不用依赖 shell 的
            # 跨命令行状态。PID 写进 /tmp/gd.pid，最后用它杀掉桌面。
            self.session.send(b"/bin/desktop & printf '%s\\n' \"$!\" > /tmp/gd.pid\n")

            home = self._wait_for_desktop_surface(
                "02_desktop_home", TAKEOVER_TIMEOUT_SECONDS
            )
            if home is None:
                return self._fail(reasons, outcomes, case_name,
                                  "no screendump after starting the desktop")
            fraction = material_fraction(home)
            if fraction < MIN_MATERIAL_FRACTION:
                return self._fail(
                    reasons, outcomes, case_name,
                    f"the desktop never took over the screen: the Material "
                    f"background covers only {fraction:.1%} of it",
                )
            if not cursor_visible(home):
                return self._fail(
                    reasons, outcomes, case_name,
                    "the pointer cursor is not visible on the wallpaper",
                )
            if is_fault_pure_color(home):
                return self._fail(reasons, outcomes, case_name,
                                  "the screen shows the fault tool's solid colour")
            colors = distinct_colors(home)
            if colors < MIN_DISTINCT_COLORS:
                return self._fail(
                    reasons, outcomes, case_name,
                    f"the desktop screen is not structured: {colors} distinct "
                    f"colours (expected >= {MIN_DISTINCT_COLORS})",
                )
            if surface_diff(terminal, home) < MIN_SURFACE_DIFF:
                return self._fail(
                    reasons, outcomes, case_name,
                    "the screen did not change after the desktop took over",
                )
            if not self._observe_check_panic(case_name, reasons):
                return self._fail(reasons, outcomes, case_name,
                                  reasons[-1] if reasons else "panic")
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS",
                             "detail": f"{colors} distinct colours, "
                                       f"background {fraction:.1%}"})

            # ── 场景 4：键盘导航（Tab → Enter 启动应用，Esc 回主屏） ──
            case_name = "desktop_keyboard"
            self.log(f"  [gui-desktop smp={self.smp}] starting {case_name}")
            time.sleep(SETTLE_SECONDS)
            # Tab：焦点落到启动器的第一个图标上（Material 焦点环由主题给出）。
            self._tap_key("tab")
            time.sleep(SETTLE_SECONDS)
            # Enter：激活焦点项 → 启动该应用（整屏替换）。
            self._tap_key("ret")
            app = self._wait_for_surface_change(home, "03_desktop_app", 10.0)
            if app is None or surface_diff(home, app) < MIN_SURFACE_DIFF:
                return self._fail(
                    reasons, outcomes, case_name,
                    "the screen did not change after Tab+Enter (the app did not start)",
                )
            # Esc：回到主屏。
            time.sleep(SETTLE_SECONDS)
            self._tap_key("esc")
            back = self._wait_for_surface_change(app, "04_desktop_back", 10.0)
            if back is None or surface_diff(app, back) < MIN_SURFACE_DIFF:
                return self._fail(
                    reasons, outcomes, case_name,
                    "the screen did not change after Esc (the app did not close)",
                )
            if not self._observe_check_panic(case_name, reasons):
                return self._fail(reasons, outcomes, case_name,
                                  reasons[-1] if reasons else "panic")
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS"})

            # ── 场景 5：杀掉桌面，guest 仍然活着 ──
            case_name = "desktop_exit"
            self.log(f"  [gui-desktop smp={self.smp}] starting {case_name}")
            ok, rc, out = self._run_cmd_with_rc(
                "kill $(cat /tmp/gd.pid 2>/dev/null) 2>/dev/null; sleep 1; printf ok",
                timeout=min(self.cmd_timeout, 10.0),
            )
            if not ok:
                return self._fail(reasons, outcomes, case_name,
                                  "could not kill the desktop")
            # guest 还活着：shell 仍然应答。
            if not self._send_canary(timeout=self.cmd_timeout):
                return self._fail(reasons, outcomes, case_name,
                                  "the guest stopped answering after the kill")
            if not self._observe_check_panic(case_name, reasons):
                return self._fail(reasons, outcomes, case_name,
                                  reasons[-1] if reasons else "panic")
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS"})

            return True, reasons, outcomes
        finally:
            self.close()

    @property
    def child_pid(self) -> Optional[int]:
        """QEMU 的 pid（没有则 None）。

        信号处理用它做**无锁**的硬杀。``close()`` 要去拿 ``Popen`` 的内部
        状态，而那把锁在主线程跑 ``poll()`` 时是被占着的 —— 在信号处理里
        等它有死锁风险。这里只读一个 int。
        """
        session = self.session
        if session is None:
            return None
        try:
            return getattr(session, "child_pid", None)
        except Exception:
            return None

    def close(self) -> None:
        """关闭 QMP 与 QEMU 子进程（幂等）。

        **必须真的杀掉 QEMU**：它持有磁盘镜像的写锁，活着就会让后续的
        ``make run`` 报「disk.img 被占用」。因此这里**不吞异常**：方法名写错
        曾经让 QEMU 变成孤儿进程（异常被 ``except: pass`` 吃掉，测试照样
        报 PASS，但镜像一直被锁着）。

        收尾失败只**记录**不**抛出**：``close()`` 会在 ``finally`` 与信号
        处理里被调用，抛异常会盖掉真正的失败原因。记录必须显眼 —— 收尾
        失败就意味着可能有孤儿。
        """
        errors: List[str] = []
        if self.qmp is not None:
            qmp, self.qmp = self.qmp, None
            try:
                qmp.close()
            except Exception as exc:
                errors.append(f"QMP close failed: {exc}")
        if self.session is not None:
            # ProcessSession 的收尾方法是 close()（不是 cleanup()）。
            session, self.session = self.session, None
            try:
                session.close()
            except Exception as exc:
                errors.append(f"QEMU session close failed: {exc}")
        for err in errors:
            print(f"  [gui-desktop] WARNING: {err}", file=sys.stderr)


# 正在跑的 runner：信号处理要用它收尾。
#
# 孤儿 QEMU 会一直占着磁盘镜像的文件锁，让后续的 ``make run`` 报
# 「disk.img 被占用」——所以 Ctrl-C / timeout 必须先杀掉子进程再退出。
_ACTIVE_RUNNERS: List["DesktopCaseRunner"] = []


def _kill_active_runners() -> List[str]:
    """硬杀所有在跑的 runner 的 QEMU 进程组。返回错误描述。

    **无锁**：这里只读 ``child_pid`` 并 ``killpg``。``close()`` 会去拿
    ``Popen`` 的内部状态，而主线程可能正持着那把锁（``poll()`` 期间）——
    在信号处理里等它有死锁风险。

    ``SIGKILL`` 不可捕获，是这里唯一可靠的保证：QEMU 活着就会占着磁盘
    镜像的写锁，后续的 ``make run`` 会报「disk.img 被占用」。
    """
    errors: List[str] = []
    for runner in list(_ACTIVE_RUNNERS):
        pid = runner.child_pid
        if not pid:
            continue
        try:
            os.killpg(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError, OSError) as exc:
            errors.append(f"could not SIGKILL the QEMU process group {pid}: {exc}")
    return errors


def _install_signal_handlers() -> None:
    """Ctrl-C / SIGTERM 时先收掉 QEMU 子进程，再退出。

    处理函数只做两件事：**无锁硬杀**在跑的 QEMU，然后抛出去让 finally
    链跑完（``run_test.py`` 借此写出 run archive）。退出码仍是
    128 + signum。
    """

    def _handler(signum, _frame):
        for err in _kill_active_runners():
            print(f"  [gui-desktop] WARNING: {err}", file=sys.stderr)
        if signum == signal.SIGINT:
            raise KeyboardInterrupt()
        raise SystemExit(128 + signum)

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(sig, _handler)
        except (ValueError, OSError):
            pass


def run_matrix(
    image: Path,
    firmware: Path,
    log: Callable[[str], None] = print,
    build_dir: Optional[Path] = None,
    session_factory: Optional[Callable] = None,
    qmp_factory: Optional[Callable] = None,
    timeout: Optional[float] = None,
    cpus: Optional[Tuple[int, ...]] = None,
) -> Tuple[bool, Dict[int, List[str]]]:
    """在 SMP 矩阵上跑桌面门禁；返回 (ok, reasons)。"""
    _install_signal_handlers()
    target_cpus = list(cpus if cpus is not None else CORE_MATRIX)
    reasons: Dict[int, List[str]] = {}
    all_outcomes: List[dict] = []
    archive = None

    if not Path(image).is_file():
        reasons[0] = [f"image {image} does not exist"]
        return False, reasons
    if not Path(firmware).is_file():
        reasons[0] = [f"firmware {firmware} does not exist"]
        return False, reasons

    image_sha_before = _sha256(Path(image))
    fw_sha_before = _sha256(Path(firmware))

    if build_dir:
        try:
            from qemutests.harness.result import RunArchive

            archive = RunArchive.create(build_dir=Path(build_dir), suite="gui-desktop")
        except Exception as exc:
            log(f"  [gui-desktop] warning: could not create RunArchive: {exc}")

    ok = True
    for smp in target_cpus:
        log(f"  [gui-desktop] === Running SMP={smp} matrix ===")
        smp_run_dir = (archive.run_dir / f"smp{smp}") if archive else None
        runner = DesktopCaseRunner(
            smp=smp,
            image=Path(image),
            firmware=Path(firmware),
            log=log,
            session_factory=session_factory,
            qmp_factory=qmp_factory,
            run_dir=smp_run_dir,
            timeout=timeout,
        )
        _ACTIVE_RUNNERS.append(runner)
        try:
            smp_ok, smp_reasons, smp_outcomes = runner.run_all()
        finally:
            _ACTIVE_RUNNERS.remove(runner)
        reasons[smp] = smp_reasons
        all_outcomes.extend(smp_outcomes)
        if not smp_ok:
            ok = False
            for r in smp_reasons:
                log(f"  [gui-desktop] SMP={smp} FAIL: {r}")
        else:
            log(f"  [gui-desktop] SMP={smp} PASS")

    # 矩阵里要求的核数必须都跑到过。调用方显式收窄了矩阵（例如调试时
    # 只跑 1 核）不算失败 —— 但要说明矩阵被收窄过。
    for req_smp in target_cpus:
        if req_smp not in reasons:
            reasons[req_smp] = reasons.get(req_smp, []) + [
                f"SMP={req_smp} missing from matrix execution"
            ]
            ok = False
    if list(target_cpus) != list(CORE_MATRIX):
        log(f"  [gui-desktop] note: the SMP matrix was narrowed to {target_cpus} "
            f"(the canonical matrix is {list(CORE_MATRIX)})")

    image_sha_after = _sha256(Path(image))
    fw_sha_after = _sha256(Path(firmware))
    if image_sha_before != image_sha_after:
        reasons[0] = reasons.get(0, []) + ["disk image sha256 changed after snapshot execution"]
        ok = False

    if archive is not None:
        _write_report(
            archive,
            Path(image),
            Path(firmware),
            image_sha_before,
            image_sha_after,
            fw_sha_before,
            fw_sha_after,
            target_cpus,
            ok,
            all_outcomes,
        )

    return ok, reasons


def _write_report(
    archive,
    image: Path,
    firmware: Path,
    img_before: Optional[str],
    img_after: Optional[str],
    fw_before: Optional[str],
    fw_after: Optional[str],
    cpus: List[int],
    ok: bool,
    outcomes: List[dict],
) -> None:
    try:
        from qemutests.harness.result import RunReport

        report = RunReport(
            schema_version=1,
            run_id=archive.run_dir.name,
            git_revision="unknown",
            git_dirty=False,
            profile=os.environ.get("OS01_PROFILE", "default"),
            suite="gui-desktop",
            request=None,
            declared_ids=None,
            observed_ids=None,
            argv=[],
            cpu_count=cpus[-1] if cpus else 1,
            memory_mib=512,
            tool_versions={},
            firmware_path=str(firmware),
            firmware_sha256_before=fw_before,
            firmware_sha256_after=fw_after,
            image_path=str(image),
            image_sha256_before=img_before,
            image_sha256_after=img_after,
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=0.0,
            runner_exit_code=0 if ok else 1,
            child_exit_code=None,
            stopped_by_runner=False,
            status="PASS" if ok else "FAIL",
            count_unit="case",
            outcomes=outcomes,
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
    except Exception as exc:
        print(f"  [gui-desktop] warning: failed to write RunReport: {exc}")


def _env_path(name: str) -> Optional[Path]:
    v = os.environ.get(name)
    return Path(v) if v else None


def test_gui_desktop(tester: Optional[Any] = None) -> bool:
    """run_test.py 的分派入口。"""
    image = _env_path("DISK_IMG") or (Path(tester.disk_img) if tester else None)
    firmware = _env_path("OVMF_FIRMWARE")
    if not image or not Path(image).is_file():
        print("FAIL: DISK_IMG is not a readable file")
        return False
    if not firmware or not Path(firmware).is_file():
        print("FAIL: OVMF_FIRMWARE is not a readable file")
        return False
    build_dir = _env_path("OS01_BUILD_DIR")
    print(f"gui-desktop suite: image={image} firmware={firmware} matrix={CORE_MATRIX}")
    ok, _ = run_matrix(image, firmware, log=print, build_dir=build_dir)
    return ok


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="OS01 GUI-3 Material desktop QEMU suite")
    ap.add_argument("--image", default=os.environ.get("DISK_IMG", "disk.img"))
    ap.add_argument("--firmware", default=os.environ.get("OVMF_FIRMWARE", ""))
    ap.add_argument("--build-dir", default=os.environ.get("OS01_BUILD_DIR"))
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--cpus", nargs="+", type=int, default=list(CORE_MATRIX))
    args = ap.parse_args(argv)

    if not args.firmware:
        print("ERROR: --firmware or OVMF_FIRMWARE required", file=sys.stderr)
        return 2

    try:
        ok, reasons = run_matrix(
            Path(args.image),
            Path(args.firmware),
            build_dir=Path(args.build_dir) if args.build_dir else None,
            timeout=args.timeout,
            cpus=args.cpus,
        )
    except KeyboardInterrupt:
        # 信号处理已经收掉了 QEMU；这里只把退出码对齐成惯例。
        print("gui-desktop suite: interrupted", file=sys.stderr)
        return 130
    if not ok:
        for smp in sorted(reasons):
            for r in reasons[smp]:
                print(f"FAIL [smp={smp}]: {r}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
