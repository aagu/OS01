#!/bin/sh
set -eu
profile=$1
mode=$2
base="build/$profile"

# The build-directory assertion (and the x86 sysroot-generations/symlink
# prelude) apply ONLY to modes that consume produced artifacts. Legacy
# scans and parse-time invocations run before, and without, any
# build-directory assertion so the RED gates fail on the static /
# parse-time checks rather than on a missing build dir.
case "$mode" in
legacy-components)
    # Static scan: the four production components must NOT retain a
    # standalone branch (toolchain.mk include, build/$(ARCH) layout,
    # "Legacy standalone" comment, or ifndef OS01_PROFILE_FILE guard).
    ! rg -n 'toolchain\.mk|build/\$\(ARCH\)|Legacy standalone|ifndef OS01_PROFILE_FILE' \
      kernel libc user boot/uefi
    # Parse-time invocations: each must fail at parse time and surface
    # the root PROFILE= interface.
    for d in kernel libc user boot/uefi; do
        log=$(mktemp)
        if make -C "$d" -n >"$log" 2>&1; then
            cat "$log"; rm -f "$log"; exit 1
        fi
        grep -F 'make PROFILE=' "$log"
        rm -f "$log"
        log=$(mktemp)
        if make -C "$d" OS01_PROFILE_FILE=/nonexistent -n >"$log" 2>&1; then
            cat "$log"; rm -f "$log"; exit 1
        fi
        grep -F "OS01_PROFILE_FILE='/nonexistent' does not exist" "$log"
        rm -f "$log"
    done
    ;;
legacy)
    # Same scan + parse-time invocations as legacy-components, extended
    # to include the hosttests/ host-test machinery (Task 4).
    ! rg -n 'toolchain\.mk|build/\$\(ARCH\)|Legacy standalone|ifndef OS01_PROFILE_FILE|hosttests/build|build/test_poll_requested\.elf' \
      kernel libc user boot/uefi hosttests
    for d in kernel libc user boot/uefi hosttests; do
        log=$(mktemp)
        if make -C "$d" -n >"$log" 2>&1; then
            cat "$log"; rm -f "$log"; exit 1
        fi
        grep -F 'make PROFILE=' "$log"
        rm -f "$log"
        log=$(mktemp)
        if make -C "$d" OS01_PROFILE_FILE=/nonexistent -n >"$log" 2>&1; then
            cat "$log"; rm -f "$log"; exit 1
        fi
        grep -F "OS01_PROFILE_FILE='/nonexistent' does not exist" "$log"
        rm -f "$log"
    done
    ;;
*)
    # Modes that consume produced artifacts need a build dir + (for x86
    # profiles) a published sysroot generation.
    case "$mode" in
    x86|sysroot|firmware|host-test)
        test -d "$base"
        if [ "$profile" != "aarch64-clang" ]; then
            test -d "$base/sysroot-generations"
            test -L "$base/sysroot"
            test ! -e "$base/sysroot/data/data/com.termux/files/usr"
        fi
        ;;
    esac
    case "$mode" in
    x86) test -f "$base/artifacts/kernel.bin"; test -f "$base/image/disk.img";
         test -f "$base/artifacts/user/busybox.elf"; test -f "$base/artifacts/user/init.elf";
         test ! -e kernel/arch/x86_64/trampoline.bin;
         test ! -e libc/libc.a;
         test ! -e libc/libk.a;
         test -f "$base/staging/kernel-headers/manifest";
         test -f "$base/staging/libc/manifest" ;;
    aarch64) test -f "$base/artifacts/kernel.elf"; test -f "$base/image/aarch64-uefi.img" ;;
    sysroot) test -f "$base/sysroot/usr/include/core/bootinfo.h"; test -f "$base/sysroot/usr/lib/libc.a";
             test ! -e "$base/sysroot/usr/include/os01-removed-header.h";
             test -f "$base/sysroot/usr/lib/libk.a";
             test -f "$base/sysroot/usr/lib/libmbedtls.a";
             test -f "$base/sysroot/usr/lib/libm.a";
             test -f "$base/sysroot/usr/lib/librt.a";
             test -n "$(ls "$base/sysroot/usr/include/mbedtls"/*.h 2>/dev/null)" ;
             test "$(readlink "$base/sysroot")" = "sysroot-generations/$(find "$base/sysroot-generations" -maxdepth 1 -mindepth 1 -type d | xargs -n1 basename | sort -n | tail -1)" ;
             test -z "$(ls -A "$base/leases" 2>/dev/null)" ;;
    firmware)
        fixture_profile=x86_64-clang-fixture
        test -f disk.img
        # The fixture profile is build-contract-only and profile clean removes
        # only build/<profile>, so a stale fixture build dir survives across
        # runs. Remove it here: the rejection assertions below require the
        # firmware to be genuinely absent (otherwise the real-file rule is up
        # to date and the `! make` rejections invert into failures).
        rm -rf "build/$fixture_profile"
        mkdir -p "build/$fixture_profile"
        sha256sum disk.img | cut -d' ' -f1 > "build/$fixture_profile/root-disk.before"
        fixture=$(mktemp)
        dd if=/dev/zero of="$fixture" bs=4096 count=1 status=none
        # print-run-paths must report the FIXTURE profile's own absolute
        # firmware and image paths, and must not require the firmware to
        # already exist.
        paths=$(make PROFILE="$fixture_profile" OVMF_FIRMWARE_SOURCE="$fixture" print-run-paths)
        echo "$paths" | grep -F "firmware=$(pwd)/build/$fixture_profile/firmware/OVMF.fd"
        echo "$paths" | grep -F "image=$(pwd)/build/$fixture_profile/image/disk.img"
        # Invalid sources are rejected with a clear error BEFORE any download
        # or copy: relative path, missing absolute path, non-HTTPS scheme.
        ! make PROFILE="$fixture_profile" OVMF_FIRMWARE_SOURCE=relative.fd \
          "$(pwd)/build/$fixture_profile/firmware/OVMF.fd"
        ! make PROFILE="$fixture_profile" OVMF_FIRMWARE_SOURCE=/no/such/OVMF.fd \
          "$(pwd)/build/$fixture_profile/firmware/OVMF.fd"
        ! make PROFILE="$fixture_profile" OVMF_FIRMWARE_SOURCE=http://insecure/OVMF.fd \
          "$(pwd)/build/$fixture_profile/firmware/OVMF.fd"
        # The rejections must not have left a partial or completed firmware
        # file behind.
        test ! -e "$(pwd)/build/$fixture_profile/firmware/OVMF.fd"
        make PROFILE="$fixture_profile" OVMF_FIRMWARE_SOURCE="$fixture" \
          "$(pwd)/build/$fixture_profile/firmware/OVMF.fd"
        make PROFILE="$fixture_profile" disk.img
        test -f "build/$fixture_profile/firmware/OVMF.fd"
        test ! -e boot/uefi/OVMF.fd
        test "$(sha256sum disk.img | cut -d' ' -f1)" = "$(cat build/$fixture_profile/root-disk.before)"
        test -f "build/$fixture_profile/image/disk.img"
        rm -f "$fixture"

        # HTTPS branch: prepend a fake wget that refuses non-HTTPS URLs,
        # writes a 4 KiB file to -O, and records the URL. Removing the
        # existing fixture firmware and re-invoking the target forces
        # the HTTPS download branch without any network access.
        fake_dir=$(mktemp -d)
        cat >"$fake_dir/wget" <<'EOF'
#!/bin/sh
url=
out=
while [ $# -gt 0 ]; do
    case "$1" in
        -O) out=$2; shift 2;;
        --) shift; break;;
        -*) shift;;
        *) url=$1; shift;;
    esac
done
case "$url" in
    https://*) ;;
    *) echo "fake-wget: refusing non-HTTPS URL: $url" >&2; exit 2;;
esac
printf 'fake-wget-url=%s\n' "$url" >>"$FAKE_WGET_LOG"
dd if=/dev/zero of="$out" bs=4096 count=1 status=none
EOF
        chmod +x "$fake_dir/wget"
        rm -f "build/$fixture_profile/firmware/OVMF.fd"
        FAKE_WGET_LOG="$fake_dir/url.log" PATH="$fake_dir:$PATH" \
            make PROFILE="$fixture_profile" \
              "$(pwd)/build/$fixture_profile/firmware/OVMF.fd"
        test -f "$fake_dir/url.log"
        head -1 "$fake_dir/url.log" | grep -E '^fake-wget-url=https://'
        rm -rf "$fake_dir"

        # ── UEFI runtime env contract: receipt invalidation ──────────
        # Changing any of the four contract inputs (UEFI_RUNTIME_CFLAGS,
        # UEFI_RUNTIME_MAKE, adapter/wrapper input paths) must invalidate
        # the runtime receipt, print "runtime input changed, recopying" and
        # rebuild the profile-private runtime — while no OS01 source file or
        # submodule file is modified. The normal build immediately after the
        # fixtures uses the default real input paths again.
        uefi_artifact="$(pwd)/build/$profile/artifacts/uefi/BOOTX64.EFI"
        uefi_receipt="$(pwd)/build/$profile/receipts/uefi-runtime.stamp.receipt"
        fixture_adapter=$(mktemp)
        fixture_wrapper=$(mktemp)
        cat mk/components/uefi.mk >"$fixture_adapter"
        cat boot/uefi/Makefile >"$fixture_wrapper"
        printf '\n# uefi-contract fixture: adapter copy (never the executing file)\n' >>"$fixture_adapter"
        printf '\n# uefi-contract fixture: wrapper copy (never the executing file)\n' >>"$fixture_wrapper"
        src_before=$(git status --porcelain | sort)
        recopy_case() {
            label=$1
            shift
            old=$(cat "$uefi_receipt" 2>/dev/null || true)
            log=$(mktemp)
            # MAKEOVERRIDES= on the root command line: GNU make otherwise
            # auto-encodes command-line variable definitions into MAKEFLAGS,
            # splitting a space-containing value into separate words; a
            # fragment like "-DFIXTURE" then leaks through os01_submake's
            # option filter into the boot wrapper's make invocation (invalid
            # option -D). MAKEOVERRIDES= keeps the fixture value out of
            # MAKEFLAGS while still defining it as a command-line variable.
            if ! make PROFILE="$profile" MAKEOVERRIDES= "$@" "$uefi_artifact" >"$log" 2>&1; then
                cat "$log"; rm -f "$log"; exit 1
            fi
            grep -F 'runtime input changed, recopying' "$log" >/dev/null || { cat "$log"; rm -f "$log"; exit 1; }
            new=$(cat "$uefi_receipt" 2>/dev/null || true)
            test -n "$new" && test "$new" != "$old" || { cat "$log"; rm -f "$log"; exit 1; }
            rm -f "$log"
            echo "  [uefi-contract] $label invalidated the runtime receipt"
        }
        recopy_case 'UEFI_RUNTIME_CFLAGS'         UEFI_RUNTIME_CFLAGS='-DUEFI_NO_UTF8 -DFIXTURE'
        recopy_case 'UEFI_RUNTIME_MAKE'           UEFI_RUNTIME_MAKE='make OUTDIR= FIXTURE=1'
        recopy_case 'UEFI_RUNTIME_ADAPTER_INPUT'  UEFI_RUNTIME_ADAPTER_INPUT="$fixture_adapter"
        recopy_case 'UEFI_RUNTIME_WRAPPER_INPUT'  UEFI_RUNTIME_WRAPPER_INPUT="$fixture_wrapper"
        # The normal build immediately after the fixtures uses the default
        # real input paths again; the receipt must flip once more.
        old=$(cat "$uefi_receipt")
        log=$(mktemp)
        if ! make PROFILE="$profile" "$uefi_artifact" >"$log" 2>&1; then
            cat "$log"; rm -f "$log"; exit 1
        fi
        grep -F 'runtime input changed, recopying' "$log" >/dev/null || { cat "$log"; rm -f "$log"; exit 1; }
        test "$(cat "$uefi_receipt")" != "$old" || { cat "$log"; rm -f "$log"; exit 1; }
        rm -f "$log"
        echo "  [uefi-contract] normal build reverted to the real adapter/wrapper inputs"
        # No fixture run may have touched any OS01 source file or the
        # posix-uefi submodule worktree.
        test "$(git status --porcelain | sort)" = "$src_before"
        test -z "$(git -C thirdpart/posix-uefi status --porcelain)"
        rm -f "$fixture_adapter" "$fixture_wrapper"
        ;;
    sysroot-headers)
        # Header-level incremental rebuild (roadmap Parked refinement):
        # kernel compiles reference libc headers through the STABLE
        # $(SYSROOT) symlink, so a republish over a one-header edit must
        # rebuild exactly the dependents — ordinary timestamp deps, not
        # the old generation-id -B hammer — and the .cflags fingerprint
        # stamp must stay put (compile flags go through the symlink too).
        # stdlib.h: enough kernel dependents to prove propagation, far
        # from all objects to prove incrementality.
        stamp="$base/kernel/.cflags"
        hdr=libc/include/stdlib.h
        marker=$(mktemp)
        src_before=$(git status --porcelain | sort)
        trap 'rm -f "$marker"; git checkout -- '"$hdr"'' EXIT

        make PROFILE="$profile" kernel.bin >/dev/null
        stamp_before=$(cat "$stamp" 2>/dev/null || true)
        total=$(find "$base/kernel" -name '*.o' -not -path '*/runtime/*' | wc -l)
        test "$total" -gt 0 || { echo "sysroot-headers: no kernel objects after warm build" >&2; exit 1; }

        touch "$marker"
        printf '\n/* sysroot-headers contract probe */\n' >> "$hdr"
        make PROFILE="$profile" kernel.bin >/dev/null
        n=$(find "$base/kernel" -name '*.o' -not -path '*/runtime/*' -newer "$marker" | wc -l)
        test "$n" -gt 0 || { echo "sysroot-headers: header edit rebuilt nothing" >&2; exit 1; }
        test "$n" -lt "$total" || { echo "sysroot-headers: header edit rebuilt everything ($n/$total) — genid -B hammer still active" >&2; exit 1; }
        test "$(cat "$stamp")" = "$stamp_before" || { echo "sysroot-headers: .cflags stamp moved on republish — flags must go through the stable symlink" >&2; exit 1; }

        # Restore the header; the rebuild leaves the tree consistent again.
        git checkout -- "$hdr"
        make PROFILE="$profile" kernel.bin >/dev/null
        test "$(git status --porcelain | sort)" = "$src_before"
        echo "  [sysroot-headers] header-level incremental rebuild contract holds"
        ;;
    flags-cache)
        # CFLAGS-only cache invalidation (roadmap Parked item): make tracks
        # file timestamps, not compile commands. A flag-only change must
        # still recompile the kernel objects (the $(BUILD_DIR)/.cflags stamp
        # is the invalidation carrier); an identical rebuild must recompile
        # nothing. kernel/runtime/ is excluded everywhere: the runtime
        # builtins archive compiles with its own RUNTIME_CFLAGS_kernel, not
        # the kernel's ALL_CFLAGS, so it legitimately neither participates
        # in the stamp nor stays timestamp-frozen across sub-makes.
        stamp="$base/kernel/.cflags"
        marker=$(mktemp)
        src_before=$(git status --porcelain | sort)
        trap 'rm -f "$marker"' EXIT

        make PROFILE="$profile" kernel.bin >/dev/null
        before=$(cat "$stamp" 2>/dev/null || true)
        test -n "$before" || { echo "flags-cache: .cflags stamp missing after a normal kernel build" >&2; exit 1; }

        # Identical rebuild: nothing may recompile.
        touch "$marker"
        make PROFILE="$profile" kernel.bin >/dev/null
        if find "$base/kernel" -name '*.o' -not -path '*/runtime/*' -newer "$marker" | grep -q .; then
            echo "flags-cache: identical rebuild recompiled kernel objects" >&2
            exit 1
        fi

        # Flag-only change: stamp must move and objects must recompile.
        touch "$marker"
        make PROFILE="$profile" KERNEL_EXTRA_CFLAGS=-DOS01_FLAGS_CACHE_PROBE kernel.bin >/dev/null
        after=$(cat "$stamp")
        test "$after" != "$before" || { echo "flags-cache: stamp did not move on flag change" >&2; exit 1; }
        n=$(find "$base/kernel" -name '*.o' -not -path '*/runtime/*' -newer "$marker" | wc -l)
        test "$n" -gt 0 || { echo "flags-cache: flag-only change recompiled nothing ($stamp moved, objects stale)" >&2; exit 1; }

        # Flip back: stamp must move again and objects must recompile again.
        touch "$marker"
        make PROFILE="$profile" kernel.bin >/dev/null
        test "$(cat "$stamp")" != "$after" || { echo "flags-cache: stamp did not move on flag revert" >&2; exit 1; }
        n=$(find "$base/kernel" -name '*.o' -not -path '*/runtime/*' -newer "$marker" | wc -l)
        test "$n" -gt 0 || { echo "flags-cache: flag revert recompiled nothing" >&2; exit 1; }

        # The mode must not touch any tracked source file.
        test "$(git status --porcelain | sort)" = "$src_before"
        echo "  [flags-cache] CFLAGS-only invalidation contract holds"
        ;;
    host-test)
        # The focused poll-test binary lives under the profile's
        # host-test dir, not under hosttests/build or root-level build/.
        test -f "$base/host-test/test_poll_requested.elf"
        test ! -e hosttests/build
        test ! -e build/test_poll_requested.elf
        # The hard `test -f` above already failed if the binary is absent.
        "$base/host-test/test_poll_requested.elf"
        make PROFILE="$profile" clean
        test ! -d "$base/host-test"
        ;;
    targets) make -n PROFILE=x86_64-clang kernel.bin disk.img lib user validate test-qemu SUITE=systest >/dev/null
             make -n PROFILE=x86_64-clang run >/dev/null
             make -n PROFILE=aarch64-clang aarch64-uefi >/dev/null
             make -n PROFILE=aarch64-clang test-aarch64 MODE=smp >/dev/null
             ! make -n PROFILE=aarch64-clang user
             ! make -n PROFILE=aarch64-clang run
             ! make -n PROFILE=aarch64-clang test-qemu SUITE=systest
             ! make -n PROFILE=x86_64-clang aarch64-uefi
             ! make -n PROFILE=x86_64-clang test-aarch64 MODE=smp
             ! make -n PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=0 test-aarch64 MODE=no-ack
             # aarch64 targets must not pull BusyBox/sysroot into ARM builds.
             dry_aarch64=$(make -n PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi-kernel)
             ! echo "$dry_aarch64" | grep -q busybox
             ! echo "$dry_aarch64" | grep -q sysroot-generations
             # ── aarch64 EL1 sync fault isolation (spec §5) ───────
             # test-aarch64 MODE=sync-fault must select the dedicated
             # variant (kernel/image under sync-fault/, not selftest/)
             # and propagate the fault injection flag through the
             # controlled sub-make boundary into the kernel compile.
             # `|| true` swallows make's exit-2 (the umbrella's
             # _test-aarch64-run-sync-fault target is Task 3's harness
             # and intentionally absent here — only its build prep
             # matters for the contract).
             drain_sf=$(make -n PROFILE=aarch64-clang test-aarch64 MODE=sync-fault 2>&1 || true)
             echo "$drain_sf" | grep -qF "KERNEL_SELFTEST=1 " \
               && echo "$drain_sf" | grep -qF "AARCH64_SYNC_FAULT_TEST=1 " \
               && echo "$drain_sf" | grep -qF "aarch64-uefi" \
               || { echo "targets: sync-fault prep did not select KERNEL_SELFTEST=1+AARCH64_SYNC_FAULT_TEST=1+aarch64-uefi" >&2; exit 1; }
             echo "$drain_sf" | grep -F "kernel/sync-fault/" >/dev/null \
               || { echo "targets: sync-fault did not isolate kernel build dir" >&2; exit 1; }
             echo "$drain_sf" | grep -F "image/sync-fault/" >/dev/null \
               || { echo "targets: sync-fault did not isolate image build dir" >&2; exit 1; }
             echo "$drain_sf" | grep -F "image/selftest/aarch64-uefi.img" >/dev/null \
               && { echo "targets: sync-fault must NOT reuse the selftest image path" >&2; exit 1; }
             # x86 profile must reject the MODE: no aarch64-sync-test is
             # ever built for x86_64, so the umbrella gate must fail it
             # cleanly (parse-time error inside test-aarch64).
             ! make -n PROFILE=x86_64-clang test-aarch64 MODE=sync-fault
             # Normal smp mode must not include the fault injection flag.
             drain_smp=$(make -n PROFILE=aarch64-clang test-aarch64 MODE=smp)
             ! echo "$drain_smp" | grep -q AARCH64_SYNC_FAULT_TEST=1
             ;;
    *) exit 64 ;;
    esac
    ;;
esac
