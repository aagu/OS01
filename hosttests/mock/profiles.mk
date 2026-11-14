# ── Shared Host-Test Mock Profiles (spec §5) ──────────────────
# Maps `mock: <name>` metadata profiles to linkable archive/object sets.

MOCK_PROFILE_mm-brk := $(TEST_BLD)/brk_stubs.o $(TEST_BLD)/libmock-core.a
MOCK_PROFILE_mm-pranges := $(TEST_BLD)/pranges_stubs.o $(TEST_BLD)/libmock-core.a
MOCK_PROFILE_mm-uwrite-cow := $(TEST_BLD)/uwrite_cow_stubs.o $(TEST_BLD)/libmock-core.a
