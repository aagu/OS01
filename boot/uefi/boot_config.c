#include "boot_config.h"

static bool is_hspace(char c)
{
    return c == ' ' || c == '\t';
}

static bool span_eq(const char *s, size_t len, const char *lit)
{
    size_t i = 0;

    while (i < len && lit[i] != '\0') {
        if (s[i] != lit[i])
            return false;
        i++;
    }
    return i == len && lit[i] == '\0';
}

static bool parse_u32_span(const char *s, size_t len, uint32_t *out)
{
    uint64_t val = 0;

    if (len == 0)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        val = val * 10u + (uint64_t)(s[i] - '0');
        if (val > UINT64_C(0xffffffff))
            return false;
    }
    if (val == 0)
        return false;
    *out = (uint32_t)val;
    return true;
}

static bool parse_resolution(const char *val, size_t len,
                             uint32_t *width_out, uint32_t *height_out)
{
    size_t x_pos = len;
    uint32_t w = 0;
    uint32_t h = 0;

    for (size_t i = 0; i < len; i++) {
        if (val[i] == 'x') {
            x_pos = i;
            break;
        }
    }
    if (x_pos == 0 || x_pos + 1u >= len)
        return false;
    if (!parse_u32_span(val, x_pos, &w))
        return false;
    if (!parse_u32_span(val + x_pos + 1u, len - x_pos - 1u, &h))
        return false;
    *width_out = w;
    *height_out = h;
    return true;
}

static void set_defaults(struct uefi_boot_config *out)
{
    out->platform = UEFI_BOOT_PLATFORM_AUTO;
    out->console = UEFI_BOOT_CONSOLE_DTB;
    out->width = UEFI_BOOT_CONFIG_DEFAULT_WIDTH;
    out->height = UEFI_BOOT_CONFIG_DEFAULT_HEIGHT;
}

int uefi_boot_config_parse(const char *data, size_t size, bool aarch64,
                           struct uefi_boot_config *out,
                           unsigned *error_line)
{
    size_t pos = 0;
    unsigned line_no = 0;
    unsigned console_line = 0;
    bool seen_platform = false;
    bool seen_console = false;

    if (error_line)
        *error_line = 0;
    if (!out)
        return -1;

    set_defaults(out);

    if (size > UEFI_BOOT_CONFIG_MAX_BYTES)
        return -1;
    if (size == 0)
        return 0;
    if (!data)
        return -1;

    while (pos < size) {
        size_t line_start = pos;
        size_t line_end;
        size_t s;
        size_t e;
        size_t key_end;
        size_t val_start;

        line_no++;
        while (pos < size) {
            char c = data[pos];
            if (c == '\0') {
                if (error_line)
                    *error_line = line_no;
                return -1;
            }
            if (c == '\r') {
                if (pos + 1u >= size || data[pos + 1u] != '\n') {
                    if (error_line)
                        *error_line = line_no;
                    return -1;
                }
                break;
            }
            if (c == '\n')
                break;
            pos++;
        }

        line_end = pos;
        if (pos < size && data[pos] == '\r')
            pos += 2u;
        else if (pos < size && data[pos] == '\n')
            pos += 1u;

        s = line_start;
        e = line_end;
        while (s < e && is_hspace(data[s]))
            s++;
        while (e > s && is_hspace(data[e - 1u]))
            e--;

        if (s == e || data[s] == '#')
            continue;

        key_end = s;
        while (key_end < e && !is_hspace(data[key_end]))
            key_end++;

        val_start = key_end;
        while (val_start < e && is_hspace(data[val_start]))
            val_start++;

        if (aarch64) {
            if (span_eq(data + s, key_end - s, "platform")) {
                if (seen_platform) {
                    if (error_line)
                        *error_line = line_no;
                    return -1;
                }
                seen_platform = true;
                if (span_eq(data + val_start, e - val_start, "auto")) {
                    out->platform = UEFI_BOOT_PLATFORM_AUTO;
                } else if (span_eq(data + val_start, e - val_start,
                                   "qemu-virt")) {
                    out->platform = UEFI_BOOT_PLATFORM_QEMU_VIRT;
                } else if (span_eq(data + val_start, e - val_start, "rpi5")) {
                    out->platform = UEFI_BOOT_PLATFORM_RPI5;
                } else if (span_eq(data + val_start, e - val_start,
                                   "dragon-q8b")) {
                    out->platform = UEFI_BOOT_PLATFORM_DRAGON_Q8B;
                } else {
                    if (error_line)
                        *error_line = line_no;
                    return -1;
                }
            } else if (span_eq(data + s, key_end - s, "console")) {
                if (seen_console) {
                    if (error_line)
                        *error_line = line_no;
                    return -1;
                }
                seen_console = true;
                console_line = line_no;
                if (span_eq(data + val_start, e - val_start, "dtb")) {
                    out->console = UEFI_BOOT_CONSOLE_DTB;
                } else if (span_eq(data + val_start, e - val_start,
                                   "rp1-header")) {
                    out->console = UEFI_BOOT_CONSOLE_RP1_HEADER;
                } else if (span_eq(data + val_start, e - val_start, "acpi")) {
                    out->console = UEFI_BOOT_CONSOLE_ACPI;
                } else {
                    if (error_line)
                        *error_line = line_no;
                    return -1;
                }
            }
        } else {
            if (span_eq(data + s, key_end - s, "resolution")) {
                uint32_t w = 0;
                uint32_t h = 0;

                if (parse_resolution(data + val_start, e - val_start, &w, &h)) {
                    out->width = w;
                    out->height = h;
                }
            }
        }
    }

    if (aarch64 && out->console == UEFI_BOOT_CONSOLE_RP1_HEADER &&
        out->platform != UEFI_BOOT_PLATFORM_RPI5) {
        if (error_line)
            *error_line = console_line;
        return -1;
    }
    if (aarch64 && out->console == UEFI_BOOT_CONSOLE_ACPI &&
        out->platform != UEFI_BOOT_PLATFORM_DRAGON_Q8B &&
        out->platform != UEFI_BOOT_PLATFORM_AUTO) {
        if (error_line)
            *error_line = console_line;
        return -1;
    }

    return 0;
}
