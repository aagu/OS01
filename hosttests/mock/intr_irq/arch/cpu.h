#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <config/limits.h>
void arch_cpu_pause(void);
void arch_cpu_halt(void);
uint64_t rdtscp_serialized(void);
