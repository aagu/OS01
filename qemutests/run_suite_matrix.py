#!/usr/bin/env python3
"""QEMU Suite Matrix runner and execution isolation (Task C5).

Manages immutable prepared image inputs, isolated execution environments
(private overlays/disks, unique QMP sockets, port ranges, and guards),
and bounded concurrent scheduling distinguishing shared vs exclusive suites.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple


class MatrixExecutionError(Exception):
    """Raised when suite matrix execution or verification fails."""
    pass


def compute_sha256(path: Path) -> str:
    """Compute hex-encoded SHA-256 of file at path."""
    hasher = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            hasher.update(chunk)
    return hasher.hexdigest()


@dataclass
class RunEnv:
    run_dir: Path
    disk_path: Path
    firmware_path: Path
    qmp_socket_path: Path
    guard_before_path: Path
    guard_after_path: Path
    allocated_ports: List[int]


class PortPool:
    """Thread-safe allocator of local TCP/UDP port blocks."""

    def __init__(self, base_port: int = 15000, max_port: int = 25000) -> None:
        self.lock = threading.Lock()
        self.base_port = base_port
        self.max_port = max_port
        self.current = base_port
        self.allocated: set[int] = set()

    def allocate(self, ports_count: int = 1) -> List[int]:
        with self.lock:
            for cand in range(self.base_port, self.max_port - ports_count + 1):
                if all((cand + i) not in self.allocated for i in range(ports_count)):
                    ports = [cand + i for i in range(ports_count)]
                    self.allocated.update(ports)
                    return ports
            raise MatrixExecutionError(f"No available ports in range {self.base_port}-{self.max_port}")

    def release(self, ports: List[int]) -> None:
        with self.lock:
            for p in ports:
                self.allocated.discard(p)


@dataclass
class ScheduledTask:
    manifest_path: Path
    config_id: str
    is_exclusive: bool


def allocate_run_env(
    manifest_path: Path,
    config_id: str,
    build_dir: Path,
    port_pool: Optional[PortPool] = None,
) -> RunEnv:
    """Allocate an isolated execution environment for a prepared configuration."""
    if not manifest_path.is_file():
        raise MatrixExecutionError(f"manifest not found: {manifest_path}")

    try:
        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
    except Exception as e:
        raise MatrixExecutionError(f"corrupt manifest JSON: {e}") from e

    if manifest_data.get("schema_version") != 1:
        raise MatrixExecutionError(f"unsupported schema_version: {manifest_data.get('schema_version')}")

    suite_id = manifest_data.get("suite_id", "unknown")
    cases = manifest_data.get("cases", [])
    matching_case = None
    for c in cases:
        if c.get("config_id") == config_id:
            matching_case = c
            break

    if not matching_case:
        raise MatrixExecutionError(f"config_id '{config_id}' not found in {manifest_path}")

    orig_img = Path(matching_case["image_path"])
    if not orig_img.is_file():
        raise MatrixExecutionError(f"image not found: {orig_img}")

    actual_img_sha = compute_sha256(orig_img)
    expected_img_sha = matching_case.get("image_sha256")
    if actual_img_sha != expected_img_sha:
        raise MatrixExecutionError(
            f"checksum mismatch for image {orig_img}: actual {actual_img_sha}, expected {expected_img_sha}"
        )

    orig_fw = Path(matching_case["firmware_path"]) if matching_case.get("firmware_path") else None
    if orig_fw and orig_fw.is_file():
        actual_fw_sha = compute_sha256(orig_fw)
        expected_fw_sha = matching_case.get("firmware_sha256")
        if expected_fw_sha and actual_fw_sha != expected_fw_sha:
            raise MatrixExecutionError(
                f"checksum mismatch for firmware {orig_fw}: actual {actual_fw_sha}, expected {expected_fw_sha}"
            )

    run_token = uuid.uuid4().hex[:12]
    run_dir = build_dir / "run_isolated" / suite_id / f"{config_id}_{run_token}"
    run_dir.mkdir(parents=True, exist_ok=True)

    # Isolated disk copy in run_dir so source image is never mutated
    disk_path = run_dir / "disk.img"
    shutil.copy2(orig_img, disk_path)

    fw_path = orig_fw if orig_fw else (run_dir / "OVMF.fd")
    qmp_socket = run_dir / "qmp.sock"
    guard_before = run_dir / "normal.before"
    guard_after = run_dir / "normal.after"

    ports = port_pool.allocate(ports_count=4) if port_pool else []

    return RunEnv(
        run_dir=run_dir,
        disk_path=disk_path,
        firmware_path=fw_path,
        qmp_socket_path=qmp_socket,
        guard_before_path=guard_before,
        guard_after_path=guard_after,
        allocated_ports=ports,
    )


def run_command_bounded(
    argv: List[str],
    timeout_s: float,
    env: Optional[Dict[str, str]] = None,
    cwd: Optional[Path] = None,
) -> Tuple[int, str]:
    """Execute command in a new process group with bounded timeout."""
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)

    try:
        proc = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
            env=merged_env,
            cwd=str(cwd) if cwd else None,
        )
    except Exception as e:
        return 2, f"Failed to launch {argv}: {e}"

    try:
        out, _ = proc.communicate(timeout=timeout_s)
        return proc.returncode, out
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        out, _ = proc.communicate()
        return 1, (out or "") + f"\n[run_suite_matrix] TIMEOUT after {timeout_s}s"


def execute_matrix_tasks(
    tasks: List[ScheduledTask],
    jobs: int = 1,
    executor: Optional[Callable[[ScheduledTask], int]] = None,
) -> int:
    """Schedule and execute matrix tasks respecting shared vs exclusive resources."""
    if executor is None:
        def default_exec(t: ScheduledTask) -> int:
            cmd = ["make", "_test-suite-run", f"PREPARED_MANIFEST={t.manifest_path}", f"RUN_CONFIG={t.config_id}"]
            rc, _ = run_command_bounded(cmd, timeout_s=300.0)
            return rc
        executor = default_exec

    max_rc = 0
    # Partition tasks while maintaining scheduling order
    # Any exclusive task runs standalone
    shared_batch: List[ScheduledTask] = []

    def flush_shared() -> int:
        nonlocal shared_batch
        if not shared_batch:
            return 0
        local_max = 0
        if jobs <= 1 or len(shared_batch) == 1:
            for t in shared_batch:
                rc = executor(t)
                if rc > local_max:
                    local_max = rc
        else:
            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
                futures = [pool.submit(executor, t) for t in shared_batch]
                for fut in concurrent.futures.as_completed(futures):
                    rc = fut.result()
                    if rc > local_max:
                        local_max = rc
        shared_batch = []
        return local_max

    for task in tasks:
        if task.is_exclusive:
            # Drain any pending shared tasks before starting exclusive task
            shared_rc = flush_shared()
            if shared_rc > max_rc:
                max_rc = shared_rc
            # Run exclusive task strictly alone
            ex_rc = executor(task)
            if ex_rc > max_rc:
                max_rc = ex_rc
        else:
            shared_batch.append(task)
            if len(shared_batch) >= (jobs * 2):
                batch_rc = flush_shared()
                if batch_rc > max_rc:
                    max_rc = batch_rc

    final_rc = flush_shared()
    if final_rc > max_rc:
        max_rc = final_rc

    return max_rc


def prepare_suite(
    suite_id: str,
    manifest_path: Path,
    build_dir: Path,
    profile: str,
    img_path: Path,
    fw_path: Path,
    variant: str = "none",
) -> None:
    """Create prepared immutable inputs and manifest for suite_id."""
    prep_dir = build_dir / "prepared" / suite_id
    prep_dir.mkdir(parents=True, exist_ok=True)
    prep_img = prep_dir / img_path.name
    shutil.copy2(img_path, prep_img)

    prep_fw = prep_dir / fw_path.name if fw_path.is_file() else fw_path
    if fw_path.is_file() and not prep_fw.is_file():
        shutil.copy2(fw_path, prep_fw)

    manifest_data = {
        "schema_version": 1,
        "batch_id": f"batch_{int(time.time())}",
        "suite_id": suite_id,
        "profile": profile,
        "variant": variant,
        "tool_versions": {"qemu": "8.2.2"},
        "cases": [
            {
                "config_id": f"{suite_id}_default",
                "smp": 1,
                "memory_mib": 512,
                "image_path": str(prep_img.resolve()),
                "firmware_path": str(prep_fw.resolve()) if prep_fw.is_file() else "",
                "image_sha256": compute_sha256(prep_img),
                "firmware_sha256": compute_sha256(prep_fw) if prep_fw.is_file() else "",
                "is_exclusive": (suite_id == "terminal-present"),
            }
        ],
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest_data, indent=2) + "\n", encoding="utf-8")


def run_suite_config(
    manifest_path: Path,
    config_id: str,
    build_dir: Path,
    profile: str,
) -> int:
    """Run a single prepared suite configuration under complete isolation."""
    env = allocate_run_env(manifest_path, config_id, build_dir)
    manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
    suite_id = manifest_data.get("suite_id")

    # Dispatch to existing suite runner without rebuilding
    sub_env = {
        "DISK_IMG": str(env.disk_path),
        "OVMF_FIRMWARE": str(env.firmware_path),
        "OS01_BUILD_DIR": str(build_dir.resolve()),
        "OS01_ISOLATED_RUN_DIR": str(env.run_dir.resolve()),
    }

    if suite_id == "kernel-selftest":
        cmd = ["python3", "qemutests/run_kernel_selftest.py"]
    else:
        cmd = ["python3", "qemutests/run_test.py", suite_id]

    rc, out = run_command_bounded(cmd, timeout_s=300.0, env=sub_env)
    print(out)
    return rc


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Manage QEMU suite matrix execution")
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    # prepare
    prep_p = subparsers.add_parser("prepare", help="Prepare suite inputs and manifest")
    prep_p.add_argument("--suite-id", required=True)
    prep_p.add_argument("--manifest", required=True, type=Path)
    prep_p.add_argument("--build-dir", required=True, type=Path)
    prep_p.add_argument("--profile", required=True)
    prep_p.add_argument("--image", required=True, type=Path)
    prep_p.add_argument("--firmware", required=True, type=Path)
    prep_p.add_argument("--variant", default="none")

    # run
    run_p = subparsers.add_parser("run", help="Run a prepared suite configuration")
    run_p.add_argument("--manifest", required=True, type=Path)
    run_p.add_argument("--config", required=True)
    run_p.add_argument("--build-dir", required=True, type=Path)
    run_p.add_argument("--profile", required=True)

    # matrix
    mat_p = subparsers.add_parser("matrix", help="Run catalog suite matrix")
    mat_p.add_argument("--catalog", required=True, type=Path)
    mat_p.add_argument("--jobs", default=1, type=int)
    mat_p.add_argument("--suites", default=None)

    args = parser.parse_args(argv)

    if args.subcommand == "prepare":
        try:
            prepare_suite(
                suite_id=args.suite_id,
                manifest_path=args.manifest,
                build_dir=args.build_dir,
                profile=args.profile,
                img_path=args.image,
                fw_path=args.firmware,
                variant=args.variant,
            )
            return 0
        except Exception as e:
            print(f"[run_suite_matrix] ERROR in prepare: {e}", file=sys.stderr)
            return 1

    elif args.subcommand == "run":
        try:
            return run_suite_config(
                manifest_path=args.manifest,
                config_id=args.config,
                build_dir=args.build_dir,
                profile=args.profile,
            )
        except Exception as e:
            print(f"[run_suite_matrix] ERROR in run: {e}", file=sys.stderr)
            return 1

    return 2


if __name__ == "__main__":
    sys.exit(main())
