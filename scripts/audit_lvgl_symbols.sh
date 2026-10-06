#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build/x86_64-clang"
LVGL_OBJ_DIR="$BUILD_DIR/liblvgl/obj"
LIBC_A="$BUILD_DIR/staging/libc/usr/lib/libc.a"
OUT_DIR="$BUILD_DIR/audit"
mkdir -p "$OUT_DIR"

echo "[audit] Merging all liblvgl objects into $OUT_DIR/lvgl_combined.o..."
OBJS=$(find "$LVGL_OBJ_DIR" -name '*.o')
ld.lld -r $OBJS -o "$OUT_DIR/lvgl_combined.o"

echo "[audit] Extracting undefined symbols from LVGL combined object..."
llvm-nm --undefined-only "$OUT_DIR/lvgl_combined.o" | awk '{print $2}' | sort -u > "$OUT_DIR/lvgl_undef.txt"
llvm-nm --defined-only "$LIBC_A" | awk '{print $3}' | sort -u > "$OUT_DIR/libc_def.txt"

echo "[audit] Checking for forbidden math and pthread symbols..."
FORBIDDEN=$(grep -E '^(cosf|sinf|tanf|roundf|powf|sqrtf|pthread_|sem_)' "$OUT_DIR/lvgl_undef.txt" || true)
if [ -n "$FORBIDDEN" ]; then
  echo "ERROR: Forbidden symbols detected:"
  echo "$FORBIDDEN"
  exit 1
fi

echo "[audit] Checking set difference: all undefined symbols must be in libc_def..."
DIFF=$(comm -23 "$OUT_DIR/lvgl_undef.txt" "$OUT_DIR/libc_def.txt")
if [ -n "$DIFF" ]; then
  echo "ERROR: Undefined symbols not provided by libc.a:"
  echo "$DIFF"
  exit 1
fi

echo "[audit] Performing relocatable link with OS01 libc.a..."
ld.lld -r "$OUT_DIR/lvgl_combined.o" "$LIBC_A" -o "$OUT_DIR/lvgl_libc_combined.o"

REMAINING=$(llvm-nm --undefined-only "$OUT_DIR/lvgl_libc_combined.o")
if [ -n "$REMAINING" ]; then
  echo "ERROR: Unresolved symbols remain after linking with libc.a:"
  echo "$REMAINING"
  exit 1
fi

echo "[audit] SUCCESS: Zero unresolved symbols remain. Symbol audit passed."
