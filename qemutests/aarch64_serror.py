#!/usr/bin/env python3
# @os01-test
# id: aarch64-serror
# layer: qemu_e2e
# subsystem: intr
# timing: strict
# tier: nightly
# scenarios: []
"""Acceptance harness for aarch64 synthetic SError vector entry & park (Task 4)."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests.harness.process import ProcessSession  # noqa: E402
from qemutests.harness.qmp import QmpClient  # noqa: E402
from qemutests.harness.result import RunArchive  # noqa: E402

SUITE = "aarch64-serror"

EXPECTED_SPSR = {
    "el1t": 0x244,
    "el1h": 0x245,
    "el0t": 0x240,
}

EXPECTED_ESR = 0x00000000BE000055

_DAIF_RE = re.compile(
    r"^\r?\[aarch64-serror-test\] daif cpu=([01]) a=([01]) i=([01])\r?$",
    re.MULTILINE,
)
_ARMED_RE = re.compile(
    r"^\r?\[aarch64-serror-test\] armed origin=(el1t|el1h|el0t) mpidr=0x([0-9a-f]{16})\r?$",
    re.MULTILINE,
)
_FATAL_ANY_RE = re.compile(r"\[aarch64-serror\] FATAL\b")
_FATAL_LINE_RE = re.compile(
    r"^\r?\[aarch64-serror\] FATAL origin=(el1t|el1h|el0t) mpidr=0x([0-9a-f]{16}) "
    r"ec=0x([0-9a-f]{2}) esr=0x([0-9a-f]{16}) elr=0x([0-9a-f]{16}) "
    r"spsr=0x([0-9a-f]{16}) far=n/a\r?$",
    re.MULTILINE,
)
_FORBIDDEN_RE = re.compile(
    r"\[aarch64-serror-test\] returned\b|"
    r"\[aarch64-serror-test\] FAIL\b|"
    r"\[aarch64-sync\] FATAL\b"
)
_PC_RE = re.compile(r"\bPC=([0-9a-fA-F]+)\b")


def serror_evidence(
    log: str,
    origin: str,
    elr: int,
    park_pc: int,
    park_start: int,
    park_size: int,
) -> bool:
    if origin not in EXPECTED_SPSR:
        return False
    if park_size <= 0 or not (park_start <= park_pc < park_start + park_size):
        return False
    if _FORBIDDEN_RE.search(log):
        return False

    daif_seen: dict[str, tuple[str, str]] = {}
    for m in _DAIF_RE.finditer(log):
        cpu, a, i = m.groups()
        if cpu in daif_seen:
            return False
        daif_seen[cpu] = (a, i)
    if daif_seen.get("0") != ("0", "1") or daif_seen.get("1") != ("0", "1"):
        return False

    armed_matches = list(_ARMED_RE.finditer(log))
    if len(armed_matches) != 1:
        return False
    armed_origin, armed_mpidr = armed_matches[0].groups()
    if armed_origin != origin:
        return False

    if len(_FATAL_ANY_RE.findall(log)) != 1:
        return False
    fatal_matches = list(_FATAL_LINE_RE.finditer(log))
    if len(fatal_matches) != 1:
        return False
    if armed_matches[0].start() >= fatal_matches[0].start():
        return False

    f_origin, f_mpidr, f_ec, f_esr, f_elr, f_spsr = fatal_matches[0].groups()
    if f_origin != origin:
        return False
    if f_mpidr != armed_mpidr:
        return False
    if int(f_ec, 16) != 0x2F:
        return False
    if int(f_esr, 16) != EXPECTED_ESR:
        return False
    if int(f_elr, 16) != elr:
        return False
    if int(f_spsr, 16) != EXPECTED_SPSR[origin]:
        return False
    return True


def _file_sha256(path: Path) -> str:
    with path.open("rb") as fp:
        return hashlib.file_digest(fp, "sha256").hexdigest()


def verify_image_kernel_sha256(image_path: Path, elf_path: Path, tmp_dir: Path) -> str:
    extracted = tmp_dir / "extracted_kernel.elf"
    res = subprocess.run(
        ["mcopy", "-i", str(image_path), "::/kernel.elf", str(extracted)],
        capture_output=True,
        text=True,
    )
    if res.returncode != 0:
        subprocess.run(
            ["mcopy", "-i", f"{image_path}@@1048576", "::/kernel.elf", str(extracted)],
            check=True,
            capture_output=True,
            text=True,
        )
    img_sha = _file_sha256(extracted)
    elf_sha = _file_sha256(elf_path)
    if img_sha != elf_sha:
        raise RuntimeError(
            f"kernel.elf SHA-256 mismatch: image={img_sha} vs elf={elf_sha}"
        )
    return elf_sha


def read_serror_elf_symbols(elf_path: Path) -> tuple[int, int, int]:
    nm = os.environ.get("LLVM_NM", "llvm-nm")
    res = subprocess.run(
        [nm, "-S", str(elf_path)],
        check=True,
        capture_output=True,
        text=True,
    )
    elr = None
    park_start = None
    park_size = None
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) == 4:
            addr_s, size_s, _, name = parts
            if name == "aarch64_serror_park":
                park_start = int(addr_s, 16)
                park_size = int(size_s, 16)
            elif name == "aarch64_serror_test_returned":
                elr = int(addr_s, 16)
        elif len(parts) == 3:
            addr_s, _, name = parts
            if name == "aarch64_serror_test_returned":
                elr = int(addr_s, 16)
    if elr is None or park_start is None or park_size is None or park_size <= 0:
        raise RuntimeError(f"missing SError test symbols in {elf_path}")
    return elr, park_start, park_size


def _build_diagnostic_dtb(qemu: str, firmware: Path, out_dtb: Path) -> None:
    del firmware
    sparse = out_dtb.with_suffix(".dtb.sparse")
    subprocess.run(
        [
            qemu, "-M", "virt,gic-version=2", "-cpu", "cortex-a53",
            "-smp", "2", "-machine", f"dumpdtb={sparse}",
            "-display", "none", "-m", "512",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        ["dtc", "-I", "dtb", "-O", "dtb", "-o", str(out_dtb), str(sparse)],
        check=True,
        capture_output=True,
        text=True,
    )
    sparse.unlink(missing_ok=True)


def _parse_pc(hmp_output: str) -> int | None:
    m = _PC_RE.search(hmp_output or "")
    if not m:
        return None
    return int(m.group(1), 16)


def run_case(
    args: argparse.Namespace,
    *,
    build_dir: str | None = None,
    profile: str = "default",
) -> int:
    origin = args.origin
    image_path = Path(args.image)
    elf_path = Path(args.elf)
    firmware_path = Path(args.firmware)

    if build_dir:
        archive = RunArchive.create(Path(build_dir), f"{SUITE}-{origin}")
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir) / origin
    run_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix=f"serror-{origin}-") as tmp_s:
        tmp_dir = Path(tmp_s)
        elf_sha = verify_image_kernel_sha256(image_path, elf_path, tmp_dir)
        elr, park_start, park_size = read_serror_elf_symbols(elf_path)

        dtb_path = run_dir / "virt.dtb"
        _build_diagnostic_dtb(args.qemu, firmware_path, dtb_path)
        qmp_sock = tmp_dir / "qmp.sock"

        command = [
            args.qemu, "-M", "virt,gic-version=2,acpi=off",
            "-cpu", "cortex-a53", "-smp", "2", "-m", "512",
            "-drive", f"if=pflash,format=raw,readonly=on,file={firmware_path}",
            "-drive", f"if=none,file={image_path},format=raw,readonly=on,id=disk",
            "-device", "virtio-blk-device,drive=disk",
            "-dtb", str(dtb_path),
            "-qmp", f"unix:{qmp_sock},server=on,wait=off",
            "-serial", "stdio", "-display", "none",
            "-no-reboot", "-no-shutdown",
        ]

        deadline = time.monotonic() + float(args.timeout)
        session = ProcessSession(argv=command, run_dir=run_dir, timeout_s=args.timeout)
        qmp = QmpClient(str(qmp_sock), timeout=float(args.timeout))
        qmp.transcript = run_dir / "qmp.jsonl"

        park_pc = None
        reg_outputs: list[str] = []
        fatal_seen = False
        error_msg = None

        try:
            session.start()
            session.wait_for(
                lambda text: bool(_FORBIDDEN_RE.search(text))
                or (len(_FATAL_LINE_RE.findall(text)) == 1 and bool(_ARMED_RE.search(text)))
            )
            if session.timed_out:
                error_msg = "timeout waiting for fatal evidence"
            elif _FORBIDDEN_RE.search(session.text):
                error_msg = "forbidden marker observed before stop"
            elif session.returncode is not None:
                error_msg = f"QEMU exited early (rc={session.returncode})"
            else:
                fatal_seen = True
                qmp.connect(deadline=deadline)
                while time.monotonic() < deadline:
                    if _FORBIDDEN_RE.search(session.text):
                        error_msg = "forbidden marker observed during park sampling"
                        break
                    qmp.execute("stop", deadline=deadline)
                    qmp.wait_event("STOP", deadline=deadline)
                    hmp_res = qmp.execute(
                        "human-monitor-command",
                        {"command-line": "info registers", "cpu-index": 0},
                        deadline=deadline,
                    )
                    reg_text = hmp_res if isinstance(hmp_res, str) else json.dumps(hmp_res)
                    reg_outputs.append(reg_text)
                    pc = _parse_pc(reg_text)
                    if pc is None:
                        error_msg = "failed to parse CPU0 PC from info registers"
                        break
                    park_pc = pc
                    if park_start <= pc < park_start + park_size:
                        break
                    qmp.execute("cont", deadline=deadline)
                    time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
                else:
                    error_msg = "deadline expired before CPU0 PC entered park range"
        except Exception as exc:  # noqa: BLE001
            error_msg = str(exc)
        finally:
            try:
                qmp.close()
            except Exception:
                pass
            try:
                session.stop()
                session.close()
            except Exception:
                pass

        (run_dir / "registers.txt").write_text("\n---\n".join(reg_outputs))
        qemu_self_exited = fatal_seen and not session.stopped_by_runner and not session.timed_out
        accepted = (
            error_msg is None
            and not session.timed_out
            and not qemu_self_exited
            and park_pc is not None
            and serror_evidence(session.text, origin, elr, park_pc, park_start, park_size)
        )

        metadata = {
            "origin": origin,
            "profile": profile,
            "elf": str(elf_path),
            "elf_sha256": elf_sha,
            "image": str(image_path),
            "elr": f"0x{elr:016x}",
            "park_start": f"0x{park_start:016x}",
            "park_size": park_size,
            "park_pc": f"0x{park_pc:016x}" if park_pc is not None else None,
            "error": error_msg,
            "qemu_self_exited": qemu_self_exited,
            "result": "PASS" if accepted else "FAIL",
        }
        (run_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        print(json.dumps({"event": "serror-case", **metadata}))
        return 0 if accepted else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--build-dir", default=None)
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE", "default"))
    parser.add_argument("--origin", choices=("el1t", "el1h", "el0t"), default=None)
    parser.add_argument("--image", default=None)
    parser.add_argument("--elf", default=None)
    parser.add_argument("--el1t-image", default=None)
    parser.add_argument("--el1t-elf", default=None)
    parser.add_argument("--el1h-image", default=None)
    parser.add_argument("--el1h-elf", default=None)
    parser.add_argument("--el0t-image", default=None)
    parser.add_argument("--el0t-elf", default=None)
    args = parser.parse_args()

    if args.origin:
        if not args.image or not args.elf:
            parser.error("--image and --elf are required with --origin")
        return run_case(args, build_dir=args.build_dir, profile=args.profile)

    rc = 0
    for origin in ("el1t", "el1h", "el0t"):
        img = getattr(args, f"{origin}_image")
        elf = getattr(args, f"{origin}_elf")
        if not img or not elf:
            parser.error(f"--{origin}-image and --{origin}-elf are required")
        sub = argparse.Namespace(**vars(args))
        sub.origin = origin
        sub.image = img
        sub.elf = elf
        if run_case(sub, build_dir=args.build_dir, profile=args.profile) != 0:
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
