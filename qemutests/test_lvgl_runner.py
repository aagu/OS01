#!/usr/bin/env python3
import subprocess, sys, time, os, re, selectors
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
paths = subprocess.check_output(['make', '-s', 'PROFILE=x86_64-clang', 'print-run-paths'], text=True, cwd=ROOT)
fw = re.search(r'^firmware=(.*)$', paths, re.M).group(1)
img = re.search(r'^image=(.*)$', paths, re.M).group(1)

log_path = ROOT / "build/x86_64-clang/test_lvgl_qemu.log"
log_path.parent.mkdir(parents=True, exist_ok=True)
log_file = open(log_path, "w", encoding="utf-8")

cmd = [
    'qemu-system-x86_64', '-M', 'q35',
    '-drive', f'if=pflash,format=raw,readonly=on,file={fw}',
    '-drive', f'file={img},format=raw,if=none,id=disk',
    '-device', 'ahci,id=ahci',
    '-device', 'ide-hd,drive=disk,bus=ahci.0',
    '-object', 'rng-random,filename=/dev/urandom,id=rng0',
    '-device', 'virtio-rng-pci,rng=rng0',
    '-m', '512', '-smp', '1',
    '-serial', 'stdio', '-display', 'none',
    '-no-reboot', '-no-shutdown'
]

proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
sel = selectors.DefaultSelector()
sel.register(proc.stdout, selectors.EVENT_READ)

buffer = ""
try:
    # 阶段 1：等待 Shell 就绪提示符 "# "（上限 25s）
    ready = False
    boot_deadline = time.monotonic() + 25.0
    while True:
        remaining = boot_deadline - time.monotonic()
        if remaining <= 0:
            break
        events = sel.select(timeout=min(remaining, 0.5))
        if events:
            chunk = os.read(proc.stdout.fileno(), 4096)
            if not chunk:
                break
            text = chunk.decode('utf-8', errors='replace').replace('\r', '')
            buffer += text
            sys.stdout.write(text)
            sys.stdout.flush()
            log_file.write(text)
            log_file.flush()
            if buffer.endswith("# ") or "\n# " in buffer:
                ready = True
                break

    if not ready:
        print("\nERROR: Boot timeout (25s) before seeing '# ' shell prompt", file=sys.stderr)
        sys.exit(1)

    # 握手验证：在启动阶段剩余预算内，发送拼接命令以消除终端回显误判
    proc.stdin.write(b"printf '__SHELL_%s__\\n' CANARY\n")
    proc.stdin.flush()
    
    handshake_ok = False
    handshake_buf = ""
    while time.monotonic() < boot_deadline:
        remaining = boot_deadline - time.monotonic()
        if remaining <= 0:
            break
        events = sel.select(timeout=min(remaining, 0.2))
        if events:
            chunk = os.read(proc.stdout.fileno(), 4096)
            if not chunk:
                break
            text = chunk.decode('utf-8', errors='replace').replace('\r', '')
            handshake_buf += text
            sys.stdout.write(text)
            sys.stdout.flush()
            log_file.write(text)
            log_file.flush()
            # 严格断言在命令发送后出现独立整行 __SHELL_CANARY__
            if re.search(r'(^|\n)__SHELL_CANARY__(\n|$)', handshake_buf):
                handshake_ok = True
                break
    if not handshake_ok:
        print("\nERROR: Shell failed to execute canary handshake within boot deadline", file=sys.stderr)
        sys.exit(1)

    # 阶段 2：注入测试命令并等待测试执行结果（独立上限 10s）
    proc.stdin.write(b'/bin/test_lvgl\necho LVGL_EXIT_CODE:$?\n')
    proc.stdin.flush()

    test_pass = False
    exit_code_zero = False
    exec_buf = ""
    exec_deadline = time.monotonic() + 10.0

    while True:
        remaining = exec_deadline - time.monotonic()
        if remaining <= 0:
            break
        events = sel.select(timeout=min(remaining, 0.5))
        if events:
            chunk = os.read(proc.stdout.fileno(), 4096)
            if not chunk:
                break
            text = chunk.decode('utf-8', errors='replace').replace('\r', '')
            exec_buf += text
            sys.stdout.write(text)
            sys.stdout.flush()
            log_file.write(text)
            log_file.flush()
            if "[TEST PASS] LVGL compatibility smoke test succeeded." in exec_buf:
                test_pass = True
            if re.search(r'(^|\n)LVGL_EXIT_CODE:0(\n|$)', exec_buf):
                exit_code_zero = True
            if test_pass and exit_code_zero:
                break

    if not test_pass:
        print("\nERROR: [TEST PASS] message not found within execution deadline", file=sys.stderr)
        sys.exit(1)
    if not exit_code_zero:
        print("\nERROR: LVGL_EXIT_CODE:0 not found within execution deadline", file=sys.stderr)
        sys.exit(1)

    print("\nQEMU headless test verified successfully.")

finally:
    log_file.close()
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
