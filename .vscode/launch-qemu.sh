#!/usr/bin/env bash

set -u

gdb_listener() {
    ss -ltnH 'sport = :1234' | grep -q .
}

if gdb_listener; then
    echo 'GDB port 1234 is already in use' >&2
    exit 1
fi

echo OS01_QEMU_STARTING
setsid make DEBUG=1 KERNEL_SELFTEST=1 \
    DEBUG_CHANNELS=sched,tty,vfs,mm,fs,net,ipc,drivers,init,syscall \
    DISPLAY="${OS01_QEMU_DISPLAY:-gtk}" debug &
debug_pid=$!

cleanup() {
    kill -- "-$debug_pid" 2>/dev/null || true
    wait "$debug_pid" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

while kill -0 "$debug_pid" 2>/dev/null; do
    if gdb_listener; then
        echo OS01_QEMU_GDB_READY
        wait "$debug_pid"
        exit $?
    fi
    sleep 0.2
done

wait "$debug_pid"
