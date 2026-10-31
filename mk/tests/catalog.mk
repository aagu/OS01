# mk/tests/catalog.mk — Test suite catalog metadata and export
#
# Schema: id profile capability variant cpus memory_mib timeout_s image_policy resource tier entrypoint prepare_target run_target

define TEST_SUITE_CATALOG_DATA
phase-0	x86_64-clang	rootfs	none	1,2	512	60	read-only	shared	pr	test-qemu SUITE=phase-0	_test-suite-prepare	_test-suite-run
systest	x86_64-clang	rootfs	systest	1,2	512	120	isolated	shared	pr	test-qemu SUITE=systest	_test-suite-prepare	_test-suite-run
inittab-phase	x86_64-clang	rootfs	inittab-test	1	512	60	isolated	shared	nightly	test-qemu SUITE=inittab-phase	_test-suite-prepare	_test-suite-run
network	x86_64-clang	rootfs	nettest	1	512	60	isolated	shared	nightly	test-qemu SUITE=network	_test-suite-prepare	_test-suite-run
gfx	x86_64-clang	rootfs	none	1	512	60	read-only	shared	nightly	test-qemu SUITE=gfx	_test-suite-prepare	_test-suite-run
resolution	x86_64-clang	rootfs	none	1	512	60	read-only	shared	nightly	test-qemu SUITE=resolution	_test-suite-prepare	_test-suite-run
resolution-fault	x86_64-clang	rootfs	resolution-test	1	512	60	isolated	shared	nightly	FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution	_test-suite-prepare	_test-suite-run
driver-model	x86_64-clang	rootfs	driver-model	1,2,4	512	180	isolated	shared	nightly	test-qemu SUITE=driver-model	_test-suite-prepare	_test-suite-run
monitor	x86_64-clang	rootfs	none	1,2,8	512	90	read-only	shared	nightly	test-qemu SUITE=monitor	_test-suite-prepare	_test-suite-run
terminal-present	x86_64-clang	rootfs	terminal-present	1	512	60	isolated	exclusive	nightly	test-qemu SUITE=terminal-present	_test-suite-prepare	_test-suite-run
intr-x86	x86_64-clang	rootfs	intr-x86	1	512	120	isolated	shared	nightly	test-qemu SUITE=intr-x86	_test-suite-prepare	_test-suite-run
gui	x86_64-clang	rootfs	none	1,2	512	60	read-only	shared	nightly	test-qemu SUITE=gui	_test-suite-prepare	_test-suite-run
gui-desktop	x86_64-clang	rootfs	none	1	512	60	read-only	shared	nightly	test-qemu SUITE=gui-desktop	_test-suite-prepare	_test-suite-run
kernel-selftest	x86_64-clang	rootfs	selftest	8	512	75	isolated	shared	pr	test-kernel-selftest	-	-
systest-repeat	x86_64-clang	rootfs	none	1	512	180	read-only	shared	nightly	test-syscall-repeat	-	-
aarch64-smp	aarch64-clang	uefi	selftest	1,2,4	512	90	isolated	shared	pr	test-aarch64 MODE=smp	-	-
aarch64-no-ack	aarch64-clang	uefi	no-ack	2	512	90	isolated	shared	nightly	test-aarch64 MODE=no-ack	-	-
aarch64-gic-spi	aarch64-clang	uefi	selftest	2	512	90	isolated	shared	nightly	test-aarch64 MODE=gic-spi	-	-
aarch64-sync-fault	aarch64-clang	uefi	sync-fault	2	512	90	isolated	shared	nightly	test-aarch64 MODE=sync-fault	-	-
aarch64-m3-probe	aarch64-clang	uefi	selftest	2	512	90	isolated	shared	nightly	test-aarch64 MODE=m3-probe	-	-
aarch64-m1-ram	aarch64-clang	uefi	m1-ram	2	512	90	isolated	shared	nightly	test-aarch64 MODE=m1-ram	-	-
aarch64-m1-sparse	aarch64-clang	uefi	m1-sparse	2	512	90	isolated	shared	nightly	test-aarch64 MODE=m1-sparse	-	-
aarch64-m1-arena-exhaust	aarch64-clang	uefi	m1-arena-exhaust	2	512	90	isolated	shared	nightly	test-aarch64 MODE=m1-arena-exhaust	-	-
aarch64-m1-table-exhaust	aarch64-clang	uefi	m1-table-exhaust	2	512	90	isolated	shared	nightly	test-aarch64 MODE=m1-table-exhaust	-	-
aarch64-m1-ap-bad-root	aarch64-clang	uefi	m1-ap-bad-root	2	512	90	isolated	shared	nightly	test-aarch64 MODE=m1-ap-bad-root	-	-
aarch64-sched	aarch64-clang	uefi	sched	1,2,4,8	512	120	isolated	shared	nightly	test-aarch64 MODE=sched	-	-
endef

export TEST_SUITE_CATALOG_DATA

.PHONY: print-test-catalog
print-test-catalog:
	@printf '%s\n' "$$TEST_SUITE_CATALOG_DATA" | grep -v '^[[:space:]]*$$' | sort -k1,1
