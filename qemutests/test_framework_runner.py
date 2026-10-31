#!/usr/bin/env python3
"""Contract tests for qemutests/run_framework_tests.py supervisor (Task C1)."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER_SCRIPT = ROOT / "qemutests" / "run_framework_tests.py"


class FrameworkRunnerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="test-framework-runner-")
        self.addCleanup(self.tmp.cleanup)
        self.tmp_path = Path(self.tmp.name)

    def _create_dummy_module(self, code: str, name: str = "dummy_test") -> str:
        mod_file = self.tmp_path / f"{name}.py"
        mod_file.write_text(code, encoding="utf-8")
        return name

    def _run_supervisor(self, args: list[str], timeout: float = 30.0) -> subprocess.CompletedProcess:
        env = dict(os.environ)
        env["PYTHONPATH"] = f"{self.tmp_path}:{ROOT}:{env.get('PYTHONPATH', '')}"
        return subprocess.run(
            [sys.executable, str(RUNNER_SCRIPT)] + args,
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout,
        )

    def test_hang_before_first_output_times_out(self):
        """A module that hangs before producing any output must be terminated on module timeout."""
        code = (
            "import unittest, time\n"
            "class HangTest(unittest.TestCase):\n"
            "    def test_hang(self):\n"
            "        time.sleep(60)\n"
        )
        mod_name = self._create_dummy_module(code, "test_hang")
        t0 = time.time()
        res = self._run_supervisor(["--module-timeout", "2", "--total-timeout", "10", mod_name])
        dur = time.time() - t0
        self.assertLess(dur, 6.0, f"Supervisor took too long: {dur:.2f}s")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("TIMEOUT", res.stdout + res.stderr)

    def test_stderr_flood_does_not_deadlock(self):
        """A child process that writes >128KB to stderr must not deadlock the supervisor."""
        code = (
            "import unittest, sys\n"
            "class FloodTest(unittest.TestCase):\n"
            "    def test_flood(self):\n"
            "        for _ in range(5000):\n"
            "            sys.stderr.write('E' * 100 + '\\n')\n"
            "            sys.stderr.flush()\n"
        )
        mod_name = self._create_dummy_module(code, "test_flood")
        res = self._run_supervisor(["--module-timeout", "10", "--total-timeout", "20", mod_name])
        self.assertEqual(res.returncode, 0, f"Failed with {res.stderr}")
        self.assertIn("PASS", res.stdout)

    def test_child_and_grandchild_cleaned_after_timeout(self):
        """When a test module spawns a grandchild and times out, both child and grandchild must be reaped."""
        pid_file = self.tmp_path / "grandchild.pid"
        code = (
            "import unittest, subprocess, sys, time\n"
            f"pid_file = {str(pid_file)!r}\n"
            "class GrandchildTest(unittest.TestCase):\n"
            "    def test_spawn(self):\n"
            "        proc = subprocess.Popen([sys.executable, '-c', f'import os, time; open({pid_file!r}, \"w\").write(str(os.getpid())); time.sleep(60)'])\n"
            "        time.sleep(60)\n"
        )
        mod_name = self._create_dummy_module(code, "test_grandchild")
        res = self._run_supervisor(["--module-timeout", "2", "--total-timeout", "10", mod_name])
        self.assertNotEqual(res.returncode, 0)
        self.assertTrue(pid_file.exists(), "Grandchild PID file was never written")
        grandchild_pid = int(pid_file.read_text().strip())

        # Verify grandchild process is no longer running
        time.sleep(0.5)
        try:
            os.kill(grandchild_pid, 0)
            alive = True
        except OSError:
            alive = False
        self.assertFalse(alive, f"Grandchild PID {grandchild_pid} is still alive after supervisor timeout")

    def test_ctrl_c_exit_130(self):
        """SIGINT to supervisor must terminate child process group and exit with code 130."""
        code = (
            "import unittest, time\n"
            "class SleepTest(unittest.TestCase):\n"
            "    def test_sleep(self):\n"
            "        time.sleep(30)\n"
        )
        mod_name = self._create_dummy_module(code, "test_ctrl_c")
        env = dict(os.environ)
        env["PYTHONPATH"] = f"{self.tmp_path}:{ROOT}:{env.get('PYTHONPATH', '')}"

        proc = subprocess.Popen(
            [sys.executable, str(RUNNER_SCRIPT), "--module-timeout", "30", "--total-timeout", "60", mod_name],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        # Give it a moment to start
        time.sleep(0.5)
        # Send SIGINT to the supervisor's process group
        os.killpg(proc.pid, signal.SIGINT)
        stdout, stderr = proc.communicate(timeout=5)
        self.assertEqual(proc.returncode, 130, f"Expected returncode 130, got {proc.returncode}. Stderr: {stderr}")

    def test_one_module_failure_does_not_hide_later_modules(self):
        """Failure in module 1 must not prevent module 2 from running, and overall batch must fail."""
        code_fail = (
            "import unittest\n"
            "class FailTest(unittest.TestCase):\n"
            "    def test_fail(self):\n"
            "        self.fail('intentional failure')\n"
        )
        code_pass = (
            "import unittest\n"
            "class PassTest(unittest.TestCase):\n"
            "    def test_pass(self):\n"
            "        pass\n"
        )
        mod_fail = self._create_dummy_module(code_fail, "test_mod_fail")
        mod_pass = self._create_dummy_module(code_pass, "test_mod_pass")
        res = self._run_supervisor(["--module-timeout", "5", "--total-timeout", "15", mod_fail, mod_pass])
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("FAIL", res.stdout)
        self.assertIn("PASS", res.stdout)
        self.assertIn("test_mod_pass", res.stdout)


if __name__ == "__main__":
    unittest.main()
