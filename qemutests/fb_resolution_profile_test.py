#!/usr/bin/env python3
"""Build-profile isolation contract for FB_RESOLUTION_TEST (Task 9).

FB_RESOLUTION_TEST is a test-only build profile.  This host-only harness
asserts, without building anything, that:

  * FB_RESOLUTION_TEST=1 selects the isolated ``resolution-test`` image dir
    while FB_RESOLUTION_TEST=0 (and unset) keeps the normal image path;
  * an unknown value is rejected at parse time;
  * it is mutually exclusive with OS01_SYSTEST / OS01_NETTEST /
    KERNEL_SELFTEST / KERNEL_CANARY_SELFTEST / ARCH9_FAULT;
  * a non-x86 target rejects FB_RESOLUTION_TEST=1 explicitly.

All make invocations use cheap introspection targets (print-run-paths /
validate-profile); nothing is compiled.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

failures = []


def run_make(args):
    env = dict(os.environ)
    # The outer host-test runs under a controlled sub-make; drop the
    # recursive-make plumbing so the nested invocation starts clean.
    for var in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES"):
        env.pop(var, None)
    for key in list(env):
        if key.startswith("MAKE_"):  # jobserver fds
            env.pop(key, None)
    proc = subprocess.run(
        ["make", "-C", ROOT, "--no-print-directory"] + args,
        capture_output=True, text=True, env=env, timeout=120,
    )
    return proc.returncode, proc.stdout + proc.stderr


def check(name, condition, detail=""):
    if condition:
        print(f"  [PASS] {name}")
    else:
        print(f"  [FAIL] {name} {detail}")
        failures.append(name)


def test_paths():
    rc, out = run_make(["PROFILE=x86_64-clang", "FB_RESOLUTION_TEST=1",
                        "print-run-paths"])
    check("FB_RESOLUTION_TEST=1 parse+build path", rc == 0, out)
    check("FB_RESOLUTION_TEST=1 isolated image dir",
          "image/resolution-test/disk.img" in out, out)

    rc0, out0 = run_make(["PROFILE=x86_64-clang", "FB_RESOLUTION_TEST=0",
                          "print-run-paths"])
    check("FB_RESOLUTION_TEST=0 parse", rc0 == 0, out0)
    check("FB_RESOLUTION_TEST=0 keeps the normal image",
          "/image/disk.img" in out0 and "resolution-test" not in out0, out0)

    rcu, outu = run_make(["PROFILE=x86_64-clang", "print-run-paths"])
    check("unset keeps the normal image",
          rcu == 0 and "/image/disk.img" in outu and "resolution-test" not in outu,
          outu)

    check("test/production image dirs differ",
          "image/resolution-test/disk.img" in out and "/image/disk.img" in out0)


def test_unknown_value():
    rc, out = run_make(["PROFILE=x86_64-clang", "FB_RESOLUTION_TEST=2",
                        "print-run-paths"])
    check("unknown FB_RESOLUTION_TEST rejected at parse time",
          rc != 0 and "must be 0 or 1" in out, out)


def test_conflicts():
    for extra in ("OS01_SYSTEST=1", "OS01_NETTEST=1", "KERNEL_SELFTEST=1",
                  "KERNEL_CANARY_SELFTEST=1", "ARCH9_FAULT=observe"):
        rc, out = run_make(["PROFILE=x86_64-clang", "FB_RESOLUTION_TEST=1",
                            extra, "print-run-paths"])
        check(f"FB_RESOLUTION_TEST=1 conflicts with {extra}",
              rc != 0 and "cannot be combined" in out, out)


def test_non_x86():
    rc, out = run_make(["PROFILE=aarch64-clang", "FB_RESOLUTION_TEST=1",
                        "validate-profile"])
    check("non-x86 target rejects FB_RESOLUTION_TEST=1",
          rc != 0 and "only supported on x86" in out, out)


def main():
    print("=== FB_RESOLUTION_TEST build-profile contract ===")
    test_paths()
    test_unknown_value()
    test_conflicts()
    test_non_x86()
    print("")
    if failures:
        print(f"FAILED ({len(failures)}): {', '.join(failures)}")
        return 1
    print("ALL PROFILE CONTRACT CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
