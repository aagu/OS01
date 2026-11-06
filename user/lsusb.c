/* user/lsusb.c — USB device list and descriptor inspection utility */
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <ctype.h>

static const char *get_class_name(uint8_t cls)
{
    switch (cls) {
    case 0x00: return "Device";
    case 0x01: return "Audio";
    case 0x02: return "Communications";
    case 0x03: return "Human Interface Device";
    case 0x05: return "Physical";
    case 0x06: return "Image";
    case 0x07: return "Printer";
    case 0x08: return "Mass Storage";
    case 0x09: return "Hub";
    case 0x0a: return "CDC-Data";
    case 0x0b: return "Smart Card";
    case 0x0d: return "Content Security";
    case 0x0e: return "Video";
    case 0x0f: return "Personal Healthcare";
    case 0xdc: return "Diagnostic Device";
    case 0xe0: return "Wireless Controller";
    case 0xef: return "Miscellaneous";
    case 0xfe: return "Application Specific";
    case 0xff: return "Vendor Specific";
    default:   return "Unknown";
    }
}

int lsusb_parse_filter(const char *arg, int *out_bus, int *out_devnum)
{
    if (!arg || !*arg || !out_bus || !out_devnum) return -1;
    *out_bus = -1;
    *out_devnum = -1;

    const char *colon = strchr(arg, ':');
    if (!colon) {
        for (const char *p = arg; *p; p++) {
            if (!isdigit((unsigned char)*p)) return -1;
        }
        long val = strtol(arg, NULL, 10);
        if (val < 0 || val > 255) return -1;
        *out_devnum = (int)val;
        return 0;
    }

    if (strchr(colon + 1, ':')) return -1;

    if (colon == arg && *(colon + 1) == '\0') {
        return -1;
    }

    if (colon > arg) {
        for (const char *p = arg; p < colon; p++) {
            if (!isdigit((unsigned char)*p)) return -1;
        }
        char tmp[16];
        size_t blen = (size_t)(colon - arg);
        if (blen >= sizeof(tmp)) return -1;
        memcpy(tmp, arg, blen);
        tmp[blen] = '\0';
        long bval = strtol(tmp, NULL, 10);
        if (bval < 0 || bval > 255) return -1;
        *out_bus = (int)bval;
    }

    if (*(colon + 1) != '\0') {
        const char *dpart = colon + 1;
        for (const char *p = dpart; *p; p++) {
            if (!isdigit((unsigned char)*p)) return -1;
        }
        long dval = strtol(dpart, NULL, 10);
        if (dval < 0 || dval > 255) return -1;
        *out_devnum = (int)dval;
    }

    return 0;
}

int lsusb_unpack_descriptors(const uint8_t *buf, size_t len, char *out_text, size_t max_out)
{
    if (!buf || len < 18 || !out_text || max_out == 0) return -1;

    uint8_t bLength = buf[0];
    uint8_t bDescType = buf[1];
    if (bLength < 18 || bDescType != 1) return -1;

    uint16_t bcdUSB = (uint16_t)(buf[2] | (buf[3] << 8));
    uint8_t bDeviceClass = buf[4];
    uint8_t bDeviceSubClass = buf[5];
    uint8_t bDeviceProtocol = buf[6];
    uint8_t bMaxPacketSize0 = buf[7];
    uint16_t idVendor = (uint16_t)(buf[8] | (buf[9] << 8));
    uint16_t idProduct = (uint16_t)(buf[10] | (buf[11] << 8));
    uint8_t bNumConfigurations = buf[17];

    size_t off = 0;
    int n = snprintf(out_text + off, max_out > off ? max_out - off : 0,
        "Device Descriptor:\n"
        "  bLength                %u\n"
        "  bDescriptorType        %u\n"
        "  bcdUSB               %x.%02x\n"
        "  bDeviceClass           %u\n"
        "  bDeviceSubClass        %u\n"
        "  bDeviceProtocol        %u\n"
        "  bMaxPacketSize0        %u\n"
        "  idVendor           0x%04x\n"
        "  idProduct          0x%04x\n"
        "  bNumConfigurations     %u\n",
        (unsigned)bLength,
        (unsigned)bDescType,
        (unsigned)(bcdUSB >> 8), (unsigned)(bcdUSB & 0xFF),
        (unsigned)bDeviceClass,
        (unsigned)bDeviceSubClass,
        (unsigned)bDeviceProtocol,
        (unsigned)bMaxPacketSize0,
        (unsigned)idVendor,
        (unsigned)idProduct,
        (unsigned)bNumConfigurations);
    if (n < 0) return -1;
    off += (size_t)n;

    size_t pos = 18;
    while (pos < len) {
        if (pos + 2 > len) break;
        uint8_t sub_len = buf[pos];
        uint8_t sub_type = buf[pos + 1];
        if (sub_len < 2 || pos + sub_len > len) break;

        if (sub_type == 2 && sub_len >= 9) {
            uint16_t wTotalLength = (uint16_t)(buf[pos + 2] | (buf[pos + 3] << 8));
            uint8_t bNumInterfaces = buf[pos + 4];
            uint8_t bConfigurationValue = buf[pos + 5];
            n = snprintf(out_text + off, max_out > off ? max_out - off : 0,
                "  Configuration Descriptor:\n"
                "    bLength                %u\n"
                "    bDescriptorType        %u\n"
                "    wTotalLength           %u\n"
                "    bNumInterfaces         %u\n"
                "    bConfigurationValue    %u\n",
                (unsigned)sub_len,
                (unsigned)sub_type,
                (unsigned)wTotalLength,
                (unsigned)bNumInterfaces,
                (unsigned)bConfigurationValue);
            if (n > 0) off += (size_t)n;
        } else if (sub_type == 4 && sub_len >= 9) {
            uint8_t bInterfaceNumber = buf[pos + 2];
            uint8_t bNumEndpoints = buf[pos + 4];
            uint8_t bInterfaceClass = buf[pos + 5];
            const char *cname = get_class_name(bInterfaceClass);
            n = snprintf(out_text + off, max_out > off ? max_out - off : 0,
                "    Interface Descriptor:\n"
                "      bLength                %u\n"
                "      bDescriptorType        %u\n"
                "      bInterfaceNumber       %u\n"
                "      bNumEndpoints          %u\n"
                "      bInterfaceClass        %u %s\n",
                (unsigned)sub_len,
                (unsigned)sub_type,
                (unsigned)bInterfaceNumber,
                (unsigned)bNumEndpoints,
                (unsigned)bInterfaceClass,
                cname);
            if (n > 0) off += (size_t)n;
        } else if (sub_type == 5 && sub_len >= 7) {
            uint8_t bEndpointAddress = buf[pos + 2];
            uint8_t bmAttributes = buf[pos + 3];
            uint16_t wMaxPacketSize = (uint16_t)(buf[pos + 4] | (buf[pos + 5] << 8));
            uint8_t bInterval = buf[pos + 6];

            uint8_t ep_num = bEndpointAddress & 0x0F;
            const char *ep_dir = (bEndpointAddress & 0x80) ? "IN" : "OUT";
            const char *ttype = "Control";
            switch (bmAttributes & 0x03) {
            case 0: ttype = "Control"; break;
            case 1: ttype = "Isochronous"; break;
            case 2: ttype = "Bulk"; break;
            case 3: ttype = "Interrupt"; break;
            }

            n = snprintf(out_text + off, max_out > off ? max_out - off : 0,
                "      Endpoint Descriptor:\n"
                "        bLength                %u\n"
                "        bDescriptorType        %u\n"
                "        bEndpointAddress     0x%02x  EP %u %s\n"
                "        bmAttributes           %u\n"
                "          Transfer Type          %s\n"
                "        wMaxPacketSize     0x%04x\n"
                "        bInterval              %u\n",
                (unsigned)sub_len,
                (unsigned)sub_type,
                (unsigned)bEndpointAddress,
                (unsigned)ep_num,
                ep_dir,
                (unsigned)bmAttributes,
                ttype,
                (unsigned)wMaxPacketSize,
                (unsigned)bInterval);
            if (n > 0) off += (size_t)n;
        }
        pos += sub_len;
    }
    return 0;
}

#ifndef LSUSB_NO_MAIN

static const struct {
    uint16_t vid;
    uint16_t pid;
    const char *name;
} kKnownDevices[] = {
    { 0x0627, 0x0001, "QEMU USB Tablet" },
    { 0x0627, 0x0002, "QEMU USB Mouse" },
    { 0x0627, 0x0003, "QEMU USB Keyboard" },
    { 0x05e3, 0x0608, "Genesys Logic, Inc. Hub" },
    { 0x1b1c, 0x0000, "Corsair" },
    { 0x046d, 0x0000, "Logitech, Inc." },
};

static const char *get_device_name(uint16_t vid, uint16_t pid)
{
    for (size_t i = 0; i < sizeof(kKnownDevices) / sizeof(kKnownDevices[0]); i++) {
        if (kKnownDevices[i].vid == vid) {
            if (kKnownDevices[i].pid == pid || kKnownDevices[i].pid == 0) {
                return kKnownDevices[i].name;
            }
        }
    }
    return NULL;
}

struct usb_dev_entry {
    char name[32];
    int bus;
    int devnum;
    int port;
    uint16_t vid;
    uint16_t pid;
    uint8_t cls;
};

static int cmp_dev_entry(const void *a, const void *b)
{
    const struct usb_dev_entry *da = (const struct usb_dev_entry *)a;
    const struct usb_dev_entry *db = (const struct usb_dev_entry *)b;
    if (da->bus != db->bus) return da->bus - db->bus;
    return da->port - db->port;
}

static int read_attr_int(const char *dev_name, const char *attr, int base)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/usb/%s/%s", dev_name, attr);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return (int)strtol(buf, NULL, base);
}

int main(int argc, char **argv)
{
    bool verbose = false;
    int filter_bus = -1, filter_dev = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: lsusb [-v] [-s [[bus]:][devnum]] [-h]\n");
            return 0;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (++i >= argc || lsusb_parse_filter(argv[i], &filter_bus, &filter_dev) != 0) {
                fprintf(stderr, "lsusb: invalid -s filter syntax\n");
                return 2;
            }
        } else {
            fprintf(stderr, "lsusb: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    DIR *dir = opendir("/dev/usb");
    if (!dir) {
        fprintf(stderr, "lsusb: cannot open /dev/usb\n");
        return 1;
    }

    struct usb_dev_entry entries[32];
    int dev_count = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.' || !strchr(de->d_name, '-')) continue;
        if (dev_count >= 32) break;

        struct usb_dev_entry *e = &entries[dev_count];
        strncpy(e->name, de->d_name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';

        e->bus = read_attr_int(de->d_name, "bus", 10);
        e->devnum = read_attr_int(de->d_name, "devnum", 10);
        e->port = read_attr_int(de->d_name, "port", 10);
        e->vid = (uint16_t)read_attr_int(de->d_name, "vendor", 16);
        e->pid = (uint16_t)read_attr_int(de->d_name, "product", 16);
        e->cls = (uint8_t)read_attr_int(de->d_name, "class", 16);

        if (filter_bus != -1 && e->bus != filter_bus) continue;
        if (filter_dev != -1 && e->devnum != filter_dev) continue;

        dev_count++;
    }
    closedir(dir);

    qsort(entries, (size_t)dev_count, sizeof(struct usb_dev_entry), cmp_dev_entry);

    for (int i = 0; i < dev_count; i++) {
        struct usb_dev_entry *e = &entries[i];
        const char *name = get_device_name(e->vid, e->pid);
        if (!name) name = get_class_name(e->cls);

        printf("Bus %03d Device %03d: ID %04x:%04x %s\n",
               e->bus, e->devnum, e->vid, e->pid, name);

        if (verbose) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/usb/%s/descriptors", e->name);
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                uint8_t raw[512];
                ssize_t r = read(fd, raw, sizeof(raw));
                close(fd);
                if (r > 0) {
                    char text[2048];
                    if (lsusb_unpack_descriptors(raw, (size_t)r, text, sizeof(text)) == 0) {
                        printf("%s\n", text);
                    }
                }
            }
        }
    }

    return 0;
}

#endif /* LSUSB_NO_MAIN */
