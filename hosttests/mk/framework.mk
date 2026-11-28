# ── Framework Subsystem Host Tests (spec §5) ───────────────────




# Negative-contract binary: framework exit-code integrity
# ── userland ld.so unit tests (test_ldso_units) ────────────────
# Self-contained like test_framework_contract: the single case TU
# includes user/ldso/ldso.c directly (one translation unit, statics
# reachable) and links against nothing but libc.  -I$(TESTS_DIR) lets
# the quoted include "user/ldso/ldso.c" resolve from the repo root.
$(TEST_BLD)/test_ldso_units.o: $(TEST_CASES)/test_ldso_units.c \
		$(TESTS_DIR)/user/ldso/ldso.c $(TESTS_DIR)/user/ldso/ldso_elf.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
		-I$(TESTS_DIR) -c $< -o $@

$(TEST_BLD)/test_ldso_units.elf: $(TEST_BLD)/test_ldso_units.o
	$(HOST_CC) -o $@ $^


$(TEST_BLD)/test_framework_contract.elf: $(TEST_CASES)/test_framework_contract.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $(TEST_BLD)/test_framework_contract.o
	$(HOST_CC) -o $@ $(TEST_BLD)/test_framework_contract.o

.PHONY: test-framework-contract
test-framework-contract: $(TEST_BLD)/test_framework_contract.elf
	@echo "  [framework-contract] running $< (must exit EXACTLY 1: negative contract)"
	@out=`$<`; rc=$$?; \
	 if [ $$rc -eq 1 ]; then \
	     echo "  [framework-contract] PASS: exit 1 (assertion exit-code contract holds)"; \
	 else \
	     echo "  [framework-contract] FAIL: expected exit 1, got $$rc" >&2; \
	     echo "$$out" | sed 's/^/    /'; \
	     case $$rc in \
	         0) echo "    hint: assertions no longer propagate (TEST_RESULTS may be resetting __test_stats)";; \
	         2) echo "    hint: assertion macro evaluated its argument more than once";; \
	         3) echo "    hint: TEST_RESULTS() reset __test_stats (the pre-2d594ce0 bug)";; \
	         *) echo "    hint: unexpected exit code; see test_framework_contract.c header for the contract";; \
	     esac; \
	     exit 1; \
	 fi

# Runtime fixtures.
$(TEST_BLD)/runtime_udivti3_link_probe.o: $(TEST_SRC)/../runtime/selftest/udivti3_link_probe.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -ffreestanding -fno-builtin -c $< -o $@

.PHONY: runtime-udivti3-link-probe
runtime-udivti3-link-probe: $(TEST_BLD)/runtime_udivti3_link_probe.o
	$(HOST_CC) -nostdlib -Wl,-e,_start -o $(TEST_BLD)/runtime_udivti3-link-probe.elf $<

$(TEST_BLD)/test_runtime_udivti3.o: $(TEST_SRC)/../runtime/selftest/test_udivti3.c \
	$(TEST_SRC)/../runtime/selftest/udivti3_vectors.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/runtime/include -I$(TESTS_DIR)/runtime/selftest -c $< -o $@

$(TEST_BLD)/runtime_udivti3_host.o: $(TESTS_DIR)/runtime/builtins/udivti3.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -fno-stack-protector -I$(TESTS_DIR)/runtime/include -c $< -o $@

$(TEST_BLD)/test_runtime_udivti3.elf: $(TEST_BLD)/test_runtime_udivti3.o $(TEST_BLD)/runtime_udivti3_host.o
	$(HOST_CC) -o $@ $^

.PHONY: test-runtime-udivti3
test-runtime-udivti3: $(TEST_BLD)/test_runtime_udivti3.elf
	$<

# page_table_runner meta-test
$(TEST_BLD)/test_page_table_runner.o: $(TEST_CASES)/test_page_table_runner.c $(TEST_INC)/page_table_test_runner.h $(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_page_table_runner.elf: $(TEST_BLD)/test_page_table_runner.o
	$(HOST_CC) -o $@ $^

# bootinfo ABI
$(TEST_BLD)/test_bootinfo_abi.o: $(TEST_CASES)/test_bootinfo_abi.c \
		$(TESTS_DIR)/kernel/include/core/bootinfo.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/test_bootinfo_abi.elf: $(TEST_BLD)/test_bootinfo_abi.o
	$(HOST_CC) -o $@ $^

.PHONY: test-bootinfo-abi
test-bootinfo-abi: $(TEST_BLD)/test_bootinfo_abi.elf
	$<

# Shared core mock archive (Task 10 — opt-in only)
MOCK_CORE_DIR := $(TEST_BLD)/mock-core
MOCK_CORE_SRCS := \
	$(TEST_MOCK)/core/mock_core.c \
	$(TEST_MOCK)/core/mock_alloc.c \
	$(TEST_MOCK)/core/mock_printk.c
MOCK_CORE_OBJS := \
	$(MOCK_CORE_DIR)/mock_core.o \
	$(MOCK_CORE_DIR)/mock_alloc.o \
	$(MOCK_CORE_DIR)/mock_printk.o
HOST_AR := $(if $(strip $(TARGET_AR)),$(TARGET_AR),ar)

$(MOCK_CORE_DIR)/%.o: $(TEST_MOCK)/core/%.c $(TEST_INC)/mock_core.h
	@mkdir -p $(MOCK_CORE_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/libmock-core.a: $(MOCK_CORE_OBJS)
	@mkdir -p $(TEST_BLD)
	rm -f $@
	$(HOST_AR) rcs $@ $^

$(TEST_BLD)/test_mock_core.o: $(TEST_CASES)/test_mock_core.c $(TEST_INC)/test_framework.h $(TEST_INC)/mock_core.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_mock_core.elf: $(TEST_BLD)/test_mock_core.o $(TEST_BLD)/libmock-core.a
	$(HOST_CC) -o $@ $^

# Fixture v2 (test_fixture_v2.o / .elf rules generated in mk/generated/test-link-rules.mk)

# CPU features facade (x86_64 + aarch64 decoders)
$(TEST_BLD)/x86_cpu_features_production.o: $(TESTS_DIR)/kernel/arch/x86_64/cpu/features.c \
		$(TESTS_DIR)/kernel/include/arch/cpu_features.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/cpu_features.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h \
		$(TESTS_DIR)/kernel/include/arch/cpuid.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/aarch64_cpu_features_production.o: $(TESTS_DIR)/kernel/arch/aarch64/cpu/features.c \
		$(TESTS_DIR)/kernel/include/arch/cpu_features.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/cpu_features.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DOS01_HOST_TEST -I$(TESTS_DIR)/kernel/include -c $< -o $@

LINK_CFLAGS_cpu-features = -I$(TESTS_DIR)/kernel/include $(FRAMEWORK_INC)
LINK_GROUP_cpu-features := $(TEST_BLD)/x86_cpu_features_production.o \
                           $(TEST_BLD)/aarch64_cpu_features_production.o

# Wall-clock timekeeping
$(TEST_BLD)/timekeeping_production.o: $(TESTS_DIR)/kernel/time/timekeeping.c \
		$(TESTS_DIR)/kernel/include/time/timekeeping.h \
		$(TESTS_DIR)/kernel/include/driver/rtc.h \
		$(TESTS_DIR)/kernel/include/arch/clocksource.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/kernel/include -c $< -o $@

LINK_CFLAGS_timekeeping = -I$(TESTS_DIR)/kernel/include $(FRAMEWORK_INC)
LINK_GROUP_timekeeping := $(TEST_BLD)/timekeeping_production.o
