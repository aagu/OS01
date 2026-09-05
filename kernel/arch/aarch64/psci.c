#include <stdint.h>
#include <kernel/arch/aarch64/psci.h>

#define PSCI_VERSION_FID       UINT64_C(0x84000000)
#define PSCI_CPU_ON64_FID      UINT64_C(0xc4000003)

static enum psci_conduit psci_conduit = PSCI_CONDUIT_NONE;

static uint64_t psci_call(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
    switch (psci_conduit) {
    case PSCI_CONDUIT_SMC:
        return psci_call_smc(fid, a1, a2, a3);
    case PSCI_CONDUIT_HVC:
        return psci_call_hvc(fid, a1, a2, a3);
    default:
        return UINT64_C(0xffffffff);
    }
}

int psci_init(enum psci_conduit conduit)
{
    uint64_t raw_version;
    uint32_t version;
    int32_t status;
    uint16_t major;
    uint16_t minor;

    psci_conduit = PSCI_CONDUIT_NONE;
    if (conduit != PSCI_CONDUIT_SMC && conduit != PSCI_CONDUIT_HVC)
        return -1;

    psci_conduit = conduit;
    raw_version = psci_call(PSCI_VERSION_FID, 0, 0, 0);
    version = (uint32_t)raw_version;
    status = (int32_t)version;
    if (status < 0)
        goto unavailable;

    major = (uint16_t)(version >> 16);
    minor = (uint16_t)version;
    if (major == 0 && minor < 2)
        goto unavailable;
    return 0;

unavailable:
    psci_conduit = PSCI_CONDUIT_NONE;
    return -1;
}

int32_t psci_cpu_on(uint64_t target_mpidr, uint64_t entry, uint64_t context_id)
{
    if (psci_conduit == PSCI_CONDUIT_NONE)
        return -1;
    return (int32_t)(uint32_t)psci_call(PSCI_CPU_ON64_FID, target_mpidr,
                                         entry, context_id);
}
