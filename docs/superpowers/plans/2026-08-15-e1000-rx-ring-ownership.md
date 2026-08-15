# E1000 RX Ring Ownership Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the tcpip thread the sole E1000 RX-ring consumer, return every consumed descriptor to hardware in order, and eliminate the lost IRQ-to-mailbox wake window.

**Architecture:** The E1000 interrupt handler reads ICR and records a persistent mailbox wake, but never touches RX descriptors. The tcpip thread drains the hardware ring, copies packets into the existing software queue, returns each descriptor with `arch_wmb()` followed by `RDT=i`, and then delivers queued packets to lwIP.

**Tech Stack:** C11 kernel code, Intel E1000/e1000e descriptor MMIO, x86 DMA barriers, lwIP `sys_arch`, Python QEMU regression harness, GNU Make.

## Global Constraints

- Preserve the handler's ICR read: it acknowledges the IRQ and clears QEMU's RX latch.
- Every consumed DD descriptor, including every drop path, must execute clear DD → `arch_wmb()` → `RDT=i` → advance index.
- `e1000_poll_rx()` has exactly one caller: `net_poll_rx()` in tcpip-thread context.
- `sys_mbox_wake()` uses `spin_lock_irqsave`, sets `idle_wakeup=1`, unlocks, then calls `wait_queue_wake_one()`.
- Keep the existing software RX queue to minimize the change surface; direct `pbuf` delivery is out of scope.
- Do not add per-packet logging, TCP retries, or result-parser relaxation.
- Do not change VirtIO-net; its similar dual-consumer shape is follow-up work.
- Preserve the accepted `localhost` DNS prerequisite and fixed host ports.
- Preserve unrelated dirty paths, especially `thirdpart/busybox-1.36.1`, `thirdpart/posix-uefi`, and `docs/superpowers/plans/2026-08-15-proc-fd-observability.md` if present.
- Run `make clean` before final kernel verification because this repository lacks header dependencies.

## File Map

- Modify `kernel/driver/e1000.c`: single RX owner, exact descriptor return, DMA ordering, handler-only ICR acknowledgement, ownership comments.
- Modify `kernel/net/sys_arch.c`: persistent IRQ-safe mailbox wake and corrected comments.
- Modify `kernel/net/net.c`: document tcpip-thread ownership at the only E1000 poll call.
- Test through existing `tests/run_test.py` and `Makefile` targets; do not modify the harness for this fix.

---

### Task 1: Prove the network regression detects a deterministically stalled RX ring

**Files:**
- Temporarily modify, then restore: `kernel/driver/e1000.c:98-139,367-373`
- Test: `tests/run_test.py:302-326`

**Interfaces:**
- Consumes: existing `make test-network` aggregate `[NET TEST] RESULT` parser.
- Produces: 20-run RED evidence; no persistent source change or commit.

- [ ] **Step 1: Record the clean task boundary**

Run:

```bash
git status --short
git diff -- kernel/driver/e1000.c kernel/net/sys_arch.c kernel/net/net.c
```

Expected: the three implementation files have no pre-existing changes. Stop and report the overlap if any is modified; do not overwrite it.

- [ ] **Step 2: Apply the named descriptor-starvation mutation**

In `e1000_poll_rx()`, temporarily delete both fixed tail writes:

```c
e1000_write(E1000_REG_RDT, 30);
e1000_write(E1000_REG_RDT, 31);
```

In `e1000_init()`, temporarily replace:

```c
e1000_write(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
```

with:

```c
e1000_write(E1000_REG_RDT, 1);
```

Do not add any replacement `RDT` write while polling. This exposes only descriptors 0 and 1 to hardware.

- [ ] **Step 3: Clean-build the mutation**

Run:

```bash
make clean
make OS01_NETTEST=1 disk.img boot/uefi/OVMF.fd
```

Expected: build succeeds.

- [ ] **Step 4: Run 20 fresh mutation iterations**

Run each iteration as a separate QEMU process, saving its complete output:

```bash
mkdir -p /tmp/os01-e1000-mutation
for i in $(seq 1 20); do
    python3 tests/run_test.py network >"/tmp/os01-e1000-mutation/run-$i.log" 2>&1
    test $? -ne 0 || exit 1
    rg '\[NET TEST\] RESULT: [0-9]+ passed, [1-9][0-9]* failed' "/tmp/os01-e1000-mutation/run-$i.log" || exit 1
done
```

Expected: the shell exits 0; all 20 logs contain an aggregate result with at least one failure. `DHCP: PASS` is expected because the guest initially has a static IP; UDP, DNS, TCP, or wget must expose the RX starvation.

- [ ] **Step 5: Restore only the mutation and verify the restoration**

Use `apply_patch` to restore the two fixed `RDT=30/31` writes and `RDT=E1000_NUM_RX_DESC-1`. Then run:

```bash
git diff -- kernel/driver/e1000.c
```

Expected: no diff. Do not use `git checkout`, `git reset`, or a broad restore command.

---

### Task 2: Implement single-owner RX descriptor lifecycle and persistent IRQ wake

**Files:**
- Modify: `kernel/driver/e1000.c:8,74-139,159-182`
- Modify: `kernel/net/sys_arch.c:134-152`
- Modify: `kernel/net/net.c:90-100`
- Test: `tests/run_test.py:302-326`

**Interfaces:**
- Consumes: `arch_wmb()` from `kernel/include/kernel/arch/barrier.h`; `sys_mbox_wake(void)`; `net_poll_rx(void)`.
- Produces: `e1000_poll_rx(void)` callable only by `net_poll_rx()`; IRQ-safe persistent `sys_mbox_wake(void)`.

- [ ] **Step 1: Add the DMA barrier include and correct ownership documentation**

Add to `kernel/driver/e1000.c`:

```c
#include <kernel/arch/barrier.h>
```

Rewrite the RX section comments so they state that both `e1000_poll_rx()` and `e1000_process_rx()` run in tcpip-thread context, and the software queue is retained only to minimize this change. Change `e1000_rxq_t.head` and `.tail` comments from IRQ/task ownership to tcpip-thread fill/drain ownership.

- [ ] **Step 2: Return every consumed descriptor exactly once**

Restructure `e1000_poll_rx()` around a stable descriptor index. Its loop body must have one common return path after copy-or-drop:

```c
while (e1000.rx_descs[e1000.rx_tail].status & E1000_RXD_STAT_DD) {
    uint32_t i = e1000.rx_tail;
    uint16_t len = e1000.rx_descs[i].length;

    if (len > 0 && len < 1600) {
        int next = (e1000_rxq.head + 1) % E1000_RXQ_DEPTH;
        if (next != e1000_rxq.tail) {
            uint8_t *buf = (uint8_t *)kmalloc(len);
            if (buf) {
                memcpy(buf, e1000.rx_bufs[i], len);
                e1000_rxq.buf[e1000_rxq.head] = buf;
                e1000_rxq.len[e1000_rxq.head] = len;
                e1000_rxq.head = next;
            }
        }
    }

    e1000.rx_descs[i].status = 0;
    arch_wmb();
    e1000_write(E1000_REG_RDT, i);
    e1000.rx_tail = (i + 1) % E1000_NUM_RX_DESC;
}
```

Remove the queue-full early `continue`, the poll-side ICR read, fixed `RDT=30/31` writes, and both obsolete workaround comment blocks. Invalid length, queue full, and `kmalloc()` failure must all fall through the same descriptor-return sequence.

- [ ] **Step 3: Make the handler acknowledge and wake without consuming descriptors**

Keep the ICR read at the start of `e1000_handler()` and remove its `e1000_poll_rx()` call. The RX branch must be:

```c
if (icr & (E1000_ICR_RXQ0 | E1000_ICR_RXT0 | E1000_ICR_RXDMT0)) {
    // Reading ICR above acknowledges the interrupt and clears QEMU's RX
    // latch. The tcpip thread is the sole descriptor-ring consumer.
    extern void sys_mbox_wake(void);
    sys_mbox_wake();
}
```

Delete the stale statement that MSI-X is unwired or no IRQ fires. Do not move or duplicate the ICR read in the poller.

- [ ] **Step 4: Make `sys_mbox_wake()` persistent and IRQ-safe**

Replace its bare wake in `kernel/net/sys_arch.c` with the same ordering as `mbox_idle_callback()`:

```c
void sys_mbox_wake(void)
{
    os_mbox_t *mb = g_tcpip_mbox;
    if (!mb) return;
    uint64_t flags = spin_lock_irqsave(&mb->lock);
    mb->idle_wakeup = 1;
    spin_unlock_irqrestore(&mb->lock, flags);
    wait_queue_wake_one(&mb->wq);
}
```

Update the preceding comment: the IRQ handler only acknowledges RX and requests this persistent wake; the tcpip thread calls `net_poll_rx()`. Confirm the existing fetch-side check remains under `mb->lock`:

```c
if (mb->idle_wakeup) {
    mb->idle_wakeup = 0;
    spin_unlock_irqrestore(&mb->lock, _f);
    continue;
}
```

- [ ] **Step 5: Mark the only hardware-ring consumer in `net.c`**

Replace the two misleading inline comments with:

```c
e1000_poll_rx();    // tcpip thread: sole E1000 hardware-ring consumer
e1000_process_rx(); // tcpip thread: deliver the buffered packets to lwIP
```

Do not alter the VirtIO branch.

- [ ] **Step 6: Build from clean state**

Run:

```bash
make clean
make OS01_NETTEST=1 disk.img boot/uefi/OVMF.fd
```

Expected: build succeeds without warnings introduced by these edits.

- [ ] **Step 7: Run the focused green regression**

Run:

```bash
python3 tests/run_test.py network
```

Expected: exit 0 and `[NET TEST] RESULT: 5 passed, 0 failed`.

- [ ] **Step 8: Inspect ownership and descriptor-return invariants**

Run:

```bash
rg -n 'e1000_poll_rx\(' kernel
rg -n 'E1000_REG_RDT|E1000_REG_ICR|arch_wmb|idle_wakeup' kernel/driver/e1000.c kernel/net/sys_arch.c kernel/net/net.c
```

Expected:

- `e1000_poll_rx()` has one definition and one call in `kernel/net/net.c`.
- The poller contains `arch_wmb()` immediately before `RDT=i` and no ICR read.
- The handler contains the ICR read and no descriptor poll call.
- No `RDT=30/31` workaround remains.
- Both wake producer and fetch consumer access `idle_wakeup` under `mb->lock`.

- [ ] **Step 9: Commit the implementation**

```bash
git add kernel/driver/e1000.c kernel/net/sys_arch.c kernel/net/net.c
git commit -m "fix: serialize E1000 RX ring ownership"
```

---

### Task 3: Prove stability and absence of regressions

**Files:**
- Verify: `kernel/driver/e1000.c`
- Verify: `kernel/net/sys_arch.c`
- Verify: `kernel/net/net.c`
- Test: `tests/run_test.py`
- Test: `Makefile`

**Interfaces:**
- Consumes: implementation commit from Task 2 and existing test targets.
- Produces: 20-run GREEN evidence, full-suite evidence, and review findings.

- [ ] **Step 1: Run 20 fresh green network iterations**

Build once from clean state, then run each test in a new QEMU process:

```bash
make clean
make OS01_NETTEST=1 disk.img boot/uefi/OVMF.fd
mkdir -p /tmp/os01-e1000-green
for i in $(seq 1 20); do
    python3 tests/run_test.py network >"/tmp/os01-e1000-green/run-$i.log" 2>&1 || exit 1
    rg '\[NET TEST\] RESULT: 5 passed, 0 failed' "/tmp/os01-e1000-green/run-$i.log" || exit 1
done
```

Expected: all 20 fresh QEMU runs exit 0 and contain exactly `5 passed, 0 failed`; no per-packet diagnostic logging is present.

- [ ] **Step 2: Run the full host test suite**

Run:

```bash
make test
```

Expected: all host tests pass.

- [ ] **Step 3: Run syscall E2E after a clean rebuild**

Run:

```bash
make clean
make OS01_SYSTEST=1 test-syscall
```

Expected: QEMU systest completes with zero failures. The explicit outer `OS01_SYSTEST=1` is retained for the repository's documented invocation convention even though the target currently repeats it internally.

- [ ] **Step 4: Check diff hygiene without touching unrelated changes**

Run:

```bash
git status --short
git diff --check HEAD~1 -- kernel/driver/e1000.c kernel/net/sys_arch.c kernel/net/net.c
git show --stat --oneline HEAD
```

Expected: no whitespace errors in the implementation commit; only the three intended kernel files are in that commit. Unrelated dirty submodules or documents remain untouched.

- [ ] **Step 5: Request an independent driver-focused review**

Ask the reviewer to verify these exact claims against the diff:

```text
1. e1000_handler reads ICR and persistently wakes, but never consumes RX descriptors.
2. e1000_poll_rx has exactly one task-context caller.
3. Every DD path, including all drops, clears status, calls arch_wmb, writes RDT=i, then advances.
4. sys_mbox_wake uses irqsave and set → unlock → wake ordering; fetch clears under the same lock.
5. No stale no-IRQ, poll-side-ICR, fixed-tail, or IRQ-owner comment remains.
6. VirtIO and TCP behavior are unchanged.
```

Expected: no blocking findings. If review changes code, repeat Tasks 3 Steps 1–4 before claiming completion.

- [ ] **Step 6: Record verification evidence**

In the final handoff, report the implementation commit, the 20/20 mutation result, the 20/20 green result, `make test`, syscall E2E, and any accepted environmental prerequisite. Do not claim success from cached or pre-change output.
