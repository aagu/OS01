#ifndef OS01_AARCH64_BOOT_DIRECT_MAP_H
#define OS01_AARCH64_BOOT_DIRECT_MAP_H
#include <stdint.h>
#include <arch/aarch64/runtime_tree.h>
uint64_t aarch64_at_translation(uint64_t va);
uint64_t aarch64_read_ttbr1(void);
void aarch64_install_ttbr1(uint64_t root_pa);
void aarch64_tlb_flush_all(void);
uint64_t aarch64_runtime_root_address(void);
uint64_t aarch64_probe_pa_address(void);
uint64_t aarch64_probe_expected_address(void);
uint64_t aarch64_installed_root(void);
const struct aarch64_runtime_tree *aarch64_runtime_tree_get(void);
#endif
