#include <intr/interrupt.h>
#include <arch/irq.h>
#include <core/debug.h>
#include <stddef.h>
#include <intr/softirq.h>
#include <string.h>
#include <errno.h>
#include <memory/slab.h>

// kernel/intr/irq.c — arch-neutral IRQ registration API & sparse registry.
//
// All public functions (register_irq, unregister_irq, irq_mask,
// irq_unmask) take a GSI number. The GSI→vector/hwirq translation
// and the controller-selection logic live in arch-specific hooks
// declared in kernel/include/arch/irq.h.

/* Legacy/intermediate compatibility array retained until all direct
 * references in test suites / drivers are fully migrated. */
irq_desc_t irq_table[NR_IQRS] = {0};

/* Early static 16-entry storage for PIC before slab/percpu is available. */
static irq_desc_t s_pic_descs[16];
static bool s_pic_mode = false;

/* Dynamic range bucket for sparse GSI ranges */
struct irq_range_bucket {
    uint32_t base;
    uint32_t count;
    irq_desc_t *descs;
};

static struct irq_range_bucket *s_ranges = NULL;
static size_t s_range_count = 0;

void irq_registry_init_pic(void)
{
    /* If an active handler exists in PIC storage or dynamic ranges, refuse reset */
    if (s_ranges != NULL) {
        for (size_t i = 0; i < s_range_count; i++) {
            for (uint32_t j = 0; j < s_ranges[i].count; j++) {
                if (s_ranges[i].descs[j].handler != NULL)
                    return;
            }
        }
    } else if (s_pic_mode) {
        for (size_t j = 0; j < 16; j++) {
            if (s_pic_descs[j].handler != NULL)
                return;
        }
    }

    arch_irq_state_t flags = arch_local_irq_save();
    struct irq_range_bucket *old_buckets = s_ranges;
    size_t old_count = s_range_count;

    __atomic_store_n(&s_ranges, NULL, __ATOMIC_RELEASE);
    s_range_count = 0;
    memset(s_pic_descs, 0, sizeof(s_pic_descs));
    s_pic_mode = true;
    arch_local_irq_restore(flags);

    if (old_buckets != NULL) {
        for (size_t i = 0; i < old_count; i++) {
            kfree(old_buckets[i].descs);
        }
        kfree(old_buckets);
    }
}

irq_desc_t *irq_desc_for_gsi(uint32_t gsi)
{
    struct irq_range_bucket *ranges = __atomic_load_n(&s_ranges, __ATOMIC_ACQUIRE);
    if (ranges != NULL) {
        size_t range_count = s_range_count;
        for (size_t i = 0; i < range_count; i++) {
            uint32_t base = ranges[i].base;
            uint32_t count = ranges[i].count;
            if (gsi >= base && (uint64_t)gsi < (uint64_t)base + count) {
                return &ranges[i].descs[gsi - base];
            }
        }
        return NULL;
    }

    if (s_pic_mode) {
        if (gsi < 16) {
            return &s_pic_descs[gsi];
        }
        return NULL;
    }

    /* Explicit temporary 24-entry adaptation before migration in Task 3/4 */
    if (gsi < 24) {
        return &irq_table[gsi];
    }

    return NULL;
}

int irq_registry_init_ranges(const struct irq_gsi_range *ranges, size_t count)
{
    if (ranges == NULL || count == 0) {
        return -EINVAL;
    }

    /* Refuse reset if any active handler exists */
    if (s_ranges != NULL) {
        for (size_t i = 0; i < s_range_count; i++) {
            for (uint32_t j = 0; j < s_ranges[i].count; j++) {
                if (s_ranges[i].descs[j].handler != NULL)
                    return -EBUSY;
            }
        }
    } else if (s_pic_mode) {
        for (size_t j = 0; j < 16; j++) {
            if (s_pic_descs[j].handler != NULL)
                return -EBUSY;
        }
    } else {
        for (size_t j = 0; j < 24; j++) {
            if (irq_table[j].handler != NULL)
                return -EBUSY;
        }
    }

    /* Validate endpoints and overlaps */
    for (size_t i = 0; i < count; i++) {
        if (ranges[i].count == 0)
            return -EINVAL;

        uint64_t end = (uint64_t)ranges[i].base + ranges[i].count;
        if (end > (uint64_t)UINT32_MAX + 1)
            return -EINVAL;

        for (size_t j = 0; j < i; j++) {
            uint64_t end_j = (uint64_t)ranges[j].base + ranges[j].count;
            if (!((uint64_t)ranges[i].base >= end_j || end <= (uint64_t)ranges[j].base)) {
                return -EINVAL; /* Overlapping */
            }
        }
    }

    /* Allocate new range buckets */
    struct irq_range_bucket *new_buckets = kmalloc(count * sizeof(struct irq_range_bucket));
    if (new_buckets == NULL) {
        return -ENOMEM;
    }
    memset(new_buckets, 0, count * sizeof(struct irq_range_bucket));

    for (size_t i = 0; i < count; i++) {
        new_buckets[i].base = ranges[i].base;
        new_buckets[i].count = ranges[i].count;

        size_t bytes = (size_t)ranges[i].count * sizeof(irq_desc_t);
        new_buckets[i].descs = kmalloc(bytes);
        if (new_buckets[i].descs == NULL) {
            for (size_t k = 0; k < i; k++) {
                kfree(new_buckets[k].descs);
            }
            kfree(new_buckets);
            return -ENOMEM;
        }
        memset(new_buckets[i].descs, 0, bytes);
    }

    /* Transactional publish under short irqsave lock */
    arch_irq_state_t flags = arch_local_irq_save();
    struct irq_range_bucket *old_buckets = s_ranges;
    size_t old_count = s_range_count;

    s_range_count = count;
    __atomic_store_n(&s_ranges, new_buckets, __ATOMIC_RELEASE);
    s_pic_mode = false;
    arch_local_irq_restore(flags);

    /* Free old buckets outside lock */
    if (old_buckets != NULL) {
        for (size_t i = 0; i < old_count; i++) {
            kfree(old_buckets[i].descs);
        }
        kfree(old_buckets);
    }

    return 0;
}

int32_t register_irq(uint32_t gsi, void *arg,
        void (*handler)(uint64_t nr, uint64_t parameter, pt_regs_t *regs),
        uint64_t parameter, uint32_t flags, const char *irq_name)
{
    if (handler == NULL || irq_name == NULL) {
        debug_irq("IRQ: invalid handler or name for GSI %u\n", gsi);
        return 0;
    }

    size_t name_len = strlen(irq_name);
    if (name_len == 0 || name_len >= sizeof(((irq_desc_t *)0)->irq_name)) {
        debug_irq("IRQ: invalid name length for GSI %u\n", gsi);
        return 0;
    }

    irq_desc_t *p = irq_desc_for_gsi(gsi);
    if (p == NULL) {
        debug_irq("IRQ: GSI %u not found in registry\n", gsi);
        return 0;
    }

    hw_int_controller_t *controller = arch_irq_select_controller(gsi);
    if (controller == NULL) {
        debug_irq("IRQ: no controller available for GSI %u\n", gsi);
        return 0;
    }

    uint64_t vector = 0;
    int prep_rc = arch_irq_route_prepare(gsi, &vector);
    if (prep_rc < 0) {
        debug_irq("IRQ: arch route prepare failed for GSI %u: %d\n", gsi, prep_rc);
        return 0;
    }

    arch_irq_state_t irq_flags = arch_local_irq_save();
    if (p->handler != NULL || p->controller != NULL) {
        arch_local_irq_restore(irq_flags);
        debug_irq("IRQ: GSI %u already occupied\n", gsi);
        return 0;
    }

    p->controller = controller;
    strncpy(p->irq_name, irq_name, sizeof(p->irq_name) - 1);
    p->irq_name[sizeof(p->irq_name) - 1] = '\0';
    p->parameter = parameter;
    p->flags = flags;
    p->handler = handler;
    arch_local_irq_restore(irq_flags);

    /* Invoke controller outside the registry lock */
    if (controller->install != NULL) {
        uint64_t install_rc = controller->install(vector, arg);
        if (install_rc != 0) {
            arch_irq_state_t rollback_flags = arch_local_irq_save();
            p->handler = NULL;
            p->controller = NULL;
            p->irq_name[0] = '\0';
            p->parameter = 0;
            p->flags = 0;
            arch_local_irq_restore(rollback_flags);
            debug_irq("IRQ: controller install failed for GSI %u: %lu\n", gsi, install_rc);
            return 0;
        }
    }
    if (controller->enable != NULL) {
        controller->enable(vector);
    }

    return 1;
}

uint32_t unregister_irq(uint32_t gsi)
{
    irq_desc_t *p = irq_desc_for_gsi(gsi);
    if (p == NULL) {
        debug_irq("unregister_irq: GSI %u not found in registry\n", gsi);
        return 0;
    }

    arch_irq_state_t irq_flags = arch_local_irq_save();
    if (p->handler == NULL && p->controller == NULL) {
        arch_local_irq_restore(irq_flags);
        return 0;
    }

    hw_int_controller_t *ctrl = p->controller;
    /* Clear handler under lock so no new invocations begin, while keeping
     * p->controller intact so in-flight interrupts can still ACK. */
    p->handler = NULL;
    arch_local_irq_restore(irq_flags);

    uint64_t vector = arch_irq_gsi_to_vector(gsi);
    if (ctrl != NULL) {
        if (ctrl->disable != NULL)
            ctrl->disable(vector);
        if (ctrl->uninstall != NULL)
            ctrl->uninstall(vector);
    }

    /* Clear controller and metadata under lock once controller disable/uninstall finishes */
    irq_flags = arch_local_irq_save();
    p->controller = NULL;
    p->irq_name[0] = '\0';
    p->parameter = 0;
    p->flags = 0;
    arch_local_irq_restore(irq_flags);

    return 1;
}

void irq_install(void)
{
    irq_registry_init_pic();
    arch_irq_install();
    softirq_init();
}

void irq_mask(uint32_t gsi)
{
    irq_desc_t *p = irq_desc_for_gsi(gsi);
    if (p != NULL && p->controller != NULL && p->controller->disable != NULL) {
        p->controller->disable(arch_irq_gsi_to_vector(gsi));
    }
}

void irq_unmask(uint32_t gsi)
{
    irq_desc_t *p = irq_desc_for_gsi(gsi);
    if (p != NULL && p->controller != NULL && p->controller->enable != NULL) {
        p->controller->enable(arch_irq_gsi_to_vector(gsi));
    }
}
