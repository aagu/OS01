# AArch64 RP1 UART0 Interrupt Validation

Date: 2026-11-28
Branch: `feat/aarch64-rp1-irq`

## 1. Host & Static Verification (PASS)

- `make PROFILE=x86_64-clang test-aarch64-rp1-irq-host CASE=handoff`: PASS (`test_aarch64_uefi_rp1_pci` 24/24, `test_bootinfo_abi`, `test_aarch64_boot_layout` 10291/10291)
- `make PROFILE=x86_64-clang validate-uefi` & `BOOTAA64.EFI`: PASS (x86 UEFI excludes RP1 protocol code)
- `make PROFILE=x86_64-clang test-aarch64-rp1-irq-host CASE=topology`: PASS (`test_aarch64_rp1_topology` 53/53, `test_aarch64_rp1_xhci` 32/32)
- `make PROFILE=x86_64-clang test-aarch64-rp1-irq-host CASE=mip`: PASS (`test_aarch64_bcm2712_mip` 66/66, `test_gic_driver` 199/199)
- `make PROFILE=x86_64-clang test-aarch64-pcie-host`: PASS
- `make PROFILE=x86_64-clang test-aarch64-rp1-irq-host CASE=irq`: PASS (`test_aarch64_rp1_irq` 55/55)
- `make PROFILE=x86_64-clang test-aarch64-rp1-irq-host CASE=console`: PASS (`test_aarch64_rp1_console` 28/28, `OS01_SELFTEST` compile check clean)
- `make PROFILE=x86_64-clang check-test-catalog test-host test-static`: PASS

## 2. QEMU `qemu-virt` Regression (PASS)

- `make PROFILE=aarch64-clang AARCH64_PLATFORM=qemu-virt test-aarch64 MODE=gic-spi`: PASS
- `make PROFILE=aarch64-clang AARCH64_PLATFORM=qemu-virt test-aarch64-pcie`: PASS (`ecam-virt`, `ahci-virt`, `no-ecam`)
- QEMU `qemu-virt` UEFI serial console smoke (`echo QEMU-CONSOLE-IRQ-OK`): PASS (`PL011 console RX armed intid=33` and exclusive command output verified)

## 3. RPi 5 Diagnostic Images (Built & Probe-Injected)

### IRQ Diagnostic Image (`AARCH64_RP1_IRQ_DIAGNOSTIC=1`)

- Build: `make PROFILE=aarch64-clang AARCH64_PLATFORM=rpi5 AARCH64_BUILD_ROOT=/tmp/os01-rp1-irq-board KERNEL_EXTRA_CFLAGS=-DAARCH64_RP1_IRQ_DIAGNOSTIC=1 aarch64-uefi`
- Probe injection: `usb_boot.inject_root_probe("/tmp/os01-rp1-irq-board/rpi5-irq-test.img", "/tmp/os01-rp1-irq-board/artifacts/user/rp1_irq_probe.elf", "rp1-irq-probe")`
- SHA-256:
  - `BOOTAA64.EFI`: `f2a241fa02bd00bb5a524bd934d5c68c10464b0bfae4f24b4c50e0032a54f820`
  - `kernel.elf`: `ef526880ee4cb30eac9f3a524bf29a645d73c187aa361f2d2f4be4ebabe5893a`
  - `rp1_irq_probe.elf`: `fdf13d3f2e3dd8a180d21d3d92203fbe676f39bd5f12faa688f1f530af6855af`
  - `rpi5-irq-test.img`: `9975f068e4548668b6cd4b0e81fa6546d5fa57566636e5c25cfb33ad7164f9e0`

### Forced-Poll Diagnostic Image (`AARCH64_RP1_IRQ_DIAGNOSTIC=1 -DAARCH64_RP1_IRQ_FORCE_POLL=1`)

- Build: `make PROFILE=aarch64-clang AARCH64_PLATFORM=rpi5 AARCH64_BUILD_ROOT=/tmp/os01-rp1-poll-board KERNEL_EXTRA_CFLAGS="-DAARCH64_RP1_IRQ_DIAGNOSTIC=1 -DAARCH64_RP1_IRQ_FORCE_POLL=1" aarch64-uefi`
- Probe injection: `usb_boot.inject_root_probe("/tmp/os01-rp1-poll-board/rpi5-poll-test.img", "/tmp/os01-rp1-poll-board/artifacts/user/rp1_irq_probe.elf", "rp1-irq-probe")`
- SHA-256:
  - `BOOTAA64.EFI`: `8d21618d8a0501b66e754db29e8a2411c6ddfa1e5e42e7a308c06e37c6f55f7f`
  - `kernel.elf`: `ed04b05812a7cd83182e5d68f766e9355ed3b5b58efe4393fde0297c20e3e344`
  - `rp1_irq_probe.elf`: `fdf13d3f2e3dd8a180d21d3d92203fbe676f39bd5f12faa688f1f530af6855af`
  - `rpi5-poll-test.img`: `594297ae0956b4ec6a353541aacc9a51b0f4f1df6f8bfe853ff5b6f37e974d51`

## 4. RPi 5 Hardware Cold-Boot Validation: **PASS (IRQ Armed & Interactive Console Verified)**

Physical RPi 5 (`boardrev c04170`, UEFI v0.3) cold-boot with `console rp1-header` verified on `/dev/ttyUSB0` (`115200 8N1`):

- `UEFI-A64: layout entry=0x40080000 context=0x37f43000 size=240` (RP1 `rev=0x00` PCI BAR0 capture succeeded)
- `[rp1-irq-topology] rc=0x0x1000120000 mip=0x0x1000130000 source=25 spi_base=128 count=64 offset=0`
- `[rp1-irq-bar0] bdf=01:00.0 pci=0x0xc0410000 cpu=0x0x1f00410000 size=0x0x4000 table=0x0 pba=0x2000`
- `[rp1-irq-window] action=program slot=4 pci=0x0xfffffff000 cpu=0x0x1000130000 span=0x0x1000 readback=ok`
- `[rp1-irq-msix] source=25 address=0x0xfffffff000 data=0 intid=160 probe=ok`
- `aarch64: PL011 console RX armed via RP1 source=25 intid=160` (`console_rx_poll_needed=false`)
- BusyBox `ash` serial interactive command `echo RP1-IRQ-CONSOLE-OK` received, executed, and echoed via the armed RP1 MSI-X IRQ path.

Optional A/B diagnostic `rp1_irq_probe` counter runs (`AARCH64_RP1_IRQ_DIAGNOSTIC=1` `irq` vs `poll` modes):

```bash
python3 tools/tests/aarch64_rp1_irq_board.py --port /dev/ttyUSB0 --image /tmp/os01-rp1-irq-board/rpi5-irq-test.img --log /tmp/rpi5-irq.log --mode irq
python3 tools/tests/aarch64_rp1_irq_board.py --port /dev/ttyUSB0 --image /tmp/os01-rp1-poll-board/rpi5-poll-test.img --log /tmp/rpi5-poll.log --mode poll
```
