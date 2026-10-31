# ── Host Test Runner & Execution (spec §5) ────────────────────

HOST_TEST_TIMEOUT ?= 60
HOST_TEST_JOBS ?= 1

# One `--exit-status <id>` per exempted binary (id = basename without .elf).
EXIT_STATUS_FLAGS := $(foreach b,$(TEST_EXIT_STATUS_BINS),--exit-status $(basename $(notdir $(b))))

.PHONY: run check-test-bins-dup
run: $(TEST_BINS) $(TEST_EXIT_STATUS_BINS) check-test-bins-dup check-registry-unique
	@echo ""
	@echo "=============================================="
	@echo "  OS01 Host-Based Test Suite"
	@echo "=============================================="
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) --jobs $(HOST_TEST_JOBS) $(EXIT_STATUS_FLAGS) \
	    $(TEST_BINS) $(TEST_EXIT_STATUS_BINS)
	@echo "  [hosttests] all host test binaries passed"

check-test-bins-dup:
	@if [ "$$(echo $(TEST_BINS) $(TEST_EXIT_STATUS_BINS) | tr ' ' '\n' | sort | uniq -d)" != "" ]; then \
	    echo "ERROR: duplicate entries in TEST_BINS/TEST_EXIT_STATUS_BINS"; exit 1; \
	fi
