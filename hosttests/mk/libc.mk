# ── Libc Subsystem Host Tests (spec §5) ────────────────────────

$(eval $(call register_host_test,test_libc_string,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_list,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_vsprintf,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_fflush,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_stdio_registry,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_fread_fwrite_validate,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_getopt,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_color_printk_abi,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_api,libc,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_libc_string.elf \
    $(TEST_BLD)/test_libc_list.elf \
    $(TEST_BLD)/test_libc_vsprintf.elf \
    $(TEST_BLD)/test_libc_fflush.elf \
    $(TEST_BLD)/test_libc_stdio_registry.elf \
    $(TEST_BLD)/test_libc_fread_fwrite_validate.elf \
    $(TEST_BLD)/test_libc_getopt.elf \
    $(TEST_BLD)/test_aarch64_color_printk_abi.elf \
    $(TEST_BLD)/test_libc_api.elf

# getopt test links against libc_getopt.o whose globals are declared by <getopt.h>; build with -fcommon
$(TEST_BLD)/test_libc_getopt.o: $(TEST_CASES)/test_libc_getopt.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -fcommon $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_getopt.elf: $(TEST_BLD)/test_libc_getopt.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

# vsprintf test exercises printf/vasprintf/write_all with the scripted write shim
$(TEST_BLD)/test_libc_vsprintf.o: $(TEST_CASES)/test_libc_vsprintf.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_vsprintf.elf: $(TEST_BLD)/test_libc_vsprintf.o $(LIBC_OBJS) \
		$(TEST_BLD)/libc_vasprintf.o $(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_stdio_file.o $(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

# fflush test exercises printf + fflush + fopen/fclose + write-shim capture
$(TEST_BLD)/test_libc_fflush.o: $(TEST_CASES)/test_libc_fflush.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_fflush.elf: $(TEST_BLD)/test_libc_fflush.o $(LIBC_OBJS) \
		$(TEST_BLD)/libc_vasprintf.o $(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_stdio_file.o $(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

# stdio-registry cap / fdopen / slot-reuse tests
$(TEST_BLD)/test_libc_stdio_registry.o: $(TEST_CASES)/test_libc_stdio_registry.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_stdio_registry.elf: \
		$(TEST_BLD)/test_libc_stdio_registry.o \
		$(TEST_BLD)/libc_stdio_file.o \
		$(TEST_BLD)/libc_stdio_extras.o \
		$(TEST_BLD)/libc_vsprintf.o \
		$(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_floatconv.o \
		$(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-stdio-registry
test-libc-stdio-registry: $(TEST_BLD)/test_libc_stdio_registry.elf
	$<

# fread/fwrite stream-validation test
$(TEST_BLD)/test_libc_fread_fwrite_validate.o: $(TEST_CASES)/test_libc_fread_fwrite_validate.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_fread_fwrite_validate.elf: \
		$(TEST_BLD)/test_libc_fread_fwrite_validate.o \
		$(TEST_BLD)/libc_stdio_file.o \
		$(TEST_BLD)/libc_vsprintf.o \
		$(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_floatconv.o \
		$(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-fread-fwrite-validate
test-libc-fread-fwrite-validate: $(TEST_BLD)/test_libc_fread_fwrite_validate.elf
	$<

$(TEST_BLD)/test_libc_string.elf: $(TEST_BLD)/test_libc_string.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_libc_list.elf: $(TEST_BLD)/test_libc_list.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

# color_printk / serial_printk ABI + behaviour
PRINTK_ABI_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) \
	-I$(TEST_MOCK)/aarch64_color_printk $(KERNEL_INC)

$(TEST_BLD)/test_aarch64_color_printk_abi.o: $(TEST_CASES)/test_aarch64_color_printk_abi.c \
		$(TESTS_DIR)/kernel/include/core/printk.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PRINTK_ABI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/printk_production.o: \
		$(TESTS_DIR)/kernel/core/printk.c \
		$(TESTS_DIR)/kernel/include/core/printk.h \
		$(TESTS_DIR)/kernel/include/driver/serial.h \
		$(TESTS_DIR)/kernel/include/driver/fb_state.h \
		$(TESTS_DIR)/kernel/include/driver/font.h \
		$(TEST_MOCK)/aarch64_color_printk/driver/serial.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PRINTK_ABI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/printk_capture.o: $(TEST_MOCK)/aarch64_color_printk/printk_capture.c \
		$(TEST_MOCK)/aarch64_color_printk/driver/serial.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) \
		-I$(TEST_MOCK)/aarch64_color_printk $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_color_printk_abi.elf: \
		$(TEST_BLD)/test_aarch64_color_printk_abi.o \
		$(TEST_BLD)/printk_production.o \
		$(TEST_BLD)/printk_capture.o
	$(HOST_CC) -o $@ $^

.PHONY: test-aarch64-color-printk-abi
test-aarch64-color-printk-abi: $(TEST_BLD)/test_aarch64_color_printk_abi.elf
	$(TEST_BLD)/test_aarch64_color_printk_abi.elf

# ── libc completion: api group (T1) ─────────────────────────────────
# The api group audits the PUBLIC HEADER contract of the OS01 libc:
# every public header is self-contained, has exactly one definition
# across translation units, and gives its declarations C linkage so a
# C++ consumer links against the same symbols.
#
# The manifest (which headers, which interfaces, which group owns them)
# lives in scripts/audit_libc_api.py — the single source of truth.  It is
# emitted as a C header for the test, so the test and the audit script
# can never disagree about the inventory.
#
# The binary links the REAL production libc string objects under their
# `os01_` prefix (see common.mk), so a call in the test can never
# resolve to the host libc's same-named implementation.
LIBC_API_MANIFEST := $(TEST_BLD)/libc_api_manifest.h
LIBC_API_PROBE_DIR := $(TEST_BLD)/api-probe

$(LIBC_API_MANIFEST): $(TESTS_DIR)/scripts/audit_libc_api.py
	@mkdir -p $(TEST_BLD)
	python3 $< --emit-c-header $@

$(TEST_BLD)/test_libc_api.o: $(TEST_CASES)/test_libc_api.c \
		$(LIBC_API_MANIFEST) $(TEST_MOCK)/libc_completion_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_PREFIX_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
		-I$(TEST_MOCK) -I$(TEST_BLD) \
		-DOS01_API_CC='"$(HOST_CC)"' \
		-DOS01_API_CXX='"$(HOST_CXX)"' \
		-DOS01_API_NM='"$(NM_TOOL)"' \
		-DOS01_API_INCLUDE_DIR='"$(LIBC_INC_DIR)"' \
		-DOS01_API_PROBE_DIR='"$(LIBC_API_PROBE_DIR)"' \
		-DOS01_API_PREFIX_DEFS='"$(OS01_LIBC_PREFIX_DEFS)"' \
		-DOS01_API_PREFIXED_OBJS='"$(OS01_LIBC_PREFIX_OBJS)"' \
		-c $< -o $@

$(TEST_BLD)/test_libc_api.elf: $(TEST_BLD)/test_libc_api.o $(OS01_LIBC_PREFIX_OBJS)
	@mkdir -p $(LIBC_API_PROBE_DIR)
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-api
test-libc-api: $(TEST_BLD)/test_libc_api.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<
