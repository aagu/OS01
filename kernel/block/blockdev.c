// kernel/block/blockdev.c — dynamic block device registry (ARCH-10)
//
// The registry is a linked list of individually allocated objects.  Every
// registration allocates its own object, so the number of block devices is
// bounded by memory instead of by a compile-time table size.
//
// Two invariants drive the layout:
//
//   * A published object never moves.  AHCI, GPT, devfs and the boot path
//     hold block_device_t pointers for the lifetime of the system, so the
//     registry must not be a reallocatable array.
//   * Allocation and driver/VFS callbacks happen outside the registry
//     lock.  Only the duplicate-name re-check and the publish run inside
//     the short irqsave critical section.
//
// The list node lives in a private wrapper, so the published struct
// block_device keeps the shape the ARCH-9 consumers already know.

#include <block/blockdev.h>
#include <core/debug.h>
#include <errno.h>
#include <kernel.h>          // container_of
#include <list.h>
#include <memory/slab.h>     // kmalloc, kfree
#include <stdbool.h>
#include <string.h>
#include <arch/spinlock.h>

// ── Registry ───────────────────────────────────────────────────────
//
// Private wrapper: the published object is embedded in it, so one
// allocation owns both the registry linkage and the object.  The
// wrapper is never moved or reallocated, so &entry->dev is a stable
// address for the whole lifetime of the device.
struct block_device_entry {
    list_t         node;  /* registry linkage (private) */
    block_device_t dev;   /* the published object */
};

// Head sentinels.  g_registry holds the enumerable devices in
// registration order (g_registry.next is the first, g_registry.prev the
// last one); g_retired holds the devices that were rolled back during
// boot.  Both are statically initialized, so block_device_init() never
// has to reset (and thereby lose) the registry.
static list_t     g_registry = { &g_registry, &g_registry };
static list_t     g_retired  = { &g_retired,  &g_retired  };
static spinlock_T g_registry_lock = { 1 };
static int        g_registry_ready = 0;

// ── Internal helpers (caller holds g_registry_lock) ────────────────

// Membership test by pointer identity.  The candidate pointer is only
// compared, never dereferenced, so a foreign pointer is safe to pass.
static struct block_device_entry *registry_find(block_device_t *dev)
{
    for (list_t *n = list_next(&g_registry); n && n != &g_registry; n = list_next(n)) {
        struct block_device_entry *e = container_of(n, struct block_device_entry, node);
        if (&e->dev == dev)
            return e;
    }
    return NULL;
}

static bool registry_name_taken(const char *name)
{
    for (list_t *n = list_next(&g_registry); n && n != &g_registry; n = list_next(n)) {
        struct block_device_entry *e = container_of(n, struct block_device_entry, node);
        if (strcmp(e->dev.name, name) == 0)
            return true;
    }
    return false;
}

// ── Initialization ─────────────────────────────────────────────────

// Idempotent.  The registry is statically initialized, so there is
// nothing to set up here and nothing that could reset (and leak) the
// objects published by an earlier call.
void block_device_init(void)
{
    if (g_registry_ready)
        return;

    g_registry_ready = 1;
    debug_block("block: subsystem initialized\n");
}

// ── Registration ──────────────────────────────────────────────────

int block_device_register(const struct block_device_desc *desc, block_device_t **out)
{
    if (out)
        *out = NULL;

    if (!desc || !desc->name || desc->name[0] == '\0')
        return -EINVAL;

    if (strlen(desc->name) >= BLOCKDEV_NAME_MAX)
        return -EINVAL;

    // read op is required; write and flush are optional
    if (!desc->ops || !desc->ops->read)
        return -EINVAL;

    block_device_init();   // idempotent

    // Allocate and initialize outside the registry lock: the object is
    // published only after the duplicate-name re-check below.
    struct block_device_entry *entry = kmalloc(sizeof(*entry));
    if (!entry) {
        debug_block("block: allocation failed for %s\n", desc->name);
        return -ENOMEM;
    }

    memset(&entry->dev, 0, sizeof(entry->dev));
    strncpy(entry->dev.name, desc->name, BLOCKDEV_NAME_MAX - 1);
    entry->dev.sector_count = desc->sector_count;
    entry->dev.sector_size  = desc->sector_size ? desc->sector_size : 512;
    entry->dev.present      = 1;
    entry->dev.kind         = desc->kind;
    entry->dev.ops          = desc->ops;
    entry->dev.private_data = desc->private_data;
    entry->dev.parent       = desc->parent;
    entry->dev.last_error   = 0;
    list_init(&entry->node);

    // Publish: re-check the duplicate name against the live registry and
    // append at the tail, so the enumeration order follows the
    // successful registration order.  A device registered after a boot
    // unregister therefore lands at the end, not in the hole the
    // unregistered device left behind.
    uint64_t flags = spin_lock_irqsave(&g_registry_lock);
    if (registry_name_taken(desc->name)) {
        spin_unlock_irqrestore(&g_registry_lock, flags);
        kfree(entry);
        debug_block("block: duplicate name %s\n", desc->name);
        return -EEXIST;
    }
    list_add_to_before(&g_registry, &entry->node);   // append at the tail
    spin_unlock_irqrestore(&g_registry_lock, flags);

    if (out)
        *out = &entry->dev;

    debug_block("block: registered %s (%lu sectors, %lu MB, kind=%d)\n",
                entry->dev.name, entry->dev.sector_count,
                entry->dev.sector_count * entry->dev.sector_size / 1024 / 1024,
                entry->dev.kind);
    return 0;
}

// ── Boot unregistration ───────────────────────────────────────────

int block_device_unregister_boot(block_device_t *dev)
{
    if (!dev)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&g_registry_lock);

    // Membership is decided by registry identity, not by an address
    // range: a pointer that is not a member returns -EINVAL and is
    // never dereferenced.
    struct block_device_entry *entry = registry_find(dev);
    if (!entry) {
        spin_unlock_irqrestore(&g_registry_lock, flags);
        return -EINVAL;
    }

    // A disk that still has registered partition children cannot be
    // rolled back — the children have to go first.
    for (list_t *n = list_next(&g_registry); n && n != &g_registry; n = list_next(n)) {
        struct block_device_entry *child = container_of(n, struct block_device_entry, node);
        if (child->dev.parent == dev) {
            spin_unlock_irqrestore(&g_registry_lock, flags);
            debug_block("block: %s still has the partition %s\n",
                        entry->dev.name, child->dev.name);
            return -EBUSY;
        }
    }

    // Release the device from the enumerable registry.  The object
    // itself is retired, not freed: consumers (devfs, GPT, the AHCI
    // port) may still hold the pointer, and the documented behaviour
    // of an unregistered device is "reads fail with -EINVAL" — which
    // needs the object to stay readable.  A retired object keeps its
    // address, so no other object moves either.
    list_del(&entry->node);
    list_add_to_before(&g_retired, &entry->node);
    entry->dev.present = 0;
    entry->dev.ops = NULL;
    entry->dev.private_data = NULL;

    char name[BLOCKDEV_NAME_MAX];
    strncpy(name, entry->dev.name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    spin_unlock_irqrestore(&g_registry_lock, flags);

    debug_block("block: unregistering %s\n", name);
    return 0;
}

// ── Mark device error ─────────────────────────────────────────────
void block_device_mark_failed(block_device_t *dev, int error)
{
    if (!dev)
        return;
    dev->last_error = error;
}

// ── Read ──────────────────────────────────────────────────────────
int block_device_read(block_device_t *dev, uint64_t lba,
                      uint32_t count, void *buffer)
{
    if (!dev || !dev->present) {
        debug_block("block: read on invalid device\n");
        return -EINVAL;
    }

    if (count > 0 && !buffer)
        return -EINVAL;

    if (lba + count < lba)
        return -EINVAL;

    if (count == 0) {
        if (lba <= dev->sector_count)
            return 0;
        return -EINVAL;
    }

    if (lba + count > dev->sector_count) {
        debug_block("block: read past end of device\n");
        return -EINVAL;
    }

    if (!dev->ops || !dev->ops->read)
        return -EINVAL;

    int ret = dev->ops->read(dev, lba, count, buffer);
    if (ret != 0)
        dev->last_error = ret;
    return ret;
}

// ── Write ─────────────────────────────────────────────────────────
int block_device_write(block_device_t *dev, uint64_t lba,
                       uint32_t count, const void *buffer)
{
    if (!dev || !dev->present) {
        debug_block("block: write on invalid device\n");
        return -EINVAL;
    }

    if (!dev->ops || !dev->ops->write)
        return -EROFS;

    if (count > 0 && !buffer)
        return -EINVAL;

    if (lba + count < lba)
        return -EINVAL;

    if (count == 0) {
        if (lba <= dev->sector_count)
            return 0;
        return -EINVAL;
    }

    if (lba + count > dev->sector_count) {
        debug_block("block: write past end of device\n");
        return -EINVAL;
    }

    int ret = dev->ops->write(dev, lba, count, buffer);
    if (ret != 0)
        dev->last_error = ret;
    return ret;
}

// ── Flush ─────────────────────────────────────────────────────────
int block_device_flush(block_device_t *dev)
{
    if (!dev || !dev->present)
        return -EINVAL;

    if (!dev->ops || !dev->ops->flush)
        return 0;

    int ret = dev->ops->flush(dev);
    if (ret != 0)
        dev->last_error = ret;
    return ret;
}

// ── Enumeration ───────────────────────────────────────────────────

block_device_t *block_device_get(int index)
{
    if (index < 0)
        return NULL;

    uint64_t flags = spin_lock_irqsave(&g_registry_lock);
    block_device_t *found = NULL;
    int current = 0;
    for (list_t *n = list_next(&g_registry); n && n != &g_registry; n = list_next(n)) {
        struct block_device_entry *e = container_of(n, struct block_device_entry, node);
        if (current == index) {
            found = &e->dev;
            break;
        }
        current++;
    }
    spin_unlock_irqrestore(&g_registry_lock, flags);
    return found;
}

int block_device_count(void)
{
    uint64_t flags = spin_lock_irqsave(&g_registry_lock);
    int count = 0;
    for (list_t *n = list_next(&g_registry); n && n != &g_registry; n = list_next(n))
        count++;
    spin_unlock_irqrestore(&g_registry_lock, flags);
    return count;
}

// ── Host-test reset ───────────────────────────────────────────────
#ifdef OS01_HOST_TEST
// Host-test only.  Drops every registered and every retired device so
// a test case starts from a known state.  Production has no equivalent:
// published objects live until power-off.
void block_device_reset_for_test(void)
{
    uint64_t flags = spin_lock_irqsave(&g_registry_lock);

    list_t *lists[2] = { &g_registry, &g_retired };
    for (int i = 0; i < 2; i++) {
        list_t *n = list_next(lists[i]);
        while (n && n != lists[i]) {
            list_t *next = list_next(n);
            struct block_device_entry *e = container_of(n, struct block_device_entry, node);
            list_del(n);
            kfree(e);
            n = next;
        }
    }

    spin_unlock_irqrestore(&g_registry_lock, flags);
}
#endif
