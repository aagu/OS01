#include <selftest/boot_contract_fixture.h>
#include <subsys/subsys.h>
#include <log/log.h>

#if defined(BOOT_CONTRACT_CASE_REVERSE)

static const char *const consumer_reqs[] = {"boot4-producer"};

static int boot4_consumer_init(void)
{
    int ready = (subsys_require_ready("boot4-producer") == 0) ? 1 : 0;
    log_info("[boot-4-test] callback=consumer dependency_status=%d\n", ready);
    return 0;
}

static int boot4_producer_init(void)
{
    log_info("[boot-4-test] callback=producer\n");
    return 0;
}

void boot4_test_register(void)
{
    log_info("[boot-4-test] variant=boot-contract-reverse\n");
    /* Register consumer first, producer second in phase 5 */
    register_subsys_deps("boot4-consumer", boot4_consumer_init,
                         SUBSYS_PHASE_DEVICE_IRQ, 0,
                         consumer_reqs, 1);
    register_subsys("boot4-producer", boot4_producer_init,
                    SUBSYS_PHASE_DEVICE_IRQ, 0);
}

#elif defined(BOOT_CONTRACT_CASE_MISSING)

static const char *const missing_reqs[] = {"boot4-absent"};

static int boot4_consumer_init(void)
{
    return 0;
}

void boot4_test_register(void)
{
    log_info("[boot-4-test] variant=boot-contract-missing\n");
    register_subsys_deps("boot4-consumer", boot4_consumer_init,
                         SUBSYS_PHASE_DEVICE_IRQ, 0,
                         missing_reqs, 1);
}

#elif defined(BOOT_CONTRACT_CASE_CYCLE)

static const char *const cons_reqs[] = {"boot4-producer"};
static const char *const prod_reqs[] = {"boot4-consumer"};

static int boot4_consumer_init(void)
{
    return 0;
}

static int boot4_producer_init(void)
{
    return 0;
}

void boot4_test_register(void)
{
    log_info("[boot-4-test] variant=boot-contract-cycle\n");
    /* Consumer requires producer, producer requires consumer */
    register_subsys_deps("boot4-consumer", boot4_consumer_init,
                         SUBSYS_PHASE_DEVICE_IRQ, 0,
                         cons_reqs, 1);
    register_subsys_deps("boot4-producer", boot4_producer_init,
                         SUBSYS_PHASE_DEVICE_IRQ, 0,
                         prod_reqs, 1);
}

#else

void boot4_test_register(void)
{
}

#endif
