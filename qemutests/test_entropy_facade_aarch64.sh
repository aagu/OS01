#!/usr/bin/env bash
#
# scripts/test_entropy_facade_aarch64.sh
#
# AAGU-5.8 — aarch64 entropy facade 环境测试（reviewer 反馈 §2：tier2 必须实际跑）
#
# 三个 tier（按 AAGU-5.8 spec）：
#   tier1 (default)        -cpu cortex-a53（无 RNDR/RNDRRS）
#   tier2 (with-rndrrs)    -cpu max（RNDR + RNDRRS）
#   tier3 (real hardware)  homeserver（AMD Zen 4）无 aarch64 物理硬件；本节显式声明
#
# 重要 — 当前 aarch64 kernel 是 **bring-up**：
#   - `kernel/arch/aarch64/head.S:255` 显式 MIDR_EL1 sanity check 为 Cortex-A53
#     （非 A53 触发 panic — 实测抓得到，会落到 log file 供 reviewer）
#   - `kernel/arch/aarch64/main.c` 是 UEFI handoff + SMP + IRQ 路径，**不**调
#     kernel_main() / random_init()，所以 boot log 不会自然出现 CSPRNG 行
#   - `kernel/Makefile aarch64` source list 是显式枚举（不 wildcard selftest/）
#
# 上记 2/3 条限制让 aarch64 无法通过 boot log 直接断言 STRONG/WEAK/NONE 的
# 契约；reviewer 反馈 §2 要求 tier2 至少**实际跑 QEMU 抓 panic log**，让行为
# 可复现。本脚本按此改：tier2 不再无条件 `fail`，而是真正 `-cpu max` 启动
# 抓 MIDR panic 并记入 log，可复现至 reviewer。
#
# 用法：
#   scripts/test_entropy_facade_aarch64.sh                  # 跑全部
#   scripts/test_entropy_facade_aarch64.sh tier1-default   # 单 tier
#
# 退出码 0 / 非 0 同 x86_64 脚本。
#
# 依赖：AAGU-5.6 / AAGU-5.7 已合并。branch 基线 feat/aagu-5-7-at-random-strong-only。

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKTREE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PROFILE="${PROFILE:-aarch64-clang}"
QEMU="${QEMU:-qemu-system-aarch64}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-30}"
SMP="${SMP:-2}"
MEMORY="${MEMORY:-512}"
LOG_DIR="${LOG_DIR:-$WORKTREE_ROOT/build/$PROFILE/test-results/entropy-facade}"

AARCH64_FIRMWARE="${AARCH64_FIRMWARE:-$HOME/OS01/build/aarch64-clang/image/QEMU_EFI.fd}"
AARCH64_UEFI_SOURCE="${AARCH64_UEFI_FIRMWARE_SOURCE:-$AARCH64_FIRMWARE}"

RED='\033[1;31m'; GRN='\033[1;32m'; YEL='\033[1;33m'; NC='\033[0m'
pass() { echo -e "${GRN}PASS${NC} $*"; }
fail() { echo -e "${RED}FAIL${NC} $*"; FAIL_COUNT=$((FAIL_COUNT+1)); }
warn() { echo -e "${YEL}WARN${NC} $*"; }
section() { echo; echo "── $* ──"; }

FAIL_COUNT=0
TOTAL=0

usage() {
    cat <<EOF
用法: $0 [TIER]...

TIER（默认全部）:
    tier1-default-no-virtio   -cpu cortex-a53, 无 virtio-rng
    tier1-default-virtio      -cpu cortex-a53, +virtio-rng
    tier2-with-rndrrs         -cpu max（实际跑 QEMU 抓 MIDR panic log）
    tier3-real-hardware       显式 declare no-aarch64-hardware

环境：
    PROFILE=aarch64-clang QEMU=qemu-system-aarch64 LOG_DIR=...
EOF
    exit 2
}

ensure_paths() {
    cd "$WORKTREE_ROOT"

    if [ -z "$AARCH64_FIRMWARE" ] || [ ! -f "$AARCH64_FIRMWARE" ]; then
        AARCH64_FIRMWARE="$WORKTREE_ROOT/build/aarch64-clang/image/QEMU_EFI.fd"
    fi
    IMAGE="$WORKTREE_ROOT/build/aarch64-clang/image/aarch64-uefi.img"

    echo "PROFILE=$PROFILE"
    echo "firmware=$AARCH64_FIRMWARE"
    echo "image=$IMAGE"
}

ensure_built() {
    cd "$WORKTREE_ROOT"
    if [ ! -f "$IMAGE" ] || [ ! -f "$AARCH64_FIRMWARE" ]; then
        echo "aarch64 image/firmware missing; building..."
        AARCH64_UEFI_FIRMWARE_SOURCE="$AARCH64_UEFI_SOURCE" \
        AARCH64_UEFI_DISK=image/aarch64-uefi.img \
        make -s PROFILE=aarch64-clang aarch64-uefi 2>&1 | tail -5
        ensure_paths
    fi
}

ensure_dtb() {
    DTB_FILE="$WORKTREE_ROOT/build/aarch64-clang/test-results/qemu-virt.dtb"
    mkdir -p "$(dirname "$DTB_FILE")"
    if [ ! -f "$DTB_FILE" ] || [ "$1" = "force" ]; then
        rm -f "$DTB_FILE" "$DTB_FILE.sparse"
        qemu-system-aarch64 -M virt,gic-version=2,acpi=off -cpu "${2:-cortex-a53}" \
            -smp "$SMP" -m "$MEMORY" \
            -machine "dumpdtb=$DTB_FILE.sparse" -display none 2>/dev/null || true
        if [ -f "$DTB_FILE.sparse" ]; then
            dtc -I dtb -O dtb -o "$DTB_FILE" "$DTB_FILE.sparse" 2>/dev/null || true
            rm -f "$DTB_FILE.sparse"
        fi
    fi
    if [ ! -f "$DTB_FILE" ]; then
        DTB_FILE="$DTB_FILE.sparse"
    fi
    echo "$DTB_FILE"
}

# ── 实际跑 QEMU 抓 panic log（reviewer §2 反馈） ───────────
#
# run_qemu_boot_tier name cpu_model with_virtio expected_marker
#
# 真实启动 QEMU，捕获 stdout/stderr 到 log file。
# 检查 boot 是否达到 expected_marker。
# 同时记录 panic / MIDR check 等行为，便于 reviewer 验证 fail-closed 行为。
run_qemu_boot_tier() {
    local name="$1"
    local cpu_model="$2"
    local with_virtio="$3"
    local expected_marker="$4"

    TOTAL=$((TOTAL+1))
    section "TIER: $name  cpu_model=$cpu_model  virtio-rng=$with_virtio"

    local log_file="$LOG_DIR/$name.log"
    mkdir -p "$LOG_DIR"
    rm -f "$log_file"

    # DTB 必须按当前 cpu_model 重新生成，否则 dtb 内 CPU 节点与 -cpu 不匹配
    local dtb
    dtb="$(ensure_dtb force "$cpu_model")"

    local virtio_arg=()
    if [ "$with_virtio" = "1" ]; then
        virtio_arg=(
            -object rng-random,filename=/dev/urandom,id=rng0
            -device virtio-rng-pci,rng=rng0
        )
    fi

    local qemu_args=(
        "$QEMU" -M virt,gic-version=2,acpi=off
        -cpu "$cpu_model"
        -smp "$SMP"
        -m "$MEMORY"
        -dtb "$dtb"
        -drive if=pflash,format=raw,file="$AARCH64_FIRMWARE"
        -drive if=none,file="$IMAGE",format=raw,readonly=on,id=disk
        -device virtio-blk-device,drive=disk
        "${virtio_arg[@]}"
        -serial "file:$log_file"
        -display none -no-reboot -no-shutdown
    )
    {
        echo "# command: ${qemu_args[*]}"
    } >> "$log_file"

    timeout "$QEMU_TIMEOUT" "${qemu_args[@]}" >/dev/null 2>&1
    local qemu_rc=$?
    if [ "$qemu_rc" -ne 0 ] && [ "$qemu_rc" -ne 124 ]; then
        warn "$name: qemu rc=$qemu_rc（保留真实退出码以便诊断）"
    fi

    if [ ! -s "$log_file" ]; then
        fail "$name: 空日志 (QEMU 启动失败)"
        return 1
    fi

    # 检查关键 boot marker
    if grep -aq "$expected_marker" "$log_file"; then
        pass "$name: boot 通过关键 marker '$expected_marker'"
        return 0
    fi

    # 检查 kernel panic（已知 aarch64 bring-up MIDR_EL1 sanity check 仅 A53 通过）
    # kernel panic 在 head.S 内 halt 时不会写 kernel-level 日志（serial_printk 还没
    # 安装），所以"log 含 UEFI banner + 无 phase1 marker + -cpu 非 A53"就是 panic。
    if grep -aq "MIDR\|panic\|PANIC\|kernel panic" "$log_file"; then
        fail "$name: kernel panic（head.S: MIDR ≠ A53 — 当前 bring-up 不支持 $cpu_model）"
        tail -30 "$log_file" | sed 's/^/    /'
        return 1
    fi

    # 推断 panic：UEFI banner 出，kernel load 完成（FileSystem 找到 kernel.elf），
    # 但 phase1 不在 → kernel 加载成功但 boot 没跑（head.S: panic halt，无 serial log）
    if grep -aq "UEFI\|BdsDxe" "$log_file" && ! grep -aq "phase1" "$log_file"; then
        fail "$name: kernel 加载但未到 phase1 boot（推断 head.S 早期 panic，最常见：MIDR_EL1 ≠ A53）"
        tail -30 "$log_file" | sed 's/^/    /'
        return 1
    fi

    fail "$name: 未到 $expected_marker"
    tail -30 "$log_file" | sed 's/^/    /'
    return 1
}

# ── Tier 定义 ───────────────────────────────
TIER1_DEFAULT_NO_VIRTIO() {
    run_qemu_boot_tier "tier1-default-no-virtio" "cortex-a53" 0 "OS01 aarch64 phase1 boot ok"
}
TIER1_DEFAULT_VIRTIO() {
    run_qemu_boot_tier "tier1-default-virtio" "cortex-a53" 1 "OS01 aarch64 phase1 boot ok"
}
# tier2: -cpu max（RNDR + RNDRRS）。reviewer 反馈 §2 要求实际跑 QEMU
# 抓 log，不要无条件 fail。
TIER2_WITH_RNDRRS() {
    warn "tier2 期望 kernel 跑过 -cpu max（RNDR + RNDRRS），已知触发 head.S:255 MIDR_EL1 sanity check panic。"
    warn "本 tier 现在按 reviewer §2 反馈改为真实跑 QEMU 抓 log，而非无条件 fail。"
    run_qemu_boot_tier "tier2-with-rndrrs" "max" 0 "OS01 aarch64 phase1 boot ok"
}

# ── main ───────────────────────────────
if [ "$#" -eq 0 ]; then
    set -- tier1-default-no-virtio tier1-default-virtio \
           tier2-with-rndrrs tier3-real-hardware
fi

echo "=== OS01 aarch64 entropy facade 测试 ==="
ensure_paths
ensure_built

for t in "$@"; do
    case "$t" in
        tier1-default-no-virtio)  TIER1_DEFAULT_NO_VIRTIO ;;
        tier1-default-virtio)     TIER1_DEFAULT_VIRTIO ;;
        tier1-default)            TIER1_DEFAULT_NO_VIRTIO; TIER1_DEFAULT_VIRTIO ;;
        tier2-with-rndrrs)        TIER2_WITH_RNDRRS ;;
        tier2-with-rndr)          TIER2_WITH_RNDRRS ;;  # alias
        tier3-real-hardware)
            TOTAL=$((TOTAL+1))
            section "TIER: tier3-real-hardware"
            echo "    homeserver 主机（homeserver 主机内 \$HOME/OS01）:"
            echo "    - CPU: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/^[^:]*: *//' || echo unknown)"
            echo "    - Architecture: x86_64"
            echo "    - AArch64 物理硬件：none（homeserver 是单一 x86_64 工作站；无 aarch64 dev board / 没有 ARM dev cloud 接入）"
            fail "tier3-real-hardware: 无 aarch64 物理硬件可测（homeserver 是 x86_64）"
            ;;
        all|ALL)
            TIER1_DEFAULT_NO_VIRTIO; TIER1_DEFAULT_VIRTIO; TIER2_WITH_RNDRRS
            # 注意：tier3-real-hardware 也算进 all（环境声明 FAIL）
            TOTAL=$((TOTAL+1))
            section "TIER: tier3-real-hardware（all 展开）"
            echo "    homeserver CPU: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/^[^:]*: *//' || echo unknown)"
            echo "    AArch64 物理硬件：none（homeserver 是单一 x86_64 工作站）"
            fail "tier3-real-hardware: 无 aarch64 物理硬件可测"
            ;;
        -h|--help)                usage ;;
        *)
            # 未知参数：计入 FAIL（spec 要求显式 fail，不允许 silent skip）
            TOTAL=$((TOTAL+1))
            fail "未知 tier: $t（spec 要求显式 fail）"
            ;;
    esac
done

echo
echo "================================="
echo "Result: $((TOTAL-FAIL_COUNT))/$TOTAL passed (FAIL=$FAIL_COUNT)"
echo "Logs at: $LOG_DIR"
exit "$FAIL_COUNT"
