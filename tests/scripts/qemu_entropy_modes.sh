#!/usr/bin/env bash
# tests/scripts/qemu_entropy_modes.sh — AAGU-5 entropy facade 三档验证编排（v5）
#
# 用法：tests/scripts/qemu_entropy_modes.sh [ARCH] [MODE]
#   ARCH = x86_64 (默认) | aarch64
#   MODE = NONE | WEAK | STRONG | all（默认 all）
#
# 通过白名单 env vars 触发 build system variant 化（Task 3.5）：
#   - KERNEL_SELFTEST=1 → variant=selftest (build/<profile>/kernel/selftest/)
#   - KERNEL_TEST_FORCE_NO_RNDRRS=1 + KERNEL_SELFTEST=1 → variant=weak-selftest
#     (build/<profile>/kernel/weak-selftest/)
# 无任何 env 覆盖 / CFLAGS 透传 / KERNEL_BUILD_DIR 覆盖。

set -euo pipefail

ARCH="${1:-x86_64}"
MODE="${2:-all}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROFILE="${ARCH}-clang"

# 每个 mode 对应一组 (label | qemu_cpu_flag | kernel_build_subdir | extra_make_env)
make_mode_entry() {
    local label="$1" cpu="$2" subdir="$3" extra_env="$4"
    echo "$label|$cpu|$subdir|$extra_env|entropy_quality_selftest_current_mode"
}

declare -a MODES=()
case "$ARCH:$MODE" in
    x86_64:all)
        # v8: KERNEL_SELFTEST=1 → variant=selftest → kernel/selftest/kernel.bin
        # 不能用空 subdir（否则启动 kernel//kernel.bin 不存在）
        MODES=(
            "$(make_mode_entry NONE   qemu64                selftest '')"
            "$(make_mode_entry WEAK   qemu64,+rdrand        selftest '')"
            "$(make_mode_entry STRONG qemu64,+rdrand,+rdseed selftest '')"
        ) ;;
    aarch64:all)
        # aarch64 三模式都跑真实 UEFI image（mk/components/run.mk:153 模板）；
        # NONE / STRONG 用 regular selftest variant；WEAK 用 weak-selftest variant。
        MODES=(
            "$(make_mode_entry NONE   cortex-a53 selftest '')"
            "$(make_mode_entry WEAK   max         weak-selftest 'KERNEL_TEST_FORCE_NO_RNDRRS=1')"
            "$(make_mode_entry STRONG max         selftest '')"
        ) ;;
    *:NONE)
        cpu="$([[ $ARCH == x86_64 ]] && echo qemu64 || echo cortex-a53)"
        MODES=("$(make_mode_entry NONE "$cpu" selftest '')") ;;
    *:WEAK)
        cpu="$([[ $ARCH == x86_64 ]] && echo qemu64,+rdrand || echo max)"
        # v8: x86_64 WEAK uses selftest variant (KERNEL_SELFTEST=1); aarch64 uses weak-selftest + flag
        if [[ $ARCH == x86_64 ]]; then
            MODES=("$(make_mode_entry WEAK "$cpu" selftest '')")
        else
            MODES=("$(make_mode_entry WEAK "$cpu" weak-selftest 'KERNEL_TEST_FORCE_NO_RNDRRS=1')")
        fi ;;
    *:STRONG)
        cpu="$([[ $ARCH == x86_64 ]] && echo qemu64,+rdrand,+rdseed || echo max)"
        MODES=("$(make_mode_entry STRONG "$cpu" selftest '')") ;;
    *) echo "MODE must be NONE | WEAK | STRONG | all (got: $MODE)" >&2; exit 2 ;;
esac

SELFTEST_NAME="entropy_quality_selftest_current_mode"
LOG_DIR="${LOG_DIR:-/tmp/aagu5_entropy_${ARCH}_$$}"
mkdir -p "$LOG_DIR"
trap 'rm -rf "$LOG_DIR"' EXIT

# build_kernel_for_variant：调 build system，whitelisted env vars 让 build 系统
# 自己决定 variant 与 build dir。不覆盖 KERNEL_BUILD_DIR，不透传 CFLAGS。
build_kernel_for_variant() {
    local subdir="$1" extra_env="$2"
    local build_dir="$REPO_ROOT/build/${PROFILE}/kernel/${subdir}"

    # v8: image path 同步 run_qemu() 的 variant-aware 计算 — 避免 build 与 run 分叉
    # 导致 WEAK 重建后 run_qemu 才发现 image 缺失，或反过来。
    local image_subdir=""
    if [[ "$subdir" == "weak-selftest" ]]; then
        image_subdir="/weak-selftest"
    fi
    local image_path="$REPO_ROOT/build/${PROFILE}/image${image_subdir}/aarch64-uefi.img"
    local firmware_path="$REPO_ROOT/build/${PROFILE}/image${image_subdir}/QEMU_EFI.fd"

    local kernel_bin="$build_dir/kernel.bin"
    local needs_build=0
    [[ ! -f "$kernel_bin" ]] && needs_build=1
    # aarch64 需要 UEFI image + firmware
    if [[ "$ARCH" == "aarch64" ]]; then
        [[ ! -f "$image_path" ]] && needs_build=1
        [[ ! -f "$firmware_path" ]] && needs_build=1
    fi
    if [[ "$needs_build" -eq 0 ]]; then
        return 0
    fi

    echo "Building ${subdir} variant for $ARCH..."
    env $extra_env KERNEL_SELFTEST=1 \
        make -C "$REPO_ROOT" PROFILE="$PROFILE" all >/dev/null 2>&1 || true
    if [[ ! -f "$kernel_bin" ]]; then
        echo "ERROR: kernel build failed for $PROFILE variant=$subdir" >&2
        echo "Hint: extra_env was '$extra_env'; check KERNEL_TEST_FORCE_NO_RNDRRS whitelisted" >&2
        return 1
    fi
    if [[ "$ARCH" == "aarch64" ]]; then
        env $extra_env KERNEL_SELFTEST=1 \
            make -C "$REPO_ROOT" PROFILE="$PROFILE" aarch64-uefi >/dev/null 2>&1 || true
        if [[ ! -f "$image_path" ]]; then
            echo "ERROR: aarch64-uefi image build failed at $image_path" >&2
            return 1
        fi
        if [[ ! -f "$firmware_path" ]]; then
            echo "ERROR: QEMU_EFI.fd not found at $firmware_path" >&2
            return 1
        fi
    fi
    return 0
}

# 直接调 QEMU
run_qemu() {
    local kernel_path="$1" cpu="$2" log="$3"
    if [[ "$ARCH" == "x86_64" ]]; then
        timeout 30 qemu-system-x86_64 -M q35 -smp 1 -m 256M \
            -kernel "$kernel_path" -nographic -no-reboot \
            -cpu "$cpu" \
            -append "OS01_KERNEL_SELFTEST=1" \
            > "$log" 2>&1 || true
    else
        # v6: variant-aware image path（与 KERNEL_BUILD_DIR 同步）
        local subdir="$4"
        local image_subdir=""
        if [[ "$subdir" == "weak-selftest" ]]; then
            image_subdir="/weak-selftest"
        fi
        local image="$REPO_ROOT/build/${PROFILE}/image${image_subdir}/aarch64-uefi.img"
        local firmware="$REPO_ROOT/build/${PROFILE}/image${image_subdir}/QEMU_EFI.fd"
        if [[ ! -f "$image" || ! -f "$firmware" ]]; then
            echo "ERROR: aarch64 UEFI image/firmware not found at image${image_subdir}/" >&2
            return 1
        fi
        local extra_dtb=""
        if [[ "${AARCH64_UEFI_SMP_DIAGNOSTIC_DTB:-1}" != "0" ]]; then
            local dtb_dir="$LOG_DIR/dtb"
            mkdir -p "$dtb_dir"
            local sparse="$dtb_dir/qemu-virt.dtb.sparse"
            local packed="$dtb_dir/qemu-virt.dtb"
            qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 \
                -machine "dumpdtb=$sparse" -display none -m 256M >/dev/null 2>&1 || true
            if command -v dtc >/dev/null && [[ -f "$sparse" ]]; then
                dtc -I dtb -O dtb -o "$packed" "$sparse" || true
                rm -f "$sparse"
                extra_dtb="-dtb $packed"
            fi
        fi
        timeout 45 qemu-system-aarch64 -M virt,gic-version=2${extra_dtb:+,acpi=off} \
            -cpu "$cpu" -smp 1 -m 256M \
            -drive if=pflash,format=raw,readonly=on,file="$firmware" \
            -drive if=none,file="$image",format=raw,readonly=on,id=disk \
            -device virtio-blk-device,drive=disk \
            $extra_dtb \
            -serial stdio -display none -no-reboot \
            > "$log" 2>&1 || true
    fi
}

fail=0
for entry in "${MODES[@]}"; do
    IFS='|' read -r label cpu subdir extra_env selftest <<< "$entry"
    log="$LOG_DIR/${label}.log"
    kernel_bin="$REPO_ROOT/build/${PROFILE}/kernel/${subdir}/kernel.bin"

    # Build (whitelisted env vars only — no KERNEL_BUILD_DIR or CFLAGS override)
    if ! build_kernel_for_variant "$subdir" "$extra_env"; then
        echo "FAIL: $label — build for variant=$subdir failed"
        fail=1
        continue
    fi

    echo "=== $ARCH $label (cpu=$cpu, variant=$subdir) ==="
    if ! run_qemu "$kernel_bin" "$cpu" "$log" "$subdir"; then
        echo "FAIL: $label — QEMU invocation failed"
        fail=1
        continue
    fi

    # 1. selftest PASS
    if ! grep -qE "\[selftest\] ${SELFTEST_NAME}\.\.\. PASS" "$log"; then
        echo "FAIL: $label — selftest did not PASS"
        tail -10 "$log"
        fail=1
        continue
    fi

    # 2. CSPRNG seed log 匹配 quality 档
    case "$label" in
        NONE)   expect_log="CSPRNG: no hardware entropy source" ;;
        WEAK)   expect_log="CSPRNG: pool seeded WEAK" ;;
        STRONG) expect_log="CSPRNG: pool seeded STRONG" ;;
    esac
    if ! grep -qE "$expect_log" "$log"; then
        echo "FAIL: $label — expected CSPRNG log '$expect_log' not found"
        tail -10 "$log"
        fail=1
        continue
    fi

    # 3. NONE / WEAK 档必须确认"没有 UEFI GetRNG 早返回"
    if [[ "$label" == "NONE" || "$label" == "WEAK" ]]; then
        if grep -qE "CSPRNG: pool seeded STRONG from UEFI GetRNG" "$log"; then
            echo "FAIL: $label — UEFI GetRNG bypassed facade (boot_entropy set unexpectedly)"
            tail -10 "$log"
            fail=1
            continue
        fi
    fi

    # 4. variant 校验 — 启动的 kernel 必须匹配期望 variant
    case "$label" in
        NONE|STRONG) expect_variant="selftest" ;;
        WEAK)        expect_variant="weak-selftest" ;;
    esac
    if ! grep -qE "CSPRNG: kernel build variant=${expect_variant}\b" "$log"; then
        echo "FAIL: $label — kernel build variant mismatch (expected ${expect_variant})"
        grep "CSPRNG: kernel build variant" "$log" || echo "  (no variant log line found)"
        fail=1
        continue
    fi

    echo "PASS: $ARCH $label"
done

if [[ $fail -eq 0 ]]; then
    echo "=== All modes PASS for $ARCH ==="
fi
exit $fail
