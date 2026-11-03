# terminal-present Task-3 baseline (durable copy)

The terminal-present suite gates a >=30 s steady-state monitor window
against the Task-3 baseline. The original authoritative baseline lived
in `/tmp/os01-terminal-present-baseline/` (per the plan:
`docs/superpowers/plans/2026-10-12-terminal-partial-present.md`, Task 3
step 7) and was deliberately never committed — it vanished on reboot,
which made `make test-qemu SUITE=terminal-present` unconditionally fail
with "evidence missing" everywhere, including nightly CI.

This directory is the durable home for that baseline pair:

- `serial.log` — full serial transcript of the capture run (the
  acceptance run's records are NOT compared against it; the gate
  `evaluate_present_window` evaluates the current run only).
- `manifest.json` — capture identity snapshot.

`_verify_baseline` treats missing files as hard FAILs and every hash
mismatch (image / firmware / serial) as a soft, non-failing note, so
this copy stays valid across kernel/QEMU drift. Re-capture only with
`python3 -m qemutests.terminal_present_test --capture-baseline
--output-dir <new dir>` (see `mk/components/run.mk`:
`TERMINAL_PRESENT_BASELINE_LOG`), then rebuild `serial.log` +
`manifest.json` the same way and keep the run artifacts out of git.
