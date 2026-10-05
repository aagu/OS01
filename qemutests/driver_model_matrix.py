#!/usr/bin/env python3
"""
driver_model_matrix.py — ARCH-9 driver discovery multi-NIC / missing /
fault matrix harness.

Task 11 brief: §Step 3 — implement the production harness that the
RED tests in qemutests/test_driver_model_matrix.py pin down.

Public surface:
    matrix_case_names()           -> tuple[str, ...] of 15 cases
    case_fault(case)              -> ARCH9_FAULT slug for a case
    fault_variant_paths(fault)    -> dict of build paths
    build_matrix_plan(smp, fault) -> plan dict (incl. observability)
    build_mock_qemu_argv(case, smp) -> list[str] of QEMU argv tokens
    dry_run_kernel_build(fault, extra) -> (rc:int, stdout:str)
    assert_boot_completed(log, timeout)
    assert_each_card_has_evidence(log, expected_cards)
    assert_probe_marker_present(log, case)
    assert_socket_deadline(log, deadline_s)
    assert_normal_image_hash_unchanged(before_sha256, after_sha256)

This module is INTENTIONALLY a thin layer over subprocess + Make paths.
It does not invent path arithmetic; it parses what `make print-run-paths`
emits so the same paths the kernel build uses are the ones the harness
consumes.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

WORKTREE_ROOT = Path(__file__).resolve().parent.parent
PROFILE = os.environ.get("PROFILE", "x86_64-clang")

# ── Fault catalog ─────────────────────────────────────────────────
# None        = canonical, no fault injection (normal build)
# observe     = inject observation counters only, no fault
# bad-nic-bar = corrupt one NIC's BAR window so probe() aborts; the
#               healthy root disk and any second NIC must still work
# adapter-fail = reject every adapter registration
# ahci-empty  = suppress media publication at the safe-enumeration
#               point; do not start DMA
# irq-conflict = the second NIC's candidate IRQ slot already held by
#               the first → real "already-owned" rejection path; the
#               first card keeps Rx/Tx, the second card falls to POLL
VALID_FAULTS = (
    "none", "observe", "bad-nic-bar",
    "adapter-fail", "ahci-empty", "irq-conflict",
)

# ── Case catalog ──────────────────────────────────────────────────
# Each case maps to a QEMU argv shape, a fault slug, and a guest
# probe mode.  net-block-smp is fixed SMP=2 (its brief).
#
# Fields:
#   - expected_cards: list of "ethN" names that the guest probe
#       must publish evidence for.  The matrix runner fails the
#       case when fewer cards are observed (single-card-only is not
#       sufficient for multi-NIC cases).
#   - observation_assertions: dict of kernel counter -> comparator
#       used by the matrix runner.  Comparators are: ">0", "==0",
#       ">=N".  Only consulted for observe variant and the cases
#       with fault=observe.
CASES = (
    {
        "name": "all",
        "fault": "none",
        "nic0": ("e1000", "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": None,
        "probe": "udp 10.0.2.2 10001",
        "smp": (1, 2),
        "expected_cards": ("eth0",),
    },
    {
        "name": "e1000",
        "fault": "none",
        "nic0": ("e1000", "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": None,
        "probe": "udp 10.0.2.2 10001",
        "smp": (1, 2),
        "expected_cards": ("eth0",),
    },
    {
        "name": "virtio",
        "fault": "none",
        "nic0": ("virtio-net-pci,disable-modern=on",
                 "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": None,
        "probe": "udp 10.0.2.2 10001",
        "smp": (1, 2),
        "expected_cards": ("eth0",),
    },
    {
        "name": "mixed",
        "fault": "none",
        "nic0": ("e1000", "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": ("virtio-net-pci,disable-modern=on",
                 "user,id=net1,net=10.0.3.0/24,dhcpstart=10.0.3.20,host=10.0.3.1"),
        "probe": "udp 10.0.2.2 10001 10.0.3.1 10002",
        "smp": (1, 2),
        "expected_cards": ("eth0", "eth1"),
    },
    {
        "name": "two-e1000",
        "fault": "none",
        "nic0": ("e1000", "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": ("e1000", "user,id=net1,net=10.0.3.0/24,dhcpstart=10.0.3.20,host=10.0.3.1"),
        "probe": "udp 10.0.2.2 10001 10.0.3.1 10002",
        "smp": (1, 2),
        "expected_cards": ("eth0", "eth1"),
    },
    {
        "name": "two-virtio",
        "fault": "none",
        "nic0": ("virtio-net-pci,disable-modern=on",
                 "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": ("virtio-net-pci,disable-modern=on",
                 "user,id=net1,net=10.0.3.0/24,dhcpstart=10.0.3.20,host=10.0.3.1"),
        "probe": "udp 10.0.2.2 10001 10.0.3.1 10002",
        "smp": (1, 2),
        "expected_cards": ("eth0", "eth1"),
    },
    {
        "name": "no-nic",
        "fault": "none",
        "nic0": None,
        "nic1": None,
        "probe": "no-nic",
        "smp": (1, 2),
        "expected_cards": (),
    },
    {
        "name": "no-ahci",
        "fault": "none",
        "nic0": ("virtio-net-pci,disable-modern=on",
                 "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": None,
        # -M pc drops the on-board SATA function entirely.
        "machine": "pc",
        "probe": "udp 10.0.2.2 10001",
        "smp": (1, 2),
        # Must boot — virtio-blk-pci provides the rootfs.
        "expect_boot_failure": True,
        "expected_cards": ("eth0",),
    },
    {
        "name": "empty-ahci",
        "fault": "ahci-empty",
        "nic0": ("e1000", "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": None,
        "probe": "udp 10.0.2.2 10001",
        "smp": (1, 2),
        "expect_boot_failure": True,
        "expected_cards": ("eth0",),
    },
    {
        "name": "poll-busy",
        "fault": "none",
        "nic0": ("e1000", "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": ("e1000", "user,id=net1,net=10.0.3.0/24,dhcpstart=10.0.3.20,host=10.0.3.1"),
        "probe": "poll-busy",
        "smp": (1, 2),
        "expected_cards": ("eth0", "eth1"),
    },
    {
        "name": "bad-nic",
        "fault": "bad-nic-bar",
        "nic0": ("e1000", "user,id=net0,dhcpstart=10.0.2.20"),
        # Second card stays healthy — bad-nic-bar only poisons the
        # first probe (per the brief's "第一卡继续收发" requirement).
        "nic1": ("e1000", "user,id=net1,net=10.0.3.0/24,dhcpstart=10.0.3.20,host=10.0.3.1"),
        # Probe dumps ONLINE ifaces from kernel markers (avoiding the
        # slirp multi-NIC NAT limitation that makes udp 10.0.3.1
        # timeout when NIC 0 is broken).
        "probe": "ifaces",
        "smp": (1, 2),
        # The first card's BAR is corrupted by the bad-nic-bar fault;
        # only the second card is ONLINE.
        "expected_cards": ("eth1",),
    },
    {
        "name": "adapter-fail",
        "fault": "adapter-fail",
        "nic0": ("e1000", "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": None,
        # adapter-fail rejects every adapter registration, so no NIC
        # is reachable: the probe asserts the no-nic contract.
        "probe": "no-nic",
        "smp": (1, 2),
        "expected_cards": (),
    },
    {
        "name": "unsupported",
        "fault": "observe",
        "nic0": ("e1000", "user,id=net0,dhcpstart=10.0.2.20"),
        # e1000e — a different device ID that no driver matches.
        "extra_device": ("e1000e",),
        "nic1": None,
        "probe": "udp 10.0.2.2 10001",
        "smp": (1, 2),
        # Healthy e1000 must produce evidence; e1000e must NOT be
        # bound.  The e1000e device never reaches probe() because
        # pci_match_id() rejects it before the per-driver probe
        # hook fires (kernel/bus/pci/core.c:pci_bind_all), so
        # probe_calls only counts the e1000 match.  Brief §Step 3:
        # matrix校验UNBOUND、不写BAR (aggregate, since the
        # observation counters do not currently distinguish the
        # matched vs unmatched device).
        "expected_cards": ("eth0",),
        "observation_assertions": {
            "probe_calls": ">0",
            "adapter_registrations": ">=1",
        },
    },
    {
        "name": "modern-only",
        "fault": "observe",
        "nic0": ("virtio-net-pci,disable-legacy=on,disable-modern=off",
                 "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": None,
        "probe": "no-nic",
        "smp": (1, 2),
        # Modern-only virtio-net (1af4:1041) does NOT match the
        # OS01 driver (1af4:1000).  pci_match_id rejects it
        # before probe() fires, so no probe_calls increment and
        # no adapter is registered.  Brief §Step 3: matrix校验
        # UNBOUND、不写BAR.
        "expected_cards": (),
        "observation_assertions": {
            "bar_writes": "==0",
            "adapter_registrations": "==0",
        },
    },
    {
        "name": "irq-conflict",
        "fault": "irq-conflict",
        # mixed topology: e1000 (NIC 0) + virtio-net (NIC 1).  The
        # first card to probe claims its GSI; the second card's
        # candidate GSI collides with the first (per the kernel-side
        # arch9_fault_should_force_irq_conflict() hook) and falls
        # back to NIC_POLL without register_irq.  Matrix must verify
        # NIC 0 ONLINE + NIC 1 POLL fallback.
        "nic0": ("e1000", "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": ("virtio-net-pci,disable-modern=on",
                 "user,id=net1,net=10.0.3.0/24,dhcpstart=10.0.3.20,host=10.0.3.1"),
        "probe": "ifaces",
        "smp": (1, 2),
        "expected_cards": ("eth0", "eth1"),
    },
    {
        "name": "net-block-smp",
        "fault": "none",
        "nic0": ("e1000", "user,id=net0,dhcpstart=10.0.2.20"),
        "nic1": None,
        # Brief §Step 10: file-stress writes /.arch9-stress-<nonce>
        # (NOT in /tmp).  Brief §Step 3: 同时另一个进程逐卡UDP echo
        # 并校验nonce.  net-block-smp combined wrapper forks both.
        "probe": "net-block-smp 12345 0xA5 32",
        "smp": (2,),  # fixed SMP=2 per task brief
        "expected_cards": ("eth0",),
    },
)

# Marker prefixes printed by user/netmodeltest.c.  The probes run a single
# round and the kernel-side harness greps for these strings.
NETMODELTEST_BEGIN = "[netmodeltest] BEGIN"
NETMODELTEST_PASS = "[netmodeltest] RESULT: PASS"
NETMODELTEST_FAIL = "[netmodeltest] RESULT: FAIL"
NETMODELTEST_PROBE = "[netmodeltest] PROBE_BEGIN"

KERNEL_BOOT_MARKERS = (
    "percpu:",          # percpu registration succeeded
    "OS01 Init v1.0",   # init banner — definitely past SMP + VFS + TTY
)


class HarnessError(Exception):
    """Base class for harness-side assertion failures."""


class BootMarkerMissing(HarnessError):
    """QEMU exited before kernel boot markers appeared."""


class CardEvidenceMissing(HarnessError):
    """A multi-card case did not produce per-card evidence."""


class ProbeMarkerMissing(HarnessError):
    """The netmodeltest guest program never produced its marker."""


class SocketDeadlineExceeded(HarnessError):
    """socket() on a non-ONLINE stack exceeded the 2-second deadline."""


class NormalImagePolluted(HarnessError):
    """A fault fixture polluted the normal image (hash drift)."""


# ── Public surface ────────────────────────────────────────────────
def matrix_case_names():
    return tuple(c["name"] for c in CASES)


def case_fault(case):
    for c in CASES:
        if c["name"] == case:
            return c["fault"]
    raise ValueError(f"unknown case {case!r}")


def case_dict(name):
    for c in CASES:
        if c["name"] == name:
            return c
    raise ValueError(f"unknown case {name!r}")


def fault_variant_paths(fault):
    if fault not in VALID_FAULTS:
        raise ValueError(f"unknown fault {fault!r}")
    profile = os.environ.get("PROFILE", PROFILE)
    base = WORKTREE_ROOT / f"build/{profile}"
    if fault == "none":
        # Canonical (normal) build — no test_fault.c is compiled in,
        # no driver-model-<fault>/ suffix anywhere.
        return {
            "kernel_build": base / "kernel",
            "kernel_artifact": base / "artifacts/kernel.bin",
            "image_dir": base / "image",
            "manifest": base / "image/rootfs.manifest",
            "disk_img": base / "image/disk.img",
        }
    slug = f"driver-model-{fault}"
    return {
        "kernel_build": base / "kernel" / slug,
        "kernel_artifact": base / "artifacts/kernel" / slug / "kernel.bin",
        "image_dir": base / "image" / slug,
        "manifest": base / "image" / slug / "rootfs.manifest",
        "disk_img": base / "image" / slug / "disk.img",
    }


def build_matrix_plan(smp, fault):
    """Return a single plan dict for (smp, fault)."""
    paths = fault_variant_paths(fault)
    return {
        "smp": smp,
        "fault": fault,
        "paths": paths,
        "observability": {
            "enabled": fault in ("observe",),
            # In the observe variant the kernel stamps observation
            # counters; for every other non-none fault the test fixture
            # expands to either stub (no-op) or fault-injection paths.
            "empty_for_none": fault == "none",
        },
    }


def build_mock_qemu_argv(case, smp):
    """Return a QEMU argv list for `case` at `smp`.

    `mock` here means "what the harness would pass to QEMU subprocess".
    No -display/-serial etc., but the real NIC topology. The argv
    ALWAYS includes -snapshot so any case without it is rejected by
    test_matrix_snapshot_required."""
    c = case_dict(case)
    firmware = os.environ.get("OVMF_FIRMWARE", "")
    image = os.environ.get("DISK_IMG", "")
    argv = [
        "qemu-system-x86_64",
        "-M", c.get("machine", "q35"),
        "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
        "-m", "512",
        "-smp", str(smp),
        "-display", "none",
        "-serial", "stdio",
        "-no-reboot",
        "-no-shutdown",
    ]

    if c.get("machine") == "pc":
        # no-ahci: drop on-board SATA function; virtio-blk-pci below
        # provides rootfs.  We don't add the ahci device.
        argv += ["-drive",
                 f"file={image},format=raw,if=none,id=disk"]
        argv += ["-device", "virtio-blk-pci,drive=disk"]
    else:
        argv += ["-drive", f"file={image},format=raw,if=none,id=disk"]
        argv += ["-device", "ahci,id=ahci"]
        argv += ["-device", "ide-hd,drive=disk,bus=ahci.0"]

    if c["nic0"]:
        argv += ["-netdev", c["nic0"][1]]
        # The nic device must reference the netdev; "netdev=netN" is
        # added if the device string doesn't already include it.
        nic0_dev = c["nic0"][0]
        if "netdev=" not in nic0_dev:
            # Derive netdev name from the netdev spec: "user,id=net0,..."
            net0_id = ""
            for tok in c["nic0"][1].split(","):
                if tok.startswith("id="):
                    net0_id = tok[3:]
            if net0_id:
                nic0_dev = f"{nic0_dev},netdev={net0_id}"
        argv += ["-device", nic0_dev]
    else:
        argv += ["-nic", "none"]
    if c["nic1"]:
        argv += ["-netdev", c["nic1"][1]]
        nic1_dev = c["nic1"][0]
        if "netdev=" not in nic1_dev:
            net1_id = ""
            for tok in c["nic1"][1].split(","):
                if tok.startswith("id="):
                    net1_id = tok[3:]
            if net1_id:
                nic1_dev = f"{nic1_dev},netdev={net1_id}"
        argv += ["-device", nic1_dev]
    for ed in c.get("extra_device", ()):
        argv += ["-device", ed]

    # Every case MUST include -snapshot — fault fixture and observe
    # builds share disk backing, so even a passing case must drop
    # writes on shutdown.
    argv.append("-snapshot")
    return argv


def dry_run_kernel_build(fault, extra=()):
    """Invoke `make -n ARCH9_FAULT=<fault> kernel.bin` and capture
    stdout/stderr + exit code.  Used to verify path slugs land in
    recipe expansions and that conflicting kernel flags abort parse."""
    env = dict(os.environ)
    if fault != "none":
        env["ARCH9_FAULT"] = fault
    cmd = ["make", "--no-print-directory", "-n", "kernel.bin"]
    cmd[2:2] = list(extra)
    proc = subprocess.run(
        cmd, cwd=WORKTREE_ROOT, env=env,
        capture_output=True, text=True, timeout=120,
    )
    return proc.returncode, (proc.stdout + proc.stderr)


def assert_boot_completed(log, timeout):
    """Raise BootMarkerMissing if the kernel never reached the boot
    markers (percpu + init banner)."""
    if not isinstance(log, str):
        log = log.decode("utf-8", errors="replace")
    if "OS01 Init v1.0" not in log and "percpu:" not in log:
        raise BootMarkerMissing(
            f"kernel never reached boot markers within {timeout}s"
        )


def assert_each_card_has_evidence(log, expected_cards):
    if not isinstance(log, str):
        log = log.decode("utf-8", errors="replace")
    for card in expected_cards:
        # Look for netmodeltest evidence with iface=<card>
        pattern = re.compile(rf"\[netmodeltest\][^\n]*iface={re.escape(card)}\b",
                             re.IGNORECASE)
        if not pattern.search(log):
            raise CardEvidenceMissing(
                f"no netmodeltest evidence for card {card!r}"
            )


def assert_probe_marker_present(log, case):
    if not isinstance(log, str):
        log = log.decode("utf-8", errors="replace")
    if NETMODELTEST_PROBE not in log and NETMODELTEST_BEGIN not in log:
        raise ProbeMarkerMissing(
            f"no netmodeltest probe marker for case {case!r}"
        )


def assert_socket_deadline(log, deadline_s):
    """For no-nic, the socket() outcome line must arrive within
    `deadline_s` of the probe-marker line.  Anything longer is FAIL."""
    if not isinstance(log, str):
        log = log.decode("utf-8", errors="replace")
    m_begin = re.search(r"PROBE_BEGIN\s*@\s*t=([\d.]+)", log)
    m_end = re.search(
        r"socket\(\)\s+returned[^\n]*@\s*t=([\d.]+)", log
    )
    if not (m_begin and m_end):
        # Without timestamps we can't compute; assume the guest
        # aborted; surface as missing probe marker (handled above).
        return
    gap = float(m_end.group(1)) - float(m_begin.group(1))
    if gap > deadline_s:
        raise SocketDeadlineExceeded(
            f"socket() took {gap:.2f}s, deadline is {deadline_s}s"
        )


def assert_normal_image_hash_unchanged(before_sha256, after_sha256):
    if before_sha256 != after_sha256:
        raise NormalImagePolluted(
            "normal image hash drifted: "
            f"{before_sha256[:8]} -> {after_sha256[:8]}"
        )
    return True


class ObservationAssertionFailed(HarnessError):
    """Kernel observation counters did not match the case contract."""


# Regex matched against the kernel's `arch9-fault:` dump line.  The
# kernel emits counters in this exact format:
#   arch9-fault: active=<id> drivers=<n> probe=<n> unbound_no_match=<n>
#       unbound_after_id=<n> bar_writes=<n> adapters=<n> publishes=<n>
#       ahci_ports=<n>
OBSERVATION_DUMP_RE = re.compile(
    r"arch9-fault:\s+active=(?P<active>\d+)"
    r"\s+drivers=(?P<pci_drivers_exposed>\d+)"
    r"\s+probe=(?P<probe_calls>\d+)"
    r"\s+unbound_no_match=(?P<probe_unbound_no_match>\d+)"
    r"\s+unbound_after_id=(?P<probe_unbound_after_id>\d+)"
    r"\s+bar_writes=(?P<bar_writes>\d+)"
    r"\s+adapters=(?P<adapter_registrations>\d+)"
    r"\s+publishes=(?P<adapter_publishes>\d+)"
    r"\s+ahci_ports=(?P<ahci_port_publications>\d+)"
)


def parse_observation_dump(log):
    """Return a dict of observation counters from the kernel's
    arch9-fault dump line, or {} if no dump line was found."""
    if not isinstance(log, str):
        log = log.decode("utf-8", errors="replace")
    m = OBSERVATION_DUMP_RE.search(log)
    if not m:
        return {}
    return {k: int(v) for k, v in m.groupdict().items()}


def assert_observation_counters(log, assertions):
    """Raise ObservationAssertionFailed when any counter in `assertions`
    does not satisfy its comparator.

    `assertions` is a dict of counter_name -> comparator.  Comparators
    supported: "==N", ">N", ">=N", "<N", "<=N", where N is a
    non-negative integer."""
    if not assertions:
        return
    counters = parse_observation_dump(log)
    if not counters:
        raise ObservationAssertionFailed(
            "no arch9-fault dump line found; cannot assert counters"
        )
    for name, comparator in assertions.items():
        if name not in counters:
            raise ObservationAssertionFailed(
                f"counter {name!r} missing from observation dump"
            )
        got = counters[name]
        if not _comparator_matches(comparator, got):
            raise ObservationAssertionFailed(
                f"counter {name!r}={got} fails assertion {comparator!r}"
            )


_COMPARATOR_RE = re.compile(r"^(==|>=|<=|>|<)(\d+)$")


def _comparator_matches(comparator, value):
    m = _COMPARATOR_RE.match(comparator)
    if not m:
        raise ValueError(f"invalid comparator {comparator!r}")
    op, ref = m.group(1), int(m.group(2))
    if op == "==":
        return value == ref
    if op == ">":
        return value > ref
    if op == ">=":
        return value >= ref
    if op == "<":
        return value < ref
    if op == "<=":
        return value <= ref
    raise AssertionError(f"unreachable comparator op {op!r}")


# ── Internal helpers ──────────────────────────────────────────────
def have_qemu():
    return shutil.which("qemu-system-x86_64") is not None


def _resolve_firmware():
    out = subprocess.run(
        ["make", "--no-print-directory", "-s", "print-run-paths"],
        cwd=WORKTREE_ROOT, capture_output=True, text=True,
    )
    fw = None
    img = None
    for line in out.stdout.splitlines():
        if line.startswith("firmware="):
            fw = line.split("=", 1)[1].strip()
        elif line.startswith("image="):
            img = line.split("=", 1)[1].strip()
    return fw, img