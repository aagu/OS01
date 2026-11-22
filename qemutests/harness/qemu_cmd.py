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
    storage: str = "ahci",
    ahci_image: str | None = None,
    usb_speed: str = "2.0",
    usb_readonly: bool = True,
    usb_hub: bool = False,
    usb_input: str = "off",
) -> list[str]:
    """Build an x86_64 QEMU argv list with the paired virtio-rng device.

    ``storage`` selects the root-disk topology:
    - ``"ahci"`` (default): the historical q35 AHCI or ``-machine pc``
      virtio-blk disk.
    - ``"usb"``: a USB-only topology (qemu-xhci + usb-storage) with no
      implicit AHCI/IDE/virtio disk, used by the USB boot gate.
    - ``"dual"``: both USB (``bootindex=0``, readonly) and AHCI
      (``bootindex=1``, writable ``ahci_image``) attached.
    """
    serial_arg = _resolve_serial(serial, serial_path)
    if storage not in ("ahci", "usb", "dual"):
        raise ValueError(f"Unsupported storage mode: {storage!r}")
    if usb_speed not in ("2.0", "3.0"):
        raise ValueError(f"Unsupported USB speed: {usb_speed!r}")
    if usb_hub and (storage != "usb" or usb_speed != "2.0"):
        raise ValueError("usb_hub=True requires storage='usb' and usb_speed='2.0'")
    if usb_input not in ("off", "direct", "hub"):
        raise ValueError(f"Unsupported usb_input mode: {usb_input!r}")
    if usb_input == "hub" and (storage != "usb" or not usb_hub):
        raise ValueError("usb_input='hub' requires storage='usb' and usb_hub=True")
    if usb_input != "off" and "i8042=off" not in machine.split(","):
        machine = f"{machine},i8042=off"
    xhci_props = (
        "qemu-xhci,id=xhci,p2=4,p3=0"
        if usb_speed == "2.0"
        else "qemu-xhci,id=xhci,p2=0,p3=4"
    )
    usb_drive_spec = f"file={image},format=raw,if=none,id=usbdisk"
    if usb_readonly:
        usb_drive_spec += ",readonly=on"
    if storage == "dual":
        if not ahci_image:
            raise ValueError("storage='dual' requires ahci_image")
        disk_args = [
            "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
            "-drive", usb_drive_spec,
            "-drive", f"file={ahci_image},format=raw,if=none,id=ahcidisk",
            "-device", xhci_props,
            "-device", "usb-storage,drive=usbdisk,bus=xhci.0,port=1,bootindex=0",
            "-device", "ahci,id=ahci",
            "-device", "ide-hd,drive=ahcidisk,bus=ahci.0,bootindex=1",
        ]
    elif storage == "usb":
        if usb_hub:
            usb_devs = [
                "-device", "usb-hub,bus=xhci.0,port=1",
                "-device", "usb-storage,drive=usbdisk,bus=xhci.0,port=1.2,bootindex=0",
            ]
        else:
            usb_devs = [
                "-device", "usb-storage,drive=usbdisk,bus=xhci.0,port=1,bootindex=0",
            ]
        disk_args = [
            "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
            "-drive", usb_drive_spec,
            "-device", xhci_props,
            *usb_devs,
        ]
    elif machine == "pc":
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

    if usb_input == "direct":
        if storage == "ahci":
            disk_args.extend(["-device", xhci_props])
        disk_args.extend([
            "-device", "usb-kbd,id=kbd,bus=xhci.0,port=2,usb_version=2",
            "-device", "usb-mouse,id=mouse,bus=xhci.0,port=3,usb_version=2",
        ])
    elif usb_input == "hub":
        disk_args.extend([
            "-device", "usb-kbd,id=kbd,bus=xhci.0,port=1.3,usb_version=1",
            "-device", "usb-mouse,id=mouse,bus=xhci.0,port=1.4,usb_version=1",
        ])

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
