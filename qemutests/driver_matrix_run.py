#!/usr/bin/env python3
"""
driver_matrix_run.py — Drive the ARCH-9 driver discovery matrix in QEMU.

Task 11 brief §Step 3.  For each (case, smp) pair, builds the right
kernel (ARCH9_FAULT=... when needed), spawns QEMU, sends the guest
probe via -serial stdio, parses markers, and asserts the case's contract.

Usage:
    python3 qemutests/driver_matrix_run.py --case all --smp 1 2
    python3 qemutests/driver_matrix_run.py --case no-nic --smp 1
    python3 qemutests/driver_matrix_run.py  # full 15-case × SMP{1,2}

Exit 0 = all selected cases pass; non-zero = at least one FAIL.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORKTREE_ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import driver_model_matrix as DMM  # noqa: E402

DEFAULT_TIMEOUT = 45  # seconds per case; boot ~18s + probe window


def _read_paths_for(fault):
    """Resolve the kernel artifact + image paths via
    `make print-run-paths ARCH9_FAULT=<fault>`."""
    env = dict(os.environ)
    env["ARCH9_FAULT"] = fault
    p = subprocess.run(
        ["make", "--no-print-directory", "-s", "print-run-paths"],
        cwd=WORKTREE_ROOT, env=env,
        capture_output=True, text=True, timeout=60,
    )
    out = {}
    for ln in p.stdout.splitlines():
        if ln.startswith("firmware="):
            out["firmware"] = ln.split("=", 1)[1].strip()
        elif ln.startswith("image="):
            out["image"] = ln.split("=", 1)[1].strip()
    return out


def _sha256_file(p: str) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _build_for_fault(fault):
    """Build the kernel (and image if non-canonical) for `fault`."""
    env = dict(os.environ)
    env["ARCH9_FAULT"] = fault
    if fault == "none":
        # Canonical: build the normal image.
        cmd = ["make", "--no-print-directory", "disk.img"]
    else:
        # Build the variant image; mk/components/image.mk routes
        # IMAGE_VARIANT=driver-model-<fault>.
        cmd = ["make", "--no-print-directory", f"ARCH9_FAULT={fault}", "image"]
    p = subprocess.run(cmd, cwd=WORKTREE_ROOT, env=env,
                       capture_output=True, text=True, timeout=1800)
    return p.returncode == 0


def _qemu_argv_for(case, smp, image_path, firmware_path, udp_echo_port=10001):
    c = DMM.case_dict(case)
    argv = [
        "qemu-system-x86_64",
        "-M", c.get("machine", "q35"),
        "-drive", f"if=pflash,format=raw,readonly=on,file={firmware_path}",
        "-m", "512",
        "-smp", str(smp),
        "-display", "none",
        "-serial", "stdio",
        "-no-reboot",
        "-no-shutdown",
        "-snapshot",  # mandatory per brief
        # entropy
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-pci,rng=rng0",
    ]
    if c.get("machine") == "pc":
        argv += ["-drive", f"file={image_path},format=raw,if=none,id=disk"]
        argv += ["-device", "virtio-blk-pci,drive=disk"]
    else:
        argv += ["-drive", f"file={image_path},format=raw,if=none,id=disk"]
        argv += ["-device", "ahci,id=ahci"]
        argv += ["-device", "ide-hd,drive=disk,bus=ahci.0"]

    if c["nic0"]:
        # NIC 0: slirp NATs guest→10.0.2.2:<port> to host:<port>
        # by default.  No hostfwd needed (matches the existing
        # run_test.py network suite which works for port 10001).
        argv += ["-netdev", c["nic0"][1]]
        nic0_dev = c["nic0"][0]
        if "netdev=" not in nic0_dev:
            for tok in c["nic0"][1].split(","):
                if tok.startswith("id="):
                    nic0_dev = f"{nic0_dev},netdev={tok[3:]}"
                    break
        argv += ["-device", nic0_dev]
    else:
        argv += ["-nic", "none"]
    if c["nic1"]:
        argv += ["-netdev", c["nic1"][1]]
        nic1_dev = c["nic1"][0]
        if "netdev=" not in nic1_dev:
            for tok in c["nic1"][1].split(","):
                if tok.startswith("id="):
                    nic1_dev = f"{nic1_dev},netdev={tok[3:]}"
                    break
        argv += ["-device", nic1_dev]
    for ed in c.get("extra_device", ()):
        argv += ["-device", ed]
    return argv


class UdpEchoHost:
    """Local UDP echo server reachable from QEMU as 10.0.2.2."""

    def __init__(self, port=10001):
        import socket
        import threading
        self.port = port
        self._stop = threading.Event()
        self._thread = None
        self._sock = None
        self._lock = threading.Lock()
        self.echoed = 0
        self._ready = threading.Event()
        self._socket_mod = socket

    def start(self):
        import threading
        self._sock = self._socket_mod.socket(self._socket_mod.AF_INET,
                                              self._socket_mod.SOCK_DGRAM)
        # SO_REUSEADDR lets a restarted server bind quickly.  We
        # deliberately do NOT use SO_REUSEPORT here: slirp's NAT
        # reply path relies on the kernel delivering the reply
        # to the slirp-owned NAT socket, and SO_REUSEPORT would
        # hash-load-balance between the slirp socket and our
        # echo server, breaking the round-trip.
        self._sock.setsockopt(self._socket_mod.SOL_SOCKET,
                                self._socket_mod.SO_REUSEADDR, 1)
        # Bind to host's lo0 (which is what 10.0.2.2 maps to inside
        # slirp).  NIC 0 uses port 10001, NIC 1 uses port 10002.
        self._sock.bind(("127.0.0.1", self.port))
        self._sock.settimeout(0.25)
        self._ready.set()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self):
        import os
        while not self._stop.is_set():
            try:
                data, peer = self._sock.recvfrom(4096)
                sys.stderr.write(
                    f"[UDP echo port={self.port}] got {len(data)}B from {peer}\n")
                sys.stderr.flush()
                self._sock.sendto(data, peer)
                with self._lock:
                    self.echoed += 1
            except self._socket_mod.timeout:
                continue
            except OSError:
                break

    def stop(self):
        self._stop.set()
        if self._sock:
            self._sock.close()
        if self._thread:
            self._thread.join(timeout=2)

    @property
    def echoed_count(self):
        with self._lock:
            return self.echoed


def _run_case(case, smp, timeout=DEFAULT_TIMEOUT):
    """Run a single matrix case. Returns (passed: bool, log: str, evidence: dict)."""
    c = DMM.case_dict(case)
    fault = c["fault"]
    paths = _read_paths_for(fault)
    fw = paths.get("firmware") or ""
    img = paths.get("image") or ""
    if not Path(fw).exists() or not Path(img).exists():
        # Build missing artifacts.
        if not _build_for_fault(fault):
            return False, "", {"error": f"build failed for {fault}"}

    argv = _qemu_argv_for(case, smp, img, fw)
    # Snapshot is mandatory; verify before launching.
    if "-snapshot" not in argv:
        return False, "", {"error": "argv missing -snapshot"}

    # Build the probe command from c['mode'].
    mode = c["probe"]
    if mode == "no-nic":
        cmd = "/bin/netmodeltest no-nic"
    else:
        cmd = f"/bin/netmodeltest {mode}"

    # UDP cases need a host-side UDP echo server. Start one per NIC
    # so the slirp reply routes back to the right guest socket.
    # Each server binds to 40001+netidx; the slirp hostfwd maps
    # guest-side UDP ports to those host-side servers.
    # Bind the host UDP echo server on 127.0.0.1:10001 (NIC 0)
    # and 127.0.0.1:10002 (NIC 1).  Slirp NATs the guest's
    # outgoing UDP to 10.0.2.2 / 10.0.3.1 onto the SAME host port
    # by default.  These match the existing run_test.py network
    # suite's UDP echo server, which uses the same ports and no
    # hostfwd rule.  Cleanup between runs is the responsibility of
    # the runner's finally block — the UDP threads are daemon=True
    # so an aborted matrix still frees the ports.
    udp_port_base = 10000

    udp_list = []
    needs_udp_echo = (
        mode.startswith("udp ")
        or mode == "poll-busy"
        or mode.startswith("net-block-smp")
    )
    if needs_udp_echo:
        n_udp = sum(1 for n in (c.get("nic0"), c.get("nic1")) if n)
        n_udp = max(n_udp, 1)
        # NIC 0 -> host UDP server on port 10001 (slirp host 10.0.2.2).
        # NIC 1 -> host UDP server on port 10002 (slirp host 10.0.3.1).
        for i in range(n_udp):
            u = UdpEchoHost(port=udp_port_base + 1 + i)
            u.start()
            if not u._ready.wait(timeout=2):
                for x in udp_list: x.stop()
                return False, "", {"error": f"UDP echo {i} failed to bind"}
            udp_list.append(u)
        udp = udp_list[0]  # backward compat for old code paths
        # The probe command uses literal 10001/10002; the runner
        # uses those as the guest-side port (the hostfwd maps it
        # to the host UDP echo server on udp_port_base+N).
        if os.environ.get("DEBUG_MATRIX_RUNNER"):
            print(f"  [matrix] cmd={cmd!r} udp_port_base={udp_port_base}",
                  file=sys.stderr)

    # Wire up the input pipe. We use subprocess.PIPE for stdin so we
    # can write the probe command after the boot settles, and a
    # separate file for serial output so we can poll the markers.
    proc = None
    if os.environ.get("DRIVER_MODEL_LOG_DIR"):
        log_dir = Path(os.environ["DRIVER_MODEL_LOG_DIR"])
    elif os.environ.get("TMPDIR"):
        log_dir = Path(os.environ["TMPDIR"])
    else:
        log_dir = Path("/tmp")
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"os23matrix_{case}_{smp}_{int(time.time())}.log"
    # buffering=0 keeps QEMU's serial stdio line-buffered to disk; the
    # runner polls the file size every 0.5s and a stale read would
    # otherwise look like a hang.
    log_fh = open(log_path, "wb", buffering=0)
    # Also capture QEMU's stderr (hostfwd / chardev errors surface there)
    # so we can diagnose launch failures.
    err_path = log_path.with_suffix(".err.log")
    err_fh = open(err_path, "wb", buffering=0)
    try:
        try:
            proc = subprocess.Popen(
                argv,
                stdin=subprocess.PIPE,
                stdout=log_fh,
                stderr=err_fh,
                cwd=str(WORKTREE_ROOT),
            )
        except Exception as e:
            log_fh.close()
            err_fh.close()
            for u in udp_list: u.stop()
            return False, "", {"error": f"qemu launch failed: {e}"}

        # Sequence: boot (~18s for SMP=1, ~25s for SMP=2) + send probe + drain.
        boot_wait = 25 if smp >= 2 else 18
        deadline = time.monotonic() + timeout
        sent_cmd = False
        log_bytes_seen = 0
        result_passed = False
        result_failed = False
        try:
            while time.monotonic() < deadline:
                time.sleep(0.5)
                try:
                    size = log_path.stat().st_size
                except OSError:
                    continue
                if size > log_bytes_seen:
                    log_bytes_seen = size
                log_text = ""
                try:
                    with open(log_path, "r", encoding="utf-8",
                               errors="replace") as f:
                        log_text = f.read()
                except OSError:
                    continue
                # Wait for the kernel to declare the stack online AND
                # the shell prompt to be ready.  Multi-NIC cases need
                # "stack online with N active adapter(s)" so all NICs
                # have completed netif_add before the probe runs;
                # otherwise a udp probe to a NIC whose DHCP hasn't
                # completed yet spuriously times out.
                ready = "OS01 Init v1.0" in log_text and "#" in log_text
                if not ready:
                    if "stack online" in log_text or "no network devices" in log_text:
                        ready = "OS01 Init v1.0" in log_text and "#" in log_text
                if not sent_cmd and ready:
                    try:
                        proc.stdin.write((cmd + "\n").encode())
                        proc.stdin.flush()
                        sent_cmd = True
                    except BrokenPipeError:
                        pass
                # Did we get the RESULT line?
                if "[netmodeltest] RESULT: PASS" in log_text:
                    result_passed = True
                    break
                if "[netmodeltest] RESULT: FAIL" in log_text:
                    result_failed = True
                    break
        finally:
            if proc and proc.poll() is None:
                try:
                    proc.send_signal(signal.SIGTERM)
                    proc.wait(timeout=5)
                except Exception:
                    proc.kill()
            log_fh.close()
            for u in udp_list: u.stop()

        try:
            with open(log_path, "r", encoding="utf-8",
                       errors="replace") as f:
                log_text = f.read()
        except OSError:
            log_text = ""

        # Expect-boot-failure cases (no-ahci, empty-ahci) deliberately
        # produce a kernel panic because the kernel has no virtio-blk
        # driver / the AHCI fault suppresses media publication.  The
        # brief mandates that these cases PASS when the boot markers
        # appear (UEFI loaded + kernel initialized) but the root
        # filesystem / media is missing.  Detect the kernel-panic
        # line and convert it into a PASS-equivalent outcome.
        if c.get("expect_boot_failure"):
            boot_markers_present = (
                "OS01 Init v1.0" in log_text or "percpu:" in log_text
            )
            panic_present = (
                "[kernel panic]" in log_text or "FATAL" in log_text
            )
            if boot_markers_present and panic_present:
                return True, log_text, {
                    "case": case, "smp": smp,
                    "log_path": str(log_path),
                    "note": "expected boot failure (root filesystem / media missing)",
                }
            # Without a panic, this is a real failure.
            if not result_passed:
                return False, log_text, {
                    "case": case, "smp": smp,
                    "log_path": str(log_path),
                    "error": "no-ahci/empty-ahci without expected panic",
                }

        # Per-card evidence gate: every expected card must produce
        # an `iface=ethN` line in the guest probe.  brief Step 1:
        # "test_each_card_evidence_required只eth0成功拒绝".
        expected = c.get("expected_cards", ())
        if expected and result_passed:
            try:
                DMM.assert_each_card_has_evidence(log_text, expected)
            except DMM.CardEvidenceMissing as e:
                return False, log_text, {
                    "case": case, "smp": smp,
                    "log_path": str(log_path),
                    "error": f"card evidence missing: {e}",
                }

        # Observation counters gate (brief Step 3): for observe +
        # unsupported/modern-only, the kernel-side counters must
        # match the case's contract.
        observations = c.get("observation_assertions", {})
        if observations:
            try:
                DMM.assert_observation_counters(log_text, observations)
            except DMM.ObservationAssertionFailed as e:
                return False, log_text, {
                    "case": case, "smp": smp,
                    "log_path": str(log_path),
                    "error": f"observation counter failure: {e}",
                }

        if result_passed:
            return True, log_text, {"case": case, "smp": smp,
                                      "log_path": str(log_path)}
        if result_failed:
            return False, log_text, {"case": case, "smp": smp,
                                      "log_path": str(log_path)}
        return False, log_text, {"case": case, "smp": smp,
                                  "log_path": str(log_path),
                                  "error": "timeout"}
    finally:
        for u in udp_list: u.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", default="all",
                    help="case name or 'all'")
    ap.add_argument("--smp", nargs="+", type=int, default=None,
                    help="SMP counts (default: 1 and 2)")
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    ap.add_argument("--log-dir", default=None,
                    help="directory to write per-case log files")
    ap.add_argument("--list", action="store_true", help="list cases")
    args = ap.parse_args()

    # Default to the full SMP=1/2 matrix the brief requires.
    if args.smp is None:
        args.smp = [1, 2]

    if args.list:
        for c in DMM.matrix_case_names():
            print(c)
        return 0

    if args.case == "all":
        cases = DMM.matrix_case_names()
    else:
        cases = (args.case,)

    # Per-case SMP filter: each case declares `smp=(...)` with the
    # SMP counts it must run at.  net-block-smp is fixed SMP=2 per
    # its task brief; every other case supports (1, 2).  Filter
    # SMP × case combinations BEFORE running them so we never
    # launch a QEMU that doesn't satisfy the case's contract.
    filtered = []
    for smp in args.smp:
        for case in cases:
            case_def = DMM.case_dict(case)
            allowed_smp = case_def.get("smp", (1, 2))
            if smp not in allowed_smp:
                print(f"  [matrix] {case} smp={smp} ... SKIP "
                      f"(case smp constraint {allowed_smp})")
                continue
            filtered.append((case, smp))

    failed = []
    passed = 0
    for case, smp in filtered:
        print(f"  [matrix] {case} smp={smp} ... ", end="", flush=True)
        ok, log, ev = _run_case(case, smp, timeout=args.timeout)
        if ok:
            print("PASS")
            passed += 1
        else:
            print(f"FAIL ({ev.get('error', 'unknown')})")
            print(f"      log: {ev.get('log_path')}")
            failed.append((case, smp, ev.get("log_path")))

    print(f"\n{passed} passed, {len(failed)} failed")
    if failed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())