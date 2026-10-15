/*
 * kernel/arch/x86_64/cpu/thermal_decode.c — pure decoder
 *
 * Pure function (no MSR access, no global state, no kernel
 * symbols) — pulled out of the strong backend so the host fixture
 * can compile and link it directly.  Spec §3.2 decode math; see
 * kernel/arch/x86_64/cpu/thermal.c for the surrounding probe +
 * publish state machine.
 *
 * Input/output contract (monitor plan §Task 3):
 *   target      = TjMax in °C (from MSR_TEMPERATURE_TARGET[23:16])
 *   status      = raw IA32_THERM_STATUS register value
 *                 bits 31   = reading valid
 *                 bits 22:16 = reading  (unsigned 7-bit, == 0 means
 *                                     "no current temperature")
 *                 bits 26:20 = distance (signed 7-bit two's
 *                                     complement; °C below TjMax)
 *   temp_mc     = on success: (TjMax - distance) in millidegrees C
 *                 — may be negative
 *
 *   return  0   on success
 *           -EINVAL  on (status.valid==0) OR (reading==0) OR (out==NULL)
 */

#include <arch/thermal.h>
#include <errno.h>
#include <stdint.h>

int thermal_decode_core(uint64_t target, uint64_t status, int32_t *temp_mc)
{
    if (!temp_mc)
        return -EINVAL;

    /* Per Intel SDM Vol 4 §14.7.2 (IA32_THERM_STATUS, MSR 0x19C):
     *   bit  31  : Reading valid (1 = reading is trustworthy)
     *   bits 22:16: "Digital readout" — TjMax − Tcur in degrees C,
     *               7-bit unsigned, range 0..127.
     *   bits 27:23: Reserved.
     *
     * Plan §Task 3 documentation calls this the "distance" field
     * (degrees below TjMax).  It is the SAME 7 bits as the
     * "reading" field; both names refer to the same encodings in
     * the same register.
     */
    uint32_t valid   = (uint32_t)((status >> 31) & 1U);
    uint32_t reading = (uint32_t)((status >> 16) & 0x7FU);

    if (valid == 0 || reading == 0)
        return -EINVAL;

    int32_t dist = (int32_t)reading;

    int32_t c = (int32_t)target - dist;
    *temp_mc = c * 1000;
    return 0;
}
