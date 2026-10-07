"""Public runtime layer shared by OS01 QEMU and host suites.

Modules in this package own lifecycle, output, and cleanup of a
single subprocess; they do not interpret PASS/FAIL — that is the
caller's job (see docs/superpowers/specs/2026-10-05-test-framework-design.md
§5.3 and §7.2).
"""