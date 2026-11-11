#!/usr/bin/env python3
"""qemutests/fb_pixel_verify.py — host-visible framebuffer pixel acceptance.

Boots the OS01 guest with the framebuffer pixel-acceptance program as the
only display owner, waits for its DONE markers on the serial log and
compares a QMP screendump of the settled frame against the pixel pattern
recomputed HERE (independently of the guest program).

What it proves:
  * the full present put the expected pixels on the screen (every pixel is
    compared, not a checksum);
  * the rect present changed exactly the requested rectangle and nothing
    else.

What it does NOT prove:
  * that the display never showed a half-updated frame while the guest was
    still writing (a screendump of a settled frame says nothing about the
    sampling history).

Usage:
  qemutests/fb_pixel_verify.py --disk <disk.img> [--accel kvm|tcg]
                               [--out <dir>] [--timeout 180]
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import time

# ── The pixel contract (must match user/fb_pixel_test/fb_pixel_test.c) ──
FRAME_OLD = 1
FRAME_FULL = 2
FRAME_RECT = 3
RECT_X, RECT_Y, RECT_W, RECT_H = 17, 23, 127, 61


def pattern_pixel(frame, x, y):
    chk = ((x >> 4) ^ (y >> 4)) & 1
    r = (x * 7 + frame * 13 + chk * 97) & 0xFF
    g = (y * 11 + frame * 29 + chk * 53) & 0xFF
    b = ((x ^ y) * 3 + frame * 41) & 0xFF
    return (r, g, b)


def expected_full(x, y):
    return pattern_pixel(FRAME_FULL, x, y)


def expected_after_rect(x, y):
    if RECT_X <= x < RECT_X + RECT_W and RECT_Y <= y < RECT_Y + RECT_H:
        return pattern_pixel(FRAME_RECT, x, y)
    return pattern_pixel(FRAME_FULL, x, y)


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 PPM: %r" % data[:16])
    # Header: P6 <w> <h> <maxval>, whitespace separated, '#' comments allowed.
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx:idx + 1] not in (b"\n", b"\r"):
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    idx += 1  # single whitespace after maxval
    w, h, maxval = fields
    if maxval != 255:
        raise ValueError("unexpected maxval %d" % maxval)
    body = data[idx:]
    if len(body) < w * h * 3:
        raise ValueError("truncated PPM: %d bytes for %dx%d" % (len(body), w, h))
    return w, h, body


def dump_and_compare(qmp, outdir, tag, stamp, expected_fn, tries=12, delay=0.5):
    """Take screendumps until the picture matches, but at most `tries`
    times.  QEMU refreshes the display surface periodically, so the first
    dump after a present can still show the previous content; the retry
    is BOUNDED and the delay is reported, so a permanently wrong picture
    is never hidden by an endless retry."""
    attempts = []
    first_ppm = None
    w = h = 0
    body = b""
    res = None
    ppm = None
    for i in range(tries):
        # Every attempt gets its OWN file: with a shared name the later
        # attempts would overwrite the first one and "first_ppm" would
        # point at a different picture than the one it was taken from.
        ppm = os.path.join(outdir, "%s-%s-attempt%d.ppm" % (tag, stamp, i))
        resp = qmp.execute("screendump", filename=ppm, format="ppm")
        if "error" in resp:
            return {"error": "screendump failed: %r" % resp, "attempts": attempts}
        if first_ppm is None:
            first_ppm = ppm
        w, h, body = read_ppm(ppm)
        res = compare(w, h, body, expected_fn)
        res["attempt"] = i
        res["ppm"] = ppm
        attempts.append({"attempt": i, "ppm": ppm, "width": w, "height": h,
                         "pixels": res["pixels"], "mismatches": res["mismatches"],
                         "kind": res["kind"]})
        if res["mismatches"] == 0:
            res["first_ppm"] = first_ppm
            res["tries_used"] = i + 1
            res["attempts"] = attempts
            return res
        time.sleep(delay)
    res["attempt"] = tries - 1
    res["ppm"] = ppm
    res["first_ppm"] = first_ppm
    res["tries_used"] = tries
    res["attempts"] = attempts
    return res


def compare(w, h, body, expected_fn):
    """Compare every pixel; return a classification of the mismatch."""
    total = w * h
    mismatches = 0
    old_matches = 0
    first_bad = None
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            off = row + x * 3
            got = (body[off], body[off + 1], body[off + 2])
            exp = expected_fn(x, y)
            if got == exp:
                continue
            mismatches += 1
            if first_bad is None:
                first_bad = {"x": x, "y": y, "got": list(got), "expected": list(exp)}
            if got == pattern_pixel(FRAME_OLD, x, y):
                old_matches += 1
    kind = "match"
    # "the whole screen still shows the OLD frame" has to be checked
    # FIRST: when every pixel is the old frame, every pixel mismatches
    # as well, so the "mismatches == total" test below would swallow
    # this classification and the report would say "nothing matches".
    if old_matches == total:
        kind = "the whole screen still shows the OLD frame"
    elif mismatches == total:
        kind = "nothing matches the expected frame"
    elif mismatches:
        kind = "partial mismatch"
    return {
        "width": w,
        "height": h,
        "pixels": total,
        "mismatches": mismatches,
        "old_frame_pixels": old_matches,
        "kind": kind,
        "first_mismatch": first_bad,
    }


class Qmp:
    def __init__(self, path, timeout=20):
        deadline = time.time() + timeout
        self.sock = None
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                self.sock = s
                break
            except OSError:
                time.sleep(0.2)
        if self.sock is None:
            raise RuntimeError("cannot connect to QMP socket %s" % path)
        self.buf = b""
        self._read_json()          # greeting

    def _read_json(self):
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = self.buf[:nl]
                self.buf = self.buf[nl + 1:]
                if line.strip():
                    return json.loads(line.decode())
                continue
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("QMP closed")
            self.buf += chunk

    def execute(self, cmd, **args):
        req = {"execute": cmd}
        if args:
            req["arguments"] = args
        self.sock.sendall((json.dumps(req) + "\n").encode())
        while True:
            msg = self._read_json()
            if "event" in msg:
                continue          # async event
            return msg

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def wait_for(log_path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(log_path, "r", errors="replace") as fh:
                data = fh.read()
        except OSError:
            data = ""
        if needle in data:
            for line in data.splitlines():
                if needle in line:
                    return line
        time.sleep(0.2)
    return None


def parse_kv(line):
    out = {}
    for part in line.split():
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disk", required=True)
    ap.add_argument("--ovmf", required=True)
    ap.add_argument("--accel", default="kvm", choices=["kvm", "tcg"])
    ap.add_argument("--out", default="/tmp/fb-pixel")
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--smp", default="2")
    ap.add_argument("--mem", default="512M")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    serial = os.path.join(args.out, "serial-%s.log" % stamp)
    qmp_sock = os.path.join(args.out, "qmp-%s.sock" % stamp)
    result_path = os.path.join(args.out, "result-%s.json" % stamp)

    cmd = [
        "qemu-system-x86_64", "-M", "q35", "-smp", args.smp,
        "-drive", "if=pflash,format=raw,readonly=on,file=%s" % args.ovmf,
    ]
    if args.accel == "kvm":
        cmd += ["-accel", "kvm"]
    cmd += [
        "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
        "-drive", "file=%s,format=raw,if=none,id=disk" % args.disk,
        "-device", "ahci,id=ahci", "-device", "ide-hd,drive=disk,bus=ahci.0",
        "-device", "qemu-xhci,id=xhci", "-device", "usb-tablet,bus=xhci.0",
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-pci,rng=rng0",
        "-m", args.mem, "-display", "none",
        "-serial", "file:%s" % serial,
        "-no-reboot",
        "-qmp", "unix:%s,server=on,wait=off" % qmp_sock,
    ]

    result = {"command": cmd, "accel": args.accel, "serial_log": serial}
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    qmp = None
    try:
        line = wait_for(serial, "FULL_DONE", args.timeout)
        if line is None:
            result["error"] = "no FULL_DONE marker within %ds" % args.timeout
            with open(serial, errors="replace") as fh:
                result["serial_tail"] = fh.read()[-4000:]
            return finish(result, result_path)

        kv = parse_kv(line)
        width = int(kv["width"])
        height = int(kv["height"])
        result["full_marker"] = line.strip()
        result["width"] = width
        result["height"] = height
        if width <= 0 or height <= 0:
            result["error"] = ("guest reported a non-positive display size "
                               "%dx%d" % (width, height))
            return finish(result, result_path)

        qmp = Qmp(qmp_sock)
        qmp.execute("qmp_capabilities")

        full = dump_and_compare(qmp, args.out, "full", stamp, expected_full)
        if "error" in full:
            result["error"] = full["error"]
            return finish(result, result_path)
        result["full"] = full
        result["ppm_size"] = [full.get("width"), full.get("height")]

        # ── the screendump must cover the whole display ──
        # The comparison range is decided by the PPM's own header, so a
        # cropped picture would silently shrink the acceptance area.
        # The screenshot therefore has to match the size the guest
        # reported, and both the expected and the actual size are kept
        # in the result.
        result["full_dimensions"] = {
            "expected": [width, height],
            "actual": [full.get("width"), full.get("height")],
        }
        result["full_dimensions"]["ok"] = (
            result["full_dimensions"]["actual"] ==
            result["full_dimensions"]["expected"])

        # ── the rect step ──
        line = wait_for(serial, "RECT_DONE", args.timeout)
        if line is None:
            result["error"] = "no RECT_DONE marker within %ds" % args.timeout
            return finish(result, result_path)
        result["rect_marker"] = line.strip()

        rkv = parse_kv(line)
        rect_mode_w = int(rkv["width"])
        rect_mode_h = int(rkv["height"])
        rect_x = int(rkv["x"])
        rect_y = int(rkv["y"])
        rect_w = int(rkv["w"])
        rect_h = int(rkv["h"])

        geom = {
            "reported": {"x": rect_x, "y": rect_y, "w": rect_w, "h": rect_h},
            "expected": {"x": RECT_X, "y": RECT_Y, "w": RECT_W, "h": RECT_H},
            "same_display_mode": (rect_mode_w == width and rect_mode_h == height),
            "inside_display": (rect_w > 0 and rect_h > 0 and rect_x >= 0 and
                               rect_y >= 0 and rect_x + rect_w <= width and
                               rect_y + rect_h <= height),
        }
        geom["matches_contract"] = (geom["reported"] == geom["expected"])
        geom["ok"] = (geom["same_display_mode"] and geom["inside_display"] and
                      geom["matches_contract"])
        result["rect_geometry"] = geom

        rect = dump_and_compare(qmp, args.out, "rect", stamp, expected_after_rect)
        if "error" in rect:
            result["error"] = rect["error"]
            return finish(result, result_path)
        result["rect"] = rect

        result["rect_dimensions"] = {
            "expected": [width, height],
            "actual": [rect.get("width"), rect.get("height")],
        }
        result["rect_dimensions"]["ok"] = (
            result["rect_dimensions"]["actual"] ==
            result["rect_dimensions"]["expected"])

        ok = (result["full_dimensions"]["ok"] and
              result["rect_dimensions"]["ok"] and
              result["rect_geometry"]["ok"] and
              result["full"]["mismatches"] == 0 and
              result["rect"]["mismatches"] == 0)

        reasons = []
        if not result["full_dimensions"]["ok"]:
            reasons.append(
                "full screendump is %sx%s but the guest display is %dx%d" % (
                    result["full_dimensions"]["actual"][0],
                    result["full_dimensions"]["actual"][1], width, height))
        if not result["rect_dimensions"]["ok"]:
            reasons.append(
                "rect screendump is %sx%s but the guest display is %dx%d" % (
                    result["rect_dimensions"]["actual"][0],
                    result["rect_dimensions"]["actual"][1], width, height))
        if not result["rect_geometry"]["ok"]:
            reasons.append(
                "rect geometry %s is not usable (same display mode: %s, "
                "inside display: %s, matches contract: %s)" % (
                    geom["reported"], geom["same_display_mode"],
                    geom["inside_display"], geom["matches_contract"]))
        if result["full"]["mismatches"]:
            reasons.append("full frame: %d/%d pixels differ (%s)" % (
                result["full"]["mismatches"], result["full"]["pixels"],
                result["full"]["kind"]))
        if result["rect"]["mismatches"]:
            reasons.append("rect frame: %d/%d pixels differ (%s)" % (
                result["rect"]["mismatches"], result["rect"]["pixels"],
                result["rect"]["kind"]))
        result["fail_reasons"] = reasons

        result["verdict"] = "PASS" if ok else "FAIL"
        return finish(result, result_path, ok)
    finally:
        if qmp is not None:
            try:
                qmp.execute("quit")
            except Exception:
                pass
            qmp.close()
        time.sleep(0.5)
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def finish(result, path, ok=False):
    result["ok"] = bool(ok)
    with open(path, "w") as fh:
        json.dump(result, fh, indent=2)
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ("command", "serial_tail")}, indent=2))
    print("RESULT: %s  (%s)" % (result.get("verdict", "ERROR"), path))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
