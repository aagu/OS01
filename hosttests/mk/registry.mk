# ── Host Test Registry (spec §5) ──────────────────────────────
# Consumes generated inventory and bucketed binary lists from
# mk/generated/test-registry.mk; uniqueness verification and inventory export.

REGISTRY_GEN := $(TESTS_DIR)/mk/generated/test-registry.mk
ifeq ($(wildcard $(REGISTRY_GEN)),)
$(error test-registry.mk missing; run 'make regen-test-catalog' at repo root)
endif
include $(REGISTRY_GEN)

.PHONY: print-inventory check-registry-unique
print-inventory:
	@for item in $(HOST_TEST_INVENTORY); do \
	    echo "$$item" | tr ':' '\t'; \
	done | sort

check-registry-unique:
	+@dup=$$(for item in $(HOST_TEST_INVENTORY); do echo "$${item%%:*}"; done | sort | uniq -d); \
	if [ -n "$$dup" ]; then \
	    echo "ERROR: duplicate test ID in registry: $$dup" >&2; \
	    exit 1; \
	fi
