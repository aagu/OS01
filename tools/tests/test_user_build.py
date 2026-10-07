#!/usr/bin/env python3
"""Exercise user discovery/linking with real make, compiler and linker."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class UserBuildTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.user = self.root / 'user'
        self.user.mkdir()
        for src in (ROOT / 'user').glob('*'):
            if src.name == 'Makefile' or src.suffix == '.mk':
                shutil.copy(src, self.user / src.name)
        (self.user / 'crt0.S').write_text('.global _start\n_start: jmp _start\n')
        (self.user / 'sigreturn_trampoline.S').write_text('.text\n')
        (self.user / 'linker.ld').write_text('ENTRY(_start)\nSECTIONS { . = 0x400000; .text : { *(.text*) } .data : { *(.data*) } .bss : { *(.bss*) } }\n')
        self.lib = self.root / 'sysroot/usr/lib'
        self.lib.mkdir(parents=True)
        subprocess.run(['ar', 'rcs', str(self.lib / 'libc.a')], check=True)
        self.profile = self.root / 'profile.mk'
        self.profile.write_text(f'OS01_ROOT := {self.root}\nUSER_BUILD_DIR := {self.root}/out\nEFFECTIVE_CC := clang\nTARGET_LD := ld.lld\nOBJ_CPY := true\n')
        (self.user / 'hello.c').write_text('int main(void) { return 0; }\n')
        app = self.user / 'demo'
        app.mkdir()
        (app / 'app.mk').write_text('APP_SOURCES := main.c helper.c\nAPP_CFLAGS := -DAPP_VALUE=7\n')
        (app / 'value.h').write_text('#define VALUE 7\n')
        (app / 'main.c').write_text('int helper(void); int main(void) { return helper(); }\n')
        (app / 'helper.c').write_text('#include "value.h"\n#if APP_VALUE != 7\n#error missing app flags\n#endif\nint helper(void) { return VALUE; }\n')

    def make(self, *args, ok=True):
        result = subprocess.run(['make', '--no-print-directory', '-C', str(self.user),
                                 f'OS01_PROFILE_FILE={self.profile}',
                                 f'SYSROOT_GENERATION_DIR={self.root}/sysroot',
                                 'CFLAGS=-ffreestanding -fno-pie', *args],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout)
        return result

    def test_multifile_and_flat_discovery_incremental_build(self):
        self.make('-j4', 'all')
        self.assertTrue((self.root / 'out/hello.elf').is_file())
        self.assertTrue((self.root / 'out/demo.elf').is_file(), 'app.mk program was not discovered')
        self.assertNotIn('clang ', self.make('all').stdout)
        header = self.user / 'demo/value.h'
        header.write_text('#define VALUE 8\n')
        stamp = (self.root / 'out/obj/demo/helper.o').stat().st_mtime + 2
        os.utime(header, (stamp, stamp))
        output = self.make('all').stdout
        self.assertIn('helper.c', output)
        self.assertNotIn('-c hello.c', output)
        (self.user / 'new.c').write_text('int main(void) { return 1; }\n')
        self.make('all')
        self.assertTrue((self.root / 'out/new.elf').is_file())

    def test_root_publication_and_packaging_discover_new_programs(self):
        wrapper = self.root / 'root.mk'
        wrapper.write_text(f'OS01_ROOT := {self.root}\nPROFILE_CAPABILITIES := userland\nUSER_ARTIFACT_DIR := {self.root}/artifacts\ninclude {ROOT}/mk/components/user.mk\ninclude {ROOT}/config/rootfs.mk\n.PHONY: show\nshow:\n\t@echo programs=$(USER_PROGRAMS)\n\t@echo files=$(ROOTFS_FILES)\n')
        result = subprocess.run(['make', '-f', str(wrapper), 'show'], cwd=self.root,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('programs=demo hello', result.stdout)
        self.assertIn(f'/bin/demo={self.root}/artifacts/demo.elf:0755', result.stdout)
        self.assertIn(f'/bin/hello={self.root}/artifacts/hello.elf:0755', result.stdout)

    def test_app_libraries_assembly_resources_and_flags_do_not_leak(self):
        app = self.user / 'demo'
        (app / 'main.c').write_text('int helper(void); int extra(void); int resource(void); int main(void) { return helper() + extra() + resource(); }\n')
        (self.root / 'extra.c').write_text('int extra(void) { return 1; }\n')
        subprocess.run(['clang', '-c', str(self.root / 'extra.c'), '-o', str(self.root / 'extra.o')], check=True)
        subprocess.run(['ar', 'rcs', str(self.lib / 'libextra.a'), str(self.root / 'extra.o')], check=True)
        (app / 'resource.S').write_text('.text\n.global resource\nresource: mov $2, %eax; ret\n')
        with (app / 'app.mk').open('a') as manifest:
            manifest.write('APP_LDLIBS := -lextra\nifeq ($(USER_BUILD_CONTEXT),1)\nAPP_EXTRA_OBJS := $(OBJ_DIR)/demo/resource.o\n$(OBJ_DIR)/demo/resource.o: demo/resource.S\n\t@mkdir -p $(dir $@)\n\t$(EFFECTIVE_CC) $(CFLAGS) -c $< -o $@\nendif\n')
        plain = self.user / 'plain'
        plain.mkdir()
        (plain / 'app.mk').write_text('APP_SOURCES := main.c\n')
        (plain / 'main.c').write_text('#ifdef APP_VALUE\n#error flags leaked\n#endif\nint main(void) { return 0; }\n')
        self.make('-j4')
        self.assertTrue((self.root / 'out/plain.elf').is_file())
        self.assertTrue((self.root / 'out/demo.elf').is_file())

    def test_empty_manifest_is_rejected(self):
        (self.user / 'demo/app.mk').write_text('APP_LDLIBS := -lc\n')
        result = self.make('all', ok=False)
        self.assertNotEqual(result.returncode, 0, 'empty application accepted')
        self.assertIn('APP_SOURCES', result.stdout)

    def test_native_program_overrides_busybox_applet_except_sh(self):
        for name in ('cat', 'sh'):
            (self.user / f'{name}.c').write_text('int main(void) { return 0; }\n')
        wrapper = self.root / 'root.mk'
        wrapper.write_text(f'OS01_ROOT := {self.root}\nUSER_ARTIFACT_DIR := {self.root}/artifacts\ninclude {self.user}/apps.mk\ninclude {ROOT}/config/rootfs.mk\nshow:\n\t@echo files=$(ROOTFS_FILES)\n\t@echo links=$(ROOTFS_SYMLINKS)\n')
        result = subprocess.run(['make', '-f', str(wrapper), 'show'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(f'/bin/cat={self.root}/artifacts/cat.elf:0755', result.stdout)
        self.assertNotIn('/bin/cat=busybox', result.stdout)
        self.assertIn('/bin/sh=busybox', result.stdout)
        self.assertNotIn(f'/bin/sh={self.root}/artifacts/sh.elf', result.stdout)

    def test_incremental_rootfs_removes_deleted_programs(self):
        config = self.root / 'config'
        config.mkdir()
        shutil.copy(ROOT / 'config/rootfs.mk', config / 'rootfs.mk')
        artifacts = self.root / 'artifacts'
        artifacts.mkdir()
        for name in ('hello', 'demo', 'busybox'):
            (artifacts / f'{name}.elf').write_bytes(b'fixture')
        for name in ('kernel.bin', 'inittab'):
            (self.root / name).write_bytes(b'fixture')
        wrapper = self.root / 'root.mk'
        wrapper.write_text(f'OS01_ROOT := {self.root}\nPROFILE_CAPABILITIES := rootfs\nPROFILE := fixture\nBUILD_DIR := {self.root}/build\nUSER_ARTIFACT_DIR := {artifacts}\nKERNEL_ARTIFACT := {self.root}/kernel.bin\nINITTAB_FILE := {self.root}/inittab\ninclude {self.user}/apps.mk\nUSER_ARTIFACTS := $(addprefix $(USER_ARTIFACT_DIR)/,$(addsuffix .elf,$(USER_PROGRAMS)))\ninclude {ROOT}/mk/components/image.mk\n.PHONY: FORCE\nFORCE:\n')
        manifest = self.root / 'build/image/rootfs.manifest'
        def stage():
            result = subprocess.run(['make', '-f', str(wrapper), str(manifest)],
                                    cwd=self.root, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
        stage()
        self.assertIn('/bin/hello', manifest.read_text())
        self.assertIn('/bin/demo', manifest.read_text())
        previous_mtime = manifest.stat().st_mtime_ns
        stage()
        self.assertEqual(manifest.stat().st_mtime_ns, previous_mtime, 'unchanged inputs restaged rootfs')
        (self.user / 'hello.c').unlink()
        (self.user / 'demo/app.mk').unlink()
        stage()
        self.assertNotIn('/bin/hello', manifest.read_text())
        self.assertNotIn('/bin/demo', manifest.read_text())
        self.assertFalse((self.root / 'build/image/rootfs.next/bin/demo').exists())

    def test_duplicate_program_name_is_rejected(self):
        (self.user / 'demo.c').write_text('int main(void) { return 0; }\n')
        result = self.make('all', ok=False)
        self.assertNotEqual(result.returncode, 0, 'ambiguous program name accepted')
        self.assertIn('duplicate', result.stdout.lower())


if __name__ == '__main__':
    unittest.main()
