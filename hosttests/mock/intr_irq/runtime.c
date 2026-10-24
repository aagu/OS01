/* Hardware CPU context replacement; production percpu layout is retained. */
#include <percpu/percpu.h>
#include <arch/x86_64/gate.h>
#include <intr/interrupt.h>
percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus = 2;
static unsigned selected_cpu;
struct gate_struct IDT_Table[256];
void *arch_this_cpu_ptr(void) { return &percpu_data[selected_cpu]; }
void irq_test_cpu(unsigned id) {
    selected_cpu = id;
    percpu_data[id].cpu_id = id;
    percpu_data[id].need_resched = 0;
    percpu_data[id].watchdog_counter = 0;
    percpu_data[id].tlb_ack_gen = 100;
}
uint64_t irq_test_need(void) { return percpu_data[selected_cpu].need_resched; }
uint64_t irq_test_watchdog(void) { return percpu_data[selected_cpu].watchdog_counter; }
uint64_t irq_test_gen(void) { return percpu_data[selected_cpu].tlb_ack_gen; }
void ret_from_intr(void) { __builtin_trap(); }
