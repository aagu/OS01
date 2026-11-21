#!/usr/bin/env python3
"""Unit tests for the aarch64 synthetic SError acceptance runner (Task 4)."""

import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from unittest import mock

TEST_MANIFEST = {
    "id": "qemutests.test_aarch64_serror",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests import aarch64_serror as runner  # noqa: E402


def _make_log(
    origin: str = "el1h",
    mpidr: str = "0000000080000000",
    esr: str = "00000000be000055",
    elr: int = 0xFFFF000040081234,
    spsr: int = 0x245,
    far: str = "n/a",
    daif0: str = "[aarch64-serror-test] daif cpu=0 a=0 i=1",
    daif1: str = "[aarch64-serror-test] daif cpu=1 a=0 i=1",
    armed_origin: str | None = None,
    armed_mpidr: str | None = None,
    fatal_first: bool = False,
    extra_after: str = "",
) -> str:
    ao = armed_origin if armed_origin is not None else origin
    am = armed_mpidr if armed_mpidr is not None else mpidr
    armed_line = f"[aarch64-serror-test] armed origin={ao} mpidr=0x{am}"
    fatal_line = (
        f"[aarch64-serror] FATAL origin={origin} mpidr=0x{mpidr} "
        f"ec=0x2f esr=0x{esr} elr=0x{elr:016x} spsr=0x{spsr:016x} far={far}"
    )
    lines = [
        "UEFI: booting OS01",
        daif0,
        daif1,
        fatal_line if fatal_first else armed_line,
        armed_line if fatal_first else fatal_line,
    ]
    if extra_after:
        lines.append(extra_after)
    return "\n".join(lines) + "\n"


class TestSerrorEvidence(unittest.TestCase):
    ELR = 0xFFFF000040081234
    PARK_START = 0xFFFF000040082000
    PARK_SIZE = 0x10

    def test_valid_all_origins(self):
        for origin, spsr in (("el1t", 0x244), ("el1h", 0x245), ("el0t", 0x240)):
            log = _make_log(origin=origin, spsr=spsr, elr=self.ELR)
            self.assertTrue(
                runner.serror_evidence(
                    log, origin, self.ELR, self.PARK_START + 4,
                    self.PARK_START, self.PARK_SIZE
                ),
                f"expected valid evidence for {origin}",
            )

    def test_missing_cpu1_daif_fails(self):
        log = _make_log(daif1="")
        self.assertFalse(runner.serror_evidence(
            log, "el1h", self.ELR, self.PARK_START, self.PARK_START, self.PARK_SIZE
        ))

    def test_daif_a_set_or_i_clear_fails(self):
        for bad in (
            "[aarch64-serror-test] daif cpu=0 a=1 i=1",
            "[aarch64-serror-test] daif cpu=1 a=0 i=0",
        ):
            log = _make_log(daif0=bad if "cpu=0" in bad else "[aarch64-serror-test] daif cpu=0 a=0 i=1",
                            daif1=bad if "cpu=1" in bad else "[aarch64-serror-test] daif cpu=1 a=0 i=1")
            self.assertFalse(runner.serror_evidence(
                log, "el1h", self.ELR, self.PARK_START, self.PARK_START, self.PARK_SIZE
            ))

    def test_wrong_origin_fails(self):
        log = _make_log(origin="el1t", spsr=0x245)
        self.assertFalse(runner.serror_evidence(
            log, "el1h", self.ELR, self.PARK_START, self.PARK_START, self.PARK_SIZE
        ))

    def test_mismatched_mpidr_fails(self):
        log = _make_log(armed_mpidr="0000000080000001")
        self.assertFalse(runner.serror_evidence(
            log, "el1h", self.ELR, self.PARK_START, self.PARK_START, self.PARK_SIZE
        ))

    def test_wrong_esr_elr_spsr_far_fail(self):
        for kwargs in (
            {"esr": "00000000be000056"},
            {"elr": self.ELR + 4},
            {"spsr": 0x345},
            {"far": "0x0000000000000000"},
        ):
            log = _make_log(**kwargs)
            self.assertFalse(runner.serror_evidence(
                log, "el1h", self.ELR, self.PARK_START, self.PARK_START, self.PARK_SIZE
            ), f"expected failure for {kwargs}")

    def test_fatal_before_armed_fails(self):
        log = _make_log(fatal_first=True)
        self.assertFalse(runner.serror_evidence(
            log, "el1h", self.ELR, self.PARK_START, self.PARK_START, self.PARK_SIZE
        ))

    def test_duplicate_fatal_fails(self):
        extra = (
            "[aarch64-serror] FATAL origin=el1h mpidr=0x0000000080000000 "
            f"ec=0x2f esr=0x00000000be000055 elr=0x{self.ELR:016x} "
            "spsr=0x0000000000000245 far=n/a"
        )
        log = _make_log(extra_after=extra)
        self.assertFalse(runner.serror_evidence(
            log, "el1h", self.ELR, self.PARK_START, self.PARK_START, self.PARK_SIZE
        ))

    def test_continuation_or_sync_fatal_fails(self):
        for extra in (
            "[aarch64-serror-test] returned",
            "[aarch64-sync] FATAL mpidr=0x0000000080000000 ec=0x25 esr=0x0000000096000007 elr=0x0000000000000000 spsr=0x00000000000003c5 far=n/a",
        ):
            log = _make_log(extra_after=extra)
            self.assertFalse(runner.serror_evidence(
                log, "el1h", self.ELR, self.PARK_START, self.PARK_START, self.PARK_SIZE
            ))

    def test_park_pc_outside_range_fails(self):
        log = _make_log()
        for bad_pc in (self.PARK_START - 4, self.PARK_START + self.PARK_SIZE):
            self.assertFalse(runner.serror_evidence(
                log, "el1h", self.ELR, bad_pc, self.PARK_START, self.PARK_SIZE
            ))


class TestSerrorRunnerFailures(unittest.TestCase):
    def _run_with_mocks(self, *, text, timed_out=False, stopped_by_runner=True,
                        returncode=None, hmp_out="PC=ffff000040082004\n"):
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            fw = td_path / "fw.fd"
            img = td_path / "disk.img"
            elf = td_path / "kernel.elf"
            fw.write_bytes(b"fw")
            img.write_bytes(b"img")
            elf.write_bytes(b"elf")

            fake_session = mock.MagicMock()
            fake_session.text = text
            fake_session.timed_out = timed_out
            fake_session.stopped_by_runner = stopped_by_runner
            fake_session.returncode = returncode

            fake_qmp = mock.MagicMock()
            fake_qmp.execute.return_value = hmp_out

            args = Namespace(
                origin="el1h",
                firmware=str(fw),
                image=str(img),
                elf=str(elf),
                qemu="qemu-system-aarch64",
                log_dir=str(td_path / "logs"),
                build_dir=None,
                profile="aarch64-clang",
                timeout=5.0,
            )
            with mock.patch.object(runner, "verify_image_kernel_sha256", return_value="abc"), \
                 mock.patch.object(runner, "read_serror_elf_symbols",
                                   return_value=(0xFFFF000040081234, 0xFFFF000040082000, 0x10)), \
                 mock.patch.object(runner, "QmpClient", return_value=fake_qmp), \
                 mock.patch.object(runner, "ProcessSession", return_value=fake_session):
                return runner.run_case(args, build_dir=None, profile="aarch64-clang")

    def test_qemu_self_exited_fails(self):
        rc = self._run_with_mocks(
            text=_make_log(),
            stopped_by_runner=False,
            returncode=0,
        )
        self.assertNotEqual(0, rc)

    def test_unparseable_pc_fails(self):
        rc = self._run_with_mocks(
            text=_make_log(),
            hmp_out="NO_PC_HERE\n",
        )
        self.assertNotEqual(0, rc)

    def test_timeout_fails(self):
        rc = self._run_with_mocks(
            text="UEFI: booting OS01\n",
            timed_out=True,
        )
        self.assertNotEqual(0, rc)


if __name__ == "__main__":
    unittest.main()
