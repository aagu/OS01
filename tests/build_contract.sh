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
    # to include the test/ host-test machinery (Task 4).
    ! rg -n 'toolchain\.mk|build/\$\(ARCH\)|Legacy standalone|ifndef OS01_PROFILE_FILE|test/build|build/test_poll_requested\.elf' \
      kernel libc user boot/uefi test
    for d in kernel libc user boot/uefi test; do
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
    sysroot) test -f "$base/sysroot/usr/include/kernel/bootinfo.h"; test -f "$base/sysroot/usr/lib/libc.a";
             test ! -e "$base/sysroot/usr/include/os01-removed-header.h";
             test -f "$base/sysroot/usr/lib/libk.a";
             test -f "$base/sysroot/usr/lib/libmbedtls.a";
             test -f "$base/sysroot/usr/lib/libm.a";
             test -f "$base/sysroot/usr/lib/librt.a";
             test -n "$(ls "$base/sysroot/usr/include/mbedtls"/*.h 2>/dev/null)" ;
             test "$(readlink "$base/sysroot")" = "sysroot-generations/$(find "$base/sysroot-generations" -maxdepth 1 -mindepth 1 -type d | xargs -n1 basename | sort -n | tail -1)" ;
             test -z "$(ls -A "$base/leases" 2>/dev/null)" ;;
    firmware)
        default_profile=x86_64-clang
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
    host-test)
        # The focused poll-test binary lives under the profile's
        # host-test dir, not under test/build or root-level build/.
        test -f "$base/host-test/test_poll_requested.elf"
        test ! -e test/build
        test ! -e build/test_poll_requested.elf
        # Run the focused binary if it exists (Task 4 may not have
        # built it yet; the path assertions above already fail RED).
        if [ -f "$base/host-test/test_poll_requested.elf" ]; then
            "$base/host-test/test_poll_requested.elf"
        fi
        make PROFILE="$profile" clean
        test ! -d "$base/host-test"
        ;;
    targets) make -n PROFILE=x86_64-clang kernel.bin disk.img lib user validate test-syscall >/dev/null
             make -n PROFILE=x86_64-clang run >/dev/null
             make -n PROFILE=aarch64-clang aarch64-uefi >/dev/null
             ! make -n PROFILE=aarch64-clang user
             ! make -n PROFILE=aarch64-clang run
             ! make -n PROFILE=aarch64-clang test-syscall
             ! make -n PROFILE=x86_64-clang aarch64-uefi ;;
    *) exit 64 ;;
    esac
    ;;
esac