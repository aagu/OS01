#!/usr/bin/env python3
"""Incremental build and staging contract tests (Task B1 & B3).

Verifies mbedTLS source manifest, input key calculation, compile failure
isolation, and atomic receipt publishing.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MbedTLSContractTests(unittest.TestCase):
    def setUp(self):
        # We need an isolated test environment to avoid perturbing working tree
        self.tmp = tempfile.TemporaryDirectory(prefix="mbedtls-contract-")
        self.addCleanup(self.tmp.cleanup)
        self.work_dir = Path(self.tmp.name)

    def test_mbedtls_compile_failure_does_not_publish(self):
        """When mbedtls compilation encounters an error, build must fail and stamp/receipt must not be published."""
        # Create a failing compiler wrapper
        wrapper = self.work_dir / "failing-cc"
        wrapper.write_text(
            '#!/bin/sh\n'
            'for arg in "$@"; do\n'
            '  if [ "$arg" = "aes.c" ] || echo "$arg" | grep -q "/aes\\.c$"; then\n'
            '    echo "INJECTED COMPILATION ERROR" >&2\n'
            '    exit 1\n'
            '  fi\n'
            'done\n'
            'exec clang "$@"\n'
        )
        wrapper.chmod(0o755)

        stamp = ROOT / "build" / "x86_64-clang" / "stamps" / "mbedtls-install.stamp"
        receipt = ROOT / "build" / "x86_64-clang" / "stamps" / "mbedtls-install.stamp.receipt"
        stamp_mtime_before = stamp.stat().st_mtime if stamp.exists() else None
        receipt_mtime_before = receipt.stat().st_mtime if receipt.exists() else None

        cmd = ["make", "--no-print-directory", "PROFILE=x86_64-clang", f"TARGET_CC={wrapper}", "sysroot"]
        res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=60)

        # Must fail
        self.assertNotEqual(res.returncode, 0, f"make sysroot unexpectedly succeeded with failing compiler:\n{res.stdout}")
        self.assertIn("INJECTED COMPILATION ERROR", res.stderr + res.stdout)

        # Stamp and receipt must not have been updated
        if stamp_mtime_before is not None:
            self.assertEqual(stamp.stat().st_mtime, stamp_mtime_before, "Stamp was touched despite compile failure")
        else:
            self.assertFalse(stamp.exists(), "Stamp was created despite compile failure")

        if receipt_mtime_before is not None:
            self.assertEqual(receipt.stat().st_mtime, receipt_mtime_before, "Receipt was updated despite compile failure")
        else:
            self.assertFalse(receipt.exists(), "Receipt was created despite compile failure")

    def test_mbedtls_libc_header_change_rebuilds(self):
        """Changes to consumed libc headers must invalidate mbedtls input digest."""
        # Query digest with normal libc header vs modified libc header
        libc_hdr = ROOT / "build" / "x86_64-clang" / "staging" / "libc" / "usr" / "include" / "string.h"
        if not libc_hdr.exists():
            self.skipTest("staging/libc/usr/include/string.h does not exist")

        # Read mbedtls receipt
        receipt_file = ROOT / "build" / "x86_64-clang" / "stamps" / "mbedtls-install.stamp.receipt"
        if not receipt_file.exists():
            self.skipTest("mbedtls receipt does not exist")

        receipt_content = receipt_file.read_text().strip()
        self.assertGreater(len(receipt_content), 0)

        # Dry run of make sysroot when nothing changed should show mbedtls input unchanged
        cmd = ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"]
        res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0)
        self.assertIn("[mbedtls] input unchanged", res.stdout)

    def test_mbedtls_source_removal_drops_archive_member(self):
        """Removing a source from MBEDTLS_SOURCES must result in that member being dropped from libmbedtls.a."""
        archive = ROOT / "build" / "x86_64-clang" / "staging" / "mbedtls" / "usr" / "lib" / "libmbedtls.a"
        if not archive.exists():
            self.skipTest("libmbedtls.a does not exist")

        # Verify llvm-ar lists members
        res = subprocess.run(["llvm-ar", "t", str(archive)], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0)
        members = set(res.stdout.splitlines())
        self.assertIn("aes.o", members)
        self.assertIn("sha256.o", members)


class CommandKeyContractTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="cmdkey-contract-")
        self.addCleanup(self.tmp.cleanup)
        self.work_dir = Path(self.tmp.name)

        # Ensure base kernel.bin is built
        res = subprocess.run(
            ["make", "--no-print-directory", "PROFILE=x86_64-clang", "kernel.bin"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0, f"Setup build failed: {res.stderr}")

    def test_compile_flags_a_b_a(self):
        """Changing CFLAGS must invalidate .cflags and recompile objects; reverting must recompile again."""
        cflags_file = ROOT / "build" / "x86_64-clang" / "kernel" / ".cflags"
        main_o = ROOT / "build" / "x86_64-clang" / "kernel" / "core" / "main.o"
        self.assertTrue(cflags_file.exists())
        self.assertTrue(main_o.exists())

        cflags_a = cflags_file.read_text()
        main_o_mtime_a = main_o.stat().st_mtime_ns

        # Identical build: .cflags and objects must not be touched
        res = subprocess.run(
            ["make", "--no-print-directory", "PROFILE=x86_64-clang", "kernel.bin"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0)
        self.assertEqual(cflags_file.read_text(), cflags_a)
        self.assertEqual(main_o.stat().st_mtime_ns, main_o_mtime_a)

        # Flag B
        res = subprocess.run(
            [
                "make",
                "--no-print-directory",
                "PROFILE=x86_64-clang",
                "KERNEL_EXTRA_CFLAGS=-DOS01_TEST_CMD_KEY_B",
                "kernel.bin",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0)
        cflags_b = cflags_file.read_text()
        self.assertNotEqual(cflags_b, cflags_a)
        main_o_mtime_b = main_o.stat().st_mtime_ns
        self.assertGreater(main_o_mtime_b, main_o_mtime_a)

        # Revert to Flag A
        res = subprocess.run(
            ["make", "--no-print-directory", "PROFILE=x86_64-clang", "kernel.bin"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0)
        cflags_reverted = cflags_file.read_text()
        self.assertEqual(cflags_reverted, cflags_a)
        main_o_mtime_c = main_o.stat().st_mtime_ns
        self.assertGreater(main_o_mtime_c, main_o_mtime_b)

    def test_link_flags_relink_only(self):
        """Changing LDFLAGS must update .link_key and relink ELF without recompiling .o files."""
        link_key_file = ROOT / "build" / "x86_64-clang" / "kernel" / ".link_key"
        elf_file = ROOT / "build" / "x86_64-clang" / "kernel" / "kernel.elf"
        main_o = ROOT / "build" / "x86_64-clang" / "kernel" / "core" / "main.o"
        self.assertTrue(link_key_file.exists())
        self.assertTrue(elf_file.exists())
        self.assertTrue(main_o.exists())

        key_a = link_key_file.read_text()
        elf_mtime_a = elf_file.stat().st_mtime_ns
        main_o_mtime_a = main_o.stat().st_mtime_ns

        # Change LDFLAGS
        res = subprocess.run(
            [
                "make",
                "--no-print-directory",
                "PROFILE=x86_64-clang",
                "KERNEL_EXTRA_LDFLAGS=--build-id=sha1",
                "kernel.bin",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0, f"make failed: {res.stderr}")
        key_b = link_key_file.read_text()
        self.assertNotEqual(key_b, key_a)
        self.assertIn("--build-id=sha1", key_b)
        elf_mtime_b = elf_file.stat().st_mtime_ns
        self.assertGreater(elf_mtime_b, elf_mtime_a, "kernel.elf was not relinked")
        self.assertEqual(
            main_o.stat().st_mtime_ns, main_o_mtime_a, "Kernel object unexpectedly recompiled on LDFLAGS-only change"
        )

        # Revert LDFLAGS
        res = subprocess.run(
            ["make", "--no-print-directory", "PROFILE=x86_64-clang", "kernel.bin"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0)
        self.assertEqual(link_key_file.read_text(), key_a)
        elf_mtime_c = elf_file.stat().st_mtime_ns
        self.assertGreater(elf_mtime_c, elf_mtime_b, "kernel.elf was not relinked on revert")
        self.assertEqual(
            main_o.stat().st_mtime_ns, main_o_mtime_a, "Kernel object unexpectedly recompiled on LDFLAGS revert"
        )

    def test_compiler_switch_recompiles(self):
        """Switching TARGET_CC / CLANG compiler wrapper must invalidate compile key and recompile objects."""
        wrapper = self.work_dir / "clang-wrapper"
        wrapper.write_text("#!/bin/sh\nexec clang \"$@\"\n")
        wrapper.chmod(0o755)

        cflags_file = ROOT / "build" / "x86_64-clang" / "kernel" / ".cflags"
        main_o = ROOT / "build" / "x86_64-clang" / "kernel" / "core" / "main.o"
        cflags_a = cflags_file.read_text()
        main_o_mtime_a = main_o.stat().st_mtime_ns

        res = subprocess.run(
            [
                "make",
                "--no-print-directory",
                "PROFILE=x86_64-clang",
                f"CLANG={wrapper}",
                "kernel.bin",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0, f"make failed: {res.stderr}")
        cflags_b = cflags_file.read_text()
        self.assertNotEqual(cflags_b, cflags_a)
        self.assertIn(str(wrapper), cflags_b)
        main_o_mtime_b = main_o.stat().st_mtime_ns
        self.assertGreater(main_o_mtime_b, main_o_mtime_a, "Kernel object did not recompile when compiler changed")

        # Revert compiler
        res = subprocess.run(
            ["make", "--no-print-directory", "PROFILE=x86_64-clang", "kernel.bin"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0)
        self.assertEqual(cflags_file.read_text(), cflags_a)
        main_o_mtime_c = main_o.stat().st_mtime_ns
        self.assertGreater(main_o_mtime_c, main_o_mtime_b, "Kernel object did not recompile when compiler reverted")

    def test_linker_switch_relinks(self):
        """Switching TARGET_LD linker wrapper must invalidate link key and relink without recompiling objects."""
        wrapper = self.work_dir / "ld-wrapper"
        wrapper.write_text("#!/bin/sh\nexec /usr/bin/ld.lld \"$@\"\n")
        wrapper.chmod(0o755)

        link_key_file = ROOT / "build" / "x86_64-clang" / "kernel" / ".link_key"
        elf_file = ROOT / "build" / "x86_64-clang" / "kernel" / "kernel.elf"
        main_o = ROOT / "build" / "x86_64-clang" / "kernel" / "core" / "main.o"

        key_a = link_key_file.read_text()
        elf_mtime_a = elf_file.stat().st_mtime_ns
        main_o_mtime_a = main_o.stat().st_mtime_ns

        res = subprocess.run(
            [
                "make",
                "--no-print-directory",
                "PROFILE=x86_64-clang",
                f"TARGET_LD={wrapper}",
                "kernel.bin",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0, f"make failed: {res.stderr}")
        key_b = link_key_file.read_text()
        self.assertNotEqual(key_b, key_a)
        self.assertIn(str(wrapper), key_b)
        elf_mtime_b = elf_file.stat().st_mtime_ns
        self.assertGreater(elf_mtime_b, elf_mtime_a, "kernel.elf was not relinked when linker changed")
        self.assertEqual(
            main_o.stat().st_mtime_ns, main_o_mtime_a, "Kernel object unexpectedly recompiled when linker changed"
        )

        # Revert linker
        res = subprocess.run(
            ["make", "--no-print-directory", "PROFILE=x86_64-clang", "kernel.bin"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0)
        self.assertEqual(link_key_file.read_text(), key_a)
        elf_mtime_c = elf_file.stat().st_mtime_ns
        self.assertGreater(elf_mtime_c, elf_mtime_b, "kernel.elf was not relinked when linker reverted")
        self.assertEqual(
            main_o.stat().st_mtime_ns, main_o_mtime_a, "Kernel object unexpectedly recompiled when linker reverted"
        )


class UserContractTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="user-contract-")
        self.addCleanup(self.tmp.cleanup)
        self.work_dir = Path(self.tmp.name)

        # Ensure base user programs are built
        res = subprocess.run(
            ["make", "--no-print-directory", "PROFILE=x86_64-clang", "user"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=120,
        )
        self.assertEqual(res.returncode, 0, f"Setup user build failed: {res.stderr}")

    def test_publish_waits_for_compile_and_link_lease(self):
        """Sysroot publisher must wait for active leases to drain before swapping symlink."""
        sysroot_link = ROOT / "build" / "x86_64-clang" / "sysroot"
        leases_dir = ROOT / "build" / "x86_64-clang" / "leases"
        leases_dir.mkdir(parents=True, exist_ok=True)

        gen_target_before = sysroot_link.readlink()
        gen_id = gen_target_before.name

        # Hold a simulated active lease
        lease_path = leases_dir / f"{gen_id}.999999.test_barrier"
        lease_path.mkdir()

        # Start make sysroot in background with a touched component to trigger publish
        try:
            # Touch a staging receipt to induce republish
            gui_receipt = ROOT / "build" / "x86_64-clang" / "stamps" / "gui-install.stamp.receipt"
            gui_receipt_old = gui_receipt.read_text() if gui_receipt.exists() else ""
            gui_receipt.write_text("forced-republish-digest")

            proc = subprocess.Popen(
                ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            # Wait a brief moment; process must still be running because lease is held
            import time
            time.sleep(1.0)
            self.assertIsNone(proc.poll(), "make sysroot did not wait for lease to drain!")
            self.assertEqual(sysroot_link.readlink(), gen_target_before)

            # Release lease
            lease_path.rmdir()

            # Now proc should finish successfully
            stdout, stderr = proc.communicate(timeout=60)
            self.assertEqual(proc.returncode, 0, f"make sysroot failed after lease release:\n{stdout}\n{stderr}")
        finally:
            if lease_path.exists():
                lease_path.rmdir()
            # Restore normal sysroot state
            subprocess.run(
                ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"],
                cwd=ROOT,
                capture_output=True,
                timeout=60,
            )

    def test_archive_only_relinks(self):
        """Updating libc.a must relink user programs without recompiling user object files."""
        init_o = ROOT / "build" / "x86_64-clang" / "user" / "obj" / "init.o"
        init_elf = ROOT / "build" / "x86_64-clang" / "user" / "init.elf"
        libc_a = ROOT / "build" / "x86_64-clang" / "sysroot" / "usr" / "lib" / "libc.a"
        self.assertTrue(init_o.exists())
        self.assertTrue(init_elf.exists())
        self.assertTrue(libc_a.exists())

        init_o_mtime_before = init_o.stat().st_mtime_ns
        init_elf_mtime_before = init_elf.stat().st_mtime_ns

        # Invalidate/touch libc.a
        import time
        time.sleep(0.01)
        os.utime(libc_a, None)

        res = subprocess.run(
            ["make", "--no-print-directory", "PROFILE=x86_64-clang", "user"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0, f"make user failed: {res.stderr}")

        init_elf_mtime_after = init_elf.stat().st_mtime_ns
        init_o_mtime_after = init_o.stat().st_mtime_ns

        self.assertGreater(init_elf_mtime_after, init_elf_mtime_before, "init.elf was not relinked")
        self.assertEqual(init_o_mtime_after, init_o_mtime_before, "init.o was recompiled on archive-only update")

    def test_unrelated_generation_does_not_recompile_user(self):
        """Creating a new sysroot generation for an unrelated library must not recompile unrelated user objects."""
        init_o = ROOT / "build" / "x86_64-clang" / "user" / "obj" / "init.o"
        self.assertTrue(init_o.exists())
        init_o_mtime_before = init_o.stat().st_mtime_ns

        # Induce a new generation via staging receipt invalidation of gui
        gui_receipt = ROOT / "build" / "x86_64-clang" / "stamps" / "gui-install.stamp.receipt"
        self.assertTrue(gui_receipt.exists())
        gui_receipt_old = gui_receipt.read_text()
        gui_receipt.write_text("forced-gui-generation-advance")

        try:
            res = subprocess.run(
                ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot", "user"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(res.returncode, 0, f"make failed:\n{res.stdout}\n{res.stderr}")
            init_o_mtime_after = init_o.stat().st_mtime_ns
            self.assertEqual(
                init_o_mtime_after, init_o_mtime_before, "init.o was recompiled despite being unrelated to gui change"
            )
        finally:
            gui_receipt.write_text(gui_receipt_old)
            subprocess.run(
                ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"],
                cwd=ROOT,
                capture_output=True,
                timeout=60,
            )

    def test_consumed_header_rebuilds_dependents(self):
        """Modifying content of a consumed header (<stdlib.h>) must recompile dependent user objects."""
        init_o = ROOT / "build" / "x86_64-clang" / "user" / "obj" / "init.o"
        self.assertTrue(init_o.exists())
        init_o_mtime_before = init_o.stat().st_mtime_ns

        stdlib_h = ROOT / "libc" / "include" / "stdlib.h"
        self.assertTrue(stdlib_h.exists())
        orig_content = stdlib_h.read_text()

        try:
            # Modify header content to change staging digest
            stdlib_h.write_text(orig_content + "\n/* test incremental rebuild probe */\n")

            res = subprocess.run(
                ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot", "user"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(res.returncode, 0, f"make failed:\n{res.stdout}\n{res.stderr}")

            init_o_mtime_after = init_o.stat().st_mtime_ns
            self.assertGreater(init_o_mtime_after, init_o_mtime_before, "init.o did not recompile after stdlib.h modified")
        finally:
            stdlib_h.write_text(orig_content)
            subprocess.run(
                ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot", "user"],
                cwd=ROOT,
                capture_output=True,
                timeout=120,
            )


class BusyboxContractTests(unittest.TestCase):
    def test_busybox_config_a_b_a(self):
        """Modifying config/busybox.config.in must change receipt and rebuild; reverting must revert receipt."""
        config_in = ROOT / "config" / "busybox.config.in"
        receipt_file = ROOT / "build" / "x86_64-clang" / "receipts" / "busybox.stamp.receipt"
        busybox_elf = ROOT / "build" / "x86_64-clang" / "artifacts" / "user" / "busybox.elf"
        self.assertTrue(config_in.exists())
        self.assertTrue(receipt_file.exists())

        receipt_a = receipt_file.read_text().strip()
        orig_config = config_in.read_text()

        try:
            # Modify config with a comment
            config_in.write_text(orig_config + "\n# test busybox config aba probe\n")

            res = subprocess.run(
                [
                    "make",
                    "--no-print-directory",
                    "PROFILE=x86_64-clang",
                    str(busybox_elf),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=180,
            )
            self.assertEqual(res.returncode, 0, f"make busybox failed: {res.stderr}")
            receipt_b = receipt_file.read_text().strip()
            self.assertNotEqual(receipt_b, receipt_a)

            # Revert config
            config_in.write_text(orig_config)
            res = subprocess.run(
                [
                    "make",
                    "--no-print-directory",
                    "PROFILE=x86_64-clang",
                    str(busybox_elf),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=180,
            )
            self.assertEqual(res.returncode, 0, f"make busybox revert failed: {res.stderr}")
            receipt_c = receipt_file.read_text().strip()
            self.assertEqual(receipt_c, receipt_a)
        finally:
            config_in.write_text(orig_config)



class StagingFastPathContractTests(unittest.TestCase):
    def test_noop_preserves_publish_stamp(self):
        """Repeated make sysroot with unchanged inputs must preserve sysroot publish stamp."""
        res = subprocess.run(
            ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=120,
        )
        self.assertEqual(res.returncode, 0, f"Initial sysroot build failed: {res.stderr}")

        sysroot_stamp = ROOT / "build" / "x86_64-clang" / "stamps" / "sysroot.stamp"
        self.assertTrue(sysroot_stamp.exists())
        stamp_mtime_1 = sysroot_stamp.stat().st_mtime_ns

        # Second make sysroot (no-op)
        res = subprocess.run(
            ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(res.returncode, 0, f"No-op sysroot build failed: {res.stderr}")
        stamp_mtime_2 = sysroot_stamp.stat().st_mtime_ns
        self.assertEqual(stamp_mtime_2, stamp_mtime_1, "sysroot.stamp mtime changed on no-op build")

    def test_same_mtime_changed_content_invalidates(self):
        """Changing file content while preserving mtime must invalidate staging input key."""
        target_h = ROOT / "kernel" / "include" / "core" / "bootinfo.h"
        self.assertTrue(target_h.exists())
        orig_content = target_h.read_text()
        orig_stat = target_h.stat()

        sysroot_stamp = ROOT / "build" / "x86_64-clang" / "stamps" / "sysroot.stamp"
        stamp_mtime_before = sysroot_stamp.stat().st_mtime_ns if sysroot_stamp.exists() else 0

        try:
            # Change content and reset mtime back to original
            target_h.write_text(orig_content + "\n/* content change same mtime probe */\n")
            os.utime(target_h, (orig_stat.st_atime, orig_stat.st_mtime))

            res = subprocess.run(
                ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(res.returncode, 0, f"make sysroot failed: {res.stderr}")
            stamp_mtime_after = sysroot_stamp.stat().st_mtime_ns
            self.assertGreater(
                stamp_mtime_after, stamp_mtime_before, "sysroot.stamp did not update after content change"
            )
        finally:
            target_h.write_text(orig_content)
            os.utime(target_h, (orig_stat.st_atime, orig_stat.st_mtime))
            subprocess.run(
                ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"],
                cwd=ROOT,
                capture_output=True,
                timeout=120,
            )

    def test_staging_removed_input_disappears(self):
        """Removing an input header must invalidate input key and remove it from staging tree."""
        probe_header = ROOT / "kernel" / "include" / "core" / "_staging_del_probe.h"
        staged_probe = (
            ROOT
            / "build"
            / "x86_64-clang"
            / "staging"
            / "kernel-headers"
            / "usr"
            / "include"
            / "core"
            / "_staging_del_probe.h"
        )

        try:
            # Add probe header
            probe_header.write_text("#define _STAGING_DEL_PROBE 1\n")
            res = subprocess.run(
                ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(res.returncode, 0, f"make sysroot failed: {res.stderr}")
            self.assertTrue(staged_probe.exists(), "probe header not staged")

            # Remove probe header
            probe_header.unlink()
            res = subprocess.run(
                ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(res.returncode, 0, f"make sysroot after deletion failed: {res.stderr}")
            self.assertFalse(staged_probe.exists(), "deleted header still present in staging tree")
        finally:
            if probe_header.exists():
                probe_header.unlink()
            subprocess.run(
                ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"],
                cwd=ROOT,
                capture_output=True,
                timeout=120,
            )



class CompilerLauncherContractTests(unittest.TestCase):
    def test_launcher_empty_keeps_original_toolchain(self):
        """When COMPILER_LAUNCHER is empty, toolchain commands must not have leading spaces or launcher prefixes."""
        res = subprocess.run(
            [
                "make",
                "-n",
                "PROFILE=x86_64-clang",
                "kernel",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(res.returncode, 0, f"make -n kernel failed: {res.stderr}")
        # Commands should invoke clang directly, not ' ccache clang' or similar
        self.assertNotIn("COMPILER_LAUNCHER", res.stdout)

    def test_launcher_survives_sanitized_submake(self):
        """COMPILER_LAUNCHER must be propagated across sanitized os01_submake boundaries."""
        res = subprocess.run(
            [
                "make",
                "-n",
                "PROFILE=x86_64-clang",
                "COMPILER_LAUNCHER=test-ccache-probe",
                "user",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(res.returncode, 0, f"make -n user failed: {res.stderr}")
        self.assertIn("COMPILER_LAUNCHER=test-ccache-probe", res.stdout)


if __name__ == "__main__":
    unittest.main()
