# PIT 200Hz — Root Cause, Evidence, and Stable Timer Plan (HANDOFF)

Status: HANDOFF 2026-08-17 from Hermes/agent to Claude Code on homeserver.
Read this whole file before touching timer code. Supersedes
`PIT-200HZ-ANALYSIS.md` and `PIT-200HZ-DECISION.md` where they conflict
(old files said "root cause unknown"; it is now known).

---

## 1. TL;DR (one paragraph)

OS01's PIT ticks at 200 Hz under QEMU TCG, but **this is NOT an OS01 bug**.
QEMU's IOAPIC edge-triggered delivery fires on *every* `ioapic_set_irq(level=1)`
call — there is no rising-edge detection — and QEMU's PIT channel-0 mode-3
square wave drives `level=1` TWICE per 10 ms period. Result: 100 Hz PIT × 2
deliveries = 200 Hz presented to the guest. The guest handles every delivery
exactly once (1:1, measured), so guest-side code is correct as-is. The only
real defect is that every jiffies-based interval (poll/select, EEVDF
timeslice, lwIP timeouts, CLOCK_MONOTONIC, nanosleep) runs 2× fast under
QEMU. For a robust fix, switch the OS tick source to the **LAPIC timer**
(LVT-local delivery, immune to the IOAPIC artifact) calibrated against TSC
— not the PIT. See §5 for the plan and §7 for the verification gates.

---

## 2. Final root cause (verified 2026-08-17)

### 2.1 The mechanism

QEMU `hw/intc/ioapic.c`, `ioapic_set_irq()` — edge branch (identical in
9.2.0 and system 11.1.0):

```c
} else {
    /* According to the 82093AA manual, we must ignore edge requests
     * if the input pin is masked. */
    if (level && !(entry & IOAPIC_LVT_MASKED)) {
        s->irr |= mask;
        ioapic_service(s);
    }
}
```

- NO rising-edge detection: **every call with `level==1` delivers**.
- QEMU PIT (`hw/timer/i8254.c`) `pit_irq_timer_update()` calls
  `qemu_set_irq(s->irq, irq_level)` on each timer expiry. For mode-3
  (square wave) with count=11931 the transition timer fires every half
  period (~5 ms), so in each 10 ms period `level=1` is signaled twice
  (once at the rising edge, once more while still high / at the
  half-period sample).
- Net effect: **200.0 Hz delivered to the guest from a genuine 100 Hz PIT.**

### 2.2 Why the old "root cause unknown" conclusion was wrong

Old `PIT-200HZ-ANALYSIS.md` correctly measured 200 Hz guest-side, but
mis-read `info irq` (HMP counter) as QEMU-side ground truth showing 100 Hz,
creating an apparent "QEMU says 100 Hz, guest says 200 Hz → guest bug"
contradiction. **`info irq` undercounts by 2× in this setup (QEMU stat bug).**
The authoritative counter is QEMU trace events (`apic_deliver_irq`,
`ioapic_set_irq`), which show 200.0 Hz delivery.

### 2.3 Full measured evidence chain (all 2026-08-17, homeserver,
system QEMU 11.1.0, q35, TCG; both `-smp 1` and `-smp 2`)

| # | Evidence | Method | Value |
|---|----------|--------|-------|
| 1 | QEMU delivers IRQ0 vec32 at 200 Hz | `-trace events=apic_deliver_irq` | 200.0 Hz every 10 s window |
| 2 | Guest jiffies at 200 Hz | temporary JIFRATE probe in pit_handler (REMOVED, never committed) | 199.9 Hz |
| 3 | Guest:QEMU ratio = 1.000 | same-window two-layer sample | jiffies 27000 vs 27192 level=1 calls (99.3%, boot window = rest) |
| 4 | PIT level sequence double-fired | `-trace events=ioapic_set_irq` | `1,1,0,0,1,1,0,0,...` (each level run length 2) |
| 5 | SMP irrelevant | `-smp 1` vs `-smp 2` vs `-accel tcg,thread=single` | all 200 Hz |
| 6 | PIT divisor correct | `pit_ioport_write` trace | 0x36 cmd, 0x9b+0x2e=11931 |
| 7 | Guest TSC == host TSC | TSCCAL probe (RTC-calibrated) | 2,994,315,480 vs host 2,994,492,000 (0.006%) |
| 8 | `info irq` reports 100 Hz | HMP `info irq` | **WRONG (QEMU stat bug)** — trust trace |

### 2.4 Ruled out (do NOT re-investigate)

- PIT divisor / mode-3 formula / OS01 jiffies++ site / IRQ0 routing: all
  correct (trace-verified).
- QEMU version difference (9.2.0 vs 11.1.0 i8254): zero logic diff.
- `QEMU_CLOCK_VIRTUAL` running 2× wall clock: falsified.
- PIT divider halved: falsified.
- SMP dual-CPU delivery: falsified.
- `-icount` semantics: only changes overall pacing, not the double-delivery.

### 2.5 Real hardware note

On real silicon, 8254 mode-3 + 8259/IOAPIC edge = one rising edge per
period = genuine 100 Hz. **The 200 Hz is a QEMU TCG emulation artifact.**
Do not change the PIT divisor (11931) or the OS01 10 ms/jiffy assumption
as the hardware default.

---

## 3. Current impact (what is 2× fast under QEMU right now)

- poll()/select() timeouts (kernel/fs/poll.c, select.c) — half real duration
- EEVDF scheduler timeslices — 2× fast
- lwIP sys_timeout / DHCP coarse timer — 2× fast
- CLOCK_MONOTONIC / clock_gettime — 2× fast
- busybox `sleep N` returns after N/2 wall seconds
- nanosleep: `2faccbc` fixed the wakeup blocker; the 2× duration it
  exposes is this QEMU artifact, not a nanosleep bug.

Everything is driven by jiffies (1 jiffy assumed 10 ms). Under QEMU a jiffy
is actually 5 ms.

---

## 4. Options considered (full analysis in
`~/.hermes/skills/software-development/os01-dev/references/stable-timer-options.md`)

### Option 1 — LAPIC timer as OS tick source (RECOMMENDED)
- LVT-local delivery (`apic_local_deliver`), NEVER through the IOAPIC edge
  branch → immune to the artifact BY DESIGN.
- QEMU models LAPIC timer at 1 tick = 1 ns (apic_common.c count_shift=0),
  periodic mode fires every (initial_count+1) ns — no divisor rounding.
- OS01 already has `kernel/apic/lapic_timer.c`: LAPIC_TIMER_VECTOR=0x38,
  IDT gate `set_intr_gate_raw`, per-CPU start, periodic support.
- **Current calibration is poisoned**: `lapic_timer_hz = elapsed * 100`
  measures elapsed over ONE jiffy assumed 10 ms; under QEMU that jiffy is
  5 ms → measured LAPIC delivery is ~629 Hz when asking for 100 Hz
  (measured via `apic_local_deliver` trace, LVT idx 0 = APIC_LVT_TIMER).
  Calibration must use a TSC window instead (TSC is trusted, 2.994 GHz).
- Work: small — fix calibration + switch tick source at init.

### Option 2 — TSC-deadline (LVT bit 17 = 2, MSR 0x6E0)
- Also LVT-local (immune), counter is TSC itself.
- Risk: one-shot re-arm race (handler must rewrite MSR before next
  deadline); TCG exposes CPUID leaf 15h = 0 so TSC rate still needs
  calibrating anyway. Fallback only.

### Option 3 — HPET
- Great free-running counter (QEMU models 100 MHz / 10 ns period; read the
  CAPABILITY period — do NOT hard-code 14.31818 MHz).
- Best as a TIME BASE (monotonic clock), not the tick interrupt source:
  its interrupt path re-enters IOAPIC GSI routing → same artifact exposure.
- New driver = largest implementation cost.

### Option 4 — Keep PIT, jiffy = 5 ms under QEMU (stopgap only)
- Smallest diff but hard-codes QEMU behavior; breaks on real hardware;
  brittle across QEMU upgrades. Do not ship as the permanent fix.

---

## 5. Recommended implementation plan (for Claude Code to execute after
user confirms approach — user's convention: discuss before implementing)

### Phase A — LAPIC timer calibration against TSC (in
`kernel/apic/lapic_timer.c`)

Rewrite `lapic_timer_calibrate()` to not depend on PIT jiffies:

```c
// pseudo — adapt to OS01 style
tsc0 = rdtscp_serialized();
lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
while (rdtscp_serialized() - tsc0 < TSC_HZ / 100) {}   // 10 ms TSC window
elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
lapic_timer_hz = elapsed * 100;
```

- `TSC_HZ`: derive from RTC calibration (existing TSCCAL approach) or from
  CPUID leaf 15h when valid (TCG returns 0 — fall back to RTC/TSC timing).
- Guard: if LAPIC timer absent/failed → fall back to PIT path unchanged.

### Phase B — Switch OS tick source PIT → BSP LAPIC timer

- BSP: `lapic_timer_start(100)` in periodic mode becomes the tick;
  `jiffies++` happens in the LAPIC timer handler (or a softirq it raises).
- Keep AP LAPIC timers as they are (need_resched); keep PIT masked/stopped
  as a fallback path (do NOT change divisor).
- Watch per-CPU consistency: only the BSP advances global `jiffies`.

### Phase C — Keep PIT as fallback (no divisor change)

- PIT remains initialized but masked; if LAPIC timer fails at boot, unmask
  PIT and keep the 10 ms/jiffy assumption (hardware default).

### Phase D — (optional, later) monotonic time base

- Add `rdtsc × TSC_PER_NS` or HPET counter (period from CAPABILITY) behind
  clock_gettime CLOCK_MONOTONIC to decouple timekeeping from tick jitter.

---

## 6. Verification gates (MANDATORY before reporting success)

Run each with a fresh build; 30 s steady-state windows.

1. `-trace events=apic_local_deliver` → LAPIC LVT TIMER (idx 0) fires at
   **~100 Hz** (not 200 Hz, not 629 Hz).
2. Guest JIFRATE-style probe (or serial print every 500 ticks) → jiffies
   **~100 Hz**, ratio to LAPIC delivery **1.000**.
3. `busybox sleep 5` → returns in **~5.0 s wall** (was 2.5 s under PIT).
4. `clock_gettime` Δ vs host wall → **1:1** (±1%).
5. Repeat with `-smp 1` AND `-smp 2`.
6. Regressions: scheduler (both CPUs), poll/select timeout, lwIP DHCP
   renewal, nanosleep systests — full test suite green.

Evidence of every gate must be attached (trace excerpt + timing numbers),
per user's "QEMU-verified evidence before progress claims" rule.

---

## 7. How to reproduce the measurements (for your own verification)

```bash
# QEMU with PIT + IOAPIC trace (system QEMU 11.1.0)
cd ~/OS01-nanosleep
printf 'apic_deliver_irq\nioapic_set_irq\npit_ioport_write\n' > /tmp/ev.txt
qemu-system-x86_64 -M q35 -smp 2 -pflash boot/uefi/OVMF.fd \
  -netdev user,id=net0 -device e1000e,netdev=net0 \
  -drive file=disk.img,format=raw,if=none,id=disk \
  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
  -m 512 -display none -serial file:/tmp/s.log \
  -trace events=/tmp/ev.txt,file=/tmp/t.log \
  -no-reboot &
# then: grep -c 'vector 32' /tmp/t.log twice 10 s apart → expect 200 Hz
#       (for LAPIC: trace apic_local_deliver, LVT idx 0 = TIMER)
```

Key QEMU internals to cite:
- `hw/intc/ioapic.c` edge branch (no rising-edge detect)
- `hw/timer/i8254.c` `pit_irq_timer_update` → set_irq per half-period
- `hw/intc/apic_common.c` 1 ns/tick LAPIC timer
- `include/hw/i386/apic_internal.h` LVT idx 0 = TIMER; TSCDEADLINE bit 17

---

## 8. Files / references

- Skill docs (authoritative, already updated):
  - `~/.hermes/skills/software-development/os01-dev/references/pit-200hz-resolution.md`
  - `~/.hermes/skills/software-development/os01-dev/references/stable-timer-options.md`
- Repo docs (this handoff): `PIT-200HZ-HANDOFF.md` (this file)
- Old (superseded): `PIT-200HZ-ANALYSIS.md`, `PIT-200HZ-DECISION.md`
- Code touched by plan: `kernel/apic/lapic_timer.c`, `kernel/driver/pit.c`,
  `kernel/timer/timer.c`, `kernel/subsys/subsys.c` (or
  `kernel/arch/x86_64/subsys.c`), `kernel/include/kernel/arch/x86_64/cpu.h`
  (rdtsc helpers exist)

## 9. Constraints from user (do not violate)

- Chinese communication with user; architecture-level changes need
  discussion BEFORE implementation ("先讨论再动").
- Each step needs test regression: RED → GREEN → full regression before
  advancing (superpowers workflow).
- Prefer user-space migration over kernel refactor where possible.
- Semantic commit prefixes, small commits (feat/fix), remove debug
  artifacts before commit.
- Probes must be read-only; remove temp probes before commit (JIFRATE etc.
  are already removed).