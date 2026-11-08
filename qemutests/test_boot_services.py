#!/usr/bin/env python3
"""Contract and unit tests for BOOT-5 boot-services runner and parser."""

from __future__ import annotations

import json
from pathlib import Path
import re
import unittest

from qemutests import boot_services as bs


def make_normal_stream(cpus: int = 1) -> str:
    lines = [
        "subsys: init  apic ... ok",
        "subsys: init  pic ... ok",
        "percpu: BSP  (cpu=0, apic_id=0) online",
    ]
    if cpus > 1:
        for c in range(1, cpus):
            lines.append(f"percpu: AP   (cpu={c}, apic_id={c}) registered")
    lines.extend([
        "[boot-2] context-ready",
        "subsys: init  clocksource ... ok",
        "subsys: init  timer ... ok",
        "subsys: init  lapic-timer ... ok",
        "subsys: init  pit ... ok",
        "[boot-2] runtime-open source=pit",
        "subsys: init  serial ... ok",
        "subsys: init  keyboard ... ok",
        "subsys: init  device-boot ... ok",
        "subsys: init  mouse ... ok",
        "subsys: init  thermal ... ok",
        "[boot-4] checkpoint=devices status=ok",
        "CSPRNG: kernel build variant=default",
        "[boot-2] tick-source=lapic",
        "[boot-4] checkpoint=pre-smp status=ok",
    ])
    if cpus > 1:
        for c in range(1, cpus):
            lines.append(f"[boot-1] ready cpu={c} online=1 ipi_ready=1 scheduler_ok=1")
        lines.append(f"[boot-1] complete discovered={cpus} ready={cpus}")
    else:
        lines.append("[boot-1] complete discovered=1 ready=1")
    lines.extend([
        "[boot-4] checkpoint=runtime status=ok",
        "[boot-5] vfs-runtime",
        "init: spawned user-space init, pid=1",
        "[boot-5] init-created pid=1",
        "net: starting lwip tcpip stack...",
        "[boot-5] runtime-ready window=closed",
        "+--------------------------------+",
        "|  OS01 Init v1.0 (PID 1)        |",
        "+--------------------------------+",
        "init: running as PID 1",
        "init: phase SYSINIT",
        "init: entering supervision loop",
        "BusyBox v1.36.1 (2026-11-04 00:48:47 CST) built-in shell (ash)",
        "# ",
    ])
    return "\n".join(lines) + "\n"


def make_no_net_stream(cpus: int = 1) -> str:
    lines = [
        "subsys: init  apic ... ok",
        "subsys: init  pic ... ok",
        "percpu: BSP  (cpu=0, apic_id=0) online",
    ]
    if cpus > 1:
        for c in range(1, cpus):
            lines.append(f"percpu: AP   (cpu={c}, apic_id={c}) registered")
    lines.extend([
        "[boot-2] context-ready",
        "subsys: init  clocksource ... ok",
        "subsys: init  timer ... ok",
        "subsys: init  lapic-timer ... ok",
        "subsys: init  pit ... ok",
        "[boot-2] runtime-open source=pit",
        "subsys: init  serial ... ok",
        "subsys: init  keyboard ... ok",
        "subsys: init  device-boot ... ok",
        "subsys: init  mouse ... ok",
        "subsys: init  thermal ... ok",
        "[boot-4] checkpoint=devices status=ok",
        "CSPRNG: kernel build variant=default",
        "[boot-2] tick-source=lapic",
        "[boot-4] checkpoint=pre-smp status=ok",
    ])
    if cpus > 1:
        for c in range(1, cpus):
            lines.append(f"[boot-1] ready cpu={c} online=1 ipi_ready=1 scheduler_ok=1")
        lines.append(f"[boot-1] complete discovered={cpus} ready={cpus}")
    else:
        lines.append("[boot-1] complete discovered=1 ready=1")
    lines.extend([
        "[boot-4] checkpoint=runtime status=ok",
        "[boot-5] vfs-runtime",
        "init: spawned user-space init, pid=1",
        "[boot-5] init-created pid=1",
        "net: no network devices registered, skipping stack start",
        "[boot-5] runtime-ready window=closed",
        "+--------------------------------+",
        "|  OS01 Init v1.0 (PID 1)        |",
        "+--------------------------------+",
        "init: running as PID 1",
        "init: phase SYSINIT",
        "init: entering supervision loop",
        "BusyBox v1.36.1 (2026-11-04 00:48:47 CST) built-in shell (ash)",
        "# ",
    ])
    return "\n".join(lines) + "\n"


def make_overlap_stream(cpus: int = 2, target_cpu: int = 1) -> str:
    lines = [
        "subsys: init  apic ... ok",
        "subsys: init  pic ... ok",
        "percpu: BSP  (cpu=0, apic_id=0) online",
    ]
    for c in range(1, cpus):
        lines.append(f"percpu: AP   (cpu={c}, apic_id={c}) registered")
    lines.extend([
        "[boot-2] context-ready",
        "subsys: init  clocksource ... ok",
        "subsys: init  timer ... ok",
        "subsys: init  lapic-timer ... ok",
        "subsys: init  pit ... ok",
        "[boot-2] runtime-open source=pit",
        "subsys: init  serial ... ok",
        "subsys: init  keyboard ... ok",
        "subsys: init  device-boot ... ok",
        "subsys: init  mouse ... ok",
        "subsys: init  thermal ... ok",
        "[boot-4] checkpoint=devices status=ok",
        "CSPRNG: kernel build variant=default",
        "[boot-2] tick-source=lapic",
        "[boot-4] checkpoint=pre-smp status=ok",
    ])
    for c in range(1, cpus):
        lines.append(f"[boot-1] ready cpu={c} online=1 ipi_ready=1 scheduler_ok=1")
    lines.append(f"[boot-1] complete discovered={cpus} ready={cpus}")
    lines.extend([
        "[boot-4] checkpoint=runtime status=ok",
        "[boot-5-test] variant=boot-services-overlap",
        "[boot-5] vfs-runtime",
        "init: spawned user-space init, pid=1",
        "[boot-5] init-created pid=1",
        f"[boot-5-test] overlap pid=1 cpu={target_cpu} bootstrap=0 locked=1 bsp_scheduler_ok=0 window=open",
        "+--------------------------------+",
        "|  OS01 Init v1.0 (PID 1)        |",
        "+--------------------------------+",
        "net: starting lwip tcpip stack...",
        "[boot-5] runtime-ready window=closed",
        "init: running as PID 1",
        "init: phase SYSINIT",
        "init: entering supervision loop",
        "BusyBox v1.36.1 (2026-11-04 00:48:47 CST) built-in shell (ash)",
        "# ",
    ])
    return "\n".join(lines) + "\n"


class BootServicesParserTests(unittest.TestCase):
    def test_normal_streams_pass(self):
        for cpus in (1, 2, 4, 8):
            stream = make_normal_stream(cpus)
            res = bs.parse_serial(stream, bs.CASE_NORMAL, cpus)
            self.assertTrue(res.ok, f"Failed on cpus={cpus}: {res.errors}")
            self.assertEqual(res.markers, ["vfs-runtime", "init-created", "runtime-ready"])
            self.assertIsNone(res.variant)
            self.assertIsNone(res.overlap_info)

    def test_no_net_streams_pass(self):
        for cpus in (1, 2):
            stream = make_no_net_stream(cpus)
            res = bs.parse_serial(stream, bs.CASE_NO_NET, cpus)
            self.assertTrue(res.ok, f"Failed on cpus={cpus}: {res.errors}")
            self.assertEqual(res.markers, ["vfs-runtime", "init-created", "runtime-ready"])

    def test_overlap_streams_pass(self):
        for cpus in (2, 4):
            stream = make_overlap_stream(cpus=cpus, target_cpu=1)
            res = bs.parse_serial(stream, bs.CASE_OVERLAP, cpus)
            self.assertTrue(res.ok, f"Failed on cpus={cpus}: {res.errors}")
            self.assertEqual(res.variant, "boot-services-overlap")
            self.assertIsNotNone(res.overlap_info)
            self.assertEqual(res.overlap_info["pid"], 1)
            self.assertEqual(res.overlap_info["cpu"], 1)
            self.assertEqual(res.overlap_info["bootstrap"], 0)
            self.assertEqual(res.overlap_info["locked"], 1)
            self.assertEqual(res.overlap_info["bsp_scheduler_ok"], 0)
            self.assertEqual(res.overlap_info["window"], "open")

    def test_overlap_interleaved_user_output_passes(self):
        # User output arrives after vfs-runtime but before init-created
        stream = make_overlap_stream(cpus=2, target_cpu=1)
        stream = stream.replace(
            "[boot-5] init-created pid=1\n",
            "|  OS01 Init v1.0 (PID 1)        |\n[boot-5] init-created pid=1\n"
        )
        res = bs.parse_serial(stream, bs.CASE_OVERLAP, 2)
        self.assertTrue(res.ok, f"Expected pass with interleaved output: {res.errors}")

    def test_user_output_before_vfs_runtime_rejected(self):
        stream = make_normal_stream(1)
        # Move user output above vfs-runtime
        stream = "init: running as PID 1\n" + stream
        res = bs.parse_serial(stream, bs.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("preceded vfs-runtime" in e for e in res.errors))

    def test_missing_marker_rejected(self):
        stream = make_normal_stream(1).replace("[boot-5] vfs-runtime\n", "")
        res = bs.parse_serial(stream, bs.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("vfs-runtime" in e for e in res.errors))

    def test_duplicate_marker_rejected(self):
        stream = make_normal_stream(1).replace(
            "[boot-5] vfs-runtime\n",
            "[boot-5] vfs-runtime\n[boot-5] vfs-runtime\n"
        )
        res = bs.parse_serial(stream, bs.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("appeared 2 times" in e for e in res.errors))

    def test_reordered_marker_rejected(self):
        stream = make_normal_stream(1)
        # Swap vfs-runtime and init-created
        stream = stream.replace("[boot-5] vfs-runtime\n", "PLACEHOLDER\n")
        stream = stream.replace("[boot-5] init-created pid=1\n", "[boot-5] vfs-runtime\n")
        stream = stream.replace("PLACEHOLDER\n", "[boot-5] init-created pid=1\n")
        res = bs.parse_serial(stream, bs.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("Marker order violation" in e for e in res.errors))

    def test_fatal_marker_rejected(self):
        stream = make_normal_stream(1) + "[boot-3] FATAL stage=init component=/bin/init rc=-2 phase=-1 cpu=0\n"
        res = bs.parse_serial(stream, bs.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("Unexpected fatal error" in e for e in res.errors))

    def test_overlap_cpu0_rejected(self):
        stream = make_overlap_stream(2, target_cpu=0)
        res = bs.parse_serial(stream, bs.CASE_OVERLAP, 2)
        self.assertFalse(res.ok)
        self.assertTrue(any("cpu in 1..1" in e for e in res.errors))

    def test_overlap_cpu_out_of_bounds_rejected(self):
        stream = make_overlap_stream(2, target_cpu=2)
        res = bs.parse_serial(stream, bs.CASE_OVERLAP, 2)
        self.assertFalse(res.ok)
        self.assertTrue(any("cpu in 1..1" in e for e in res.errors))

    def test_overlap_bootstrap_active_rejected(self):
        stream = make_overlap_stream(2, target_cpu=1).replace("bootstrap=0", "bootstrap=1")
        res = bs.parse_serial(stream, bs.CASE_OVERLAP, 2)
        self.assertFalse(res.ok)
        self.assertTrue(any("bootstrap=0" in e for e in res.errors))

    def test_overlap_not_locked_rejected(self):
        stream = make_overlap_stream(2, target_cpu=1).replace("locked=1", "locked=0")
        res = bs.parse_serial(stream, bs.CASE_OVERLAP, 2)
        self.assertFalse(res.ok)
        self.assertTrue(any("locked=1" in e for e in res.errors))

    def test_overlap_bsp_sched_ok_not_zero_rejected(self):
        stream = make_overlap_stream(2, target_cpu=1).replace("bsp_scheduler_ok=0", "bsp_scheduler_ok=1")
        res = bs.parse_serial(stream, bs.CASE_OVERLAP, 2)
        self.assertFalse(res.ok)
        self.assertTrue(any("bsp_scheduler_ok=0" in e for e in res.errors))

    def test_overlap_window_closed_rejected(self):
        stream = make_overlap_stream(2, target_cpu=1).replace("window=open", "window=closed")
        res = bs.parse_serial(stream, bs.CASE_OVERLAP, 2)
        self.assertFalse(res.ok)
        self.assertTrue(any("window=open" in e for e in res.errors))

    def test_overlap_missing_observation_rejected(self):
        stream = make_overlap_stream(2, target_cpu=1)
        # Remove overlap line
        stream = re.sub(r"\[boot-5-test\] overlap .*\n", "", stream)
        res = bs.parse_serial(stream, bs.CASE_OVERLAP, 2)
        self.assertFalse(res.ok)
        self.assertTrue(any("missing [boot-5-test] overlap observation line" in e for e in res.errors))

    def test_normal_with_variant_rejected(self):
        stream = "[boot-5-test] variant=boot-services-overlap\n" + make_normal_stream(1)
        res = bs.parse_serial(stream, bs.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("variant marker" in e for e in res.errors))

    def test_normal_with_overlap_token_rejected(self):
        stream = make_normal_stream(1) + "[boot-5-test] overlap pid=1 cpu=1 bootstrap=0 locked=1 bsp_scheduler_ok=0 window=open\n"
        res = bs.parse_serial(stream, bs.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("[boot-5-test]" in e for e in res.errors))

    def test_no_net_missing_skip_msg_rejected(self):
        stream = make_no_net_stream(1).replace("net: no network devices registered, skipping stack start\n", "")
        res = bs.parse_serial(stream, bs.CASE_NO_NET, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("missing expected skip message" in e for e in res.errors))

    def test_missing_shell_prompt_rejected(self):
        stream = make_normal_stream(1).replace("# \n", "")
        res = bs.parse_serial(stream, bs.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("Missing shell prompt" in e for e in res.errors))

    def test_empty_stream_rejected(self):
        res = bs.parse_serial("", bs.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("Empty serial stream" in e for e in res.errors))


def _read(rel: str) -> str:
    return (bs._ROOT / rel).read_text()


class BuildContractStatic(unittest.TestCase):
    def test_project_mk_validates_selector(self) -> None:
        text = _read("mk/project.mk")
        self.assertIn("BOOT_SERVICES_TEST ?=", text)
        self.assertIn("BOOT_SERVICES_TESTS_VALID := none overlap", text)
        self.assertIn("boot-services-$(BOOT_SERVICES_CASE)", text)

    def test_profiles_kernel_variant_slug(self) -> None:
        x86_text = _read("mk/profiles/x86_64-clang.mk")
        self.assertIn("boot-services-$(BOOT_SERVICES_CASE)", x86_text)
        arm_text = _read("mk/profiles/aarch64-clang.mk")
        self.assertIn("BOOT_SERVICES_TEST is x86-only", arm_text)

    def test_catalog_and_scenario_registered(self) -> None:
        catalog_text = _read("mk/tests/catalog.mk")
        self.assertIn("boot-services", catalog_text)
        scenarios_text = _read("config/test-scenarios.json")
        self.assertIn("boot.services.x86", scenarios_text)


if __name__ == "__main__":
    unittest.main()
