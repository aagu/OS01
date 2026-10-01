# VFS mount overwrite: boot slab pages were not reserved correctly

## Evidence

Investigated on `worktree-pmm-three-fixes`, starting at `4468e75` (including
its three PMM changes and the defensive `find_mount` guard).

A hardware write watchpoint on the `/dev` mount's `next` field at
`0xffff800000600028` stopped inside `e1000_xmit`, while constructing a DHCP
packet's transmit descriptor:

```
Old value = 0
New value = 350
pc = e1000_xmit+311
rcx = 0xffff800000600000
r12 = 0x15e
stack: e1000_xmit -> etharp_output -> ip4_output_if_src
       -> udp_sendto_if_src -> dhcp_discover -> ... -> tcpip_thread
```

The mount object was already overwritten at `task_init`, before any user
fork/exec loop. The observed `0xb00015e` is a TX descriptor's length/flags
word (350-byte packet and command flags), not evidence of a user-stack
pointer. The hardware observation disproves the earlier handoff's claim
that this corruption was independent of PMM.

At the same breakpoint, `PMMngr.pages_struct[0].phy_address` was `0x200000`.
The E820 adapter excludes low boot/kernel ranges and rounds inward, so
the first represented RAM frame is 2 MiB, not zero.

## Root cause and changes

The earlier PMM fixes made allocation/free bitmap updates RAM-relative,
but left three consumers using old absolute-page assumptions:

1. `pmm_init` skipped descriptor slot zero when reserving kernel storage.
   It is the first RAM frame, and can contain the kernel and PMM metadata.
   Reservation now covers all represented frames below the exclusive end,
   starting at zero, clamped to descriptor capacity and skipping holes.
2. `Phy_to_2M_Page` and `Virt_To_2M_Page` indexed from physical zero. They
   now subtract the physical address stored in the first RAM descriptor.
   This also corrects ELF/VMM huge-page teardown's descriptor selection.
3. `slab_init` reserved absolute bitmap bits and used an absolute descriptor
   index for metadata. It now converts physical addresses to the matching
   descriptor and marks that descriptor's RAM-relative bit.

Consequently the NIC was allocated a physical frame already backing the
64-byte boot slab, where `/dev`'s mount object resides. Network TX writes
and device DMA could overwrite live slab objects. Correct reservation
prevents duplicate physical ownership at its source. The committed VFS
guard remains, but is not treated as a repair of corrupted mount data.

No struct or UAPI changes. A clean rebuild is required to propagate the
changed address-conversion macros to ELF/VMM consumers in this build system.

## Regression coverage

`make PROFILE=x86_64-clang test-pmm-boot-reservation` compiles the real
`pmm_init`, `slab_init`, `alloc_pages`, `free_pages` and address-conversion
macros into a host executable. Only firmware input, direct-map translation
and unused privileged CPU interfaces are mocked. It exercises:

- RAM bases 0, 2 MiB and 4 GiB.
- Metadata contained in one frame and crossing into a second frame.
- Matching slab addresses/descriptors and reservation bitmap bits.
- Exhausting all available frames without handing out kernel/slab storage.
- Freeing frames through the same physical-address conversion used by VMM.

The test initially failed on slot-zero reservation. Fixing that exposed
incorrect slab descriptors; correcting conversion exposed incorrect slab
bitmap bits. All six cases pass after the complete fix. The test is also
part of the normal `make test` run.

The existing repeated-systest driver now rejects `VFS: find_mount: CORRUPT`
as well as `PF-KRN`, and still requires all five 268-test suites to finish
successfully. A lack of faults alone is insufficient: silently damaged
mounts or a hung shell must not count as a pass.

Existing limitation: boot slab placement assumes contiguous RAM following
the metadata region. This patch fixes ownership/indexing for that layout;
it does not introduce sparse-aware boot slab placement.
