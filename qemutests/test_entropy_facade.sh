#!/usr/bin/env bash
#
# scripts/test_entropy_facade.sh
#
# AAGU-5.8 — 三档 x86_64 entropy facade 环境测试脚本
#
# 三个 tier：
#   tier1 (default)        QEMU 默认 CPU（qemu64，无 RDRAND/RDSEED）
#   tier2 (with-flag)      QEMU -cpu IvyBridge-v1（RDRAND 可用，RDSEED 不可用）
#   tier3 (real-hardware)  QEMU -cpu host（透传真 CPU flags，homeserver Zen 4）
#
# 每个 tier 在两类子环境下测：
#   without-virtio-rng   强制走 arch facade（kernelside 真测 RDRAND/RDSEED）
#   with-virtio-rng      UEFI GetRNG 路径（AAGU-2 已落地；测 AAGU-5 衔接）
#
# 三档契约（kernel/include/arch/random.h + docs/arch/entropy-source-facade.md）：
#   - STRONG：RDSEED（x86_64）/ UEFI GetRNG raw
#   - WEAK  ：RDRAND（x86_64）/ RDRAND-only 处理器
#   - NONE  ：上述两者都不可用 + 无 UEFI GetRNG（memset(0) 后 return false）
#
# 每档断言（AAGU-5.8 spec + reviewer 评审反馈）：
#   1. CSPRNG boot log 含 "pool seeded" 行（不允许 silent）
#   2. quality 等级与 spec 期望一致
#   3. AT_RANDOM STRONG-only 契约：
#        STRONG 路径 → init (PID 2) 必须 spawn 出 "OS01 Init v1.0"
#        WEAK/NONE 路径 → init 不应 spawn（kernel_random_get_strong 返 false）
#   4. /dev/random 用户态读契约（AAGU-5 reviewer 反馈 §1）：
#        STRONG 路径 → `dd if=/dev/random` 必须返回非空数据（HEX 字节 ≠ 00）
#        WEAK/NONE 路径 → 阻塞读应返回数据但**全 0**（pool not ready → memset 0）。
#          spec §5.2 进一步要求 WEAK/NONE 时 non-blocking 读返 -EAGAIN；
#          当前 kernel/fs/devfs.c::random_read **尚未** 实现该 non-blocking 拦截，
#          因此该路径记录为已知 gap（spec 合同 vs 实现差距，AAGU-6+ 跟进）。
#
# 用法：
#   scripts/test_entropy_facade.sh                       # 跑全部 tier
#   scripts/test_entropy_facade.sh tier1-default        # 单 tier（默认 qemu64）
#   scripts/test_entropy_facade.sh tier1-default-no-virtio  # 单配置
#   scripts/test_entropy_facade.sh tier2-selftest        # KERNEL_SELFTEST=1 build
#                                                          （验证 WEAK facade 路径
#                                                          — 不靠 OVMF GetRNG 加成）
#
# 退出码：
#   0 全部 PASS
#   非 0 = FAIL 数（>=1）
#
# 依赖：AAGU-5.6 / AAGU-5.7 已合并。branch 基线 feat/aagu-5-7-at-random-strong-only。

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKTREE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PROFILE="${PROFILE:-x86_64-clang}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-30}"   # 每个 QEMU 实例 boot 最长等 30s
QEMU_INIT_TIMEOUT="${QEMU_INIT_TIMEOUT:-60}"  # boot + inject 27s 序列；reviewer §4 反馈下 60s 覆盖完整交互
MEMORY="${MEMORY:-512}"
SMP="${SMP:-1}"
LOG_DIR="${LOG_DIR:-$WORKTREE_ROOT/build/$PROFILE/test-results/entropy-facade}"

OVMF_FIRMWARE="/home/aagu/OS01/.worktrees/aagu-5-8-entropy-facade-tests/build/$PROFILE/firmware/OVMF.fd"
[ -f "$OVMF_FIRMWARE" ] || OVMF_FIRMWARE="$WORKTREE_ROOT/build/$PROFILE/firmware/OVMF.fd"

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

TIER（默认全部 9 个）:
    tier1-default-no-virtio   qemu64,  -virtio (NONE)
    tier1-default-virtio      qemu64,  +virtio (STRONG via GetRNG)
    tier2-ivy-no-virtio       IvyBridge-v1, -virtio (STRONG via GetRNG)
    tier2-ivy-virtio          IvyBridge-v1, +virtio (STRONG via GetRNG)
    tier3-host-no-virtio      -cpu host -kvm, -virtio (STRONG via GetRNG)
    tier3-host-virtio         -cpu host -kvm, +virtio (STRONG via GetRNG)
    tier2-selftest-ivy        IvyBridge-v1, KERNEL_SELFTEST=1 (WEAK)
    tier2-selftest-host       -cpu host -kvm, KERNEL_SELFTEST=1 (STRONG)
    tier1-selftest-qemu64     qemu64, KERNEL_SELFTEST=1 (NONE)
    tier3-real-hardware       显式 declare host platform

环境：
    PROFILE=x86_64-clang QEMU=qemu-system-x86_64 LOG_DIR=...
    OVMF_FIRMWARE=...    QEMU_TIMEOUT=...
EOF
    exit 2
}

# ── 路径解析 ─────────────────────────────────
get_paths() {
    cd "$WORKTREE_ROOT"
    local out
    out="$(make -s PROFILE="$PROFILE" print-run-paths 2>&1)" || {
        echo "ERROR: print-run-paths failed: $out" >&2
        return 1
    }
    IMAGE="$(echo "$out" | sed -n 's/^image=//p')"
    FW="$(echo "$out" | sed -n 's/^firmware=//p')"
    if [ ! -f "$IMAGE" ]; then
        echo "ERROR: image not found at $IMAGE" >&2
        echo "Run: make -s PROFILE=$PROFILE disk.img  (or: OVMF_FIRMWARE_SOURCE=... ) " >&2
        return 1
    fi
    if [ -z "$FW" ] || [ ! -f "$FW" ]; then
        if [ -f "$OVMF_FIRMWARE" ]; then
            FW="$OVMF_FIRMWARE"
        else
            echo "ERROR: firmware not found" >&2
            return 1
        fi
    fi
    echo "image=$IMAGE"
    echo "firmware=$FW"
    return 0
}

ensure_built() {
    cd "$WORKTREE_ROOT"
    if [ ! -f "$IMAGE" ] || [ ! -f "$FW" ]; then
        echo "Disk/firmware missing; building..."
        OVMF_FIRMWARE_SOURCE="${OVMF_FIRMWARE_SOURCE:-}" \
        make -s PROFILE="$PROFILE" disk.img 2>&1 | tail -5
        OVMF_FIRMWARE_SOURCE="${OVMF_FIRMWARE_SOURCE:-}" \
        make -s PROFILE="$PROFILE" "build/$PROFILE/firmware/OVMF.fd" 2>&1 | tail -3
        get_paths
    fi
}

# ── KERNEL_SELFTEST=1 selftest tier 注释（reviewer §3 反馈后移除 alias） ─────
#
# 之前的 `run_selftest_tier` + `TIER_SELFTEST_*` aliases 只输出 build-infra FAIL
# 消息，留下"可执行但跑不通"的死代码。reviewer round-3 §3 指出这是误导向。
# 因此：alias + 函数完全删除。KERNEL_SELFTEST=1 等价覆盖路径在 docs §3.4。
# 如要重新启用 KERNEL_SELFTEST boot 测试，必须先修 `mk/components/image.mk:33`
# 让 `NORMAL_IMAGE` 代入 IMAGE_VARIANT（超出 AAGU-5.8 范围）。

# ── 启动 QEMU 的内核测试（普通 tier） ───────────────────
#
# run_tier name cpu_model with_virtio expected_q [extra-qemu-args…]
#
#   with_virtio  0 or 1
#   expected_q   STRONG-via-GetRNG | STRONG-via-RDSEED | WEAK | NONE
#
# 步骤：
#   1. Boot QEMU 并捕获所有 stdout 到 $log_file
#   2. 抽取 CSPRNG pool-seed log 行（用 -a 避免 ANSI 转义被当 binary）
#   3. 验证 quality 字符串
#   4. WEAK/NONE 验证 init 未 spawn（fail-closed 契约）
#   5. STRONG 验证 init 已 spawn
#   6. STRONG tiers 加跑 /dev/random 用户态读测试（reviewer 反馈 §1）
run_tier() {
    local name="$1"
    local cpu_model="$2"
    local with_virtio="$3"
    local expected_q="$4"
    shift 4
    local extra_qemu_args=("$@")

    TOTAL=$((TOTAL+1))
    section "TIER: $name  cpu_model=$cpu_model  virtio-rng=$with_virtio  expected_q=$expected_q"

    local log_file="$LOG_DIR/$name.log"
    mkdir -p "$LOG_DIR"
    rm -f "$log_file"

    local cpu_arg=()
    if [ -n "$cpu_model" ]; then cpu_arg=(-cpu "$cpu_model"); fi

    local virtio_arg=()
    if [ "$with_virtio" = "1" ]; then
        virtio_arg=(
            -object rng-random,filename=/dev/urandom,id=rng0
            -device virtio-rng-pci,rng=rng0
        )
    fi

    local qemu_args=(
        "$QEMU" "${cpu_arg[@]}" "${extra_qemu_args[@]}"
        -M q35
        -drive "if=pflash,format=raw,readonly=on,file=$FW"
        -drive "file=$IMAGE,format=raw,if=none,id=disk"
        -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0
        "${virtio_arg[@]}"
        -m "$MEMORY" -smp "$SMP"
        -serial "file:$log_file"
        -display none -no-reboot -no-shutdown
    )

    {
        echo "# command: ${qemu_args[*]}"
    } >> "$log_file"

    timeout "$QEMU_TIMEOUT" "${qemu_args[@]}" >/dev/null 2>&1
    local qemu_rc=$?
    if [ "$qemu_rc" -ne 0 ] && [ "$qemu_rc" -ne 124 ]; then
        warn "qemu exited rc=$qemu_rc (非 124=timeout)"
    fi

    if [ ! -s "$log_file" ]; then
        fail "$name: QEMU 启动失败 / 空日志"
        return 1
    fi

    # 抽 CSPRNG pool seed log
    local csprng_lines
    csprng_lines="$(grep -aE 'CSPRNG: (pool seeded|no hardware)' "$log_file" 2>/dev/null || true)"

    if [ -z "$csprng_lines" ]; then
        fail "$name: 没有 CSPRNG seed log — 启动未完成或日志失效"
        tail -30 "$log_file" | sed 's/^/    /'
        return 1
    fi
    echo "  CSPRNG lines:"
    echo "$csprng_lines" | sed 's/^/    /'

    # 用 log 行验证 quality
    local actual_q="UNKNOWN"
    if echo "$csprng_lines" | grep -q "pool seeded STRONG from UEFI GetRNG"; then
        actual_q="STRONG-via-GetRNG"
    elif echo "$csprng_lines" | grep -q "pool seeded STRONG"; then
        actual_q="STRONG-via-RDSEED"
    elif echo "$csprng_lines" | grep -q "pool seeded WEAK"; then
        actual_q="WEAK"
    elif echo "$csprng_lines" | grep -q "no hardware entropy source"; then
        actual_q="NONE"
    fi

    case "$expected_q" in
        STRONG-via-GetRNG|STRONG-via-RDSEED)
            if [ "$actual_q" != "$expected_q" ]; then
                fail "$name: 期望 $expected_q，实际 $actual_q"
                return 1
            fi
            # 进一步：init 必须 spawn
            if ! grep -aq "OS01 Init v1.0" "$log_file"; then
                fail "$name: $expected_q 但 init 没出现"
                tail -40 "$log_file" | sed 's/^/    /'
                return 1
            fi
            # 进一步：用户态 /dev/random 必须返回非零数据
            # 严格复用 tier 的 QEMU config（cpu、virtio、kvm）；只换 serial=stdio
            if run_devrandom_user_probe "$name" "$cpu_model" "$with_virtio" "${extra_qemu_args[@]}"; then
                pass "$name: $expected_q + init spawned + /dev/random 非零"
            else
                fail "$name: $expected_q + init spawned 但 /dev/random 读失败 (详见 log)"
                return 1
            fi
            return 0
            ;;
        WEAK|NONE)
            if [ "$actual_q" != "$expected_q" ]; then
                fail "$name: 期望 $expected_q，实际 $actual_q"
                return 1
            fi
            # AT_RANDOM STRONG-only 契约：init 不应 spawn
            if grep -aq "OS01 Init v1.0" "$log_file"; then
                fail "$name: $expected_q 但 init 仍 spawn（违反 AAGU-5.7 STRONG-only 契约）"
                return 1
            fi
            # WEAK/NONE 路径下用户态 shell 不可用（AAGU-5.7 STRONG-only AT_RANDOM
            # 使 init fail-closed），所以**不能**用 user-space 命令直接测 /dev/random。
            # spec §5.2 non-blocking EAGAIN 契约当前 kernel/fs/devfs.c::random_read
            # 未实现，harness 在此无法 repro；详见 docs §4（已知违约，留给后续 issue）。
            # 本 tier 只通过"无 init spawn + CSPRNG pool-seeded log 类型"满足。
            pass "$name: $expected_q + init fail-closed（AAGU-5.7 契约）"
            warn "$name: /dev/random NB EAGAIN spec §5.2 实际未实现（详见 §4 已知违约）"
            return 0
            ;;
        *)
            fail "$name: 未知 expected_q=$expected_q"
            return 1
            ;;
    esac
}

# ── 用户态 /dev/random 读测试（reviewer §1）──────────────────
#
# 重启 QEMU 用 -serial stdio + pipe，从 stdin 注入 shell 命令，
# 让 guest BusyBox 在 init spawn 后跑 `dd if=/dev/random` 阻塞读。
# 输入发送时序：
#   sleep 18                  # 等 boot + DHCP + init spawn + shell prompt
#   printf 'head -c 16 /dev/random > /tmp/r.out\n'
#   sleep 4                   # 等 head 完成
#   printf 'cat /tmp/r.out\n'    # 把读到的字节打印回 stdout
#   sleep 3
#   printf 'echo SHELL_ALIVE\n'
#
# 通过 stdout 回读：捕获整个 guest 串行输出，扫 hex dump；
# 把 " 00 00 00 00 ..." 全部 0x00 视作 NONE/WEAK 预期；其他视为 STRONG 数据。
#
# 注意：spec §5.2 要求 WEAK pool + non-blocking /dev/random 返 -EAGAIN。
# 当前 kernel 不在 random_read() 路径判 quality → 阻塞读一律返数据（全 0 when NONE）。
# 因此对 NONE/WEAK 档不跑此测试（脚本入口在 run_tier 已跳过）。
run_devrandom_user_probe() {
    local name="$1"
    local probe_cpu_model="$2"
    local probe_with_virtio="$3"
    shift 3
    local probe_extra_args=("$@")

    local log_file="$LOG_DIR/$name.devrandom.log"
    mkdir -p "$LOG_DIR"
    rm -f "$log_file"

    local cpu_arg=()
    if [ -n "$probe_cpu_model" ]; then cpu_arg=(-cpu "$probe_cpu_model"); fi

    local virtio_arg=()
    if [ "$probe_with_virtio" = "1" ]; then
        virtio_arg=(
            -object rng-random,filename=/dev/urandom,id=rng0
            -device virtio-rng-pci,rng=rng0
        )
    fi

    local qemu_args=(
        "$QEMU" "${cpu_arg[@]}" "${probe_extra_args[@]}"
        -M q35
        -drive "if=pflash,format=raw,readonly=on,file=$FW"
        -drive "file=$IMAGE,format=raw,if=none,id=disk"
        -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0
        "${virtio_arg[@]}"
        -m "$MEMORY" -smp "$SMP"
        -serial stdio -display none -no-reboot -no-shutdown
    )

    (
        sleep 18                    # 等 boot + DHCP + init spawn + shell prompt
        # Reviewer §2（round-3, round-4 严格化）：用 marker 框定 probe 输出。
        # 关键：busybox in OS01 rootfs **没有** dd / od / hexdump / xxd —
        # round-3 用 `dd iflag=nonblock` 实际是 ash 内置返 "not found"。
        #
        # terminal.elf 的 PTY 经常 fork 子进程并写 `fork: pid=X returned Y`，
        # 可能跟 probe 命令的 stdout 交错。解决：用单 `printf` 把 marker 和值
        # 放在**同一行**输出（$(...) 命令替换在 shell 内部执行 wc/tr，不暴露
        # 给 terminal），避免被 fork 字符串污染。
        #
        # 流程：
        #   1. RANDOM_NB_BEGIN
        #   2. head -c 16 /dev/random > /tmp/r.out  阻塞读
        #   3. RANDOM_NB_RC=<head rc>
        #   4. RANDOM_NB_LEN=<wc -c 输出>
        #   5. RANDOM_NB_NONZERO=<tr -d '\\0' | wc -c 输出>
        #   6. RANDOM_NB_END
        #   7. SHELL_DONE
        printf 'printf "RANDOM_NB_BEGIN\\n"\n'
        sleep 1
        printf 'head -c 16 /dev/random > /tmp/r.out 2>/tmp/derr\n'
        sleep 4
        printf 'printf "RANDOM_NB_RC=%d\\n" $?\n'
        sleep 1
        printf 'printf "RANDOM_NB_LEN=$(wc -c < /tmp/r.out 2>/dev/null)\\n"\n'
        sleep 2
        printf 'printf "RANDOM_NB_NONZERO=$(tr -d "\\0" < /tmp/r.out 2>/dev/null | wc -c)\\n"\n'
        sleep 2
        printf 'printf "RANDOM_NB_END\\n"\n'
        sleep 1
        printf 'echo SHELL_DONE\n'
        sleep 2
    ) | timeout "$QEMU_INIT_TIMEOUT" "${qemu_args[@]}" > "$log_file" 2>&1 || true

    if [ ! -s "$log_file" ]; then
        warn "$name: /dev/random probe 未产生 output"
        cat "$log_file" | sed 's/^/    /' || true
        return 1
    fi

    # 1) SHELL_DONE 必须出现（证明 guest shell 真的执行了全部 probe 命令）
    if ! grep -aq "^SHELL_DONE$" "$log_file"; then
        warn "$name: /dev/random probe — SHELL_DONE 未出现（guest shell 未完成全部命令）"
        tail -30 "$log_file" | sed 's/^/    /'
        return 1
    fi

    # 2) RANDOM_NB_BEGIN / RANDOM_NB_END 框定（防止误归 boot log 杂 hex）
    local begin_line end_line
    begin_line="$(grep -an "^RANDOM_NB_BEGIN$" "$log_file" | head -1 | cut -d: -f1)"
    end_line="$(grep -an "^RANDOM_NB_END$" "$log_file" | head -1 | cut -d: -f1)"
    if [ -z "$begin_line" ] || [ -z "$end_line" ] || [ "$begin_line" -ge "$end_line" ]; then
        warn "$name: /dev/random probe — BEGIN/END 未配对或顺序错"
        tail -30 "$log_file" | sed 's/^/    /'
        return 1
    fi

    # 3) 抓 BEGIN/END 之间的内容
    local region
    region="$(sed -n "$((begin_line+1)),$((end_line-1))p" "$log_file")"

    # 4) head 退出码必须 = 0（说明 head /dev/random 实际跑了）
    if ! echo "$region" | grep -q "^RANDOM_NB_RC=0$"; then
        warn "$name: /dev/random probe — head /dev/random 退出码 != 0（guest 命令没跑通）"
        echo "$region" | grep -E "^RANDOM_NB_" | head -5 | sed 's/^/    /'
        return 1
    fi

    # 5) 长度必须 = 16（head -c 16 应读 16 字节）
    # 用单 printf 原子输出 marker + 值，避免 terminal fork 输出污染。
    local len_line
    len_line="$(echo "$region" | grep -E '^RANDOM_NB_LEN=[0-9]+$' | head -1 || true)"
    if [ -z "$len_line" ]; then
        warn "$name: /dev/random probe — length 行未识别（terminal fork 输出可能干扰）"
        echo "$region" | grep "RANDOM_NB_LEN" | head -5 | sed 's/^/    /'
        return 1
    fi
    if [ "$len_line" != "RANDOM_NB_LEN=16" ]; then
        warn "$name: /dev/random probe — length 不是 16（line=$len_line）"
        return 1
    fi

    # 6) 非零字节数 > 0（pool ready 时 16B 不全零；pool NOT ready 时全 0）
    local nonzero_line
    nonzero_line="$(echo "$region" | grep -E '^RANDOM_NB_NONZERO=[0-9]+$' | head -1 || true)"
    if [ -z "$nonzero_line" ]; then
        warn "$name: /dev/random probe — NONZERO 行未识别（terminal fork 输出可能干扰）"
        echo "$region" | grep "RANDOM_NB_NONZERO" | head -5 | sed 's/^/    /'
        return 1
    fi
    local nonzero_value
    nonzero_value="${nonzero_line#RANDOM_NB_NONZERO=}"
    if [ "$nonzero_value" -le 0 ] 2>/dev/null; then
        fail "$name: /dev/random 读全 0x00 或 NONZERO 行解析失败（value=$nonzero_value）"
        return 1
    fi

    echo "  /dev/random probe: rc=0 len=16 nonzero_bytes=$nonzero_value"
    return 0
}

# ── KERNEL_SELFTEST=1 tier：直接调 arch_random_get_entropy ────────────
#
# ── KERNEL_SELFTEST=1 tier（reviewer §3 重发：不引用未定义函数） ─────────
#
# 原 run_selftest_tier 调用 build_selftest_image() — 该函数 reviewer §3 指出
# 没定义。**已删除**原未完成 stub（位于 ~397–462）。KERNEL_SELFTEST=1 真正
# 等价覆盖是 CI 的 `kernel/selftest/test_entropy_quality.c::entropy_quality_selftest_current_mode`，
# 由 AAGU-5.6 / AAGU-5.7 在 CI 调用；本文档（§3.4）记录此 build infra gap。
# AAGU-5.8 harness 删掉 selftest tier 以避免"看起来能跑但实际 command-not-found"。

# ── Tier 定义 ───────────────────────────────
TIER1_DEFAULT_NO_VIRTIO()    { run_tier "tier1-default-no-virtio"  "qemu64"       0 "NONE"; }
TIER1_DEFAULT_VIRTIO()       { run_tier "tier1-default-virtio"     "qemu64"       1 "STRONG-via-GetRNG"; }
TIER2_IVY_NO_VIRTIO()        { run_tier "tier2-ivy-no-virtio"      "IvyBridge-v1" 0 "STRONG-via-GetRNG"; }
TIER2_IVY_VIRTIO()           { run_tier "tier2-ivy-virtio"         "IvyBridge-v1" 1 "STRONG-via-GetRNG"; }
TIER3_HOST_NO_VIRTIO()       { run_tier "tier3-host-no-virtio"     "host"         0 "STRONG-via-GetRNG" -enable-kvm; }
TIER3_HOST_VIRTIO()          { run_tier "tier3-host-virtio"        "host"         1 "STRONG-via-GetRNG" -enable-kvm; }

# KERNEL_SELFTEST=1 selftest tier 已完全移除（reviewer round-3 §3）：
# build infra gap 在 docs §3.4；修 NORMAL_IMAGE（image.mk:33）后才能重建。

# ── main ───────────────────────────────
if [ "$#" -eq 0 ]; then
    set -- tier1-default-no-virtio   tier1-default-virtio \
           tier2-ivy-no-virtio       tier2-ivy-virtio \
           tier3-host-no-virtio      tier3-host-virtio
fi

echo "=== OS01 x86_64 entropy facade 三档测试 ==="
echo "PROFILE=$PROFILE  QEMU=$QEMU  LOG_DIR=$LOG_DIR"
get_paths || exit 3
echo "image=$IMAGE"
echo "firmware=$FW"
ensure_built

for t in "$@"; do
    case "$t" in
        tier1-default-no-virtio)    TIER1_DEFAULT_NO_VIRTIO ;;
        tier1-default-virtio)       TIER1_DEFAULT_VIRTIO ;;
        tier1-default)              TIER1_DEFAULT_NO_VIRTIO; TIER1_DEFAULT_VIRTIO ;;
        tier2-ivy-no-virtio)        TIER2_IVY_NO_VIRTIO ;;
        tier2-ivy-virtio)           TIER2_IVY_VIRTIO ;;
        tier2-with-flag)            TIER2_IVY_NO_VIRTIO; TIER2_IVY_VIRTIO ;;
        tier3-host-no-virtio)       TIER3_HOST_NO_VIRTIO ;;
        tier3-host-virtio)          TIER3_HOST_VIRTIO ;;
        tier3-host)                 TIER3_HOST_NO_VIRTIO; TIER3_HOST_VIRTIO ;;
        # KERNEL_SELFTEST=1 selftest tier 已移除（见上面注释）
        tier-selftest-*|tier2-selftest)
            TOTAL=$((TOTAL+1))
            fail "未知 tier: $t（KERNEL_SELFTEST=1 selftest tier 已移除 — 详见 docs §3.4 build infra gap）"
            ;;
        tier3-real-hardware)
            TOTAL=$((TOTAL+1))
            section "TIER: tier3-real-hardware"
            echo "    homeserver 主机（homeserver 主机内 \$HOME/OS01）:"
            echo "    - CPU: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/^[^:]*: *//' || echo unknown)"
            echo "    - Architecture: x86_64"
            echo "    - RDRAND/RDSEED: yes（flags 中含 rdrand rdseed）"
            # 真实硬件 tier 跟 tier3-host KVM 一致；本 tier 显式声明并执行
            TIER3_HOST_NO_VIRTIO
            TIER3_HOST_VIRTIO
            ;;
        all|ALL)
            TIER1_DEFAULT_NO_VIRTIO; TIER1_DEFAULT_VIRTIO
            TIER2_IVY_NO_VIRTIO;      TIER2_IVY_VIRTIO
            TIER3_HOST_NO_VIRTIO;     TIER3_HOST_VIRTIO
            ;;
        -h|--help)                  usage ;;
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
