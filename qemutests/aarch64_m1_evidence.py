#!/usr/bin/env python3
"""Strict acceptance checks for AArch64 M1 runtime direct-map evidence."""

import argparse
import re
import sys

PAGE_2M = 2 * 1024 * 1024
WINDOW_END = 0x80000000

_HEX = r"(?:0x)?[0-9a-fA-F]+"
_ZONE = re.compile(rf"^M1 ZONE id=(\d+) start=({_HEX}) end=({_HEX}) frames=(\d+)$")
_COVERAGE = re.compile(rf"^M1 COVERAGE id=(\d+) start=({_HEX}) end=({_HEX})$")
_EDGE = re.compile(rf"^M1 EDGE PASS zone=(\d+) first=({_HEX}) last=({_HEX})$")
_BSP = re.compile(rf"^M1 BSP PASS root=({_HEX}) ranges=(\d+)$")
_AP = re.compile(rf"^M1 AP PASS cpu=(\d+) root=({_HEX}) probe=(none|{_HEX})$")
_PROBE = re.compile(rf"^M1 PROBE PASS pa=({_HEX})$")


def _lines(text: str, prefix: str) -> list[str]:
    return [line for line in text.splitlines() if line.startswith(prefix)]


def _records(text: str, prefix: str, pattern: re.Pattern[str],
             hex_fields: tuple[int, ...]) -> list[tuple[int, ...]] | None:
    lines = _lines(text, prefix)
    result: list[tuple[int, ...]] = []
    for line in lines:
        match = pattern.fullmatch(line)
        if not match:
            return None
        result.append(tuple(
            0 if value == "none" else
            int(value.removeprefix("0x"), 16) if index in hex_fields else int(value)
            for index, value in enumerate(match.groups())
        ))
    return result


def _intervals_match(zones: list[tuple[int, int, int, int]],
                     coverage: list[tuple[int, int, int]]) -> bool:
    zone_intervals = sorted((start, end) for _, start, end, _ in zones)
    coverage_intervals = sorted((start, end) for _, start, end in coverage)

    def merge(intervals: list[tuple[int, int]]) -> list[tuple[int, int]]:
        merged: list[list[int]] = []
        for start, end in intervals:
            if not merged or start > merged[-1][1]:
                merged.append([start, end])
            else:
                merged[-1][1] = max(merged[-1][1], end)
        return [(start, end) for start, end in merged]

    return merge(zone_intervals) == merge(coverage_intervals)


def _m1_evidence_ok(text: str, cpus: int, selftest: bool, ram_mib: int,
                    variant: str = "normal", *, bsp_only: bool = False) -> bool:
    if cpus < 1 or ram_mib < 1 or variant not in ("normal", "sparse"):
        return False
    text = text.replace("\r", "")
    forbidden = (r"\b(?:FATAL|PANIC|AP FAIL)\b|QEMU TIMEOUT" if bsp_only else
                 r"\b(?:FATAL|PANIC|DEGRADED|AP FAIL)\b|\b(?:QEMU )?TIMEOUT\b")
    if re.search(forbidden, text, re.I):
        return False

    zones = _records(text, "M1 ZONE", _ZONE, (1, 2))
    coverage = _records(text, "M1 COVERAGE", _COVERAGE, (1, 2))
    edges = _records(text, "M1 EDGE", _EDGE, (1, 2))
    bsps = _records(text, "M1 BSP", _BSP, (0,))
    aps = _records(text, "M1 AP", _AP, (1, 2))
    probes = _records(text, "M1 PROBE", _PROBE, (0,))
    if zones is None or coverage is None or edges is None or bsps is None or aps is None or probes is None:
        return False
    if not zones or not coverage or len(bsps) != 1:
        return False

    zone_ids = [record[0] for record in zones]
    coverage_ids = [record[0] for record in coverage]
    if (len(set(zone_ids)) != len(zone_ids) or len(set(coverage_ids)) != len(coverage_ids)
            or sorted(zone_ids) != list(range(len(zone_ids)))
            or sorted(coverage_ids) != list(range(len(coverage_ids)))):
        return False
    for _, start, end, frames in zones:
        if start >= end or start % PAGE_2M or end % PAGE_2M or frames != (end - start) // PAGE_2M:
            return False
    for _, start, end in coverage:
        if start >= end or start % PAGE_2M or end % PAGE_2M:
            return False
    zone_bounds = sorted((start, end) for _, start, end, _ in zones)
    coverage_bounds = sorted((start, end) for _, start, end in coverage)
    if any(left[1] > right[0] for left, right in zip(zone_bounds, zone_bounds[1:])):
        return False
    if any(left[1] > right[0] for left, right in zip(coverage_bounds, coverage_bounds[1:])):
        return False
    if not _intervals_match(zones, coverage):
        return False

    walk_lines = _lines(text, "M1 WALK")
    if selftest:
        walk_matches = [re.fullmatch(r"M1 WALK PASS frames=(\d+)", line) for line in walk_lines]
        if len(walk_matches) != 1 or walk_matches[0] is None:
            return False
        if int(walk_matches[0].group(1)) != sum(record[3] for record in zones):
            return False
    elif walk_lines:
        return False

    bsp_root, bsp_ranges = bsps[0]
    if bsp_root == 0 or bsp_root % 4096 or bsp_ranges != len(coverage):
        return False
    lines = text.splitlines()
    bsp_index = lines.index(next(line for line in lines if line.startswith("M1 BSP")))
    coverage_lines = _lines(text, "M1 COVERAGE")
    zone_lines = _lines(text, "M1 ZONE")
    if any(lines.index(line) >= bsp_index for line in zone_lines + coverage_lines):
        return False

    if selftest:
        if len(edges) != len(zones):
            return False
        by_zone = {zone_id: (start, end) for zone_id, start, end, _ in zones}
        edge_ids: list[int] = []
        for zone_id, first, last in edges:
            bounds = by_zone.get(zone_id)
            if bounds is None:
                return False
            start, end = bounds
            if first % PAGE_2M or last % PAGE_2M or not (start <= first < end and start <= last < end):
                return False
            edge_ids.append(zone_id)
        if len(set(edge_ids)) != len(edge_ids) or set(edge_ids) != set(zone_ids):
            return False
        for prefix, expected in (
            ("M1 SELFTEST", r"M1 SELFTEST PASS"),
            ("M1 PRUNE", r"M1 PRUNE PASS pa=0x00200000 before=valid after=fault"),
            ("M1 CLEANUP", r"M1 CLEANUP PASS slot=256"),
        ):
            markers = _lines(text, prefix)
            if markers != [expected]:
                return False
        walk_line = walk_lines[0]
        edge_indexes = [text.splitlines().index(line) for line in _lines(text, "M1 EDGE")]
        walk_index = text.splitlines().index(walk_line)
        prune_index = text.splitlines().index(_lines(text, "M1 PRUNE")[0])
        cleanup_index = text.splitlines().index(_lines(text, "M1 CLEANUP")[0])
        selftest_index = text.splitlines().index(_lines(text, "M1 SELFTEST")[0])
        if not (prune_index < cleanup_index
                and cleanup_index < min(edge_indexes)
                and max(edge_indexes) < walk_index < selftest_index):
            return False
        if not (selftest_index < min(lines.index(line) for line in zone_lines)
                and max(lines.index(line) for line in zone_lines + coverage_lines) < bsp_index):
            return False
        if len(probes) != 1:
            return False
        probe_pa = probes[0][0]
        if probe_pa % PAGE_2M or not any(start <= probe_pa < end for _, start, end, _ in zones):
            return False
        if ram_mib >= 2048 and probe_pa < WINDOW_END:
            return False
        probe_lines = _lines(text, "M1 PROBE")
        probe_index = text.splitlines().index(probe_lines[0])
        if probe_index <= bsp_index:
            return False
        if variant == "sparse":
            sparse_input = _lines(text, "M1 SPARSE INPUT")
            sparse_pass = _lines(text, "M1 SPARSE PASS")
            if sparse_input != ["M1 SPARSE INPUT start=0x50000000 end=0x50200000"]:
                return False
            if sparse_pass != ["M1 SPARSE PASS left=0x4fe00000 right=0x50200000 hole=absent"]:
                return False
            if text.splitlines().index(sparse_input[0]) >= bsp_index or text.splitlines().index(sparse_pass[0]) >= bsp_index:
                return False
            if text.splitlines().index(sparse_input[0]) >= text.splitlines().index(_lines(text, "UEFI-A64: RAM")[0]):
                return False
            if text.splitlines().index(sparse_pass[0]) >= selftest_index:
                return False
            if text.splitlines().index(sparse_pass[0]) <= walk_index:
                return False
            if any(start < 0x50200000 and end > 0x50000000 for _, start, end, _ in zones):
                return False
            if not any(start <= 0x4fe00000 < end for _, start, end, _ in zones):
                return False
            if not any(start <= 0x50200000 < end for _, start, end, _ in zones):
                return False
        elif variant != "normal":
            return False
    elif edges or probes or _lines(text, "M1 SELFTEST") or _lines(text, "M1 PRUNE") or _lines(text, "M1 CLEANUP"):
        return False

    if variant == "sparse" and not selftest:
        return False
    if variant != "sparse" and (_lines(text, "M1 SPARSE INPUT") or _lines(text, "M1 SPARSE PASS")):
        return False
    if bsp_only:
        return not aps
    expected_ap_ids = list(range(1, cpus))
    if len(aps) != len(expected_ap_ids):
        return False
    ap_ids = [record[0] for record in aps]
    if len(set(ap_ids)) != len(ap_ids) or set(ap_ids) != set(expected_ap_ids):
        return False
    for cpu, root, probe in aps:
        if root != bsp_root:
            return False
        if selftest:
            if probe != probes[0][0]:
                return False
        elif probe != 0:
            return False
    lines = text.splitlines()
    if any(lines.index(line) <= bsp_index for line in _lines(text, "M1 AP")):
        return False
    if selftest:
        probe_line = _lines(text, "M1 PROBE")[0]
        if any(lines.index(line) <= lines.index(probe_line) for line in _lines(text, "M1 AP")):
            return False

    # Keep the established SMP benchmark, timer, and GIC/IPI acceptance gates.
    try:
        from aarch64_uefi_smp import passed
    except ImportError:
        from qemutests.aarch64_uefi_smp import passed
    return passed(text, cpus=cpus, expect_selftest=False,
                  expect_gic=selftest, expect_clk=selftest)


def m1_evidence_ok(text: str, cpus: int, selftest: bool, ram_mib: int,
                   variant: str = "normal") -> bool:
    """Return true only for complete, correctly ordered M1 evidence."""
    try:
        return _m1_evidence_ok(text, cpus, selftest, ram_mib, variant)
    except (IndexError, StopIteration, ValueError):
        return False


def m1_failure_evidence_ok(text: str, variant: str, cpus: int,
                           stayed_alive: bool) -> bool:
    """Check the expected negative M1 signature after QEMU stays alive.

    The caller sets ``stayed_alive`` only after observing the signature and
    confirming that QEMU did not exit during a short grace period.
    """
    if not stayed_alive or cpus < 1:
        return False
    text = text.replace("\r", "")
    bsp = re.findall(rf"^M1 BSP PASS root=({_HEX}) ranges=(\d+)$", text, re.M)
    probe = re.findall(rf"^M1 PROBE PASS pa=({_HEX})$", text, re.M)
    ap_pass = re.findall(r"^M1 AP PASS cpu=(\d+)\b", text, re.M)
    ap_online = [int(cpu) for cpu in re.findall(
        r"^\[smp\] cpu=(\d+) online(?:\s.*)?$", text, re.M) if int(cpu) > 0]
    lines = text.splitlines()
    failure_pattern = re.compile(r"\b(?:FATAL|PANIC|FAIL|ERROR|TIMEOUT)\b", re.I)
    if variant in ("arena-exhaust", "table-exhaust"):
        fatal = _lines(text, "M1 FATAL")
        expected = [f"M1 FATAL reason={variant}"]
        if variant == "table-exhaust" and "M1 FATAL reason=runtime-init" in fatal:
            expected.append("M1 FATAL reason=runtime-init")
        if fatal != expected:
            return False
        allowed_diagnostics = (
            r"\[smp\] FATAL: arena need=\d+ MiB \(metadata \+ table pool, 2 MiB-aligned\)",
            r"\[smp\] FATAL: largest available intersection in \[0x40200000, 0x80000000\) = \d+ MiB in \[0x[0-9a-f]+, 0x[0-9a-f]+\)",
            r"\[smp\] FATAL: no RAM in \[0x40200000, 0x80000000\) window",
            r"\[smp\] FATAL: aarch64 M1 arena preflight failed",
        ) if variant == "arena-exhaust" else ()
        for line in lines:
            if failure_pattern.search(line) and line not in expected:
                if not any(re.fullmatch(pattern, line) for pattern in allowed_diagnostics):
                    return False
            if line.startswith("M1 ") and line not in expected:
                return False
        return (not bsp and not ap_pass and not ap_online
                and not re.search(r"^\[(?:tick|cntp|clocksource|IRQ)\]", text, re.M)
                and not re.search(r"^\[smp\] (?:cpu=|requested=|topology |boot el=)", text, re.M))
    if variant == "ap-bad-root":
        if cpus < 2:
            return False
        # Reuse all normal BSP/coverage/selftest gates, then validate the
        # deliberate no-ACK outcome separately. No success evidence is forged.
        try:
            if not _m1_evidence_ok(text, cpus, True, 512, bsp_only=True):
                return False
        except (IndexError, StopIteration, ValueError):
            return False
        summaries = re.findall(
            r"^\[smp\] (?:summary )?requested=(\d+) online=(\d+) status=DEGRADED$", text, re.M)
        timeouts = re.findall(r"^\[smp\] cpu=(\d+) reason=online-timeout$", text, re.M)
        if (len(summaries) != 1 or tuple(map(int, summaries[0])) != (cpus, 1)
                or sorted(map(int, timeouts)) != list(range(1, cpus))
                or ap_pass or ap_online
                or len(re.findall(r"^\[smp\] cpu=0 online(?:\s.*)?$", text, re.M)) != 1
                or _lines(text, "[spinlock]") != ["[spinlock] status=SKIP"]):
            return False
        for line in lines:
            if failure_pattern.search(line):
                if re.fullmatch(r"\[smp\] cpu=\d+ reason=online-timeout", line):
                    continue
                if line == f"[ipi] summary targets={cpus - 1} received=0 status=FAIL":
                    continue  # Deliberate missing APs cannot service the IPI.
                return False
        root = int(bsp[0][0], 16)
        coverage = _records(text, "M1 COVERAGE", _COVERAGE, (1, 2))
        return (0 < root < (1 << 40) and coverage is not None
                and any(start <= root < end for _, start, end in coverage))

    return False


def _fixture(cpus: int, *, selftest: bool = False, ram_mib: int = 512,
             probe: int = 0x40000000, variant: str = "normal") -> str:
    second_start = 0x80000000 if ram_mib >= 2048 else 0x50000000
    if variant == "sparse":
        ranges = [(0x4fe00000, 0x50000000, 1), (0x50200000, 0x50600000, 2)]
        probe = 0x4fe00000
    else:
        ranges = [(0x40000000, 0x40200000, 1), (second_start, second_start + 0x400000, 2)]
    lines = []
    if variant == "sparse":
        lines.append("M1 SPARSE INPUT start=0x50000000 end=0x50200000")
    lines.append("UEFI-A64: RAM ranges=2 pages2m=3 bytes=6291456")
    if selftest:
        lines.extend(["UEFI-A64: pmm alloc smoke OK", "UEFI-A64: pt map smoke OK"])
    if selftest:
        lines.extend(["M1 PRUNE PASS pa=0x00200000 before=valid after=fault",
                      "M1 CLEANUP PASS slot=256"])
        lines.extend(f"M1 EDGE PASS zone={i} first={start:x} last={start + (frames - 1) * PAGE_2M:x}"
                     for i, (start, _, frames) in enumerate(ranges))
        lines.append("M1 WALK PASS frames=3")
        if variant == "sparse":
            lines.append("M1 SPARSE PASS left=0x4fe00000 right=0x50200000 hole=absent")
        lines.append("M1 SELFTEST PASS")
    lines.extend(f"M1 ZONE id={i} start={start:x} end={end:x} frames={frames}"
                 for i, (start, end, frames) in enumerate(ranges))
    lines.extend(f"M1 COVERAGE id={i} start={start:x} end={end:x}"
                 for i, (start, end, _) in enumerate(ranges))
    lines.append("M1 BSP PASS root=12345000 ranges=2")
    if selftest:
        lines.append(f"M1 PROBE PASS pa={probe:x}")
    lines.extend(f"M1 AP PASS cpu={cpu} root=12345000 probe=" +
                 (f"{probe:x}" if selftest else "none") for cpu in range(1, cpus))
    lines.extend(["[smp] topology source=uefi-dtb cpus=" + str(cpus)])
    lines.extend(f"[smp] cpu={cpu} online mpidr=0x{cpu:x}" for cpu in range(cpus))
    lines.append(f"[smp] requested={cpus} online={cpus} status=PASS")
    lines.extend(f"[spinlock] cpu={cpu} done=1000000" for cpu in range(cpus))
    lines.append(f"[spinlock] active={cpus} iterations=1000000 total={cpus * 1000000} status=PASS")
    lines.append("[smp-test] no_ack_cpu=0")
    lines.extend(["[tick] 1", "[tick] 2", "[tick] 3", "[gic] GICv2 driver: intids=96",
                  "[gic] dispatch ready", "[gic-probe] save-restore OK",
                  "[gic-probe] unexpected intid=40 survived", "[ipi] send sgi=0 filter=others"])
    if cpus > 1:
        lines.extend(f"[ipi] cpu={cpu} received=1" for cpu in range(1, cpus))
    lines.append(f"[ipi] summary targets={cpus - 1} received={cpus - 1} status=PASS")
    if cpus > 1:
        lines.append("[ipi] bsp raw_iar=0x401")
    lines.extend(["[clocksource] active=true", "[clocksource] freq=62500000",
                  "[clocksource] mult=2147483648 shift=27"])
    return "\n".join(lines) + "\n"


def self_test() -> None:
    for cpus in (1, 2, 4):
        assert m1_evidence_ok(_fixture(cpus), cpus, False, 512)
    for cpus in (1, 2, 4):
        assert m1_evidence_ok(_fixture(cpus, selftest=True), cpus, True, 512)
    actual_m1 = [
        "M1 PRUNE PASS pa=0x00200000 before=valid after=fault",
        "M1 CLEANUP PASS slot=256",
        "M1 EDGE PASS zone=0 first=0x40600000 last=0x43e00000",
        "M1 EDGE PASS zone=1 first=0x44200000 last=0x45c00000",
        "M1 EDGE PASS zone=2 first=0x48000000 last=0x5fc00000",
        "M1 WALK PASS frames=236",
        "M1 SELFTEST PASS",
        "M1 ZONE id=0 start=0x40200000 end=0x44000000 frames=31",
        "M1 ZONE id=1 start=0x44200000 end=0x45e00000 frames=14",
        "M1 ZONE id=2 start=0x48000000 end=0x5fe00000 frames=191",
        "M1 COVERAGE id=0 start=0x40200000 end=0x44000000",
        "M1 COVERAGE id=1 start=0x44200000 end=0x45e00000",
        "M1 COVERAGE id=2 start=0x48000000 end=0x5fe00000",
        "M1 BSP PASS root=0x40206000 ranges=3",
        "M1 PROBE PASS pa=0x40600000",
    ]
    real_log_lines = [line for line in _fixture(1, selftest=True).splitlines()
                      if not line.startswith("M1 ")]
    topology_index = real_log_lines.index("[smp] topology source=uefi-dtb cpus=1")
    real_log_lines[topology_index:topology_index] = actual_m1
    assert m1_evidence_ok("\n".join(real_log_lines), 1, True, 512)
    no_legacy_smoke = (_fixture(1, selftest=True)
                       .replace("UEFI-A64: pmm alloc smoke OK\n", "")
                       .replace("UEFI-A64: pt map smoke OK\n", ""))
    assert m1_evidence_ok(no_legacy_smoke, 1, True, 512)
    assert m1_evidence_ok(_fixture(1, selftest=True, ram_mib=4096, probe=0x80000000), 1, True, 4096)
    assert m1_evidence_ok(_fixture(1, selftest=True, variant="sparse"), 1, True, 512, "sparse")
    bad_probe = _fixture(1, selftest=True, ram_mib=4096, probe=0x40000000)
    assert not m1_evidence_ok(bad_probe, 1, True, 4096)
    fixture = _fixture(2, selftest=True)
    marker_mutations = (
        fixture.replace("M1 ZONE id=0", "M1 ZONE id=x"),
        fixture.replace("M1 ZONE id=1 start=50000000 end=50400000 frames=2\n", ""),
        fixture + "M1 ZONE id=0 start=40000000 end=40200000 frames=1\n",
        fixture.replace("M1 COVERAGE id=0", "M1 COVERAGE id=x"),
        fixture.replace("M1 COVERAGE id=1 start=50000000 end=50400000\n", ""),
        fixture + "M1 COVERAGE id=0 start=40000000 end=40200000\n",
        fixture.replace("M1 COVERAGE id=0 start=40000000 end=40200000",
                        "M1 COVERAGE id=0 start=40000000 end=40400000"),
        fixture.replace("M1 EDGE PASS zone=0 first=40000000 last=40000000\n", ""),
        fixture + "M1 EDGE PASS zone=0 first=40000000 last=40000000\n",
        fixture.replace("M1 EDGE PASS zone=0 first=40000000 last=40000000",
                        "M1 EDGE PASS zone=x first=40000000 last=40000000"),
        fixture.replace("M1 WALK PASS frames=3\n", ""),
        fixture + "M1 WALK PASS frames=3\n",
        fixture.replace("M1 WALK PASS frames=3", "M1 WALK PASS frames=bad"),
        fixture.replace("M1 PRUNE PASS pa=0x00200000 before=valid after=fault\n", ""),
        fixture + "M1 PRUNE PASS pa=0x00200000 before=valid after=fault\n",
        fixture.replace("M1 WALK PASS frames=3", "M1 WALK PASS frames=2"),
        fixture.replace("M1 PRUNE PASS pa=0x00200000 before=valid after=fault", "M1 PRUNE PASS pa=bad"),
        fixture.replace("M1 CLEANUP PASS slot=256", ""),
        fixture + "M1 CLEANUP PASS slot=256\n",
        fixture.replace("M1 CLEANUP PASS slot=256", "M1 CLEANUP PASS slot=bad"),
        fixture.replace("M1 PROBE PASS pa=40000000\n", ""),
        fixture + "M1 PROBE PASS pa=40000000\n",
        fixture.replace("M1 PROBE PASS pa=40000000", "M1 PROBE PASS pa=none"),
        fixture.replace("M1 BSP PASS root=12345000", "M1 BSP PASS root=12345000\nM1 BSP PASS root=12345000"),
        fixture.replace("M1 BSP PASS root=12345000 ranges=2\n", ""),
        fixture.replace("M1 BSP PASS root=12345000", "M1 BSP PASS root=12345001"),
        fixture.replace("M1 AP PASS cpu=1", "M1 AP PASS cpu=2"),
        fixture.replace("M1 AP PASS cpu=1 root=12345000 probe=40000000\n", ""),
        fixture + "M1 AP PASS cpu=1 root=12345000 probe=40000000\n",
        fixture.replace("M1 AP PASS cpu=1 root=12345000", "M1 AP PASS cpu=1 root=99999000"),
        fixture.replace("M1 AP PASS cpu=1 root=12345000 probe=40000000",
                        "M1 AP PASS cpu=1 root=12345000 probe=40200000"),
        fixture + "M1 AP FAIL cpu=1 reason=probe\n",
        fixture + "M1 FATAL reason=table-exhaust\n",
        fixture + "[smp] requested=2 online=1 status=DEGRADED\n",
        "UEFI: booting OS01\n",
    )
    for bad in marker_mutations:
        assert not m1_evidence_ok(bad, 2, True, 512), bad
    wrong_m1_order = fixture.replace(
        "M1 CLEANUP PASS slot=256\n", "").replace(
            "M1 EDGE PASS zone=0", "M1 EDGE PASS zone=0", 1).replace(
                "M1 EDGE PASS zone=0 first=40000000 last=40000000\n",
                "M1 EDGE PASS zone=0 first=40000000 last=40000000\nM1 CLEANUP PASS slot=256\n")
    assert not m1_evidence_ok(wrong_m1_order, 2, True, 512)
    # Move the one valid AP marker before BSP without changing its count.
    ap_line = "M1 AP PASS cpu=1 root=12345000 probe=40000000\n"
    ap_before_bsp = fixture.replace(ap_line, "").replace(
        "M1 BSP PASS root=12345000 ranges=2\n", ap_line + "M1 BSP PASS root=12345000 ranges=2\n")
    assert not m1_evidence_ok(ap_before_bsp, 2, True, 512)
    assert not m1_evidence_ok(fixture + "QEMU timeout\n", 2, True, 512)
    sparse = _fixture(1, selftest=True, variant="sparse")
    assert not m1_evidence_ok(sparse.replace("M1 SPARSE PASS left=0x4fe00000 right=0x50200000 hole=absent\n", ""), 1, True, 512, "sparse")
    assert not m1_evidence_ok(sparse.replace("M1 SPARSE INPUT start=0x50000000 end=0x50200000", "M1 SPARSE INPUT start=bad"), 1, True, 512, "sparse")
    assert not m1_evidence_ok(_fixture(1, selftest=True).replace("M1 SELFTEST PASS\n", ""), 1, True, 512)
    assert not m1_evidence_ok(_fixture(1, selftest=True) + "M1 SELFTEST PASS\n", 1, True, 512)
    assert not m1_evidence_ok(_fixture(1, selftest=True).replace("M1 PROBE PASS pa=40000000\n", ""), 1, True, 512)
    assert m1_evidence_ok(_fixture(1, selftest=True, ram_mib=2048, probe=0x80000000), 1, True, 2048)
    assert not m1_evidence_ok(_fixture(1, selftest=True, ram_mib=2048, probe=0x40000000), 1, True, 2048)
    assert not m1_evidence_ok(_fixture(2) + "M1 AP PASS cpu=1 root=12345000 probe=none\n", 2, False, 512)
    assert not m1_evidence_ok(_fixture(2, selftest=True) + "M1 AP PASS cpu=1 root=12345000 probe=20000000\n", 2, True, 512)
    timeout = _fixture(1, selftest=True).replace("M1 PROBE PASS pa=0x40000000\n", "") + "QEMU timeout\n"
    assert not m1_evidence_ok(timeout, 1, True, 512)
    assert m1_failure_evidence_ok(
        "M1 FATAL reason=arena-exhaust\n", "arena-exhaust", 1, True)
    assert m1_failure_evidence_ok(
        "M1 FATAL reason=table-exhaust\n", "table-exhaust", 1, True)
    assert not m1_failure_evidence_ok(
        "M1 FATAL reason=arena-exhaust\n", "arena-exhaust", 1, False)
    assert not m1_failure_evidence_ok(
        "M1 FATAL reason=arena-exhaust\n", "table-exhaust", 1, True)
    assert not m1_failure_evidence_ok("UEFI: booting OS01\n", "arena-exhaust", 1, True)
    no_ap_ack = ("\n".join(actual_m1) + "\n"
                 "[smp] cpu=0 online mpidr=0x0\n"
                 "[smp] cpu=1 reason=online-timeout\n"
                 "[smp] requested=2 online=1 status=DEGRADED\n"
                 "[spinlock] status=SKIP\n[tick] 1\n")
    assert m1_failure_evidence_ok(no_ap_ack, "ap-bad-root", 2, True)
    negative_mutations = [
        no_ap_ack + "M1 FATAL reason=unexpected\n",
        no_ap_ack + "PANIC: unrelated\n",
        no_ap_ack + "[unrelated] status=FAIL\n",
        no_ap_ack.replace("root=0x40206000", "root=0"),
        no_ap_ack.replace("root=0x40206000", "root=0x40206001"),
        no_ap_ack.replace("root=0x40206000", "root=0x10000000000"),
        no_ap_ack.replace("pa=0x40600000", "pa=0"),
        no_ap_ack.replace("pa=0x40600000", "pa=0x20000000"),
        no_ap_ack.replace("cpu=1 reason=online-timeout", "cpu=2 reason=online-timeout"),
        no_ap_ack.replace("[smp] cpu=1 reason=online-timeout\n", ""),
        no_ap_ack + "[smp] cpu=1 online mpidr=0x1\n",
        no_ap_ack + "M1 BSP PASS root=0x40206000 ranges=3\n",
        no_ap_ack + "M1 PROBE PASS pa=0x40600000\n",
    ]
    for prefix in ("M1 ZONE", "M1 COVERAGE", "M1 SELFTEST", "M1 CLEANUP", "M1 PRUNE", "M1 EDGE", "M1 WALK"):
        negative_mutations.append("\n".join(line for line in no_ap_ack.splitlines() if not line.startswith(prefix)))
    for mutation in negative_mutations:
        assert not m1_failure_evidence_ok(mutation, "ap-bad-root", 2, True), mutation
    assert not m1_failure_evidence_ok(no_ap_ack, "ap-bad-root", 2, False)
    for variant in ("arena-exhaust", "table-exhaust"):
        base = f"M1 FATAL reason={variant}\n"
        for extra in ("M1 FATAL reason=unexpected\n", "PANIC: unrelated\n",
                      "[smp] cpu=1 target_mpidr=0x1 rc=0\n", "[cntp] freq=1\n",
                      "M1 SELFTEST PASS\n", "[unrelated] status=FAIL\n"):
            assert not m1_failure_evidence_ok(base + extra, variant, 1, True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if not args.self_test:
        parser.error("only --self-test is supported directly; import m1_evidence_ok for logs")
    self_test()
    print("aarch64_m1_evidence: self-test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
