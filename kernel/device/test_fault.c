/* kernel/device/test_fault.c — Driver-model matrix fault fixture (Task 11)
 *
 * Only compiled when ARCH9_FAULT != none (kernel/Makefile).  The
 * canonical (none) build never sees this TU so kernel/device/test_fault.c
 * cannot pollute the normal kernel.bin — the brief's normal-before/after
 * hash invariant.
 *
 * Source of truth: kernel/include/device/test_fault.h.
 * This TU owns the g_arch9_obs counter object and the
 * arch9_fault_get_active() runtime dispatcher.
 */

#include <device/test_fault.h>
#include <string.h>

#ifndef OS01_HOST_TEST
#include <core/debug.h>
#include <log/log.h>
#include <subsys/subsys.h>
#endif

struct arch9_obs g_arch9_obs;

#ifdef OS01_HOST_TEST
/* Host tests bypass the runtime dispatch — they directly poke
 * counters and fault predicates via the header inline hooks. */
static int s_active_for_test = OS01_TEST_FAULT_NONE;
int arch9_fault_get_active(void) { return s_active_for_test; }

/* IRQ-conflict tracking (host-test) — explicit storage so the
 * header inlines can read/write.  Production builds own this state
 * via the host-test branch below. */
static uint32_t s_first_card_gsi = 0;
static unsigned s_nic_probe_count = 0;
uint32_t arch9_fault_first_card_gsi(void) { return s_first_card_gsi; }
unsigned arch9_fault_nic_probe_count(void) { return s_nic_probe_count; }
void arch9_fault_set_card_gsi(uint32_t gsi) {
    if (s_nic_probe_count == 0 && gsi != 0) {
        s_first_card_gsi = gsi;
    }
    s_nic_probe_count++;
}
#else
/* ARCH9_FAULT_NAME is injected as -DARCH9_FAULT_NAME=<slug> from
 * kernel/Makefile.  Convert the slug to the enum value at boot. */
static int s_active_fault = OS01_TEST_FAULT_NONE;

/* IRQ-conflict tracking (production) — see header docstring. */
static uint32_t s_first_card_gsi = 0;
static unsigned s_nic_probe_count = 0;
uint32_t arch9_fault_first_card_gsi(void) { return s_first_card_gsi; }
unsigned arch9_fault_nic_probe_count(void) { return s_nic_probe_count; }
void arch9_fault_set_card_gsi(uint32_t gsi) {
    if (s_nic_probe_count == 0 && gsi != 0) {
        s_first_card_gsi = gsi;
    }
    s_nic_probe_count++;
}

static int resolve_slug(const char *slug)
{
    if (!slug) return OS01_TEST_FAULT_NONE;
    if (strcmp(slug, "observe") == 0) return OS01_TEST_FAULT_OBSERVE;
    if (strcmp(slug, "bad-nic-bar") == 0) return OS01_TEST_FAULT_BAD_NIC_BAR;
    if (strcmp(slug, "adapter-fail") == 0) return OS01_TEST_FAULT_ADAPTER_FAIL;
    if (strcmp(slug, "ahci-empty") == 0) return OS01_TEST_FAULT_AHCI_EMPTY;
    if (strcmp(slug, "irq-conflict") == 0) return OS01_TEST_FAULT_IRQ_CONFLICT;
    return OS01_TEST_FAULT_NONE;
}

int arch9_fault_get_active(void)
{
    return s_active_fault;
}

static int _fault_dispatch_at_init(void)
{
    s_active_fault = resolve_slug(ARCH9_FAULT_NAME);
    log_info("arch9-fault: slug=%s active=%d\n",
              ARCH9_FAULT_NAME, s_active_fault);
    return 0;
}

/* One-shot dump so the matrix harness can grep the boot log. */
static int _fault_dump_subsys_init(void)
{
    log_info("arch9-fault: active=%d drivers=%u probe=%u unbound_no_match=%u "
             "unbound_after_id=%u bar_writes=%u adapters=%u publishes=%u "
             "ahci_ports=%u\n",
             s_active_fault,
             g_arch9_obs.pci_drivers_exposed,
             g_arch9_obs.probe_calls,
             g_arch9_obs.probe_unbound_no_match,
             g_arch9_obs.probe_unbound_after_id,
             g_arch9_obs.bar_writes,
             g_arch9_obs.adapter_registrations,
             g_arch9_obs.adapter_publishes,
             g_arch9_obs.ahci_port_publications);
    return 0;
}

/* The dispatcher must run before any probe path that calls
 * arch9_fault_get_active(); the dump can run last so the harness can
 * observe the cumulative counters.  PHASE_5 places the dispatcher
 * ahead of AHCI (PHASE_6).  The dump subsys runs in PHASE_6 so the
 * AHCI probe has already run by the time we print. */
static int _fault_register(void)
{
    register_subsys("arch9-fault-dispatch", _fault_dispatch_at_init,
                    SUBSYS_PHASE_5, 1 /* OPTIONAL */);
    register_subsys("arch9-fault-dump", _fault_dump_subsys_init,
                    SUBSYS_PHASE_6, 1 /* OPTIONAL */);
    return 0;
}
SUBSYS_INITCALL(_fault_register);
#endif