# ── Kernel artifact contract ─────────────────────────────────
# Cross-component owner of the kernel artifact (spec: kernel.mk). Wired in
# Task 4 when the sysroot generation publisher (sysroot.mk) exists.
#
# Consumes: the sysroot generation stamp published by sysroot.mk (kernel
# headers staged by `lib` → kernel-headers, libc/libk staged by `lib` → libc)
# plus the profile configuration. Until then (Task 3) the kernel is built by
# kernel/Makefile directly through the profile build dir, reading the staged
# libc headers/libs.
#
# Produces: $(BUILD_DIR)/artifacts/kernel.bin (built by kernel/Makefile from
# the profile's KERNEL_BUILD_DIR; published as a root-level kernel.bin compat
# artifact by the root Makefile).
#
# Deliberately empty in Task 3: artifact rules are added in Task 4 together
# with the sysroot generation stamp they must depend on.
