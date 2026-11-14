#include "boot_device.h"

#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif

#include "../../kernel/include/core/bootinfo.h"

#define UEFI_DP_TYPE_MEDIA      0x04u
#define UEFI_DP_SUBTYPE_HD      0x01u
#define UEFI_DP_HD_NODE_LEN     42u
#define UEFI_DP_HD_MBR_GPT      0x02u
#define UEFI_DP_HD_SIG_GUID     0x02u

#define UEFI_DP_TYPE_END        0x7fu
#define UEFI_DP_SUBTYPE_END_ENT 0xffu
#define UEFI_DP_END_NODE_LEN    4u

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    uint64_t val = 0;
    for (int i = 0; i < 8; i++)
        val |= (uint64_t)p[i] << (8 * i);
    return val;
}

int os01_bootdev_from_devpath(const uint8_t *dp, size_t max_len,
                              struct boot_device_info *out)
{
    struct boot_device_info tmp;
    size_t offset = 0;
    int found_hd = 0;
    int saw_end_entire = 0;

    if (!dp || !out || max_len < UEFI_DP_END_NODE_LEN)
        return -1;

    while (offset < max_len) {
        size_t rem = max_len - offset;
        const uint8_t *node = dp + offset;
        uint8_t type;
        uint8_t subtype;
        size_t node_len;

        if (rem < 4u)
            return -1;

        type = node[0];
        subtype = node[1];
        node_len = (size_t)read_le16(node + 2);
        if (node_len < 4u || node_len > rem)
            return -1;

        if (type == UEFI_DP_TYPE_END) {
            if (subtype != UEFI_DP_SUBTYPE_END_ENT)
                return -1; /* Reject End Instance or unknown end subtype */
            if (node_len != UEFI_DP_END_NODE_LEN)
                return -1;
            if (offset + node_len != max_len)
                return -1; /* Trailing bytes after End Entire */
            saw_end_entire = 1;
            break;
        }

        if (type == UEFI_DP_TYPE_MEDIA && subtype == UEFI_DP_SUBTYPE_HD) {
            uint8_t mbr_type;
            uint8_t sig_type;
            uint8_t guid_nonzero = 0;

            if (node_len != UEFI_DP_HD_NODE_LEN || found_hd)
                return -1;

            mbr_type = node[40];
            sig_type = node[41];
            if (mbr_type != UEFI_DP_HD_MBR_GPT ||
                sig_type != UEFI_DP_HD_SIG_GUID)
                return -1;

            for (size_t i = 0; i < 16u; i++) {
                tmp.partition_guid[i] = node[24u + i];
                guid_nonzero |= tmp.partition_guid[i];
            }
            if (guid_nonzero == 0)
                return -1;

            tmp.partition_number = read_le32(node + 4);
            tmp.partition_start_lba = read_le64(node + 8);
            tmp.partition_size_lba = read_le64(node + 16);
            tmp.signature_type = (uint32_t)sig_type;

            if (tmp.partition_number == 0 || tmp.partition_size_lba == 0)
                return -1;

            found_hd = 1;
        }

        offset += node_len;
    }

    if (!saw_end_entire || !found_hd)
        return -1;

    *out = tmp;
    return 0;
}
