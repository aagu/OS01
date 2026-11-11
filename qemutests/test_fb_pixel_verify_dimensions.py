#!/usr/bin/env python3
"""qemutests/test_fb_pixel_verify_dimensions.py -- pixel acceptance geometry gate.

The host-visible pixel acceptance (qemutests/fb_pixel_verify.py) compares
the QEMU screendump against the expected pattern.  The comparison range is
decided by the PPM's OWN header, so a cropped screenshot silently shrinks
the acceptance area: a 1x1 crop of a 1440x900 display compared "successfully"
because the single pixel it did compare happened to be the expected one.

This module is the PERMANENT regression for that hole.  It drives the REAL
main() of the verifier -- the same code path that decides PASS/FAIL and the
process exit code -- with the QEMU/QMP transport replaced by a fake that
returns a screenshot of a chosen size.  Nothing but the QMP transport and
the serial-log wait are stubbed: read_ppm(), dump_and_compare(), compare()
and the ok-decision in main() are the production ones.

Cases:
  * a cropped (1x1) screenshot of a 1440x900 display  -> FAIL, exit != 0;
  * a cropped screenshot that also cannot cover the rect -> FAIL, exit != 0;
  * a full-size screenshot with the correct pixels     -> PASS, exit == 0
    (the positive control: the gate must not fail everything).
"""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
VERIFY = ROOT / "qemutests" / "fb_pixel_verify.py"

_spec = importlib.util.spec_from_file_location("fb_pixel_verify", VERIFY)
verify = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(verify)

WIDTH = 1440
HEIGHT = 900
STRIDE = WIDTH * 4


def _ppm_bytes(width, height, pixel_fn):
    """Build a compact P6 PPM (no guest stride involved) of the given size."""
    out = bytearray(b"P6\n%d %d\n255\n" % (width, height))
    for y in range(height):
        for x in range(width):
            r, g, b = pixel_fn(x, y)
            out += bytes((r, g, b))
    return bytes(out)


class FakeProc:
    def terminate(self):
        pass

    def wait(self, **kwargs):
        return 0


class FakeQmp:
    """Writes a screenshot of the configured size; nothing else changes."""

    def __init__(self, path, size=(WIDTH, HEIGHT), content="correct"):
        self.path = path
        self.size = size
        self.content = content
        self.dumps = []

    def execute(self, cmd, **kwargs):
        if cmd == "screendump":
            filename = kwargs["filename"]
            tag = os.path.basename(filename)
            if "rect" in tag:
                fn = verify.expected_after_rect
            else:
                fn = verify.expected_full
            width, height = self.size
            if self.content == "correct":
                data = _ppm_bytes(width, height, fn)
            elif self.content == "old":
                data = _ppm_bytes(width, height,
                                  lambda x, y: verify.pattern_pixel(
                                      verify.FRAME_OLD, x, y))
            else:
                data = _ppm_bytes(width, height, lambda x, y: (0, 0, 0))
            with open(filename, "wb") as fh:
                fh.write(data)
            self.dumps.append(filename)
            return {"return": {}}
        return {"return": {}}

    def close(self):
        pass


class DimensionGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="fb-pixel-dim-")
        self._argv = sys.argv
        self._popen = verify.subprocess.Popen
        self._wait_for = verify.wait_for
        self._qmp = verify.Qmp
        verify.subprocess.Popen = lambda *a, **kw: FakeProc()

    def tearDown(self):
        sys.argv = self._argv
        verify.subprocess.Popen = self._popen
        verify.wait_for = self._wait_for
        verify.Qmp = self._qmp

    def _run(self, size, content="correct"):
        """Run the real main() with a fake QMP of the given screenshot size."""
        out = os.path.join(self.tmp, "out-%d-%d-%s" % (size[0], size[1], content))
        os.makedirs(out, exist_ok=True)

        def fake_wait_for(log_path, needle, timeout):
            if needle == "FULL_DONE":
                return ("FULL_DONE frame=2 width=%d height=%d stride=%d "
                        "format=0" % (WIDTH, HEIGHT, STRIDE))
            return ("RECT_DONE frame=3 x=%d y=%d w=%d h=%d width=%d height=%d "
                    "stride=%d" % (verify.RECT_X, verify.RECT_Y,
                                   verify.RECT_W, verify.RECT_H,
                                   WIDTH, HEIGHT, STRIDE))

        verify.wait_for = fake_wait_for
        verify.Qmp = lambda path, *a, **kw: FakeQmp(path, size=size,
                                                     content=content)
        sys.argv = ["fb_pixel_verify.py", "--disk", "unused", "--ovmf", "unused",
                    "--accel", "kvm", "--out", out]
        rc = verify.main()
        results = sorted(pathlib.Path(out).glob("result-*.json"))
        self.assertTrue(results, "no result JSON was written")
        data = json.loads(results[-1].read_text())
        return rc, data

    # ── the negative case: a cropped screenshot must FAIL ──
    def test_cropped_screenshot_fails_with_nonzero_exit(self):
        rc, data = self._run((1, 1))
        self.assertNotEqual(0, rc, "a 1x1 crop of a %dx%d display must not "
                                  "exit 0" % (WIDTH, HEIGHT))
        self.assertEqual("FAIL", data.get("verdict"))
        self.assertFalse(data.get("ok"))
        self.assertEqual([WIDTH, HEIGHT],
                         data["full_dimensions"]["expected"])
        self.assertEqual([1, 1], data["full_dimensions"]["actual"])
        self.assertFalse(data["full_dimensions"]["ok"])
        self.assertEqual([WIDTH, HEIGHT],
                         data["rect_dimensions"]["expected"])
        self.assertEqual([1, 1], data["rect_dimensions"]["actual"])
        self.assertFalse(data["rect_dimensions"]["ok"])
        self.assertTrue(data["fail_reasons"])

    def test_smaller_but_valid_sized_screenshot_fails(self):
        """A crop that is still a sane picture must fail on size alone."""
        rc, data = self._run((640, 480))
        self.assertNotEqual(0, rc)
        self.assertEqual("FAIL", data.get("verdict"))
        self.assertEqual([640, 480], data["full_dimensions"]["actual"])
        self.assertEqual([640, 480], data["rect_dimensions"]["actual"])

    # ── the positive control: the gate must not fail everything ──
    def test_full_size_screenshot_passes(self):
        rc, data = self._run((WIDTH, HEIGHT))
        self.assertEqual(0, rc)
        self.assertEqual("PASS", data.get("verdict"))
        self.assertTrue(data["full_dimensions"]["ok"])
        self.assertTrue(data["rect_dimensions"]["ok"])
        self.assertTrue(data["rect_geometry"]["ok"])
        self.assertEqual(0, data["full"]["mismatches"])
        self.assertEqual(0, data["rect"]["mismatches"])
        self.assertEqual(WIDTH * HEIGHT, data["full"]["pixels"])

    # ── the pixel comparison itself must still be able to fail ──
    def test_full_size_but_wrong_pixels_fails(self):
        rc, data = self._run((WIDTH, HEIGHT), content="old")
        self.assertNotEqual(0, rc)
        self.assertEqual("FAIL", data.get("verdict"))
        self.assertGreater(data["full"]["mismatches"], 0)
        self.assertEqual("the whole screen still shows the OLD frame",
                         data["full"]["kind"])


if __name__ == "__main__":
    unittest.main()
