#ifndef _FS_ELF_LAYOUT_H
#define _FS_ELF_LAYOUT_H

#include <fs/elf.h>     /* elf64_ehdr_t, elf64_phdr_t, PT_LOAD, ...   */
#include <stdint.h>

/* ── ELF load-layout contract ─────────────────────────────────
 *
 * elf_layout_validate() pre-scans an ELF's program headers and
 * decides whether the load image is well-formed enough for the
 * kernel's per-4 KiB loader to consume it.  It is a PURE FUNCTION:
 *   * no file I/O (the caller has already read the program headers),
 *   * no global state mutation,
 *   * no kernel facilities required (no PMM, no VMM, no VFS).
 *
 * This decoupling lets Task 2 (loader rewrite) and Task 3 (process
 * image lifecycle) consume a single, host-testable decision.  It also
 * satisfies the isolation spec §4 invariant
 *
 *     elf_end   = max(PT_LOAD.p_vaddr + PT_LOAD.p_memsz)
 *     heap_base = ALIGN_UP(elf_end, 4 KiB)
 *
 * and rejects every static failure mode the spec §4 names:
 *   - malformed magic / wrong class / wrong endianness
 *   - wrong machine / not ET_EXEC
 *   - no PT_LOAD segments (zero-length-only image)
 *   - p_filesz > p_memsz, zero-sized PT_LOAD
 *   - program-header or file-range overflow (out of file_size)
 *   - virtual-range overflow (p_vaddr + p_memsz wraps)
 *   - any address below USER_CODE_ADDR (0x400000)
 *   - any segment end above HEAP_LIMIT (0x203ff000)
 *   - heap_base > HEAP_LIMIT
 *   - two PT_LOAD byte intervals intersecting (incl. file/BSS)
 *   - e_entry not inside any nonempty executable PT_LOAD
 *
 * Returns 0 on success and populates *out; returns -ENOEXEC on any
 * validation failure (out is left untouched on failure).
 */

typedef struct {
    uint64_t elf_end;    /* max(PT_LOAD.p_vaddr + PT_LOAD.p_memsz)      */
    uint64_t heap_base;  /* ALIGN_UP(elf_end, 4096), ∈ [vaddr, heap_limit] */
} elf_layout_t;

/* Validate an ELF layout.  Caller passes:
 *   - ehdr:        the ELF64 header (already parsed from the file)
 *   - phdrs:       program-header array (must lie within the file
 *                  but the caller does NOT need to have bounds-checked
 *                  this in advance — the validator re-checks)
 *   - file_size:   total size of the ELF file in bytes
 *   - out:         output struct, populated only on success
 *
 * Returns 0 on success, -ENOEXEC on any failure.
 */
int elf_layout_validate(const elf64_ehdr_t *ehdr,
                        const elf64_phdr_t *phdrs,
                        uint64_t file_size,
                        elf_layout_t *out);

#endif /* _FS_ELF_LAYOUT_H */
