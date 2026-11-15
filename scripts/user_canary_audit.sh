#!/usr/bin/env bash
# The seven user-canary checks (test-user-canary / test-static), verbatim
# from the pre-2025-11 recipe body — one audit command, one archived
# result.  Env: LLVM_NM SYSROOT USER_ARTIFACT_DIR ROOTFS_MANIFEST (set by
# the make recipe); CWD must be the repository root.
set -e
: "${LLVM_NM:?}" "${SYSROOT:?}" "${USER_ARTIFACT_DIR:?}" "${ROOTFS_MANIFEST:?}"

echo "[user-canary] 1/7 libc.a defines guard+fail"
test -n "$("$LLVM_NM" -P "$SYSROOT/usr/lib/libc.a" | awk '$1=="__stack_chk_guard" && ($2=="B" || $2=="D")')" \
  || { echo "ERROR: __stack_chk_guard not defined (B/D) in $SYSROOT/usr/lib/libc.a"; exit 1; }
test -n "$("$LLVM_NM" -P "$SYSROOT/usr/lib/libc.a" | awk '$1=="__stack_chk_fail" && $2=="T"')" \
  || { echo "ERROR: __stack_chk_fail not defined (T) in $SYSROOT/usr/lib/libc.a"; exit 1; }

echo "[user-canary] 2/7 libk.a does NOT define them (kernel owns its own)"
test -z "$("$LLVM_NM" -P "$SYSROOT/usr/lib/libk.a" | awk '$1=="__stack_chk_guard" || $1=="__stack_chk_fail"')" \
  || { echo "ERROR: libk.a must not carry SSP symbols"; exit 1; }

echo "[user-canary] 3/7 user ELFs link SSP in (T __stack_chk_fail)"
for e in systest canary_dump canary_smash; do
  "$LLVM_NM" -P "$USER_ARTIFACT_DIR/$e.elf" \
    | awk '$1=="__stack_chk_fail" && $2=="T" {f=1} END {exit !f}' \
    || { echo "ERROR: $e.elf has no resolved __stack_chk_fail"; exit 1; }
done

echo "[user-canary] 4/7 busybox.elf links SSP in"
"$LLVM_NM" -P "$USER_ARTIFACT_DIR/busybox.elf" \
  | awk '$1=="__stack_chk_fail" && $2=="T" {f=1} END {exit !f}' \
  || { echo "ERROR: busybox.elf has no resolved __stack_chk_fail"; exit 1; }

echo "[user-canary] 5/7 SSP flags in all three compile switches"
for f in libc/Makefile user/Makefile config/busybox.config.in; do
  grep -q -- "-fstack-protector-strong" "$f" \
    || { echo "ERROR: $f lacks -fstack-protector-strong"; exit 1; }
done
if grep -v "^LIBK_CFLAGS" libc/Makefile | grep -q -- "-fno-stack-protector"; then
  echo "ERROR: stray -fno-stack-protector in libc/Makefile (non-LIBK line)"; exit 1
fi

echo "[user-canary] 6/7 probe programs staged in rootfs manifest"
for b in canary_dump canary_smash; do
  grep -q "/bin/$b" "$ROOTFS_MANIFEST" \
    || { echo "ERROR: /bin/$b missing from $ROOTFS_MANIFEST"; exit 1; }
done

echo "[user-canary] 7/7 crt0 overlay byte-identity invariant"
cmp -s user/crt0.S config/busybox.overlay/applets/crt0.S \
  || { echo "ERROR: user/crt0.S and busybox overlay crt0.S diverged"; exit 1; }

echo "[user-canary] audit passed"
