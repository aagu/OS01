#pragma once
#include <stdint.h>
#include <stdbool.h>
#define NR_CPUS 8
void arch_cpu_pause(void);
uint64_t rdtscp_serialized(void);
