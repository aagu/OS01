#include <arch/x86_64/irq_route.h>
#include <arch/irq.h>
#include <intr/apic.h>
#include <errno.h>
#include <string.h>

#if defined(OS01_HOST_TEST)
extern void *kmalloc(size_t size);
extern void kfree(void *ptr);
#else
#include <memory/slab.h>
#endif

struct x86_route_bucket {
    uint32_t base;
    uint32_t count;
    uint8_t  ioapic_idx;
    struct x86_irq_route *routes;
};

static struct x86_route_bucket s_buckets[MAX_IOAPICS];
static size_t s_bucket_count = 0;

/* Bitmap tracking 62 extended vectors in [0x42, 0x7f].
 * bit i represents vector 0x42 + i (0 <= i < 62). */
static uint64_t s_ext_vector_bitmap = 0;

int x86_irq_routes_init(const struct irq_gsi_range *ranges, size_t count)
{
    if (!ranges || count == 0 || count > MAX_IOAPICS)
        return -EINVAL;

    /* Validate ranges: count > 0, no 32-bit addition overflow, no overlaps */
    for (size_t i = 0; i < count; i++) {
        if (ranges[i].count == 0)
            return -EINVAL;
        uint64_t end = (uint64_t)ranges[i].base + ranges[i].count;
        if (end > UINT32_MAX + 1ULL)
            return -EINVAL;
        for (size_t j = 0; j < i; j++) {
            uint64_t end_j = (uint64_t)ranges[j].base + ranges[j].count;
            if (ranges[i].base < end_j && end > ranges[j].base)
                return -EINVAL;
        }
    }

    /* Allocate memory for all buckets atomically before modifying active state */
    struct x86_irq_route *new_routes[MAX_IOAPICS];
    memset(new_routes, 0, sizeof(new_routes));

    for (size_t i = 0; i < count; i++) {
        size_t bytes = ranges[i].count * sizeof(struct x86_irq_route);
        new_routes[i] = (struct x86_irq_route *)kmalloc(bytes);
        if (!new_routes[i]) {
            for (size_t j = 0; j < i; j++) {
                kfree(new_routes[j]);
            }
            return -ENOMEM;
        }
        memset(new_routes[i], 0, bytes);
    }

    /* Populate routes */
    for (size_t i = 0; i < count; i++) {
        for (uint32_t p = 0; p < ranges[i].count; p++) {
            uint32_t gsi = ranges[i].base + p;
            struct x86_irq_route *r = &new_routes[i][p];
            r->gsi = gsi;
            r->ioapic_idx = (uint8_t)i;
            r->pin = (uint8_t)p;

            /* Determine trigger & polarity */
            uint16_t flags = 0;
            if (irq_trigger_for_gsi(gsi) == ISO_TRIGGER_LEVEL)
                flags |= IOAPIC_RED_TRIG_LEVEL;

            uint8_t pol = ISO_POLARITY_CONF_BUS;
            for (uint32_t j = 0; j < apic_info.iso_count; j++) {
                if (apic_info.isos[j].gsi == gsi) {
                    pol = apic_info.isos[j].flags & ISO_POLARITY_MASK;
                    break;
                }
            }
            if (pol == ISO_POLARITY_ACTIVE_LOW ||
                (pol == ISO_POLARITY_CONF_BUS && gsi >= 16))
                flags |= IOAPIC_RED_POL_LOW;

            r->flags = flags;

            /* Pre-reserve historical 0x20..0x37 for low GSIs 0..23 */
            if (gsi < X86_IRQ_ROUTE_LOW_GSI_MAX) {
                r->vector = X86_IRQ_ROUTE_LOW_VEC_BASE + gsi;
                r->allocated = false;
            } else {
                r->vector = X86_IRQ_VECTOR_INVALID;
                r->allocated = false;
            }
        }
    }

    /* Free old routes */
    for (size_t i = 0; i < s_bucket_count; i++) {
        if (s_buckets[i].routes) {
            kfree(s_buckets[i].routes);
            s_buckets[i].routes = NULL;
        }
    }

    /* Commit new buckets */
    for (size_t i = 0; i < count; i++) {
        s_buckets[i].base = ranges[i].base;
        s_buckets[i].count = ranges[i].count;
        s_buckets[i].ioapic_idx = (uint8_t)i;
        s_buckets[i].routes = new_routes[i];
    }
    s_bucket_count = count;
    s_ext_vector_bitmap = 0;

    return 0;
}

void x86_irq_routes_reset(void)
{
    for (size_t i = 0; i < s_bucket_count; i++) {
        if (s_buckets[i].routes) {
            kfree(s_buckets[i].routes);
            s_buckets[i].routes = NULL;
        }
    }
    s_bucket_count = 0;
    s_ext_vector_bitmap = 0;
}

int x86_irq_route_prepare(uint32_t gsi, uint64_t *vector)
{
    if (!vector)
        return -EINVAL;

    if (s_bucket_count == 0)
        return -ENOSYS;

    struct x86_irq_route *entry = NULL;
    for (size_t i = 0; i < s_bucket_count; i++) {
        if (gsi >= s_buckets[i].base && gsi < s_buckets[i].base + s_buckets[i].count) {
            entry = &s_buckets[i].routes[gsi - s_buckets[i].base];
            break;
        }
    }

    if (!entry)
        return -EINVAL;

    if (entry->allocated) {
        *vector = entry->vector;
        return 0;
    }

    if (gsi < X86_IRQ_ROUTE_LOW_GSI_MAX) {
        entry->vector = X86_IRQ_ROUTE_LOW_VEC_BASE + gsi;
        entry->allocated = true;
        *vector = entry->vector;
        return 0;
    }

    /* Allocate from extended pool 0x42..0x7f (62 slots) */
    for (uint32_t bit = 0; bit < X86_IRQ_ROUTE_EXT_COUNT; bit++) {
        if (!(s_ext_vector_bitmap & (1ULL << bit))) {
            s_ext_vector_bitmap |= (1ULL << bit);
            entry->vector = X86_IRQ_ROUTE_EXT_VEC_FIRST + bit;
            entry->allocated = true;
            *vector = entry->vector;
            return 0;
        }
    }

    /* Extended pool exhausted */
    return -ENOSPC;
}

bool x86_irq_route_lookup(uint64_t vector, uint32_t *gsi)
{
    if (vector >= X86_IRQ_ROUTE_LOW_VEC_BASE &&
        vector < X86_IRQ_ROUTE_LOW_VEC_BASE + X86_IRQ_ROUTE_LOW_GSI_MAX) {
        uint32_t cand = (uint32_t)(vector - X86_IRQ_ROUTE_LOW_VEC_BASE);
        for (size_t i = 0; i < s_bucket_count; i++) {
            if (cand >= s_buckets[i].base && cand < s_buckets[i].base + s_buckets[i].count) {
                struct x86_irq_route *r = &s_buckets[i].routes[cand - s_buckets[i].base];
                if (r->allocated && r->vector == vector) {
                    if (gsi) *gsi = r->gsi;
                    return true;
                }
            }
        }
        return false;
    }

    if (vector >= X86_IRQ_ROUTE_EXT_VEC_FIRST && vector <= X86_IRQ_ROUTE_EXT_VEC_LAST) {
        for (size_t i = 0; i < s_bucket_count; i++) {
            for (uint32_t p = 0; p < s_buckets[i].count; p++) {
                struct x86_irq_route *r = &s_buckets[i].routes[p];
                if (r->allocated && r->vector == vector) {
                    if (gsi) *gsi = r->gsi;
                    return true;
                }
            }
        }
        return false;
    }

    return false;
}

const struct x86_irq_route *x86_irq_route_find_by_gsi(uint32_t gsi)
{
    for (size_t i = 0; i < s_bucket_count; i++) {
        if (gsi >= s_buckets[i].base && gsi < s_buckets[i].base + s_buckets[i].count) {
            return &s_buckets[i].routes[gsi - s_buckets[i].base];
        }
    }
    return NULL;
}

const struct x86_irq_route *x86_irq_route_find_by_vector(uint64_t vector)
{
    if (vector >= X86_IRQ_ROUTE_LOW_VEC_BASE &&
        vector < X86_IRQ_ROUTE_LOW_VEC_BASE + X86_IRQ_ROUTE_LOW_GSI_MAX) {
        uint32_t cand = (uint32_t)(vector - X86_IRQ_ROUTE_LOW_VEC_BASE);
        const struct x86_irq_route *r = x86_irq_route_find_by_gsi(cand);
        if (r && r->allocated && r->vector == vector)
            return r;
        return NULL;
    }

    if (vector >= X86_IRQ_ROUTE_EXT_VEC_FIRST && vector <= X86_IRQ_ROUTE_EXT_VEC_LAST) {
        for (size_t i = 0; i < s_bucket_count; i++) {
            for (uint32_t p = 0; p < s_buckets[i].count; p++) {
                const struct x86_irq_route *r = &s_buckets[i].routes[p];
                if (r->allocated && r->vector == vector)
                    return r;
            }
        }
        return NULL;
    }

    return NULL;
}
