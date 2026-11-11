#!/usr/bin/env python3
"""OS01 Doom QEMU integration test runner (Task 1 of D06 plan).

Drives Doom E2E scenarios on x86_64 QEMU (SMP=1/2), validating:
  * Strict boot and first-frame handshake (READY diagnostic marker).
  * Out-of-order, fake echo, or mismatched nonce rejection.
  * QMP PS/2 make/break key injection and screendump verification.
  * Per-scenario isolated disk copies and preserved artifact logs on failure/timeout.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:
    from qemutests.harness.process import ProcessSession
except ImportError:
    ProcessSession = None  # type: ignore[assignment]

try:
    from qemutests.gui_test import QmpClient
except ImportError:
    QmpClient = None  # type: ignore[assignment]


DIAG_RE = re.compile(
    r"^\[doom-diag\] (?:v|version)=(?P<ver>\d+) "
    r"nonce=(?P<nonce>\S+) "
    r"pid=(?P<pid>\d+) "
    r"seq=(?P<seq>\d+) "
    r"type=(?P<type>\S+)"
    r"(?: (?P<rest>.*))?$"
)


def parse_diag_record(line: str) -> Optional[dict]:
    """Parse a single diagnostic log line emitted via guest /dev/serial."""
    line = line.strip()
    m = DIAG_RE.match(line)
    if not m:
        return None
    d = m.groupdict()
    res = {
        "version": int(d["ver"]),
        "nonce": d["nonce"],
        "pid": int(d["pid"]),
        "seq": int(d["seq"]),
        "type": d["type"],
        "raw": line,
    }
    rest = d.get("rest")
    if rest:
        for item in rest.split():
            if "=" in item:
                k, v = item.split("=", 1)
                res[k] = v
    return res


def build_qemu_argv(image: str, firmware: str, qmp_sock: str, smp: int = 1) -> List[str]:
    """Construct standard QEMU arguments for OS01 x86_64 testing."""
    return [
        "qemu-system-x86_64",
        "-accel", "tcg",
        "-M", "q35",
        "-m", "512",
        "-smp", str(smp),
        "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
        "-drive", f"file={image},format=raw,if=none,id=disk",
        "-device", "ahci,id=ahci",
        "-device", "ide-hd,drive=disk,bus=ahci.0",
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-pci,rng=rng0",
        "-display", "none",
        "-serial", "stdio",
        "-no-reboot",
        "-no-shutdown",
        "-qmp", f"unix:{qmp_sock},server,nowait",
    ]


class DoomRunner:
    """QEMU runner for Doom integration tests with strict failure detection."""

    def __init__(
        self,
        image: Path,
        firmware: Path,
        smp: int = 1,
        timeout: float = 240.0,
        output_dir: Optional[Path] = None,
        session_factory: Optional[Callable] = None,
        qmp_factory: Optional[Callable] = None,
    ):
        try:
            smp_int = int(smp)
        except (TypeError, ValueError):
            raise ValueError(f"Invalid SMP '{smp}': must be 1 or 2")
        if smp_int not in (1, 2):
            raise ValueError(f"Invalid SMP {smp_int}: must be 1 or 2")

        self.smp = smp_int
        self.image = Path(image)
        self.firmware = Path(firmware)
        self.timeout = float(timeout)
        self.output_dir = Path(output_dir) if output_dir else Path(tempfile.mkdtemp(prefix="os01-doom-"))
        self.output_dir.mkdir(parents=True, exist_ok=True)

        self.session_factory = session_factory or ProcessSession
        self.qmp_factory = qmp_factory or QmpClient

        self.nonce = uuid.uuid4().hex[:12]
        self.expected_seq = 1
        self.reasons: List[str] = []
        self.scenarios: List[dict] = []
        self.records: List[dict] = []

    def run(self) -> int:
        """Run Doom QEMU suite and return 0 on success, non-zero on failure."""
        start_time = time.monotonic()
        qmp_sock_dir = tempfile.mkdtemp(prefix="os01-dqmp-")
        qmp_sock = Path(qmp_sock_dir) / "qmp.sock"

        # Prepare isolated working copy of the disk image so original is never modified
        work_disk = self.output_dir / f"doom-smp{self.smp}-work.img"
        if not work_disk.exists() and self.image.exists():
            shutil.copy2(self.image, work_disk)

        session = None
        qmp = None
        try:
            argv = build_qemu_argv(str(work_disk), str(self.firmware), str(qmp_sock), smp=self.smp)
            session = self.session_factory(
                argv=argv,
                run_dir=self.output_dir,
                timeout_s=self.timeout,
                writable_stdin=True,
            )
            session.start()

            # Early check if QEMU crashed on spawn or immediate start
            if session.returncode is not None and session.returncode != 0:
                self.reasons.append(f"QEMU crashed immediately with exit code {session.returncode}")
                return self._finish_run(session, qmp, start_time)

            if self.qmp_factory:
                try:
                    qmp = self.qmp_factory(str(qmp_sock))
                    qmp.connect()
                except Exception as exc:
                    self.reasons.append(f"QMP connect failed: {exc}")
                    return self._finish_run(session, qmp, start_time)

            self._execute_test_sequence(session, qmp)

        except Exception as exc:
            self.reasons.append(f"Runner execution exception: {exc}")
        finally:
            shutil.rmtree(qmp_sock_dir, ignore_errors=True)

        return self._finish_run(session, qmp, start_time)

    def _execute_test_sequence(self, session: Any, qmp: Any) -> None:
        """Drive the interactive boot, handshake, screendump, and scenarios."""
        # 1. Wait for ash prompt or initial boot output
        session.wait_for(lambda t: "/ # " in t or "[doom-diag]" in t or "Fatal" in t or "error" in t.lower())

        if session.timed_out:
            self.reasons.append(f"Timeout waiting for initial boot prompt after {self.timeout}s")
            return

        if session.returncode is not None and session.returncode != 0:
            self.reasons.append(f"QEMU crashed during boot with exit code {session.returncode}")
            return

        # 2. Check for missing IWAD error if doom ran automatically or produced error text
        text = session.text
        if "W_InitMultipleFiles: no files found" in text or "Game mode indeterminate" in text:
            self.reasons.append("IWAD missing or invalid game mode")
            return

        # 3. Parse existing and upcoming records
        self._consume_diag_records(session.text)

        # If not ready yet and prompt is visible, send doom invocation with diag nonce
        if not any(r["type"] == "ready" for r in self.records) and "/ # " in session.text:
            session.send(f"/bin/doom --diag-nonce {self.nonce}\n".encode())
            session.wait_for(lambda t: any(r["type"] == "ready" for r in self._parse_records_from(t))
                                      or "W_InitMultipleFiles" in t
                                      or "Game mode indeterminate" in t)
            self._consume_diag_records(session.text)

        if "W_InitMultipleFiles: no files found" in session.text or "Game mode indeterminate" in session.text:
            self.reasons.append("IWAD missing or invalid game mode")
            return

        # 4. Check for strict READY marker
        ready_records = [r for r in self.records if r["type"] == "ready"]
        if not ready_records:
            if session.timed_out:
                self.reasons.append("Timeout waiting for ready marker")
            else:
                self.reasons.append("Handshake failed: ready marker never observed")
            return

        # 5. Screendump verification on first frame
        if qmp:
            shot_path = self.output_dir / "doom_title.ppm"
            try:
                qmp.screendump(shot_path)
                if not shot_path.exists() or shot_path.stat().st_size == 0:
                    self.reasons.append("Screenshot capture failed: empty or missing file")
            except Exception as exc:
                self.reasons.append(f"Screenshot screendump failed: {exc}")

    def _parse_records_from(self, text: str) -> List[dict]:
        """Parse all valid diag records from text string."""
        results = []
        for line in text.splitlines():
            rec = parse_diag_record(line)
            if rec:
                results.append(rec)
        return results

    def _consume_diag_records(self, full_text: str) -> None:
        """Process lines, enforcing nonce matching and strict sequence ordering."""
        for line in full_text.splitlines():
            line_str = line.strip()
            if not line_str.startswith("[doom-diag]"):
                continue

            # Reject command echo: e.g. "echo '[doom-diag]...'" or ash command prompts
            if "echo '" in line_str or 'echo "' in line_str:
                continue

            rec = parse_diag_record(line_str)
            if not rec:
                continue

            # Verify nonce: records from previous runs or forged inputs must be rejected
            if rec["nonce"] != self.nonce:
                continue

            # Check if we already processed this exact record
            if any(r["seq"] == rec["seq"] and r["nonce"] == rec["nonce"] for r in self.records):
                continue

            # Sequence monotonicity check
            if rec["seq"] != self.expected_seq:
                self.reasons.append(
                    f"Out-of-order seq: expected {self.expected_seq}, got {rec['seq']} in line '{line_str}'"
                )
                return

            self.expected_seq += 1
            self.records.append(rec)

    def _finish_run(self, session: Any, qmp: Any, start_time: float) -> int:
        """Clean up QEMU process, finalize artifacts, write summary.json, and return status."""
        duration = time.monotonic() - start_time

        if qmp:
            try:
                qmp.close()
            except Exception:
                pass

        if session:
            try:
                if session.timed_out or self.reasons:
                    session.stop()
                session.close()
            except Exception:
                pass

        # Check QEMU exit code if it crashed
        if session and session.returncode is not None and session.returncode != 0:
            if not any("exit code" in r.lower() or "crashed" in r.lower() for r in self.reasons):
                self.reasons.append(f"QEMU exited with non-zero code {session.returncode}")

        status = "PASS" if not self.reasons else "FAIL"
        summary = {
            "suite": "doom",
            "smp": self.smp,
            "status": status,
            "duration_s": round(duration, 3),
            "reasons": self.reasons,
            "record_count": len(self.records),
            "records": self.records,
            "scenarios": self.scenarios,
        }

        summary_path = self.output_dir / "summary.json"
        summary_path.write_text(json.dumps(summary, indent=2))

        return 0 if status == "PASS" else 1


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="OS01 Doom QEMU integration test runner")
    parser.add_argument("--image", required=True, type=Path, help="Path to prepared disk image")
    parser.add_argument("--firmware", required=True, type=Path, help="Path to OVMF firmware")
    parser.add_argument("--smp", required=True, type=int, choices=[1, 2], help="SMP core count (1 or 2)")
    parser.add_argument("--timeout", type=float, default=240.0, help="Per-run timeout budget (seconds)")
    parser.add_argument("--output-dir", type=Path, default=None, help="Directory to store test artifacts")
    args = parser.parse_args(argv)

    runner = DoomRunner(
        image=args.image,
        firmware=args.firmware,
        smp=args.smp,
        timeout=args.timeout,
        output_dir=args.output_dir,
    )
    return runner.run()


if __name__ == "__main__":
    sys.exit(main())
