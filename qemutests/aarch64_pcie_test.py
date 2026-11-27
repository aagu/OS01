#!/usr/bin/env python3
"""OS01 AArch64 PCIe ECAM & RPi5 graceful-fallback acceptance suite.

Checks
------
ecam-virt : Boots QEMU ``virt`` with ``-device virtio-rng-pci,rng=rng0``,
            verifies ``subsys: init  device-boot ... ok``, ``ls /dev/pci``,
            ``lspci``, ``lspci -n``, ``lspci -v`` (resources + ``IRQ: none``),
            and ``lspci -x`` (256-byte config space dump).
no-ecam   : Replaces ``pci-host-ecam-generic`` with ``brcm,bcm2712-pcie`` in
            the diagnostic DTB, verifies graceful fallback without
            ``FATAL``/``PANIC``, reaches the shell, and verifies ``lspci``
            exits cleanly with status 1.

``--self-test`` validates command construction, DTS transformation, and output
checkers against synthetic transcripts without invoking QEMU.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))
if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:
    from qemutests.harness.result import RunArchive
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]

SUITE = "aarch64-pcie"


def qemu_command(args: argparse.Namespace, diagnostic_dtb: str | None,
                 attach_pci_rng: bool, ahci_disk: str | None = None) -> list[str]:
    """Build the QEMU argv for one AArch64 PCIe acceptance boot."""
    command = [
        args.qemu,
        "-M", "virt,gic-version=2" + (",acpi=off" if diagnostic_dtb else ""),
        "-cpu", "cortex-a53",
        "-smp", str(getattr(args, "cpus", 1)),
        "-m", str(getattr(args, "ram_mib", 512)),
        "-drive", f"if=pflash,format=raw,readonly=on,file={args.firmware}",
        "-drive", f"if=none,file={args.image},format=raw,readonly=on,id=disk",
        "-device", "virtio-blk-device,drive=disk",
        "-serial", "stdio",
        "-display", "none",
        "-no-reboot",
        "-no-shutdown",
    ]
    if attach_pci_rng:
        command += [
            "-object", "rng-random,filename=/dev/urandom,id=rng0",
            "-device", "virtio-rng-pci,rng=rng0",
        ]
    else:
        command += [
            "-device", "virtio-rng-device",
        ]
    if ahci_disk:
        command += [
            "-device", "ich9-ahci,id=ahci",
            "-drive", f"if=none,file={ahci_disk},format=raw,id=ahci_disk",
            "-device", "ide-hd,drive=ahci_disk,bus=ahci.0",
        ]
    if diagnostic_dtb:
        command += ["-dtb", diagnostic_dtb]
    return command


def replace_ecam_with_bcm2712(dts_text: str) -> str:
    """Replace ``pci-host-ecam-generic`` compatible string with RPi5 BCM2712."""
    pattern = re.compile(r'(compatible\s*=\s*")pci-host-ecam-generic(")')
    if not pattern.search(dts_text):
        raise ValueError("DTS does not contain compatible = \"pci-host-ecam-generic\"")
    transformed = pattern.sub(r'\1brcm,bcm2712-pcie\2', dts_text)
    if "pci-host-ecam-generic" in transformed:
        raise ValueError("pci-host-ecam-generic still present after DTS transform")
    return transformed


def build_no_ecam_dtb(base_dtb: str, out_dir: Path) -> str:
    """Decompile ``base_dtb``, inject ``brcm,bcm2712-pcie``, and recompile."""
    dts_path = out_dir / "no-ecam.dts"
    dtb_path = out_dir / "no-ecam.dtb"
    decompile = subprocess.run(
        ["dtc", "-I", "dtb", "-O", "dts", base_dtb],
        check=True,
        capture_output=True,
        text=True,
    )
    dts_text = replace_ecam_with_bcm2712(decompile.stdout)
    dts_path.write_text(dts_text, encoding="utf-8")
    subprocess.run(
        ["dtc", "-I", "dts", "-O", "dtb", "-o", str(dtb_path), str(dts_path)],
        check=True,
        capture_output=True,
    )
    return str(dtb_path)


def _has_fatal_or_panic(text: str) -> list[str]:
    failures = []
    for kw in ("FATAL", "PANIC", "vmm_gate_violation"):
        if kw in text:
            failures.append(f"unexpected {kw!r} in serial log")
    return failures


def check_ecam_virt(text: str) -> list[str]:
    """Validate positive QEMU virt ECAM enumeration + /dev/pci + lspci output."""
    text = text.replace("\r", "")
    failures = _has_fatal_or_panic(text)
    if not re.search(r"subsys:\s+init\s+device-boot[^\n]*ok", text):
        failures.append("missing 'subsys: init  device-boot ... ok' boot marker")
    if "pcie: GICv2m @" not in text:
        failures.append("missing 'pcie: GICv2m @' boot marker")

    if "0000:00:00.0" not in text:
        failures.append("missing '0000:00:00.0' in /dev/pci listing")
    if "0000:00:01.0" not in text:
        failures.append("missing '0000:00:01.0' in /dev/pci listing")

    for tag in ("__LSPCI_RC=0__", "__LSPCI_N_RC=0__",
                "__LSPCI_V_RC=0__", "__LSPCI_X_RC=0__"):
        if not re.search(rf"(?m)^{re.escape(tag)}$", text):
            failures.append(f"missing standalone marker {tag!r}")

    if "00:00.0 Host bridge:" not in text:
        failures.append("missing '00:00.0 Host bridge:' in lspci output")
    if "1b36:0008" not in text:
        failures.append("missing '1b36:0008' in lspci -n output")
    if "1af4:1005" not in text and "1af4:1044" not in text:
        failures.append("missing Virtio RNG '1af4:1005'/'1af4:1044' in lspci -n output")
    if "IRQ: none" not in text:
        failures.append("missing 'IRQ: none' in lspci -v output")
    if "IRQ: 36" not in text:
        failures.append("missing 'IRQ: 36' in lspci -v output")
    if "Region " not in text or "Memory at 0x" not in text:
        failures.append("missing BAR 'Region ... Memory at 0x...' in lspci -v output")
    if "36 1b 08 00" not in text:
        failures.append("missing host bridge config space bytes '36 1b 08 00' in lspci -x")
    if "f4 1a" not in text:
        failures.append("missing virtio-rng vendor bytes 'f4 1a' in lspci -x")
    return failures


def check_no_ecam(text: str) -> list[str]:
    """Validate negative DTB graceful fallback (BUS_UNAVAILABLE -> lspci rc=1)."""
    text = text.replace("\r", "")
    failures = _has_fatal_or_panic(text)
    if not re.search(r"subsys:\s+init\s+device-boot[^\n]*ok", text):
        failures.append("missing 'subsys: init  device-boot ... ok' boot marker")
    if not re.search(r"(?m)^__NOECAM_RC=1__$", text):
        failures.append("expected standalone '__NOECAM_RC=1__' marker from lspci")
    return failures


def check_ahci_virt(text: str) -> list[str]:
    """Validate QEMU virt ich9-ahci enumeration, driver binding, and /dev/hdb read."""
    text = text.replace("\r", "")
    failures = _has_fatal_or_panic(text)
    if not re.search(r"subsys:\s+init\s+device-boot[^\n]*ok", text):
        failures.append("missing 'subsys: init  device-boot ... ok' boot marker")
    if "pcie: GICv2m @" not in text:
        failures.append("missing 'pcie: GICv2m @' boot marker")
    for tag in ("__HDB_RC=0__", "__AHCI_LSPCI_RC=0__"):
        if not re.search(rf"(?m)^{re.escape(tag)}$", text):
            failures.append(f"missing standalone marker {tag!r}")
    if "82801IR/IO/IH SATA AHCI Controller" not in text:
        failures.append("missing ICH9 AHCI controller in lspci -v output")
    if "Kernel driver in use: ahci" not in text:
        failures.append("missing 'Kernel driver in use: ahci' in lspci -v output")
    if "OS01-AARCH64-AHCI-OK" not in text:
        failures.append("missing 'OS01-AARCH64-AHCI-OK' read from /dev/hdb")
    return failures


def _run_qemu_check(args: argparse.Namespace, *, check_name: str,
                    run_dir: Path) -> tuple[dict, list[str]]:
    result = {
        "text": "",
        "timed_out": False,
        "error": None,
        "environment_error": False,
    }
    session = None
    command: list[str] = []
    try:
        from aarch64_uefi_smp import generate_diagnostic_dtb

        run_dir.mkdir(parents=True, exist_ok=True)
        cpus = int(getattr(args, "cpus", 1))
        ram_mib = int(getattr(args, "ram_mib", 512))
        diagnostic_dtb = None
        if getattr(args, "diagnostic_dtb", None):
            diagnostic_dtb = generate_diagnostic_dtb(
                args.qemu, str(run_dir), cpus, ram_mib)
            if check_name == "no-ecam":
                diagnostic_dtb = build_no_ecam_dtb(diagnostic_dtb, run_dir)

        attach_pci_rng = (check_name == "ecam-virt")
        ahci_disk = None
        if check_name == "ahci-virt":
            ahci_path = run_dir / "ahci-test.img"
            with open(ahci_path, "wb") as f:
                f.write(b"OS01-AARCH64-AHCI-OK\n")
                f.truncate(2 * 1024 * 1024)
            ahci_disk = str(ahci_path)
        command = qemu_command(args, diagnostic_dtb, attach_pci_rng, ahci_disk=ahci_disk)
        (run_dir / "command.json").write_text(
            json.dumps(command, indent=2) + "\n", encoding="utf-8")

        session = ProcessSession(
            argv=command,
            run_dir=run_dir,
            timeout_s=args.timeout,
            writable_stdin=True,
        )
        session.start()
        session.wait_for(lambda s: "# " in s)
        if session.timed_out:
            result["timed_out"] = True
            result["text"] = session.text
            return result, command

        if check_name == "ecam-virt":
            cmd = (
                "ls /dev/pci; echo __LS''_END__\n"
                "lspci; echo __LSPCI''_RC=$?__\n"
                "lspci -n; echo __LSPCI_N''_RC=$?__\n"
                "lspci -v; echo __LSPCI_V''_RC=$?__\n"
                "lspci -x; echo __LSPCI_X''_RC=$?__\n"
                "echo __PCIE''_DONE__\n"
            )
            session.send(cmd.encode("utf-8"))
            session.wait_for(lambda s: bool(re.search(r"(?m)^__PCIE_DONE__$", s.replace("\r", ""))))
        elif check_name == "ahci-virt":
            cmd = (
                "ls /dev/hdb; echo __HDB''_RC=$?__\n"
                "head -n 1 /dev/hdb; echo __HDB_READ''_END__\n"
                "lspci -v; echo __AHCI_LSPCI''_RC=$?__\n"
                "echo __AHCI''_DONE__\n"
            )
            raw = cmd.encode("utf-8")
            for i in range(0, len(raw), 8):
                session.send(raw[i:i + 8])
                time.sleep(0.01)
            session.wait_for(lambda s: bool(re.search(r"(?m)^__AHCI_DONE__$", s.replace("\r", ""))))
        else:
            cmd = "lspci; echo __NOECAM''_RC=$?__\n"
            session.send(cmd.encode("utf-8"))
            session.wait_for(lambda s: bool(re.search(r"(?m)^__NOECAM_RC=\d+__$", s.replace("\r", ""))))

        result["timed_out"] = bool(session.timed_out)
        if not result["timed_out"]:
            session.observe(0.5)
        result["text"] = session.text
    except Exception as error:  # noqa: BLE001
        result["error"] = f"{type(error).__name__}: {error}"
        result["environment_error"] = True
    finally:
        if session is not None:
            try:
                session.stop()
            finally:
                session.close()
    return result, command


def _write_suite_report(archive, *, argv, profile, cpu, memory_mib, firmware,
                        image, started_monotonic, status, runner_exit_code,
                        errors, extra=None) -> None:
    try:
        from qemutests.boot_suite import write_suite_report
        write_suite_report(
            archive,
            suite=SUITE,
            argv=argv,
            profile=profile,
            session=None,
            started_monotonic=started_monotonic,
            status=status,
            runner_exit_code=runner_exit_code,
            errors=errors,
            cpu=int(cpu),
            memory_mib=int(memory_mib or 0),
            firmware=firmware,
            image=image,
            stopped_by_runner=True,
            extra=extra,
            warn_only=True,
        )
    except Exception:
        pass


def self_test() -> None:
    """QEMU-free unit tests for command builder, DTS transform, and checkers."""
    args = argparse.Namespace(
        qemu="qemu-system-aarch64",
        firmware="QEMU_EFI.fd",
        image="aarch64-uefi.img",
        cpus=1,
        ram_mib=512,
    )
    cmd_pos = qemu_command(args, "/tmp/virt.dtb", True)
    assert "-device" in cmd_pos and "virtio-rng-pci,rng=rng0" in cmd_pos
    assert "-object" in cmd_pos and "rng-random,filename=/dev/urandom,id=rng0" in cmd_pos
    assert "-dtb" in cmd_pos and "/tmp/virt.dtb" in cmd_pos
    assert "virt,gic-version=2,acpi=off" in cmd_pos

    cmd_neg = qemu_command(args, "/tmp/no-ecam.dtb", False)
    assert "virtio-rng-pci,rng=rng0" not in cmd_neg
    assert "virtio-rng-device" in cmd_neg

    sample_dts = (
        "/dts-v1/;\n"
        "/ {\n"
        "    pcie@10000000 {\n"
        "        compatible = \"pci-host-ecam-generic\";\n"
        "        device_type = \"pci\";\n"
        "    };\n"
        "};\n"
    )
    transformed = replace_ecam_with_bcm2712(sample_dts)
    assert "brcm,bcm2712-pcie" in transformed
    assert "pci-host-ecam-generic" not in transformed

    try:
        replace_ecam_with_bcm2712("/ { };\n")
        raise AssertionError("expected ValueError for missing ECAM compatible")
    except ValueError:
        pass

    good_pos_log = "\n\r".join([
        "pcie: GICv2m @ 0x8020000 base_spi=80 num_spis=64",
        "subsys: init  device-boot ... ok",
        "0000:00:00.0",
        "0000:00:01.0",
        "00:00.0 Host bridge: Red Hat, Inc. (QEMU Virtual Machine) QEMU PCIe Host bridge (rev 00)",
        "00:01.0 Unclassified device: Red Hat, Inc. Virtio modern RNG device (rev 01)",
        "__LSPCI_RC=0__",
        "00:00.0 0600: 1b36:0008 (rev 00)",
        "00:01.0 00ff: 1af4:1044 (rev 01)",
        "__LSPCI_N_RC=0__",
        "\tIRQ: none",
        "\tIRQ: 36",
        "\tRegion 4: Memory at 0x8000000000",
        "__LSPCI_V_RC=0__",
        "00: 36 1b 08 00 07 00 10 00",
        "00: f4 1a 44 10 07 00 10 00",
        "__LSPCI_X_RC=0__",
        "__PCIE_DONE__",
    ])
    assert check_ecam_virt(good_pos_log) == []
    assert check_ecam_virt(good_pos_log + "\nPANIC: boom") != []
    assert check_ecam_virt(good_pos_log.replace("IRQ: none", "IRQ: 16")) != []
    assert check_ecam_virt(good_pos_log.replace("IRQ: 36", "IRQ: none")) != []

    good_neg_log = "\n\r".join([
        "subsys: init  device-boot ... ok",
        "[terminal] ERROR: cannot open /dev/fb",
        "lspci: cannot open /dev/pci",
        "__NOECAM_RC=1__",
    ])
    assert check_no_ecam(good_neg_log) == []
    assert check_no_ecam(good_neg_log.replace("__NOECAM_RC=1__", "__NOECAM_RC=0__")) != []
    assert check_no_ecam(good_neg_log + "\nFATAL: bad") != []


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="OS01 AArch64 PCIe acceptance suite")
    parser.add_argument("--check", choices=("all", "ecam-virt", "ahci-virt", "no-ecam"), default="all")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--firmware", default=None)
    parser.add_argument("--image", default=None)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--log-dir", default="build/aarch64-clang/logs/aarch64-pcie")
    parser.add_argument("--build-dir", default=None)
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE", "aarch64-clang"))
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--cpus", type=int, default=1)
    parser.add_argument("--ram-mib", type=int, default=512)
    parser.add_argument("--diagnostic-dtb", default="auto")
    args = parser.parse_args(argv)

    if args.self_test:
        self_test()
        print("aarch64_pcie_test: self-test passed")
        return 0

    if not (args.firmware and args.image):
        parser.error("--firmware and --image are required outside --self-test")

    if ProcessSession is None:
        print("ERROR: qemutests.harness.process is unavailable", file=sys.stderr)
        return 2

    checks = ("ecam-virt", "ahci-virt", "no-ecam") if args.check == "all" else (args.check,)
    checkers = {
        "ecam-virt": check_ecam_virt,
        "ahci-virt": check_ahci_virt,
        "no-ecam": check_no_ecam,
    }

    failures: list[str] = []
    environment_errors: list[str] = []

    for check_name in checks:
        started = time.monotonic()
        archive = None
        if RunArchive is not None and args.build_dir:
            archive = RunArchive.create(Path(args.build_dir), f"{SUITE}-{check_name}")
            run_dir = archive.run_dir
        else:
            run_dir = Path(args.log_dir) / check_name
            run_dir.mkdir(parents=True, exist_ok=True)

        capture, command = _run_qemu_check(args, check_name=check_name, run_dir=run_dir)
        problems: list[str] = []
        if capture.get("error"):
            problems.append(str(capture["error"]))
        if capture["timed_out"]:
            problems.append("timed out waiting for shell/PCIe check markers")
        if not capture.get("environment_error") and not capture["timed_out"]:
            problems.extend(checkers[check_name](capture["text"]))

        is_env = bool(capture.get("environment_error"))
        if is_env:
            environment_errors.extend(f"{check_name}: {p}" for p in problems)
        else:
            failures.extend(f"{check_name}: {p}" for p in problems)

        print(json.dumps({
            "check": check_name,
            "failures": problems,
            "environment_error": is_env,
            "run_dir": str(run_dir),
        }))

        _write_suite_report(
            archive,
            argv=command,
            profile=args.profile,
            cpu=args.cpus,
            memory_mib=args.ram_mib,
            firmware=args.firmware,
            image=args.image,
            started_monotonic=started,
            status="ERROR" if is_env else ("PASS" if not problems else "FAIL"),
            runner_exit_code=2 if is_env else (0 if not problems else 1),
            errors=problems,
            extra={"check": check_name, "timed_out": capture["timed_out"],
                   "environment_error": is_env},
        )

    if environment_errors:
        print("ERROR: aarch64 PCIe suite (environment failure)", file=sys.stderr)
        for err in environment_errors:
            print(f"  - {err}", file=sys.stderr)
        return 2
    if failures:
        print("FAIL: aarch64 PCIe suite", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("PASS: aarch64 PCIe suite")
    return 0


if __name__ == "__main__":
    sys.exit(main())
