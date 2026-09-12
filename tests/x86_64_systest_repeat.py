#!/usr/bin/env python3
"""Run systest repeatedly from the real terminal/ash path (not systest init).

An isolated disk copy keeps filesystem tests from modifying the normal image.
A shell printf completion marker is split from its numeric argument so the
serial echo of the command cannot satisfy the completion check.
"""
import argparse
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--smp', type=int, default=4)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--log', type=Path, default=Path('/tmp/os01-systest-repeat.log'))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='os01-repeat-', dir='/tmp') as td:
        disk = Path(td) / 'disk.img'
        shutil.copyfile(args.disk, disk)
        cmd = ['qemu-system-x86_64', '-M', 'q35', '-m', '512M', '-smp', str(args.smp),
               '-drive', f'if=pflash,format=raw,readonly=on,file={args.firmware}',
               '-drive', f'file={disk},format=raw,if=none,id=disk',
               '-device', 'ahci,id=ahci', '-device', 'ide-hd,drive=disk,bus=ahci.0',
               '-display', 'none', '-serial', 'stdio', '-no-reboot']
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, bufsize=0)
        selector = selectors.DefaultSelector()
        selector.register(proc.stdout, selectors.EVENT_READ)
        commands = [(b'systest', 1), (b'for i in 1..3; do systest; done', 1),
                    (b'for i in 1 2 3; do systest; done', 3)]
        stage = -1
        output = b''
        deadline = time.monotonic() + args.timeout
        try:
            with args.log.open('wb') as log:
                while time.monotonic() < deadline:
                    for key, _ in selector.select(1):
                        block = key.fileobj.read(65536)
                        if not block:
                            raise RuntimeError(f'QEMU exited before completion; see {args.log}')
                        log.write(block)
                        log.flush()
                        output += block
                    ready = stage == -1 and b'built-in shell (ash)' in output and b'# ' in output
                    if stage >= 0 and f'__REPEAT_DONE_{stage}__'.encode() in output:
                        results = re.findall(rb'\[SYS TEST\] RESULT: (\d+) passed, (\d+) failed', output)
                        expected = commands[stage][1]
                        if len(results) != expected or any(int(p) == 0 or int(f) != 0 for p, f in results):
                            raise RuntimeError(f'stage {stage}: expected {expected} passing suites, got {results}; see {args.log}')
                        print(f'SMP={args.smp}, stage={stage}: {expected} suites passed', flush=True)
                        ready = True
                    if ready:
                        stage += 1
                        if stage == len(commands):
                            return 0
                        proc.stdin.write(commands[stage][0] + b"; printf '\\n__REPEAT_DONE_%d__\\n' " + str(stage).encode() + b'\n')
                        output = b''
                raise RuntimeError(f'timed out at stage {stage}; see {args.log}')
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            selector.close()


if __name__ == '__main__':
    raise SystemExit(main())
