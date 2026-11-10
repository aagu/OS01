#!/usr/bin/env python3
"""D03 — the Doom engine build contract.

The contract this module pins (D03 plan Task 1 / D03 spec):

  * the upstream is PINNED: the manifest revision equals the submodule
    HEAD, and a wrong revision is a hard error with a locating message;
  * the engine is EXACTLY 80 explicit translation units exported from
    Makefile.soso (doomgeneric_soso.c removed) with no platform entry
    point — no wildcards, so an upstream change cannot silently add or
    drop a unit;
  * the engine is built only for a userland profile: aarch64 has no
    userland capability and must never pull the engine up;
  * EVERY build input — the upstream content, the upstream commit, the
    unit list, the patch contents and their order, the toolchain and all
    compile macros — is part of the rebuild key;
  * the production objects carry no system()/fscanf() dependency: the
    OS01 patch removes the zenity path instead of stubbing it out.

The archive-level assertions build the REAL archive through the REAL
user sub-make, in an isolated BUILD_DIR, against the profile's leased
sysroot generation.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE = os.environ.get("PROFILE", "x86_64-clang")
PROFILE_FILE = ROOT / "mk" / "profiles" / f"{PROFILE}.mk"

MANIFEST = ROOT / "thirdpart" / "doomgeneric.manifest"
SOURCES_MK = ROOT / "thirdpart" / "doomgeneric-sources.mk"
UPSTREAM = ROOT / "thirdpart" / "doomgeneric"
PATCH_DIR = ROOT / "thirdpart" / "patches" / "doomgeneric"
ENGINE_MK = ROOT / "user" / "doom" / "engine.mk"

# The upstream revision the D03 spec pins.
PINNED_REVISION = "e257a09527062bfb986b6e5e0780839b6cbd5b71"
EXPECTED_UNIT_COUNT = 80

# The platform entry points the fixed list must never contain.
FORBIDDEN_UNIT_MARKERS = (
    "sdl", "xlib", "allegro", "win", "emscripten", "soso", "linuxvt", "djgpp",
)


def read(path: Path) -> str:
    return path.read_text()


def manifest_revision() -> str:
    m = re.search(r"DOOMGENERIC_REVISION\s*:=\s*([0-9a-f]{40})", read(MANIFEST))
    return m.group(1) if m else ""


def unit_list() -> list[str]:
    text = read(SOURCES_MK)
    body = text.split("DOOMGENERIC_UNITS :=", 1)[1]
    body = body.split("\n\n", 1)[0]
    units = []
    for line in body.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        units.append(line.replace("\\", "").strip())
    return [u for u in units if u]


def run_user_make(*args: str, timeout: int = 1800) -> subprocess.CompletedProcess:
    cmd = [
        "make", "--no-print-directory", "-C", "user",
        f"OS01_PROFILE_FILE={PROFILE_FILE}",
        f"PROFILE={PROFILE}",
        *args,
    ]
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout)


def run_make(*args: str, timeout: int = 1800) -> subprocess.CompletedProcess:
    cmd = ["make", "--no-print-directory", f"PROFILE={PROFILE}", *args]
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout)


def engine_dir(build_dir: Path) -> Path:
    # USER_BUILD_DIR is $(BUILD_DIR)/user; the archive lives under it.
    return build_dir / "user" / "obj" / "doom-engine"


def stamp_path(build_dir: Path) -> Path:
    return engine_dir(build_dir) / ".doom-source.stamp"


def archive_path(build_dir: Path) -> Path:
    return engine_dir(build_dir) / "libdoom-engine.a"


def archive_members(archive: Path) -> list[str]:
    out = subprocess.run(["llvm-ar", "t", str(archive)],
                         capture_output=True, text=True, timeout=120)
    return out.stdout.split()


def undefined_symbols(archive: Path) -> set[str]:
    out = subprocess.run(["llvm-nm", "--undefined-only", str(archive)],
                         capture_output=True, text=True, timeout=120)
    syms = set()
    for line in out.stdout.splitlines():
        parts = line.split()
        if parts:
            syms.add(parts[-1])
    return syms


def member_undefined(archive: Path, member: str) -> set[str]:
    """The undefined symbols of ONE archive member (llvm-nm on a member)."""
    out = subprocess.run(["llvm-nm", "--undefined-only", "--print-file-name",
                          str(archive)],
                         capture_output=True, text=True, timeout=120)
    syms = set()
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        name = parts[0]
        if name.endswith(f"({member})"):
            syms.add(parts[-1])
    return syms


class DoomBuildContract(unittest.TestCase):
    """The D03 build contract."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="doom-build-contract-")
        cls.work = Path(cls._tmp.name)
        cls.build_dir = cls.work / "build"
        cls.sysroot = ROOT / "build" / PROFILE / "sysroot"
        if not (cls.sysroot / "usr" / "lib" / "libc.a").is_file():
            # The engine compiles against the leased immutable generation:
            # build it once when the tree does not have it yet.
            r = run_make("sysroot")
            if r.returncode != 0:
                raise unittest.SkipTest(
                    "the profile sysroot could not be built for the archive "
                    "contract:\n" + r.stdout[-2000:] + r.stderr[-2000:])
        cls.archive = None

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    # ── helpers ────────────────────────────────────────────────
    def build_archive(self, build_dir: Path, *extra: str) -> subprocess.CompletedProcess:
        return run_user_make(
            f"BUILD_DIR={build_dir}",
            f"SYSROOT_DIR={self.sysroot}",
            *extra,
            "doom-engine-archive",
        )

    def build_stamp(self, build_dir: Path, *extra: str) -> subprocess.CompletedProcess:
        return run_user_make(
            f"BUILD_DIR={build_dir}",
            f"SYSROOT_DIR={self.sysroot}",
            *extra,
            str(stamp_path(build_dir)),
        )

    def ensure_archive(self) -> Path:
        if self.archive is None:
            r = self.build_archive(self.build_dir)
            if r.returncode != 0:
                self.fail("the engine archive did not build:\n"
                          + r.stdout[-4000:] + r.stderr[-4000:])
            self.archive = archive_path(self.build_dir)
        return self.archive

    # ── the contract ───────────────────────────────────────────
    def test_engine_source_inventory(self):
        """Exactly 80 explicit units, no platform entry point."""
        units = unit_list()
        self.assertEqual(len(units), EXPECTED_UNIT_COUNT,
                         f"the unit list must hold exactly {EXPECTED_UNIT_COUNT} "
                         f"units, got {len(units)}")
        self.assertEqual(len(set(units)), len(units),
                         "the unit list must not repeat a unit")
        for unit in units:
            self.assertTrue(unit.endswith(".c"), unit)
            for marker in FORBIDDEN_UNIT_MARKERS:
                self.assertNotIn(marker, unit.lower(),
                                 f"{unit} is a platform entry point")
        # The units the spec names explicitly.
        for required in ("dummy.c", "doomgeneric.c", "i_input.c", "i_video.c"):
            self.assertIn(required, units)
        self.assertNotIn("doomgeneric_soso.c", units)

        # The list is explicit: no wildcards, no shell discovery.
        text = read(SOURCES_MK)
        self.assertNotIn("$(shell", text)
        self.assertNotIn("$(wildcard", text)
        self.assertNotIn("find ", text)

        # Every unit exists in the pinned upstream.
        for unit in units:
            self.assertTrue((UPSTREAM / "doomgeneric" / unit).is_file(),
                            f"{unit} is missing from the upstream tree")

    def test_engine_manifest_pins_the_submodule(self):
        """The manifest pins the upstream revision."""
        self.assertEqual(manifest_revision(), PINNED_REVISION,
                         "the manifest must pin the D03 upstream revision")
        head = subprocess.run(["git", "-C", str(UPSTREAM), "rev-parse", "HEAD"],
                               capture_output=True, text=True)
        if head.returncode == 0:
            self.assertEqual(head.stdout.strip(), PINNED_REVISION,
                             "the submodule must be at the pinned revision")

    def test_engine_profile_gate(self):
        """Only a userland profile builds the engine."""
        text = read(ENGINE_MK)
        self.assertIn("$(filter userland,$(PROFILE_CAPABILITIES))", text,
                      "the engine rules must be gated on the userland capability")
        profile_text = read(PROFILE_FILE)
        caps = re.search(r"PROFILE_CAPABILITIES\s*:=\s*(.*)", profile_text)
        self.assertIsNotNone(caps)
        if PROFILE != "x86_64-clang":
            self.assertNotIn("userland", caps.group(1).split())

        # The aarch64 profile has no userland capability: the root user
        # component must not even exist for it, so the engine is never
        # pulled up.
        aarch = ROOT / "mk" / "profiles" / "aarch64-clang.mk"
        aarch_caps = re.search(r"PROFILE_CAPABILITIES\s*:=\s*(.*)", read(aarch))
        self.assertIsNotNone(aarch_caps)
        self.assertNotIn("userland", aarch_caps.group(1).split(),
                         "the aarch64 profile must not have the userland capability")
        r = subprocess.run(
            ["make", "--no-print-directory", "-n", "PROFILE=aarch64-clang", "user"],
            cwd=ROOT, capture_output=True, text=True, timeout=300)
        self.assertNotIn("doom", r.stdout.lower(),
                         "the aarch64 profile must not prepare or build the engine")

    def test_engine_bad_revision_is_rejected(self):
        """A wrong upstream revision is a hard error with a locating message."""
        bad_manifest = self.work / "bad.manifest"
        bad_manifest.write_text(
            "DOOMGENERIC_SOURCE   := thirdpart/doomgeneric\n"
            f"DOOMGENERIC_REVISION := {'0' * 40}\n"
        )
        r = self.build_stamp(self.work / "bad-build",
                             f"DOOM_MANIFEST={bad_manifest}")
        self.assertNotEqual(r.returncode, 0,
                            "a wrong upstream revision must fail the build")
        out = r.stdout + r.stderr
        self.assertIn("pins", out,
                      "the failure must name the revision conflict")
        self.assertIn("0" * 40, out,
                      "the failure must show the pinned revision")

    def test_engine_input_rebuild(self):
        """Every build input is part of the rebuild key."""
        base = self.work / "digest-build"
        r = self.build_stamp(base)
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])
        stamp = stamp_path(base)
        self.assertTrue(stamp.is_file(), "the engine source stamp must exist")
        first = stamp.read_text().strip()
        self.assertTrue(first, "the stamp must carry the input digest")

        # (a) a changed patch
        patch_copy = self.work / "patches"
        shutil.copytree(PATCH_DIR, patch_copy)
        target = patch_copy / "0001-os01-no-zenity.patch"
        target.write_text(target.read_text().replace(
            "// OS01: there is no GUI error box.",
            "// OS01: there is no GUI error box (changed input)."))
        r = self.build_stamp(base, f"DOOM_PATCH_DIR={patch_copy}")
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])
        after_patch = stamp.read_text().strip()
        self.assertNotEqual(after_patch, first,
                            "a changed patch must change the rebuild key")

        # (b) a changed unit list
        sources_copy = self.work / "doomgeneric-sources.mk"
        sources_copy.write_text(read(SOURCES_MK) + "\n# changed input\n")
        r = self.build_stamp(base, f"DOOM_SOURCES_MK={sources_copy}")
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])
        after_units = stamp.read_text().strip()
        self.assertNotEqual(after_units, after_patch,
                            "a changed unit list must change the rebuild key")

        # (c) changed compile macros
        r = self.build_stamp(base, "DOOM_ENGINE_DEFS=-DNORMALUNIX -DOS01_DOOM -DOS01_EXTRA_MACRO")
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])
        after_defs = stamp.read_text().strip()
        self.assertNotEqual(after_defs, after_units,
                            "changed compile macros must change the rebuild key")

        # (d) changed upstream source content
        upstream_copy = self.work / "upstream"
        shutil.copytree(UPSTREAM, upstream_copy, symlinks=True)
        gitfile = upstream_copy / ".git"
        if gitfile.is_file():
            text = gitfile.read_text().strip()
            if text.startswith("gitdir:"):
                rel = text.split(":", 1)[1].strip()
                gitfile.write_text(f"gitdir: {(UPSTREAM / rel).resolve()}\n")
        edited = upstream_copy / "doomgeneric" / "dummy.c"
        edited.write_text(edited.read_text() + "\n/* changed input */\n")
        r = self.build_stamp(base, f"DOOM_UPSTREAM={upstream_copy}")
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])
        after_source = stamp.read_text().strip()
        self.assertNotEqual(after_source, after_defs,
                            "changed upstream content must change the rebuild key")

        # The archive itself is rebuilt when the key moves.
        archive = self.ensure_archive()
        before_mtime = archive.stat().st_mtime_ns
        r = self.build_archive(self.build_dir, f"DOOM_PATCH_DIR={patch_copy}")
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])
        self.assertGreater(archive.stat().st_mtime_ns, before_mtime,
                           "a changed input must rebuild the archive")

    def test_engine_no_system_dependency(self):
        """The production objects carry no system()/fscanf() dependency."""
        archive = self.ensure_archive()
        members = archive_members(archive)
        self.assertEqual(len(members), EXPECTED_UNIT_COUNT,
                         f"the archive must hold {EXPECTED_UNIT_COUNT} objects, "
                         f"got {len(members)}")
        self.assertIn("i_system.o", members)

        syms = undefined_symbols(archive)
        for forbidden in ("system", "fscanf", "vfscanf", "scanf"):
            self.assertNotIn(forbidden, syms,
                             f"the archive must not depend on {forbidden}()")
        i_system = member_undefined(archive, "i_system.o")
        self.assertNotIn("system", i_system,
                         "the production i_system object must not reference system()")

        # The DG_* platform interface stays UNDEFINED on purpose: D04
        # provides it.  The archive is not a linkable program yet.
        dg = {s for s in syms if s.startswith("DG_")}
        self.assertTrue(dg, "the archive must still expose the DG_* platform interface")

    def test_unknown_doom_case_is_rejected(self):
        """An unknown DOOM_CASE fails with a non-zero status."""
        r = run_make("test-doom-host", "DOOM_CASE=bogus-case")
        self.assertNotEqual(r.returncode, 0,
                            "an unknown DOOM_CASE must fail")
        self.assertIn("unknown DOOM_CASE", r.stdout + r.stderr,
                      "the failure must name the unknown case")


if __name__ == "__main__":
    unittest.main()
