#!/usr/bin/env python3
"""Pilot scenario authoring assertions for memory/sched + shadow slab retirement (Task 9)."""

from pathlib import Path
import json
import unittest

TEST_MANIFEST = {
    "id": "test_scenario_pilot",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

ROOT = Path(__file__).resolve().parents[1]

MEMORY_KERNEL_SELFTEST_IDS = [
    "slab_alloc_free",
    "slab_many_sizes",
    "slab_16_caches",
    "aarch64_pt_vmm",
    "uaccess",
    "aarch64_user_mm",
    "aarch64_user_vm",
    "aarch64_user_fork_cow",
    "aarch64_user_svc_uaccess",
]

SCHED_KERNEL_SELFTEST_IDS = [
    "kthread_self_reap",
    "sched_boot_probe",
    "init_pid",
    "pgrp_signal",
    "aarch64_sched_yield",
    "aarch64_sched_regs",
    "aarch64_sched_entry",
    "aarch64_sched_irq",
    "aarch64_sched_blocker",
    "aarch64_sched_reclaim",
    "aarch64_sched_idle",
    "aarch64_sched_timer",
    "aarch64_sched_ap",
    "aarch64_el0",
    "aarch64_el0_switch",
    "aarch64_el0_irq",
    "aarch64_el0_preempt",
    "aarch64_el0_task",
    "aarch64_user_task_lifecycle",
    "aarch64_user_waitpid",
    "aarch64_user_exec",
    "aarch64_user_signal",
    "entry",
    "yield",
    "reap",
    "work",
    "admission",
    "default-mask",
    "ready",
    "remote-wake",
    "wake-switch",
    "idle-race",
    "sgi",
    "work-sleep",
    "scan-fallback",
    "tick-ready",
    "tick-idle",
    "tick-busy",
    "tick-yield",
    "tick-deadline",
    "tick-global",
    "tick-remote",
]

MEMORY_SYSTEST_CASES = [
    "brk",
    "49_brk_read_fresh_page",
    "51_fork_brk_isolation",
    "52_fork_mmap_cow_isolation",
    "53_cow_kernel_outputs",
    "mprotect_subrange_cow",
    "mmap_exec_protection",
    "fixed_file_replace",
]

SCHED_SYSTEST_CASES = [
    "getpid_getppid",
    "fork_exec_waitpid",
    "exec_hostile_argv",
    "exec_hostile_envp",
    "sigaction",
    "kill_deliver",
    "fatal_signal_exit",
    "eevdf_fork_child",
    "setpgid_auto_fg",
    "signal_pgrp_basic",
    "kill_pgrp",
    "nanosleep",
    "nanosleep_eintr",
]


class ScenarioPilotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.scenarios_data = json.loads((ROOT / "config/test-scenarios.json").read_text(encoding="utf-8"))
        cls.scenarios = cls.scenarios_data["scenarios"]
        cls.by_id = {sc["id"]: sc for sc in cls.scenarios}
        cls.catalog_data = json.loads((ROOT / "config/test-catalog.json").read_text(encoding="utf-8"))
        cls.bridge_data = json.loads((ROOT / "config/test-bridge.json").read_text(encoding="utf-8"))

    def test_memory_and_sched_kernel_selftests_have_primary_scenarios(self) -> None:
        primary_selftest_selectors = {
            sc["primary"]["selector"]
            for sc in self.scenarios
            if sc.get("primary", {}).get("layer") == "kernel_selftest"
        }
        for tid in MEMORY_KERNEL_SELFTEST_IDS + SCHED_KERNEL_SELFTEST_IDS:
            self.assertIn(
                tid,
                primary_selftest_selectors,
                f"kernel_selftest '{tid}' has no primary scenario in config/test-scenarios.json",
            )

    def test_memory_and_sched_systest_cases_have_primary_scenarios(self) -> None:
        primary_systest_selectors = {
            sc["primary"]["selector"]
            for sc in self.scenarios
            if sc.get("primary", {}).get("layer") == "systest"
        }
        for cid in MEMORY_SYSTEST_CASES + SCHED_SYSTEST_CASES:
            self.assertIn(
                cid,
                primary_systest_selectors,
                f"systest case '{cid}' has no primary scenario in config/test-scenarios.json",
            )

    def test_shadow_slab_basic_retired_with_valid_replacement(self) -> None:
        self.assertIn("memory.slab.basic", self.by_id)
        sc = self.by_id["memory.slab.basic"]
        dec = sc["overlap_decision"]
        self.assertEqual(dec["action"], "remove")
        repl = dec.get("replacement")
        self.assertTrue(repl and repl in self.by_id, f"Invalid replacement: {repl!r}")
        self.assertEqual(self.by_id[repl]["overlap_decision"]["action"], "unique")

    def test_test_slab_basic_removed_from_catalog_bridge_and_disk(self) -> None:
        self.assertFalse((ROOT / "hosttests/cases/test_slab_basic.c").exists())
        cat_ids = {e["id"] for e in self.catalog_data["entries"]}
        self.assertNotIn("test_slab_basic", cat_ids)
        bridge_ids = {e["id"] for e in self.bridge_data["entries"]}
        self.assertNotIn("test_slab_basic", bridge_ids)


if __name__ == "__main__":
    unittest.main()
