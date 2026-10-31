# ── Libc Subsystem Host Tests (spec §5) ────────────────────────

$(eval $(call register_host_test,test_libc_string,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_list,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_vsprintf,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_fflush,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_stdio_registry,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_fread_fwrite_validate,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_getopt,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_color_printk_abi,libc,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_libc_string.elf \
    $(TEST_BLD)/test_libc_list.elf \
    $(TEST_BLD)/test_libc_vsprintf.elf \
    $(TEST_BLD)/test_libc_fflush.elf \
    $(TEST_BLD)/test_libc_stdio_registry.elf \
    $(TEST_BLD)/test_libc_fread_fwrite_validate.elf \
    $(TEST_BLD)/test_libc_getopt.elf \
    $(TEST_BLD)/test_aarch64_color_printk_abi.elf

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
