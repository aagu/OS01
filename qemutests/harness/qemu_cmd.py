"""Single QEMU command-line builder for OS01 x86_64 and aarch64 suites."""

from __future__ import annotations

from collections.abc import Sequence

_RNG_PAIR: tuple[str, ...] = (
    "-object", "rng-random,filename=/dev/urandom,id=rng0",
    "-device", "virtio-rng-pci,rng=rng0",
)


def _resolve_serial(serial: str, serial_path: str | None) -> str:
    if serial == "stdio":
        return "stdio"
    if serial == "file":
        return f"file:{serial_path or 'serial.log'}"
    if serial.startswith("file:"):
        return serial
    raise ValueError(f"Unsupported serial mode: {serial!r}")


def build_qemu_x86(
    *,
    firmware: str,
    image: str,
    smp: int | str,
    serial: str = "stdio",
    serial_path: str | None = None,
    snapshot: bool = False,
    machine: str = "q35",
    memory: int | str = "512",
    qemu: str = "qemu-system-x86_64",
    extra: Sequence[str] = (),
) -> list[str]:
    """Build an x86_64 QEMU argv list with the paired virtio-rng device."""
    serial_arg = _resolve_serial(serial, serial_path)
    if machine == "pc":
        disk_args = [
            "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
            "-drive", f"file={image},format=raw,if=none,id=disk",
            "-device", "virtio-blk-pci,drive=disk",
        ]
    else:
        disk_args = [
            "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
            "-drive", f"file={image},format=raw,if=none,id=disk",
            "-device", "ahci,id=ahci",
            "-device", "ide-hd,drive=disk,bus=ahci.0",
        ]

    cmd = [
        str(qemu),
        "-M", machine,
        *disk_args,
        *_RNG_PAIR,
        "-m", str(memory),
        "-smp", str(smp),
        "-serial", serial_arg,
        "-display", "none",
        "-no-reboot",
        "-no-shutdown",
    ]
    if snapshot:
        cmd.append("-snapshot")
    if extra:
        cmd.extend(extra)
    return cmd


def build_qemu_aarch64(
    *,
    firmware: str,
    image: str,
    cpus: int | str,
    dtb: str | None,
    snapshot: bool = False,
    memory: int | str = "512",
    qemu: str = "qemu-system-aarch64",
    extra: Sequence[str] = (),
) -> list[str]:
    """Build an aarch64 QEMU argv list with the paired virtio-rng device."""
    machine = "virt,gic-version=2" + (",acpi=off" if dtb else "")
    cmd = [
        str(qemu),
        "-M", machine,
        "-cpu", "cortex-a53",
        "-smp", str(cpus),
        "-m", str(memory),
        "-drive", f"if=pflash,format=raw,file={firmware}",
        "-drive", f"if=none,file={image},format=raw,readonly=on,id=disk",
        "-device", "virtio-blk-device,drive=disk",
        *_RNG_PAIR,
        "-serial", "stdio",
        "-display", "none",
        "-no-reboot",
        "-no-shutdown",
    ]
    if dtb:
        cmd.extend(["-dtb", dtb])
    if snapshot:
        cmd.append("-snapshot")
    if extra:
        cmd.extend(extra)
    return cmd
