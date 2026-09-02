// tools/mkdisk.c — GPT dual-partition disk image builder
// Compile: gcc -Wall -O2 -std=c11 -o mkdisk mkdisk.c
// Usage: mkdisk --output <image> --efi BOOTX64.EFI --temp-dir <dir> --rootfs-manifest <file>
//
// Builds a GPT dual-partition image: a 64 MiB FAT32 ESP at LBA 2048 and a
// 128 MiB ext2 root at LBA 133120 (geometry kept from the legacy builder;
// the verification commands depend on it). The ext2 is filled from a
// tab-separated rootfs manifest produced by mk/components/image.mk:
//   file<TAB>destination<TAB>source<TAB>mode
//   symlink<TAB>destination<TAB>target
// All temporary files live in --temp-dir (never /tmp) and the final image
// is --output (never a hardcoded "disk.img"), so parallel profiles cannot
// collide. The ESP carries BOOTX64.EFI plus the manifest's /kernel.bin entry
// (the bootloader reads kernel.bin from the ESP).
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

static const char *output_path = NULL;
static const char *temp_dir = NULL;

static void run_cmd(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("  [cmd] %s\n", buf);
    int ret = system(buf);
    if (ret != 0) {
        fprintf(stderr, "  [cmd] FAILED: returned %d\n", ret);
        exit(1);
    }
}

// debugfs exits 0 even when its command fails, and prints its banner plus
// error messages on stderr. Detect failures by grepping the merged output
// for error markers (the banner and normal output do not match them).
static void run_debugfs(const char *fmt, ...)
{
    char buf[4096], cmd[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("  [cmd] %s\n", buf);
    snprintf(cmd, sizeof(cmd),
             "%s 2>&1 | grep -Ei 'not found|no such|already exists|cannot|can.t|usage|invalid|failed' >/dev/null && exit 1 || true",
             buf);
    if (system(cmd) != 0) {
        fprintf(stderr, "  [cmd] FAILED: %s\n", buf);
        exit(1);
    }
}

#define MAX_MANIFEST_ENTRIES 128

typedef struct {
    int is_symlink;
    char dest[512];
    char src[1024];
    char mode[64];
    char target[512];
} manifest_entry_t;

// Read the tab-separated rootfs manifest. Returns the number of entries or
// -1 on error. Exceeding MAX_MANIFEST_ENTRIES is an error, not a silent
// truncation: a silently dropped row would produce an image missing files.
static int read_manifest(const char *path, manifest_entry_t *entries)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[2048];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (!len) continue;
        char *rest = strchr(line, '\t');
        if (!rest) continue;
        *rest++ = 0;
        if (strcmp(line, "file") && strcmp(line, "symlink"))
            continue;   // unknown row type: ignore
        if (n >= MAX_MANIFEST_ENTRIES) {
            fprintf(stderr, "ERROR: rootfs manifest %s exceeds %d entries\n",
                    path, MAX_MANIFEST_ENTRIES);
            fclose(f);
            return -1;
        }
        if (!strcmp(line, "file")) {
            // file<TAB>dest<TAB>src<TAB>mode
            char *dest = rest;
            char *p1 = strchr(dest, '\t'); if (!p1) continue; *p1++ = 0;
            char *src = p1;
            char *p2 = strchr(src, '\t'); if (!p2) continue; *p2++ = 0;
            char *mode = p2;
            if (!*mode) { fprintf(stderr, "ERROR: file row for '%s' missing mode\n", dest); fclose(f); return -1; }
            for (char *m = mode; *m; m++) {
                if (*m < '0' || *m > '9') {
                    fprintf(stderr, "ERROR: file row for '%s' has non-numeric mode '%s'\n", dest, mode);
                    fclose(f); return -1;
                }
            }
            entries[n].is_symlink = 0;
            snprintf(entries[n].dest,   sizeof(entries[n].dest),   "%s", dest);
            snprintf(entries[n].src,    sizeof(entries[n].src),    "%s", src);
            snprintf(entries[n].mode,   sizeof(entries[n].mode),   "%s", mode);
            n++;
        } else if (!strcmp(line, "symlink")) {
            // symlink<TAB>dest<TAB>target — retained for future use; image.mk
            // currently emits busybox applets as `file` rows (kernel has no
            // symlink exec support), so no symlink rows are produced today.
            char *dest = rest;
            char *p1 = strchr(dest, '\t'); if (!p1) continue; *p1++ = 0;
            char *target = p1;
            entries[n].is_symlink = 1;
            snprintf(entries[n].dest,   sizeof(entries[n].dest),   "%s", dest);
            snprintf(entries[n].target, sizeof(entries[n].target), "%s", target);
            n++;
        }
    }
    fclose(f);
    return n;
}

// Directories to create in the ext2 image (deduplicated, parent-before-child).
static char dirs[MAX_MANIFEST_ENTRIES * 8][256];
static int ndirs = 0;

static void add_dir(const char *d)
{
    for (int i = 0; i < ndirs; i++)
        if (!strcmp(dirs[i], d)) return;
    if (ndirs < (int)(MAX_MANIFEST_ENTRIES * 8))
        snprintf(dirs[ndirs++], 256, "%s", d);
}

// Add every absolute parent directory of an in-image destination.
static void add_parent_dirs(const char *dest)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", dest);
    for (char *p = buf; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (buf[0]) add_dir(buf);
            *p = '/';
        }
    }
}

int main(int argc, char **argv)
{
    const char *efi_path       = NULL;
    const char *manifest_path  = NULL;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--output") && i+1 < argc)         output_path   = argv[++i];
        else if (!strcmp(argv[i], "--efi") && i+1 < argc)            efi_path      = argv[++i];
        else if (!strcmp(argv[i], "--temp-dir") && i+1 < argc)       temp_dir      = argv[++i];
        else if (!strcmp(argv[i], "--rootfs-manifest") && i+1 < argc) manifest_path = argv[++i];
    }
    if (!output_path || !efi_path || !temp_dir || !manifest_path) {
        fprintf(stderr, "Usage: mkdisk --output <image> --efi BOOTX64.EFI --temp-dir <dir> --rootfs-manifest <file>\n");
        return 1;
    }

    manifest_entry_t entries[MAX_MANIFEST_ENTRIES];
    int nentries = read_manifest(manifest_path, entries);
    if (nentries < 0) return 1;
    if (nentries == 0) {
        fprintf(stderr, "ERROR: empty rootfs manifest: %s\n", manifest_path);
        return 1;
    }

    printf("Building %s: %dMB ESP + %dMB ext2 root\n", output_path, FAT32_SIZE_MB, EXT2_SIZE_MB);

    // ── Phase 1: GPT structure ───────────────────────────
    FILE *f = fopen(output_path, "wb");
    if (!f) { perror("fopen"); return 1; }

    uint8_t mbr[SECTOR_SIZE];
    build_mbr(mbr);
    fwrite(mbr, SECTOR_SIZE, 1, f);

    uint8_t gpt_hdr[92];
    build_gpt_header(gpt_hdr);

    uint8_t entries_buf[128 * 128];
    memset(entries_buf, 0, sizeof(entries_buf));
    build_partition_entry(entries_buf,       ESP_GUID,      PART1_START, PART1_END, "ESP");
    build_partition_entry(entries_buf + 128, LINUX_FS_GUID, PART2_START, PART2_END, "rootfs");

    uint32_t entries_crc = crc32(entries_buf, sizeof(entries_buf));
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
    fwrite(entries_buf, sizeof(entries_buf), 1, f);

    // Pad to PART1_START (starting from LBA 34 = after 32 sectors of entries)
    uint64_t pos = 34;
    uint8_t zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);
    while (pos < PART1_START) { fwrite(zero, SECTOR_SIZE, 1, f); pos++; }
    fclose(f);

    // Temp file paths under --temp-dir with PID for concurrent build safety
    char esp_tmp[512], rootfs_tmp[512];
    snprintf(esp_tmp, sizeof(esp_tmp), "%s/_mkdisk_esp.%d.img", temp_dir, getpid());
    snprintf(rootfs_tmp, sizeof(rootfs_tmp), "%s/_mkdisk_rootfs.%d.img", temp_dir, getpid());

    // The bootloader reads kernel.bin from the ESP; the manifest's
    // /kernel.bin file entry is its source (no separate --kernel argument).
    const char *esp_kernel_src = NULL;
    for (int i = 0; i < nentries; i++) {
        if (!entries[i].is_symlink && !strcmp(entries[i].dest, "/kernel.bin")) {
            esp_kernel_src = entries[i].src;
            break;
        }
    }

    // ── Phase 2: Build + inject ESP (FAT32) ──────────────
    printf("Building ESP partition...\n");
    run_cmd("dd if=/dev/zero of=%s bs=1M count=%d 2>/dev/null", esp_tmp, FAT32_SIZE_MB);
    run_cmd("mkfs.vfat -F 32 %s 2>/dev/null", esp_tmp);
    run_cmd("mmd -i %s ::/EFI 2>/dev/null", esp_tmp);
    run_cmd("mmd -i %s ::/EFI/BOOT 2>/dev/null", esp_tmp);
    run_cmd("mcopy -i %s %s ::/EFI/BOOT 2>/dev/null", esp_tmp, efi_path);
    if (esp_kernel_src)
        run_cmd("mcopy -i %s %s ::/kernel.bin 2>/dev/null", esp_tmp, esp_kernel_src);
    run_cmd("dd if=%s of=%s bs=512 seek=%lu conv=notrunc 2>/dev/null",
            esp_tmp, output_path, (unsigned long)PART1_START);

    // ── Phase 3: Build + inject ext2 root ────────────────
    printf("Building ext2 root filesystem...\n");
    run_cmd("dd if=/dev/zero of=%s bs=1M count=%d 2>/dev/null", rootfs_tmp, EXT2_SIZE_MB);
    run_cmd("mke2fs -t ext2 -I 128 -b 4096 %s 2>/dev/null", rootfs_tmp);

    // Explicit layout dirs (kept from the legacy builder) plus every parent
    // directory of the manifest destinations.
    add_dir("/bin");
    add_dir("/home");
    add_dir("/etc");
    add_dir("/opt");
    add_dir("/opt/test");
    for (int i = 0; i < nentries; i++)
        add_parent_dirs(entries[i].dest);
    for (int i = 0; i < ndirs; i++)
        run_debugfs("debugfs -w %s -R \"mkdir %s\"", rootfs_tmp, dirs[i]);

    // Fill the root from the manifest rows.
    for (int i = 0; i < nentries; i++) {
        if (entries[i].is_symlink) {
            run_debugfs("debugfs -w %s -R \"symlink %s %s\"",
                        rootfs_tmp, entries[i].dest, entries[i].target);
        } else {
            run_debugfs("debugfs -w %s -R \"write %s %s\"",
                        rootfs_tmp, entries[i].src, entries[i].dest);
            run_debugfs("debugfs -w %s -R \"set_inode_field %s mode 010%s\"",
                        rootfs_tmp, entries[i].dest, entries[i].mode);
        }
    }

    run_cmd("dd if=%s of=%s bs=512 seek=%lu conv=notrunc 2>/dev/null",
            rootfs_tmp, output_path, (unsigned long)PART2_START);

    // ── Phase 4: Write backup GPT at end of disk ─────────
    {
        // Backup GPT header at last LBA
        wr64(gpt_hdr, 24, TOTAL_SECTORS - 1);  // my_lba = end of disk
        wr64(gpt_hdr, 32, 1);                   // alternate_lba = primary
        wr64(gpt_hdr, 72, TOTAL_SECTORS - 33);  // partition entries at end-32 sectors
        wr32(gpt_hdr, 16, 0);
        hdr_crc = crc32(gpt_hdr, 92);
        wr32(gpt_hdr, 16, hdr_crc);

        f = fopen(output_path, "r+b");
        if (f) {
            fseek(f, (long)(TOTAL_SECTORS - 1) * SECTOR_SIZE, SEEK_SET);
            fwrite(gpt_hdr, 92, 1, f);
            fseek(f, (long)(TOTAL_SECTORS - 33) * SECTOR_SIZE, SEEK_SET);
            fwrite(entries_buf, sizeof(entries_buf), 1, f);
            fclose(f);
        }
    }

    // ── Cleanup ──────────────────────────────────────────
    unlink(esp_tmp);
    unlink(rootfs_tmp);

    // ── Self-check ───────────────────────────────────────
    printf("Self-check...\n");
    {
        uint8_t buf[1024];
        f = fopen(output_path, "rb");
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

    printf("Done: %s (%u MB)\n",
           output_path,
           (unsigned)((TOTAL_SIZE) / 1024 / 1024));
    return 0;
}
