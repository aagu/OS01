// tools/mkdisk.c — GPT dual-partition disk image builder
// Compile: gcc -Wall -O2 -std=c11 -o mkdisk mkdisk.c
// Usage: mkdisk disk.img --efi BOOTX64.EFI --kernel kernel.bin --rootfs config/fsroot/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define SECTOR_SIZE    512
#define FAT32_SIZE_MB  64
#define EXT2_SIZE_MB   128
#define ALIGN_LBA      2048
#define TOTAL_SIZE     ((FAT32_SIZE_MB + EXT2_SIZE_MB) * 1024 * 1024)
#define TOTAL_SECTORS  (TOTAL_SIZE / SECTOR_SIZE)

#define PART1_START    ALIGN_LBA
#define PART1_SECTORS  ((FAT32_SIZE_MB * 1024 * 1024) / SECTOR_SIZE)
#define PART1_END      (PART1_START + PART1_SECTORS - 1)

#define PART2_START    (((PART1_END + 1) + (ALIGN_LBA - 1)) / ALIGN_LBA * ALIGN_LBA)
#define PART2_SECTORS  ((EXT2_SIZE_MB * 1024 * 1024) / SECTOR_SIZE)
#define PART2_END      (PART2_START + PART2_SECTORS - 1)

// Standard reflected CRC32 (same as zlib, used by GPT)
static uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return crc ^ 0xFFFFFFFF;
}

static void wr32(uint8_t *b, int off, uint32_t v) {
    for (int i = 0; i < 4; i++) b[off + i] = (v >> (i * 8)) & 0xFF;
}
static void wr64(uint8_t *b, int off, uint64_t v) {
    for (int i = 0; i < 8; i++) b[off + i] = (v >> (i * 8)) & 0xFF;
}

static void build_mbr(uint8_t *sector)
{
    memset(sector, 0, SECTOR_SIZE);
    sector[446 + 4] = 0xEE;        // GPT Protective type
    // Protective partition: start=1, size=total_sectors-1
    wr32(sector, 446 + 8,  1);
    wr32(sector, 446 + 12, (TOTAL_SECTORS - 1 > 0xFFFFFFFFu)
                           ? 0xFFFFFFFFu : (uint32_t)(TOTAL_SECTORS - 1));
    sector[510] = 0x55;
    sector[511] = 0xAA;
}

static void build_gpt_header(uint8_t *hdr)
{
    memset(hdr, 0, 92);
    memcpy(hdr, "EFI PART", 8);
    wr32(hdr, 8,  0x00010000);
    wr32(hdr, 12, 92);
    wr64(hdr, 24, 1);                    // my_lba
    wr64(hdr, 32, TOTAL_SECTORS - 1);    // alternate_lba (backup)
    wr64(hdr, 40, 34);                   // first_usable_lba
    wr64(hdr, 48, TOTAL_SECTORS - 34);   // last_usable_lba
    wr64(hdr, 72, 2);                    // partition_entry_lba
    wr32(hdr, 80, 128);                  // num_partition_entries
    wr32(hdr, 84, 128);                  // size_of_partition_entry
}

// ESP GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
static const uint8_t ESP_GUID[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8,0xD2,0x11,
    0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};

// Linux filesystem GUID: 0FC63DAF-8483-4772-8E79-3D69D8477DE4
static const uint8_t LINUX_FS_GUID[16] = {
    0xAF,0x3D,0xC6,0x0F, 0x83,0x84,0x72,0x47,
    0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4
};

static void gen_random_guid(uint8_t *out)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { read(fd, out, 16); close(fd); }
    else {
        // Fallback: not great random but serviceable
        unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
        for (int i = 0; i < 16; i++) { seed = seed * 1103515245 + 12345; out[i] = (uint8_t)(seed >> 16); }
    }
}

static void write_gpt_name(uint8_t *entry, const char *name)
{
    size_t len = strlen(name);
    if (len > 36) len = 36;
    for (size_t i = 0; i < len; i++) {
        entry[56 + i * 2]     = (uint8_t)name[i];
        entry[56 + i * 2 + 1] = 0;
    }
}

static void build_partition_entry(uint8_t *entry, const uint8_t *type_guid,
                                   uint64_t start, uint64_t end, const char *name)
{
    memset(entry, 0, 128);
    memcpy(entry, type_guid, 16);
    gen_random_guid(entry + 16);
    wr64(entry, 32, start);
    wr64(entry, 40, end);
    write_gpt_name(entry, name);
}

static int run_cmd(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("  [cmd] %s\n", buf);
    int ret = system(buf);
    if (ret != 0) {
        fprintf(stderr, "  [cmd] WARNING: returned %d\n", ret);
    }
    return ret;
}

int main(int argc, char **argv)
{
    const char *efi_path    = NULL;
    const char *kernel_path = NULL;
    const char *rootfs_dir  = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--efi") && i+1 < argc)    efi_path    = argv[++i];
        else if (!strcmp(argv[i], "--kernel") && i+1 < argc) kernel_path = argv[++i];
        else if (!strcmp(argv[i], "--rootfs") && i+1 < argc)  rootfs_dir  = argv[++i];
    }
    if (!efi_path || !kernel_path || !rootfs_dir) {
        fprintf(stderr, "Usage: mkdisk --efi BOOTX64.EFI --kernel kernel.bin --rootfs fsroot/\n");
        return 1;
    }

    printf("Building disk.img: %dMB ESP + %dMB ext2 root\n", FAT32_SIZE_MB, EXT2_SIZE_MB);

    // ── Phase 1: GPT structure ───────────────────────────
    FILE *f = fopen("disk.img", "wb");
    if (!f) { perror("fopen"); return 1; }

    uint8_t mbr[SECTOR_SIZE];
    build_mbr(mbr);
    fwrite(mbr, SECTOR_SIZE, 1, f);

    uint8_t gpt_hdr[92];
    build_gpt_header(gpt_hdr);

    uint8_t entries[128 * 128];
    memset(entries, 0, sizeof(entries));
    build_partition_entry(entries,       ESP_GUID,      PART1_START, PART1_END, "ESP");
    build_partition_entry(entries + 128, LINUX_FS_GUID, PART2_START, PART2_END, "rootfs");

    uint32_t entries_crc = crc32(entries, sizeof(entries));
    wr32(gpt_hdr, 88, entries_crc);

    wr32(gpt_hdr, 16, 0);
    uint32_t hdr_crc = crc32(gpt_hdr, 92);
    wr32(gpt_hdr, 16, hdr_crc);

    fwrite(gpt_hdr, 92, 1, f);
    // Pad GPT header to fill LBA 1 (header = 92 bytes, sector = 512 bytes)
    {
        uint8_t pad[420];
        memset(pad, 0, sizeof(pad));
        fwrite(pad, sizeof(pad), 1, f);
    }
    // LBA 2: partition entry array
    fwrite(entries, sizeof(entries), 1, f);

    // Pad to PART1_START (starting from LBA 34 = after 32 sectors of entries)
    uint64_t pos = 34;
    uint8_t zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);
    while (pos < PART1_START) { fwrite(zero, SECTOR_SIZE, 1, f); pos++; }
    fclose(f);

    // ── Phase 2: Build + inject ESP (FAT32) ──────────────
    printf("Building ESP partition...\n");
    run_cmd("dd if=/dev/zero of=/tmp/_mkdisk_esp.img bs=1M count=%d 2>/dev/null", FAT32_SIZE_MB);
    run_cmd("mkfs.vfat -F 32 /tmp/_mkdisk_esp.img 2>/dev/null");
    run_cmd("mmd -i /tmp/_mkdisk_esp.img ::/EFI 2>/dev/null");
    run_cmd("mmd -i /tmp/_mkdisk_esp.img ::/EFI/BOOT 2>/dev/null");
    run_cmd("mcopy -i /tmp/_mkdisk_esp.img %s ::/EFI/BOOT 2>/dev/null", efi_path);
    run_cmd("mcopy -i /tmp/_mkdisk_esp.img %s ::/ 2>/dev/null", kernel_path);
    run_cmd("dd if=/tmp/_mkdisk_esp.img of=disk.img bs=512 seek=%lu conv=notrunc 2>/dev/null",
            (unsigned long)PART1_START);

    // ── Phase 3: Build + inject ext2 root ────────────────
    printf("Building ext2 root filesystem...\n");
    run_cmd("dd if=/dev/zero of=/tmp/_mkdisk_rootfs.img bs=1M count=%d 2>/dev/null", EXT2_SIZE_MB);
    run_cmd("mke2fs -t ext2 -I 128 -b 4096 /tmp/_mkdisk_rootfs.img 2>/dev/null");
    run_cmd("debugfs -w /tmp/_mkdisk_rootfs.img -R \"mkdir /bin\" 2>/dev/null");
    run_cmd("debugfs -w /tmp/_mkdisk_rootfs.img -R \"mkdir /home\" 2>/dev/null");
    run_cmd("debugfs -w /tmp/_mkdisk_rootfs.img -R \"mkdir /etc\" 2>/dev/null");
    run_cmd("debugfs -w /tmp/_mkdisk_rootfs.img -R \"mkdir /opt\" 2>/dev/null");
    run_cmd("debugfs -w /tmp/_mkdisk_rootfs.img -R \"mkdir /opt/test\" 2>/dev/null");

    // Copy fsroot/bin/* to /bin/ using a shell loop
    {
        char glob_cmd[1024];
        snprintf(glob_cmd, sizeof(glob_cmd),
                 "for f in %s/bin/*; do "
                 "  base=$(basename \"$f\"); "
                 "  debugfs -w /tmp/_mkdisk_rootfs.img -R \"write $f /bin/$base\" 2>/dev/null; "
                 "done", rootfs_dir);
        system(glob_cmd);
    }

    run_cmd("dd if=/tmp/_mkdisk_rootfs.img of=disk.img bs=512 seek=%lu conv=notrunc 2>/dev/null",
            (unsigned long)PART2_START);

    // ── Phase 4: Write backup GPT at end of disk ─────────
    {
        // Backup GPT header at last LBA
        wr64(gpt_hdr, 24, TOTAL_SECTORS - 1);  // my_lba = end of disk
        wr64(gpt_hdr, 32, 1);                   // alternate_lba = primary
        wr64(gpt_hdr, 72, TOTAL_SECTORS - 33);  // partition entries at end-32 sectors
        wr32(gpt_hdr, 16, 0);
        hdr_crc = crc32(gpt_hdr, 92);
        wr32(gpt_hdr, 16, hdr_crc);

        f = fopen("disk.img", "r+b");
        if (f) {
            fseek(f, (long)(TOTAL_SECTORS - 1) * SECTOR_SIZE, SEEK_SET);
            fwrite(gpt_hdr, 92, 1, f);
            fseek(f, (long)(TOTAL_SECTORS - 33) * SECTOR_SIZE, SEEK_SET);
            fwrite(entries, sizeof(entries), 1, f);
            fclose(f);
        }
    }

    // ── Cleanup ──────────────────────────────────────────
    unlink("/tmp/_mkdisk_esp.img");
    unlink("/tmp/_mkdisk_rootfs.img");

    // ── Self-check ───────────────────────────────────────
    printf("Self-check...\n");
    {
        uint8_t buf[1024];
        f = fopen("disk.img", "rb");
        if (f) {
            fseek(f, SECTOR_SIZE, SEEK_SET);
            fread(buf, 1, 92, f);
            printf("  GPT header: %s\n",
                   memcmp(buf, "EFI PART", 8) == 0 ? "OK" : "FAIL");

            fseek(f, PART2_START * 512LL + 1024, SEEK_SET);
            fread(buf, 1, 64, f);
            printf("  ext2 magic: %s\n",
                   (buf[56] == 0x53 && buf[57] == 0xEF) ? "OK" : "FAIL");

            fseek(f, PART1_START * 512LL + 510, SEEK_SET);
            fread(buf, 1, 2, f);
            printf("  FAT32 BB: %s\n",
                   (buf[0] == 0x55 && buf[1] == 0xAA) ? "OK" : "FAIL");

            fclose(f);
        }
    }

    printf("Done: disk.img (%u MB)\n",
           (unsigned)((TOTAL_SIZE) / 1024 / 1024));
    return 0;
}
