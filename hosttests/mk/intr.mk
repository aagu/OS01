# ── Intr Subsystem Host Tests (spec §5) ──────────────────────────

$(eval $(call register_host_test,test_lapic_nmi,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_gic_driver,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_ipi_broadcast,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_gic_target_probe,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_gic_marker_lines,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_gic_probe,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_sync_fault,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_tlb_serial_protocol,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_x86_ipi_ready_publish,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_intr_decode_x86,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_intr_decode_aarch64,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_intr_dispatch,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_intr_backend_aarch64,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_intr_backend_x86,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_intr_irq_x86,intr,protocol-or-legacy))
$(eval $(call register_host_test,test_x86_intr_probe,intr,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_lapic_nmi.elf \
    $(TEST_BLD)/test_gic_driver.elf \
    $(TEST_BLD)/test_aarch64_ipi_broadcast.elf \
    $(TEST_BLD)/test_aarch64_gic_target_probe.elf \
    $(TEST_BLD)/test_gic_marker_lines.elf \
    $(TEST_BLD)/test_gic_probe.elf \
    $(TEST_BLD)/test_aarch64_sync_fault.elf \
    $(TEST_BLD)/test_tlb_serial_protocol.elf \
    $(TEST_BLD)/test_x86_ipi_ready_publish.elf \
    $(TEST_BLD)/test_intr_decode_x86.elf \
    $(TEST_BLD)/test_intr_decode_aarch64.elf \
    $(TEST_BLD)/test_intr_dispatch.elf \
    $(TEST_BLD)/test_intr_backend_aarch64.elf \
    $(TEST_BLD)/test_intr_backend_x86.elf \
    $(TEST_BLD)/test_intr_irq_x86.elf \
    $(TEST_BLD)/test_x86_intr_probe.elf

# aarch64 EL1h sync diagnostic helper (Task 1)
$(TEST_BLD)/test_aarch64_sync_fault.o: $(TEST_CASES)/test_aarch64_sync_fault.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/sync_fault.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_sync_fault.elf: $(TEST_BLD)/test_aarch64_sync_fault.o
	$(HOST_CC) -o $@ $<

# Normalized event decoders
X86_DECODE_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) $(KERNEL_INC) \
                          $(LIBC_INC)

A64_DECODE_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) \
                          -I$(TEST_MOCK)/intr_dispatch/aarch64 \
                          $(KERNEL_INC) $(LIBC_INC)

$(TEST_BLD)/x86_intr_decode_production.o: \
		$(TESTS_DIR)/kernel/arch/x86_64/intr/intr_decode.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_vectors.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_decode_x86.o: $(TEST_CASES)/test_intr_decode_x86.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_vectors.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_decode_x86.elf: $(TEST_BLD)/test_intr_decode_x86.o \
		$(TEST_BLD)/x86_intr_decode_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/aarch64_intr_decode_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/intr/intr_decode.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/sync_fault.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/regs.h \
		$(TEST_MOCK)/intr_dispatch/aarch64/arch/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(A64_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_decode_aarch64.o: \
		$(TEST_CASES)/test_intr_decode_aarch64.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/regs.h \
		$(TEST_MOCK)/intr_dispatch/aarch64/arch/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(A64_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_decode_aarch64.elf: \
		$(TEST_BLD)/test_intr_decode_aarch64.o \
		$(TEST_BLD)/aarch64_intr_decode_production.o
	$(HOST_CC) -o $@ $^

# Common stateless dispatcher
INTR_DISPATCH_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) $(KERNEL_INC) \
                             -idirafter $(TESTS_DIR)/libc/include

$(TEST_BLD)/intr_dispatch_production.o: $(TESTS_DIR)/kernel/intr/dispatch.c \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(INTR_DISPATCH_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_dispatch.o: $(TEST_CASES)/test_intr_dispatch.c \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(INTR_DISPATCH_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_dispatch.elf: \
		$(TEST_BLD)/test_intr_dispatch.o \
		$(TEST_BLD)/intr_dispatch_production.o
	$(HOST_CC) -o $@ $^

# aarch64 routing backend
A64_BACKEND_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) \
                          -I$(TEST_MOCK)/intr_dispatch/aarch64 \
                          $(KERNEL_INC) -idirafter $(TESTS_DIR)/libc/include

$(TEST_BLD)/aarch64_intr_backend_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/intr/dispatch.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/gic.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/sync_fault.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/regs.h \
		$(TEST_MOCK)/intr_dispatch/aarch64/arch/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(A64_BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/intr_dispatch_aarch64_production.o: \
		$(TESTS_DIR)/kernel/intr/dispatch.c \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/regs.h \
		$(TEST_MOCK)/intr_dispatch/aarch64/arch/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(A64_BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_backend_aarch64.o: \
		$(TEST_CASES)/test_intr_backend_aarch64.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/gic.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/regs.h \
		$(TEST_MOCK)/intr_dispatch/aarch64/arch/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(A64_BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_backend_aarch64.elf: \
		$(TEST_BLD)/test_intr_backend_aarch64.o \
		$(TEST_BLD)/aarch64_intr_backend_production.o \
		$(TEST_BLD)/aarch64_intr_decode_production.o \
		$(TEST_BLD)/intr_dispatch_aarch64_production.o
	$(HOST_CC) -o $@ $^

# x86_64 routing backend
X86_BACKEND_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) $(KERNEL_INC) \
                           -idirafter $(TESTS_DIR)/libc/include

$(TEST_BLD)/x86_intr_backend_production.o: \
		$(TESTS_DIR)/kernel/arch/x86_64/intr/dispatch.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_vectors.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_backend_x86.o: \
		$(TEST_CASES)/test_intr_backend_x86.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_vectors.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_backend_x86.elf: \
		$(TEST_BLD)/test_intr_backend_x86.o \
		$(TEST_BLD)/x86_intr_backend_production.o \
		$(TEST_BLD)/x86_intr_decode_production.o \
		$(TEST_BLD)/intr_dispatch_production.o
	$(HOST_CC) -o $@ $^

# GICv2 driver
GIC_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)

$(TEST_BLD)/gic_driver_production.o: $(TESTS_DIR)/kernel/arch/aarch64/intr/gic_driver.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/gic.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GIC_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_gic_driver.o: $(TEST_CASES)/test_gic_driver.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/gic.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GIC_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_gic_driver.elf: $(TEST_BLD)/test_gic_driver.o $(TEST_BLD)/gic_driver_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_gic_driver
test_gic_driver: $(TEST_BLD)/test_gic_driver.elf
	$<

# aarch64 ipi_broadcast
AARCH64_IPI_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)

$(TEST_BLD)/aarch64_ipi_production.o: $(TESTS_DIR)/kernel/arch/aarch64/intr/ipi.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/ipi.h \
		$(TESTS_DIR)/kernel/include/intr/ipi.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/dtb.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_IPI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_aarch64_ipi_broadcast.o: $(TEST_CASES)/test_aarch64_ipi_broadcast.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/ipi.h \
		$(TESTS_DIR)/kernel/include/intr/ipi.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_IPI_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_ipi_broadcast.elf: $(TEST_BLD)/test_aarch64_ipi_broadcast.o \
		$(TEST_BLD)/aarch64_ipi_production.o $(TEST_BLD)/gic_driver_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_aarch64_ipi_broadcast
test_aarch64_ipi_broadcast: $(TEST_BLD)/test_aarch64_ipi_broadcast.elf
	$<

# aarch64 gic_target_bit_init
$(TEST_BLD)/test_aarch64_gic_target_probe.o: \
		$(TEST_CASES)/test_aarch64_gic_target_probe.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/ipi.h \
		$(TESTS_DIR)/kernel/include/intr/ipi.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/dtb.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_IPI_HOST_CFLAGS) \
		-DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
		$(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_gic_target_probe.elf: \
		$(TEST_BLD)/test_aarch64_gic_target_probe.o \
		$(TEST_BLD)/aarch64_ipi_production.o $(TEST_BLD)/gic_driver_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_aarch64_gic_target_probe
test_aarch64_gic_target_probe: $(TEST_BLD)/test_aarch64_gic_target_probe.elf
	$<

# TLB serial protocol
$(TEST_BLD)/test_tlb_serial_protocol.o: \
		$(TEST_CASES)/test_tlb_serial_protocol.c \
		$(TEST_MOCK)/tlb_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(TLB_SD_HOST_CFLAGS) -I$(TEST_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_tlb_serial_protocol.elf: \
		$(TEST_BLD)/test_tlb_serial_protocol.o \
		$(TEST_BLD)/tlb_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_tlb_serial_protocol
test_tlb_serial_protocol: $(TEST_BLD)/test_tlb_serial_protocol.elf
	$<

# x86 ipi_ready publish-point
X86_PUBLISH_CFLAGS := $(HOST_CFLAGS) -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
                      -I$(TEST_INC) -include $(TEST_MOCK)/test_platform.h

$(TEST_BLD)/test_x86_ipi_ready_publish.o: \
		$(TEST_CASES)/test_x86_ipi_ready_publish.c \
		$(TESTS_DIR)/kernel/arch/x86_64/smp/boot.c \
		$(TESTS_DIR)/kernel/arch/x86_64/smp/smp.c \
		$(TESTS_DIR)/kernel/arch/x86_64/intr/apic/ipi.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_PUBLISH_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_x86_ipi_ready_publish.elf: \
		$(TEST_BLD)/test_x86_ipi_ready_publish.o
	$(HOST_CC) -o $@ $^

.PHONY: test_x86_ipi_ready_publish
test_x86_ipi_ready_publish: $(TEST_BLD)/test_x86_ipi_ready_publish.elf
	$<

# GIC marker lines
GIC_MARKER_CFLAGS ?= $(HOST_CFLAGS) -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
                     -I$(TEST_INC) -include $(TEST_MOCK)/test_platform.h

$(TEST_BLD)/test_gic_marker_lines.o: $(TEST_CASES)/test_gic_marker_lines.c \
		$(TESTS_DIR)/kernel/arch/aarch64/intr/gic.c \
		$(TESTS_DIR)/kernel/arch/aarch64/boot/main.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GIC_MARKER_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_gic_marker_lines.elf: $(TEST_BLD)/test_gic_marker_lines.o
	$(HOST_CC) -o $@ $<

.PHONY: test_gic_marker_lines
test_gic_marker_lines: $(TEST_BLD)/test_gic_marker_lines.elf
	$<

# GIC clobber-probe
$(TEST_BLD)/test_gic_probe.o: $(TEST_CASES)/test_gic_probe.c \
		$(TESTS_DIR)/kernel/arch/aarch64/intr/irq_probe.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/gic.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GIC_HOST_CFLAGS) $(GIC_MARKER_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_gic_probe.elf: $(TEST_BLD)/test_gic_probe.o $(TEST_BLD)/gic_driver_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_gic_probe
test_gic_probe: $(TEST_BLD)/test_gic_probe.elf
	$<

# x86 IRQ chain test
IRQ_CHAIN_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -I$(TEST_MOCK)/intr_irq -I$(TEST_INC) $(KERNEL_INC)
IRQ_CHAIN_SOURCES := arch/x86_64/intr/irq_entry.c \
    arch/x86_64/intr/irq_hooks.c intr/vector_dispatch.c \
    arch/x86_64/intr/pic_8259a.c arch/x86_64/intr/apic/lapic_timer.c \
    arch/x86_64/intr/apic/ipi.c
IRQ_CHAIN_OBJS := $(TEST_BLD)/irq-chain/runtime.o \
    $(addprefix $(TEST_BLD)/irq-chain/,$(IRQ_CHAIN_SOURCES:.c=.o))

$(TEST_BLD)/irq-chain/%.o: $(TESTS_DIR)/kernel/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(IRQ_CHAIN_HOST_CFLAGS) $(LIBC_INC) -MMD -MP -c $< -o $@

$(TEST_BLD)/irq-chain/runtime.o: $(TEST_MOCK)/intr_irq/runtime.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(IRQ_CHAIN_HOST_CFLAGS) $(LIBC_INC) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_intr_irq_x86.o: $(TEST_CASES)/test_intr_irq_x86.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(IRQ_CHAIN_HOST_CFLAGS) -idirafter $(TESTS_DIR)/libc/include -MMD -MP -c $< -o $@

$(TEST_BLD)/test_intr_irq_x86.elf: $(IRQ_CHAIN_OBJS) \
        $(TEST_BLD)/test_intr_irq_x86.o \
        $(TEST_BLD)/x86_intr_backend_production.o \
        $(TEST_BLD)/x86_intr_decode_production.o \
        $(TEST_BLD)/intr_dispatch_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-intr-irq-x86
test-intr-irq-x86: $(TEST_BLD)/test_intr_irq_x86.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) $<

-include $(IRQ_CHAIN_OBJS:.o=.d)
-include $(TEST_BLD)/test_intr_irq_x86.d

# x86 intr probe
$(TEST_BLD)/test_x86_intr_probe.o: $(TEST_CASES)/test_x86_intr_probe.c $(TESTS_DIR)/kernel/include/selftest/x86_intr.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -idirafter $(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/test_x86_intr_probe.elf: $(TEST_BLD)/test_x86_intr_probe.o
	$(HOST_CC) -o $@ $^

# LAPIC NMI
$(TEST_BLD)/lapic_nmi_production.o: $(TESTS_DIR)/kernel/arch/x86_64/intr/apic/nmi.c $(TESTS_DIR)/kernel/include/intr/apic.h $(TESTS_DIR)/kernel/arch/x86_64/intr/apic/acpi.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -Wno-unused-function -ffunction-sections -fdata-sections $(KERNEL_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_lapic_nmi.o: $(TEST_CASES)/test_lapic_nmi.c $(TESTS_DIR)/kernel/include/intr/apic.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -Wno-unused-function -ffunction-sections -fdata-sections $(KERNEL_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_lapic_nmi.elf: $(TEST_BLD)/test_lapic_nmi.o $(TEST_BLD)/lapic_nmi_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-lapic-nmi
test-lapic-nmi: $(TEST_BLD)/test_lapic_nmi.elf
	$<
