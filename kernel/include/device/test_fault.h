/* kernel/include/device/test_fault.h — Driver-model matrix fault fixture */
#ifndef _DEVICE_TEST_FAULT_H
#define _DEVICE_TEST_FAULT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Task 11 brief §Step 4.  This header is the fault enum + macro API used
 * by the kernel probe paths (device/boot.c, net/lwip.c, driver/ahci.c)
 * to inject a single fault per build variant.  Only one of the six
 * ARCH9_FAULT slugs is active at any time (project.mk rejects
 * combinations and unknown values).  The fixture is a tiny TU
 * (kernel/device/test_fault.c) compiled only when ARCH9_FAULT != none.
 *
 * Architectural contract:
 *   - All hooks MUST be no-ops when ARCH9_FAULT=none.  The normal
 *     build never touches kernel/device/test_fault.c, so the canonical
 *     kernel.bin hash is byte-identical to a build that omits the
 *     fixture (verified by the brief's normal-before/after hash check).
 *   - Each fault is one symptom at the documented site.  Cross-site
 *     faults are NOT in scope (Task 12 / out-of-scope list).
 *
 * The active fault is dispatched at RUNTIME via
 * arch9_fault_get_active(): the preprocessor sees a single -D
 * ARCH9_FAULT_NAME=<slug> token and the dispatcher in test_fault.c
 * converts it to the OS01_TEST_FAULT_* enum value.  This sidesteps
 * the preprocessor's inability to substring-match in #if directives.
 */

#define OS01_TEST_FAULT_NONE          0
#define OS01_TEST_FAULT_OBSERVE       1
#define OS01_TEST_FAULT_BAD_NIC_BAR   2
#define OS01_TEST_FAULT_ADAPTER_FAIL  3
#define OS01_TEST_FAULT_AHCI_EMPTY    4
#define OS01_TEST_FAULT_IRQ_CONFLICT  5

/* ── Observation counters ─────────────────────────────────────────
 * observe (and every other non-none variant) exposes counters that the
 * harness greps via the kernel log.  For `none`, the counter macros
 * are no-ops.  Each counter is monotonically increasing; the brief
 * pins them to the corresponding probe site.
 *
 * Per-BDF tracking: the aggregate counters are not enough to verify
 * that an UNMATCHED device (e.g. e1000e 8086:10d3 in `unsupported`)
 * had zero probe hooks fired against it, because the MATCHED device
 * (e1000 8086:100e) contaminates the aggregate probe_calls counter.
 * The matrix runner can grep `arch9-fault-dev:` lines emitted by the
 * kernel for each enumerated BDF.  Per-BDF entries are registered at
 * PCI enumerate time (every device, not just probed ones) and
 * updated by the per-device hooks. */
#define ARCH9_OBS_MAX_BDF 16

struct arch9_obs_bdf {
    /* (domain << 16) | (bus << 8) | (slot << 3) | fn */
    uint16_t bdf;
    uint16_t vendor;
    uint16_t device;
    uint32_t probe_calls;
    uint32_t bar_writes;
    uint32_t adapter_registrations;
    uint32_t probe_unbound_no_match; /* probe returned without binding */
};

struct arch9_obs {
    uint32_t pci_drivers_exposed;     /* # of declared PCI drivers */
    uint32_t probe_calls;            /* # of probe() invocations   */
    uint32_t probe_unbound_no_match; /* probe returned without binding */
    uint32_t probe_unbound_after_id; /* probe rejected by id matcher */
    uint32_t bar_writes;             /* # of pci_config_write32 calls */
    uint32_t adapter_registrations;  /* # of net_device_register calls */
    uint32_t adapter_publishes;      /* # of net_device entries that reached ONLINE */
    uint32_t ahci_port_publications; /* # of block_device_register calls in AHCI */
    uint8_t bdf_count;               /* # of BDFs in bdf[]          */
    struct arch9_obs_bdf bdf[ARCH9_OBS_MAX_BDF];
};

#ifdef OS01_HOST_TEST
extern struct arch9_obs g_arch9_obs;

static inline void arch9_obs_reset(void) {
    g_arch9_obs.pci_drivers_exposed = 0;
    g_arch9_obs.probe_calls = 0;
    g_arch9_obs.probe_unbound_no_match = 0;
    g_arch9_obs.probe_unbound_after_id = 0;
    g_arch9_obs.bar_writes = 0;
    g_arch9_obs.adapter_registrations = 0;
    g_arch9_obs.adapter_publishes = 0;
    g_arch9_obs.ahci_port_publications = 0;
    g_arch9_obs.bdf_count = 0;
    for (int i = 0; i < ARCH9_OBS_MAX_BDF; i++) {
        g_arch9_obs.bdf[i].bdf = 0;
        g_arch9_obs.bdf[i].vendor = 0;
        g_arch9_obs.bdf[i].device = 0;
        g_arch9_obs.bdf[i].probe_calls = 0;
        g_arch9_obs.bdf[i].bar_writes = 0;
        g_arch9_obs.bdf[i].adapter_registrations = 0;
        g_arch9_obs.bdf[i].probe_unbound_no_match = 0;
    }
}
#else
extern struct arch9_obs g_arch9_obs;
#endif

/* Runtime dispatch: returns the active fault (or NONE) for the
 * current build.  See kernel/device/test_fault.c for the dispatch
 * table — the value is set at kernel init and never changes. */
int arch9_fault_get_active(void);

/* ── Probe-site hooks ─────────────────────────────────────────────
 * Each macro is called from the corresponding kernel probe path.
 * For NONE builds, every hook expands to nothing (the inline function
 * bodies never run because the observation counter block is inside
 * #ifdef OS01_TEST_FAULT — and OS01_TEST_FAULT is undefined when
 * ARCH9_FAULT=none).
 */

static inline void arch9_fault_on_pci_enumerate_begin(unsigned n_drivers) {
#ifdef OS01_TEST_FAULT
    g_arch9_obs.pci_drivers_exposed = n_drivers;
#endif
}

static inline void arch9_fault_on_probe_begin(const char *driver_name) {
#ifdef OS01_TEST_FAULT
    (void)driver_name;
    g_arch9_obs.probe_calls++;
#endif
}

static inline void arch9_fault_on_probe_unbound_no_match(void) {
#ifdef OS01_TEST_FAULT
    g_arch9_obs.probe_unbound_no_match++;
#endif
}

static inline void arch9_fault_on_probe_unbound_after_id(void) {
#ifdef OS01_TEST_FAULT
    g_arch9_obs.probe_unbound_after_id++;
#endif
}

static inline void arch9_fault_on_bar_write(void) {
#ifdef OS01_TEST_FAULT
    g_arch9_obs.bar_writes++;
#endif
}

static inline void arch9_fault_on_adapter_register(void) {
#ifdef OS01_TEST_FAULT
    g_arch9_obs.adapter_registrations++;
#endif
}

static inline void arch9_fault_on_adapter_publish(void) {
#ifdef OS01_TEST_FAULT
    g_arch9_obs.adapter_publishes++;
#endif
}

static inline void arch9_fault_on_ahci_port_publish(void) {
#ifdef OS01_TEST_FAULT
    g_arch9_obs.ahci_port_publications++;
#endif
}

/* ── Per-BDF observation helpers ────────────────────────────────
 * Each PCI device is registered exactly once at enumerate time
 * (kernel/device/boot.c:device_boot_init) so the dump subsys can
 * emit a per-device line.  The probe/bar/adapter hooks below update
 * the matching BDF's counters AND the aggregate counters, so the
 * existing observation_assertions keep working while the matrix
 * can also assert per-device (e.g. "the e1000e 8086:10d3 had
 * zero probe hooks fired against it"). */

static inline uint16_t arch9_obs_bdf_encode(uint8_t bus, uint8_t slot,
                                             uint8_t fn) {
    return (uint16_t)(((bus & 0xFF) << 8) | ((slot & 0x1F) << 3) |
                        (fn & 0x7));
}

static inline int arch9_obs_lookup_bdf(uint16_t bdf_key) {
    for (int i = 0; i < g_arch9_obs.bdf_count; i++) {
        if (g_arch9_obs.bdf[i].bdf == bdf_key) return i;
    }
    return -1;
}

static inline int arch9_obs_register_bdf(uint16_t bdf_key,
                                          uint16_t vendor,
                                          uint16_t device) {
    int idx = arch9_obs_lookup_bdf(bdf_key);
    if (idx >= 0) return idx;
    if (g_arch9_obs.bdf_count >= ARCH9_OBS_MAX_BDF) return -1;
    idx = g_arch9_obs.bdf_count++;
    g_arch9_obs.bdf[idx].bdf = bdf_key;
    g_arch9_obs.bdf[idx].vendor = vendor;
    g_arch9_obs.bdf[idx].device = device;
    return idx;
}

static inline void arch9_fault_register_bdf(uint16_t bdf_key,
                                             uint16_t vendor,
                                             uint16_t device) {
#ifdef OS01_TEST_FAULT
    (void)arch9_obs_register_bdf(bdf_key, vendor, device);
#else
    (void)bdf_key; (void)vendor; (void)device;
#endif
}

static inline void arch9_fault_on_probe_begin_bdf(uint16_t bdf_key,
                                                   const char *driver_name) {
#ifdef OS01_TEST_FAULT
    (void)driver_name;
    g_arch9_obs.probe_calls++;
    int idx = arch9_obs_lookup_bdf(bdf_key);
    if (idx >= 0) g_arch9_obs.bdf[idx].probe_calls++;
#endif
}

static inline void arch9_fault_on_bar_write_bdf(uint16_t bdf_key) {
#ifdef OS01_TEST_FAULT
    g_arch9_obs.bar_writes++;
    int idx = arch9_obs_lookup_bdf(bdf_key);
    if (idx >= 0) g_arch9_obs.bdf[idx].bar_writes++;
#endif
}

static inline void arch9_fault_on_adapter_register_bdf(uint16_t bdf_key) {
#ifdef OS01_TEST_FAULT
    g_arch9_obs.adapter_registrations++;
    int idx = arch9_obs_lookup_bdf(bdf_key);
    if (idx >= 0) g_arch9_obs.bdf[idx].adapter_registrations++;
#endif
}

static inline void arch9_fault_on_probe_unbound_no_match_bdf(uint16_t bdf_key) {
#ifdef OS01_TEST_FAULT
    g_arch9_obs.probe_unbound_no_match++;
    int idx = arch9_obs_lookup_bdf(bdf_key);
    if (idx >= 0) g_arch9_obs.bdf[idx].probe_unbound_no_match++;
#endif
}

/* ── Cross-driver IRQ-conflict helpers ────────────────────────────
 * Used by e1000.c / virtio-net.c to track the first card's GSI so
 * the second probe can detect a candidate GSI collision and fall to
 * POLL (ir-conflict fault).  All hooks are no-ops when ARCH9_FAULT
 * is none. */
static inline uint32_t arch9_fault_get_first_card_gsi(void) {
#ifdef OS01_TEST_FAULT
    extern uint32_t arch9_fault_first_card_gsi(void);
    return arch9_fault_first_card_gsi();
#else
    return 0;
#endif
}

static inline unsigned arch9_fault_get_nic_probe_count(void) {
#ifdef OS01_TEST_FAULT
    extern unsigned arch9_fault_nic_probe_count(void);
    return arch9_fault_nic_probe_count();
#else
    return 0;
#endif
}

static inline void arch9_fault_record_card_gsi(uint32_t gsi) {
#ifdef OS01_TEST_FAULT
    extern void arch9_fault_set_card_gsi(uint32_t gsi);
    arch9_fault_set_card_gsi(gsi);
#else
    (void)gsi;
#endif
}

/* ── Fault-injection predicates ────────────────────────────────────
 * Each returns true if the matching symptom must fire in this build
 * variant.  For OS01=none the answer is always false (the runtime
 * dispatch returns 0 and the comparison is never taken). */

/* bad-nic-bar: corrupt one NIC's BAR window so probe() aborts.
 * Returning true at the e1000 probe point makes the probe fail
 * after the id match.  The healthy root disk and any second NIC must
 * still work, so the fixture is per-call rather than per-build. */
static inline bool arch9_fault_should_inject_bad_nic_bar(unsigned probe_index) {
#ifdef OS01_TEST_FAULT
    if (arch9_fault_get_active() == OS01_TEST_FAULT_BAD_NIC_BAR) {
        return probe_index == 0;
    }
#endif
    return false;
}

/* adapter-fail: reject every adapter registration so net_device_register
 * returns -EINVAL.  Spec: "All adapter registrations rejected." */
static inline bool arch9_fault_should_inject_adapter_fail(void) {
#ifdef OS01_TEST_FAULT
    return arch9_fault_get_active() == OS01_TEST_FAULT_ADAPTER_FAIL;
#else
    return false;
#endif
}

/* ahci-empty: suppress media publication at the safe-enumeration point.
 * Driver still matches; port init runs; block_device_register is
 * skipped, so /dev/hd* is never created. */
static inline bool arch9_fault_should_inject_ahci_empty(void) {
#ifdef OS01_TEST_FAULT
    return arch9_fault_get_active() == OS01_TEST_FAULT_AHCI_EMPTY;
#else
    return false;
#endif
}

/* irq-conflict: when the second NIC tries to claim the same GSI as the
 * first, the first's already-owned holder path must fire; the second falls
 * back to POLL. */
static inline bool arch9_fault_should_force_irq_conflict(void) {
#ifdef OS01_TEST_FAULT
    return arch9_fault_get_active() == OS01_TEST_FAULT_IRQ_CONFLICT;
#else
    return false;
#endif
}

#endif /* _DEVICE_TEST_FAULT_H */