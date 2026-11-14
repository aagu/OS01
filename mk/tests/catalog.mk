# mk/tests/catalog.mk — Test suite catalog metadata and export
#
# TEST_SUITE_CATALOG_DATA is generated into mk/generated/test-dispatch.mk
# (included by mk/components/run.mk).
# Schema: id profile capability variant cpus memory_mib timeout_s image_policy resource tier entrypoint prepare_target run_target

.PHONY: print-test-catalog
print-test-catalog:
	@printf '%s\n' "$$TEST_SUITE_CATALOG_DATA" | grep -v '^[[:space:]]*$$' | sort -k1,1
