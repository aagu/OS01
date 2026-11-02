// lspci — list the PCI devices published under /dev/pci
//
// ARCH-12.  The kernel publishes every enumerated PCI device as
// /dev/pci/<BDF>/<attr>; this program enumerates that namespace with
// plain directory reads and prints one summary line per device, in the
// same shape as Linux lspci:
//
//   00:1f.2 SATA controller: Intel Corporation 82801IR/IO/IH SATA AHCI Controller (rev 00)
//
// Options:
//   -x   also dump the 256-byte configuration space
//   -n   do not resolve names (numeric vendor:device ids)
//   -v   verbose: class breakdown, IRQ, driver and BAR regions
//   -h   usage
//
// Exit codes: 0 = at least one device listed, 1 = no /dev/pci or no
// devices, 2 = usage error.

#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PCI_NS          "/dev/pci"
#define MAX_DEVICES     64
#define ATTR_BUF_MAX    64
#define CONFIG_BUF_MAX  256
#define PATH_BUF_MAX    128

// ── Class name tables ────────────────────────────────────────

// Subclass-specific names refine the base-class name.
static const struct { uint8_t base, sub; const char *name; } kClassNames[] = {
    { 0x01, 0x00, "IDE interface" },
    { 0x01, 0x01, "SCSI storage controller" },
    { 0x01, 0x02, "Floppy disk controller" },
    { 0x01, 0x03, "IPI bus controller" },
    { 0x01, 0x04, "RAID controller" },
    { 0x01, 0x05, "ATA controller" },
    { 0x01, 0x06, "SATA controller" },
    { 0x01, 0x07, "Serial attached SCSI controller" },
    { 0x01, 0x08, "NVMe controller" },
    { 0x06, 0x00, "Host bridge" },
    { 0x06, 0x01, "ISA bridge" },
    { 0x06, 0x04, "PCI bridge" },
    { 0x0c, 0x00, "FireWire (IEEE 1394) controller" },
    { 0x0c, 0x03, "USB controller" },
};

// Base-class fallback names.
static const struct { uint8_t base; const char *name; } kClassBase[] = {
    { 0x00, "Unclassified device" },
    { 0x01, "Mass storage controller" },
    { 0x02, "Ethernet controller" },
    { 0x03, "VGA compatible controller" },
    { 0x04, "Multimedia video controller" },
    { 0x05, "Memory controller" },
    { 0x06, "Bridge" },
    { 0x07, "Communication controller" },
    { 0x08, "System peripheral" },
    { 0x09, "Input device controller" },
    { 0x0a, "Processing accelerator" },
    { 0x0b, "Processing accelerator" },
    { 0x0c, "Serial bus controller" },
    { 0x0d, "Wireless controller" },
    { 0x0e, "Instrumentation controller" },
    { 0x0f, "Coprocessor" },
    { 0x10, "Unassigned class" },
    { 0x11, "Processing accelerator" },
    { 0x12, "Processing accelerator" },
    { 0x13, "Non-essential instrumentation" },
};

// Short names for the verbose "Class:" line.
static const struct { uint8_t base; const char *name; } kClassBaseShort[] = {
    { 0x00, "unclassified device" },
    { 0x01, "mass storage" },
    { 0x02, "network" },
    { 0x03, "display" },
    { 0x04, "multimedia" },
    { 0x05, "memory" },
    { 0x06, "bridge" },
    { 0x07, "communication" },
    { 0x08, "system peripheral" },
    { 0x09, "input device" },
    { 0x0a, "processing accelerator" },
    { 0x0b, "processing accelerator" },
    { 0x0c, "serial bus" },
    { 0x0d, "wireless" },
    { 0x0e, "instrumentation" },
    { 0x0f, "coprocessor" },
    { 0x10, "unassigned class" },
    { 0x11, "processing accelerator" },
    { 0x12, "processing accelerator" },
    { 0x13, "non-essential instrumentation" },
};

// Mass-storage subclass names for the verbose "Class:" line.
static const struct { uint8_t sub; const char *name; } kStorageSubShort[] = {
    { 0x00, "IDE" },
    { 0x01, "SCSI" },
    { 0x02, "floppy" },
    { 0x03, "IPI" },
    { 0x04, "RAID" },
    { 0x05, "ATA" },
    { 0x06, "SATA" },
    { 0x07, "SAS" },
    { 0x08, "NVMe" },
};

// ── Vendor / device name tables ──────────────────────────────

static const struct { uint16_t vendor, device; const char *name; } kDeviceNames[] = {
    { 0x8086, 0x100e, "82540EM Gigabit Ethernet Controller" },
    { 0x8086, 0x10d3, "82574L Gigabit Network Connection" },
    { 0x8086, 0x2922, "82801IR/IO/IH SATA AHCI Controller" },
    { 0x8086, 0x2923, "82801IR/IO/IH SATA AHCI Controller" },
    { 0x8086, 0x29c0, "Q35 Express Chipset" },
    { 0x8086, 0x7000, "82371SB PIIX3 ISA" },
    { 0x8086, 0x7010, "82371SB PIIX3 IDE" },
    { 0x8086, 0x7113, "82371AB/EB/MB PIIX4 ACPI" },
    { 0x1234, 0x1111, "QEMU Virtual Video Controller" },
    { 0x1af4, 0x1000, "Virtio network device" },
    { 0x1af4, 0x1001, "Virtio block device" },
    { 0x1af4, 0x1041, "Virtio console" },
    { 0x1af4, 0x1042, "Virtio modern block device" },
    { 0x1af4, 0x1100, "Virtio GPU" },
};

static const struct { uint16_t id; const char *name; } kVendorNames[] = {
    { 0x8086, "Intel Corporation" },
    { 0x1af4, "Red Hat, Inc." },
    { 0x1234, "QEMU/Bochs" },
    { 0x10de, "NVIDIA Corporation" },
    { 0x1002, "Advanced Micro Devices, Inc. [AMD/ATI]" },
    { 0x1b36, "Red Hat, Inc. (QEMU Virtual Machine)" },
};

// ── Lookup helpers ───────────────────────────────────────────

static const char *class_name(uint32_t class_code)
{
    uint8_t base = (uint8_t)((class_code >> 16) & 0xffu);
    uint8_t sub  = (uint8_t)((class_code >> 8) & 0xffu);

    for (size_t i = 0; i < sizeof(kClassNames) / sizeof(kClassNames[0]); i++) {
        if (kClassNames[i].base == base && kClassNames[i].sub == sub)
            return kClassNames[i].name;
    }
    for (size_t i = 0; i < sizeof(kClassBase) / sizeof(kClassBase[0]); i++) {
        if (kClassBase[i].base == base)
            return kClassBase[i].name;
    }
    return NULL;
}

static const char *class_base_short(uint32_t class_code)
{
    uint8_t base = (uint8_t)((class_code >> 16) & 0xffu);
    for (size_t i = 0; i < sizeof(kClassBaseShort) / sizeof(kClassBaseShort[0]); i++) {
        if (kClassBaseShort[i].base == base)
            return kClassBaseShort[i].name;
    }
    return "unknown";
}

static const char *class_sub_short(uint32_t class_code)
{
    uint8_t base = (uint8_t)((class_code >> 16) & 0xffu);
    uint8_t sub  = (uint8_t)((class_code >> 8) & 0xffu);
    if (base != 0x01) return NULL;
    for (size_t i = 0; i < sizeof(kStorageSubShort) / sizeof(kStorageSubShort[0]); i++) {
        if (kStorageSubShort[i].sub == sub)
            return kStorageSubShort[i].name;
    }
    return NULL;
}

static const char *vendor_name(uint16_t vendor)
{
    for (size_t i = 0; i < sizeof(kVendorNames) / sizeof(kVendorNames[0]); i++) {
        if (kVendorNames[i].id == vendor)
            return kVendorNames[i].name;
    }
    return NULL;
}

static const char *device_name(uint16_t vendor, uint16_t device)
{
    for (size_t i = 0; i < sizeof(kDeviceNames) / sizeof(kDeviceNames[0]); i++) {
        if (kDeviceNames[i].vendor == vendor && kDeviceNames[i].device == device)
            return kDeviceNames[i].name;
    }
    return NULL;
}

// Compose the "vendor device" part of the summary line.
//   - numeric:        "8086:2922"
//   - both known:     "Intel Corporation 82801IR/IO/IH SATA AHCI Controller"
//   - vendor known:   "Intel Corporation 2922"
//   - neither known:  "8086 2922"
static void format_ids(char *out, size_t size, uint16_t vendor, uint16_t device,
                       bool numeric)
{
    if (numeric) {
        snprintf(out, size, "%04x:%04x", vendor, device);
        return;
    }
    const char *vname = vendor_name(vendor);
    const char *dname = device_name(vendor, device);
    if (vname && dname)
        snprintf(out, size, "%s %s", vname, dname);
    else if (vname)
        snprintf(out, size, "%s %04x", vname, device);
    else
        snprintf(out, size, "%04x %04x", vendor, device);
}

// ── Attribute access ─────────────────────────────────────────

// Read one attribute file into `buf`.  Returns the byte count, or -1.
// The loop keeps reading until the buffer is full or the device reports
// EOF, so a short read never truncates the content.
static int read_attr(const char *bdf, const char *attr, char *buf, size_t size)
{
    char path[PATH_BUF_MAX];
    snprintf(path, sizeof(path), "%s/%s/%s", PCI_NS, bdf, attr);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    size_t total = 0;
    while (total < size) {
        int64_t n = read(fd, buf + total, size - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
    return (int)total;
}

// Read an attribute and parse it as a hexadecimal number.  Returns false
// when the attribute is missing or unparsable.
static bool read_attr_hex(const char *bdf, const char *attr, uint32_t *out)
{
    char buf[ATTR_BUF_MAX];
    int n = read_attr(bdf, attr, buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    buf[n] = '\0';
    char *end = NULL;
    unsigned long v = strtoul(buf, &end, 0);
    if (end == buf) return false;
    *out = (uint32_t)v;
    return true;
}

// Read a text attribute and strip its trailing newline.
static void read_attr_line(const char *bdf, const char *attr, char *buf, size_t size)
{
    buf[0] = '\0';
    int n = read_attr(bdf, attr, buf, size - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    for (int i = n - 1; i >= 0; i--) {
        if (buf[i] == '\n') { buf[i] = '\0'; break; }
    }
}

// ── Device rendering ─────────────────────────────────────────

static void print_device(const char *bdf, bool numeric, bool verbose, bool hexdump)
{
    uint32_t vendor = 0, device = 0, class_code = 0;
    bool have_ids = read_attr_hex(bdf, "vendor", &vendor) &&
                    read_attr_hex(bdf, "device", &device);
    bool have_class = read_attr_hex(bdf, "class", &class_code);

    // The configuration space is read once and reused for the revision
    // byte and the optional hex dump.
    char config[CONFIG_BUF_MAX];
    int config_len = read_attr(bdf, "config", config, sizeof(config));

    // "0000:00:1f.2" -> "00:1f.2" (the domain is dropped, like Linux lspci)
    char slot[32];
    const char *colon = strchr(bdf, ':');
    snprintf(slot, sizeof(slot), "%s", colon ? colon + 1 : bdf);

    char class_buf[64];
    if (have_class) {
        const char *cn = class_name(class_code);
        if (cn)
            snprintf(class_buf, sizeof(class_buf), "%s", cn);
        else
            snprintf(class_buf, sizeof(class_buf), "Unknown class [%02X%02X%02X]",
                     (class_code >> 16) & 0xffu, (class_code >> 8) & 0xffu,
                     class_code & 0xffu);
    } else {
        snprintf(class_buf, sizeof(class_buf), "Unknown class");
    }

    char ids[128];
    if (have_ids)
        format_ids(ids, sizeof(ids), (uint16_t)vendor, (uint16_t)device, numeric);
    else
        snprintf(ids, sizeof(ids), "????:????");

    char rev[8];
    if (config_len >= 9)
        snprintf(rev, sizeof(rev), "%02x", (unsigned char)config[0x08]);
    else
        snprintf(rev, sizeof(rev), "??");

    printf("%s %s: %s (rev %s)\n", slot, class_buf, ids, rev);

    if (verbose) {
        uint8_t sub  = (uint8_t)((class_code >> 8) & 0xffu);
        uint8_t prog = (uint8_t)(class_code & 0xffu);
        const char *sub_short = class_sub_short(class_code);
        if (sub_short)
            printf("\tClass: %s, subclass: %s, ProgIF: %02x\n",
                   class_base_short(class_code), sub_short, prog);
        else
            printf("\tClass: %s, subclass: 0x%02x, ProgIF: %02x\n",
                   class_base_short(class_code), sub, prog);

        char irq[ATTR_BUF_MAX];
        read_attr_line(bdf, "irq", irq, sizeof(irq));
        if (irq[0] == '\0')
            snprintf(irq, sizeof(irq), "none");
        printf("\tIRQ: %s\n", irq);

        char drv[ATTR_BUF_MAX];
        read_attr_line(bdf, "driver", drv, sizeof(drv));
        if (drv[0] == '\0')
            snprintf(drv, sizeof(drv), "none");
        printf("\tKernel driver in use: %s\n", drv);

        // The resource attribute already holds one "Region N: ..." line
        // per BAR; print each of them with a TAB in front.
        char res[512];
        int res_len = read_attr(bdf, "resource", res, sizeof(res) - 1);
        if (res_len > 0) {
            res[res_len] = '\0';
            char *line = res;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                printf("\t%s\n", line);
                line = nl ? nl + 1 : NULL;
            }
        }
    }

    if (hexdump) {
        if (config_len <= 0) {
            printf("\tconfig space unavailable\n");
        } else {
            for (int off = 0; off < config_len; off += 16) {
                printf("%02x:", off);
                for (int i = 0; i < 16; i++) {
                    if (off + i < config_len)
                        printf(" %02x", (unsigned char)config[off + i]);
                    else
                        printf("   ");
                }
                printf("\n");
            }
        }
    }
}

// ── Entry point ──────────────────────────────────────────────

static int cmp_bdf(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void usage(void)
{
    printf("usage: lspci [-x] [-n] [-v] [-h]\n");
    printf("  -x  also dump the 256-byte configuration space\n");
    printf("  -n  do not resolve names (numeric vendor:device ids)\n");
    printf("  -v  verbose: class breakdown, IRQ, driver and BAR regions\n");
    printf("  -h  this help\n");
}

int main(int argc, char **argv)
{
    bool numeric = false, verbose = false, hexdump = false;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            usage();
            return 2;
        }
        for (const char *p = argv[i] + 1; *p; p++) {
            switch (*p) {
            case 'x': hexdump = true; break;
            case 'n': numeric = true; break;
            case 'v': verbose = true; break;
            case 'h': usage(); return 0;
            default:
                usage();
                return 2;
            }
        }
    }

    DIR *dir = opendir(PCI_NS);
    if (!dir) {
        fprintf(stderr, "lspci: cannot open %s\n", PCI_NS);
        return 1;
    }

    static char bdfs[MAX_DEVICES][32];
    int count = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_DIR) continue;
        if (ent->d_name[0] == '.') continue;
        if (count >= MAX_DEVICES) break;
        snprintf(bdfs[count], sizeof(bdfs[0]), "%s", ent->d_name);
        count++;
    }
    closedir(dir);

    if (count == 0) {
        fprintf(stderr, "lspci: no PCI devices found under %s\n", PCI_NS);
        return 1;
    }

    qsort(bdfs, (size_t)count, sizeof(bdfs[0]), cmp_bdf);

    for (int i = 0; i < count; i++)
        print_device(bdfs[i], numeric, verbose, hexdump);

    return 0;
}
