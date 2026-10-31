# ── Host Test Registry (spec §5) ──────────────────────────────
# Metadata registry, uniqueness verification, and inventory export.

HOST_TEST_INVENTORY :=

# register_host_test(id, group, acceptance)
#   id: test binary name without .elf
#   group: one of {framework, libc, memory, sched, fs, intr, drivers, net, display, terminal, monitor, gui}
#   acceptance: one of {protocol-or-legacy, exit-status, expected-failure}
define register_host_test
HOST_TEST_INVENTORY += $(strip $(1)):$(strip $(2)):$(strip $(3))
endef

.PHONY: print-inventory
print-inventory:
	@for item in $(HOST_TEST_INVENTORY); do \
	    echo "$$item" | tr ':' '\t'; \
	done | sort
