#!/usr/bin/env bash
# Shared RED-2 (link-hole) evidence pipeline for the libc completion RED
# targets (test-libc-path-red / test-libc-dir-red).  Copy the shipped
# libc.a, delete the archive members the task added, link a probe against
# the result and against the pristine archive (CONTROL), and prove:
#   - the behavioural RED (RED-1, driven by the caller) FAILED;
#   - the CONTROL link SUCCEEDED (so the only variable is the member);
#   - the RED link FAILED (the hole is reproduced).
# This is evidence reproduction: the script ALWAYS exits non-zero (the
# RED must fail) after printing the exit-code transcript.
#
# Usage (env: SYSROOT OS01_ROOT LLVM_AR LLVM_NM EFFECTIVE_CC TARGET_LD
#        USER_BUILD_DIR — all set by the make recipe):
#   libc_red_link.sh <rc_run> <git-sha> <git-subdir> <src-basename> \
#                    "<member.o [member.o ...]>" "<egrep symbol re>" \
#                    <probe-include-line> <probe-body>
set -u
rc_run=$1; sha=$2; subdir=$3; src=$4; members=$5; symre=$6; include=$7; body=$8

fail() { echo "ERROR: $*" >&2; exit 1; }
: "${SYSROOT:?}" "${OS01_ROOT:?}" "${LLVM_AR:?}" "${LLVM_NM:?}" \
  "${EFFECTIVE_CC:?}" "${TARGET_LD:?}" "${USER_BUILD_DIR:?}"

echo "--- the pre-task tree had no $subdir/$src at all:"
git -C "$OS01_ROOT" ls-tree "$sha" "$subdir/" \
  | awk '{print $4}' | grep -c "^$src\$"

tmp=$(mktemp -d) || fail "mktemp -d"
cp "$SYSROOT/usr/lib/libc.a" "$tmp/libc.a" || fail "cp libc.a"

echo "--- deleting the archive members the task added ($members):"
# shellcheck disable=SC2086 — members is a deliberate word list
"$LLVM_AR" d "$tmp/libc.a" $members

echo "--- task symbols left in the archive (expected: none):"
"$LLVM_NM" -P "$tmp/libc.a" | grep -E "$symre" || true

printf '%s\n' "$include" "$body" > "$tmp/probe.c"
# EFFECTIVE_CC is a command STRING (compiler + base flags) — unquoted on
# purpose, matching the make recipe expansion semantics.
$EFFECTIVE_CC --sysroot="$SYSROOT" -ffreestanding -fno-pic -fno-pie \
  -I"$SYSROOT/usr/include" -c "$tmp/probe.c" -o "$tmp/probe.o" \
  || fail "probe compile"

echo "--- CONTROL: link the probe against the PRISTINE archive:"
"$TARGET_LD" -m elf_x86_64 -static -no-pie -T user/linker.ld \
  -L"$SYSROOT/usr/lib" -o "$tmp/probe_ctrl.elf" \
  "$USER_BUILD_DIR/obj/crt0.o" "$USER_BUILD_DIR/obj/sigreturn_trampoline.o" \
  "$tmp/probe.o" -lc
rc_ctrl=$?
echo "CONTROL exit code: $rc_ctrl"

echo "--- RED-2: link the probe against the archive WITHOUT the task members:"
"$TARGET_LD" -m elf_x86_64 -static -no-pie -T user/linker.ld \
  -L"$tmp" -o "$tmp/probe_red.elf" \
  "$USER_BUILD_DIR/obj/crt0.o" "$USER_BUILD_DIR/obj/sigreturn_trampoline.o" \
  "$tmp/probe.o" -lc
rc_link=$?
echo "RED-2 exit code: $rc_link"

rm -rf "$tmp"

[ "$rc_run" -ne 0 ] || fail "the behavioural RED run passed"
[ "$rc_ctrl" -eq 0 ] || fail "the CONTROL link failed — the RED proves nothing"
[ "$rc_link" -ne 0 ] || fail "the link RED passed — the hole is not reproduced"

echo "RED reproduced: RED-1 exit $rc_run, CONTROL exit $rc_ctrl, RED-2 exit $rc_link"
exit 1
