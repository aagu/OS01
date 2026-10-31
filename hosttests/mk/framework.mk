# ── Framework Subsystem Host Tests (spec §5) ───────────────────

$(eval $(call register_host_test,test_framework_contract,framework,expected-failure))
$(eval $(call register_host_test,test_runtime_udivti3,framework,exit-status))
$(eval $(call register_host_test,test_page_table_runner,framework,exit-status))
$(eval $(call register_host_test,test_bootinfo_abi,framework,protocol-or-legacy))

TEST_EXIT_STATUS_BINS += \
    $(TEST_BLD)/test_runtime_udivti3.elf \
    $(TEST_BLD)/test_page_table_runner.elf

TEST_BINS += \
    $(TEST_BLD)/test_bootinfo_abi.elf

# Negative-contract binary: framework exit-code integrity
$(TEST_BLD)/test_framework_contract.elf: $(TEST_CASES)/test_framework_contract.c
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
