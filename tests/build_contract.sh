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