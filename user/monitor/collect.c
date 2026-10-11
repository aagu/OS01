/*
 * user/monitor/collect.c — monitor plan Task 6: procfs collection and
 * strict parsing (spec §4.1, §5.2, §6).
 *
 * Freestanding user-space code on OS01's libc (open/read/close,
 * opendir/readdir/closedir, malloc/free).  Design points:
 *
 *   - Bounded reads to EOF with per-file caps mirroring the kernel
 *     snapshot buffers (procfs_snap.c); anything longer than the cap is
 *     invalid — never a silently truncated sample.
 *   - Strict §6 parsing: unknown keys ignored, field order and
 *     whitespace count irrelevant, Version must be 1, a required field
 *     missing / a recognized field duplicated / numeric overflow / an
 *     illegal value invalidate the whole file.
 *   - /proc/<pid>/status is opened, read and closed one PID at a time
 *     (NOFILE=16: at most the directory handle plus one status fd).
 *     PIDs must be all-decimal and representable; duplicates are
 *     deduplicated; a vanished PID (open fails) or an invalid status
 *     file only drops that process.
 *   - The process Name is DECODED into the output struct; no pointer
 *     into the temporary file text survives.
 *   - Every allocation belongs to the round: on OOM the round's array
 *     is freed and monitor_collect returns -ENOMEM.
 */

#include <monitor/collect.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── small span helpers (the text is not NUL-terminated at len) ───── */

static int span_isspace(char c)
{
    return c == ' ' || c == '\t';
}

static const char *span_skip_ws(const char *p, const char *end)
{
    while (p < end && span_isspace(*p))
        p++;
    return p;
}

static int span_is(const char *p, const char *end, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(end - p) != n)
        return 0;
    return memcmp(p, lit, n) == 0;
}

/* Parse unsigned decimal within [p, end) (no sign, digits only).  On
 * success *pp advances past the digits and *out holds the value. */
static int span_u64(const char **pp, const char *end, uint64_t *out)
{
    const char *p = *pp;
    uint64_t v = 0;
    int any = 0;

    while (p < end && *p >= '0' && *p <= '9') {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - d) / 10)
            return -EINVAL;
        v = v * 10 + d;
        any = 1;
        p++;
    }
    if (!any)
        return -EINVAL;
    *pp = p;
    *out = v;
    return 0;
}

/* Signed decimal for TempMilliC (optional '-', fits int32). */
static int span_i32(const char **pp, const char *end, int32_t *out)
{
    const char *p = *pp;
    int neg = 0;

    if (p < end && *p == '-') {
        neg = 1;
        p++;
    }
    uint64_t mag;
    if (span_u64(&p, end, &mag) != 0)
        return -EINVAL;
    if (neg) {
        if (mag > 2147483648ULL)
            return -EINVAL;
        *out = (int32_t)(-mag);
    } else {
        if (mag > 2147483647ULL)
            return -EINVAL;
        *out = (int32_t)mag;
    }
    *pp = p;
    return 0;
}

/* Unsigned decimal that must consume the whole span (after optional
 * surrounding whitespace). */
static int span_u64_line(const char *vs, const char *end, uint64_t *out)
{
    const char *p = span_skip_ws(vs, end);
    if (span_u64(&p, end, out) != 0)
        return -EINVAL;
    p = span_skip_ws(p, end);
    return p == end ? 0 : -EINVAL;
}

/* u32-bounded variant (CPU, CpuId). */
static int span_u32_line(const char *vs, const char *end, uint32_t *out)
{
    uint64_t v;
    if (span_u64_line(vs, end, &v) != 0)
        return -EINVAL;
    if (v > UINT32_MAX)
        return -EINVAL;
    *out = (uint32_t)v;
    return 0;
}

/* meminfo keeps kB on the wire ("<n> kB"); convert to bytes with an
 * overflow check (spec §4.1).  The " kB" suffix is part of the value;
 * anything else after the number is a malformed field. */
static int span_kb_to_bytes(const char *vs, const char *end, uint64_t *out)
{
    const char *p = span_skip_ws(vs, end);

    if (span_u64(&p, end, out) != 0)
        return -EINVAL;
    if (*out > UINT64_MAX / 1024)
        return -EINVAL;
    *out *= 1024;

    p = span_skip_ws(p, end);
    if ((size_t)(end - p) >= 2 && p[0] == 'k' && p[1] == 'B')
        p = span_skip_ws(p + 2, end);
    return p == end ? 0 : -EINVAL;
}

/* If the line [ls, le) is "<key>:" (leading whitespace tolerated),
 * set *vs to the value start (whitespace skipped) and return 1.
 * Otherwise return 0 and leave *vs untouched. */
static int line_value(const char *ls, const char *le, const char *key,
                      const char **vs)
{
    size_t klen = strlen(key);
    const char *p = span_skip_ws(ls, le);

    if ((size_t)(le - p) <= klen)
        return 0;
    if (memcmp(p, key, klen) != 0 || p[klen] != ':')
        return 0;
    *vs = span_skip_ws(p + klen + 1, le);
    return 1;
}

/* ── Version handling ─────────────────────────────────────────────── */

#define F_VERSION   0x01
#define F_SAMPLENS  0x02
#define F_TICKHZ    0x04
#define F_TOTAL     0x08
#define F_USED      0x10
#define F_FREE      0x20
#define F_NAME      0x40
#define F_PID       0x80
#define F_PPID      0x100
#define F_STATE     0x200
#define F_CPU       0x400
#define F_BIRTHID   0x800
#define F_STARTNS   0x1000
#define F_RTIMETICKS 0x2000
#define F_TEMP      0x4000
#define F_CPUID     0x8000
#define F_REASON    0x10000
#define F_SOURCE    0x20000

static unsigned seen_field(unsigned *have, unsigned bit)
{
    if (*have & bit)
        return 1;            /* recognized field duplicated -> invalid */
    *have |= bit;
    return 0;
}

/* Version must be present and exactly 1 (unsupported -> invalid). */
static int take_version(unsigned *have, const char *vs, const char *le,
                        uint64_t *v)
{
    if (seen_field(have, F_VERSION))
        return -EINVAL;
    if (span_u64_line(vs, le, v) != 0)
        return -EINVAL;
    if (*v != 1)
        return -EINVAL;
    return 0;
}

/* A fall-through line is ignorable when it is blank or has the
 * "something: value" shape of an unknown field.  A non-blank line
 * without any ':' is not a field at all — the file is not the format
 * we asked for -> invalid. */
static int line_ignorable(const char *p, const char *le)
{
    if (span_skip_ws(p, le) == le)
        return 1;                /* blank line */
    for (const char *q = p; q < le; q++)
        if (*q == ':')
            return 1;
    return 0;
}

/* ── /proc/stat ───────────────────────────────────────────────────── */

static int parse_cpu_line(uint32_t idx, const char *vs, const char *le,
                          struct monitor_cpu_sample *c)
{
    unsigned sf_have = 0;
    const char *p = vs;

    c->cpu_id = idx;
    c->tick_hz = 0;          /* overwritten by the file-wide TickHz */

    while (p < le) {
        const char *eq = p;
        while (eq < le && *eq != '=' && !span_isspace(*eq))
            eq++;
        if (eq >= le || *eq != '=')
            return -EINVAL;  /* token without '=' is garbage */

        size_t nlen = (size_t)(eq - p);
        const char *vp = span_skip_ws(eq + 1, le);
        uint64_t v;
        if (span_u64(&vp, le, &v) != 0)
            return -EINVAL;

        if (nlen == 6 && memcmp(p, "online", 6) == 0) {
            if (seen_field(&sf_have, F_VERSION))
                return -EINVAL;
            if (v > 1)
                return -EINVAL;
            c->online = (uint32_t)v;
        } else if (nlen == 5 && memcmp(p, "valid", 5) == 0) {
            if (seen_field(&sf_have, F_SAMPLENS))
                return -EINVAL;
            if (v > 1)
                return -EINVAL;
            c->valid = (uint32_t)v;
        } else if (nlen == 5 && memcmp(p, "epoch", 5) == 0) {
            if (seen_field(&sf_have, F_TICKHZ))
                return -EINVAL;
            c->epoch = v;
        } else if (nlen == 4 && memcmp(p, "busy", 4) == 0) {
            if (seen_field(&sf_have, F_TOTAL))
                return -EINVAL;
            c->busy_ticks = v;
        } else if (nlen == 4 && memcmp(p, "idle", 4) == 0) {
            if (seen_field(&sf_have, F_USED))
                return -EINVAL;
            c->idle_ticks = v;
        }
        /* unknown key=value pairs are ignored */

        p = span_skip_ws(vp, le);
        if (p != le && p == vp)
            return -EINVAL;      /* junk directly after the digits */
    }

    /* online/valid/epoch/busy/idle are all required */
    if (!(sf_have & F_VERSION) || !(sf_have & F_SAMPLENS) ||
        !(sf_have & F_TICKHZ) || !(sf_have & F_TOTAL) ||
        !(sf_have & F_USED))
        return -EINVAL;
    return 0;
}

int monitor_parse_stat(const char *text, size_t len,
                       struct monitor_stat_sample *out)
{
    if (!text || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));

    const char *p = text;
    const char *end = text + len;
    unsigned have = 0;
    uint32_t seen_cpu = 0;   /* bitmask: cpuN already present */
    uint64_t version = 0;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *vs = NULL;
        int rc;

        if (line_value(p, le, "Version", &vs)) {
            rc = take_version(&have, vs, le, &version);
            if (rc != 0)
                return rc;
        } else if (line_value(p, le, "SampleNs", &vs)) {
            if (seen_field(&have, F_SAMPLENS))
                return -EINVAL;
            if (span_u64_line(vs, le, &out->sampled_ns) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "TickHz", &vs)) {
            if (seen_field(&have, F_TICKHZ))
                return -EINVAL;
            if (span_u32_line(vs, le, &out->tick_hz) != 0)
                return -EINVAL;
        } else {
            /* "cpu<idx>:" family — leading whitespace tolerated */
            const char *cp = span_skip_ws(p, le);
            if ((size_t)(le - cp) >= 4 &&
                cp[0] == 'c' && cp[1] == 'p' && cp[2] == 'u') {
                const char *q = cp + 3;
                uint64_t idx;
                if (span_u64(&q, le, &idx) != 0)
                    return -EINVAL;
                if (q >= le || *q != ':')
                    return -EINVAL;
                if (idx >= MONITOR_MAX_CPU)
                    return -EINVAL;   /* beyond the 8-core protocol */
                if (seen_cpu & (uint32_t)(1u << idx))
                    return -EINVAL;   /* duplicate cpuN line */
                seen_cpu |= (uint32_t)(1u << idx);
                if (parse_cpu_line((uint32_t)idx,
                                   span_skip_ws(q + 1, le), le,
                                   &out->cpu[idx]) != 0)
                    return -EINVAL;
                out->ncpu = idx + 1 > out->ncpu ? (uint32_t)(idx + 1)
                                               : out->ncpu;
            } else if (!line_ignorable(p, le)) {
                return -EINVAL;       /* garbage line, not a field */
            }
        }

        p = nl ? nl + 1 : end;
    }

    if (!(have & F_VERSION) || !(have & F_SAMPLENS) || !(have & F_TICKHZ))
        return -EINVAL;

    /* propagate the file-wide tick rate to each cpu entry */
    for (uint32_t i = 0; i < MONITOR_MAX_CPU; i++)
        out->cpu[i].tick_hz = out->tick_hz;

    return 0;
}

/* ── /proc/meminfo ────────────────────────────────────────────────── */

int monitor_parse_meminfo(const char *text, size_t len,
                          struct monitor_mem_sample *out)
{
    if (!text || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));

    const char *p = text;
    const char *end = text + len;
    unsigned have = 0;
    uint64_t version = 0;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *vs = NULL;
        int rc;

        if (line_value(p, le, "Version", &vs)) {
            rc = take_version(&have, vs, le, &version);
            if (rc != 0)
                return rc;
        } else if (line_value(p, le, "SampleNs", &vs)) {
            if (seen_field(&have, F_SAMPLENS))
                return -EINVAL;
            if (span_u64_line(vs, le, &out->sampled_ns) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "MemTotal", &vs)) {
            if (seen_field(&have, F_TOTAL))
                return -EINVAL;
            if (span_kb_to_bytes(vs, le, &out->total_bytes) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "MemUsed", &vs)) {
            if (seen_field(&have, F_USED))
                return -EINVAL;
            if (span_kb_to_bytes(vs, le, &out->used_bytes) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "MemFree", &vs)) {
            if (seen_field(&have, F_FREE))
                return -EINVAL;
            if (span_kb_to_bytes(vs, le, &out->free_bytes) != 0)
                return -EINVAL;
        } else if (!line_ignorable(p, le)) {
            return -EINVAL;              /* garbage line, not a field */
        }

        p = nl ? nl + 1 : end;
    }

    if (!(have & F_VERSION) || !(have & F_SAMPLENS) ||
        !(have & F_TOTAL) || !(have & F_USED) || !(have & F_FREE))
        return -EINVAL;

    /* T=0: PMM unavailable (spec §4.1) */
    if (out->total_bytes == 0)
        return -EINVAL;

    return 0;
}

/* ── /proc/<pid>/status — Name decoding (spec §6) ─────────────────── */

#define NAME_DATA_MAX (MONITOR_COMM_MAX - 1)

static int span_hex4(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Decode an encoded Name value into out[MONITOR_COMM_MAX].  Only the
 * two kernel escapes are accepted ("\\\" -> '\', "\xHH" -> byte);
 * unknown or incomplete escapes, a decoded NUL byte and decoded names
 * longer than 63 bytes are rejected. */
static int decode_name(const char *p, const char *end, char *out)
{
    size_t n = 0;

    while (p < end) {
        char c = *p;

        if (c == '\\') {
            p++;
            if (p >= end)
                return -EINVAL;          /* incomplete escape */
            if (*p == '\\') {
                c = '\\';
                p++;
            } else if (*p == 'x') {
                p++;
                if (end - p < 2)
                    return -EINVAL;      /* truncated \xHH */
                int hi = span_hex4(p[0]);
                int lo = span_hex4(p[1]);
                if (hi < 0 || lo < 0)
                    return -EINVAL;      /* unknown escape */
                c = (char)((hi << 4) | lo);
                p += 2;
            } else {
                return -EINVAL;          /* unknown escape */
            }
        } else {
            p++;
        }

        if (c == '\0')
            return -EINVAL;              /* decoded NUL */
        if (n >= NAME_DATA_MAX)
            return -EINVAL;              /* name longer than 63 bytes */
        out[n++] = c;
    }

    out[n] = '\0';
    return 0;
}

int monitor_parse_status(const char *text, size_t len,
                         struct monitor_process_sample *out)
{
    if (!text || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));

    const char *p = text;
    const char *end = text + len;
    unsigned have = 0;
    uint64_t version = 0;
    uint64_t cpu_val = 0;
    const char *name_vs = NULL, *name_le = NULL;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *vs = NULL;
        int rc;

        if (line_value(p, le, "Version", &vs)) {
            rc = take_version(&have, vs, le, &version);
            if (rc != 0)
                return rc;
        } else if (line_value(p, le, "Name", &vs)) {
            if (seen_field(&have, F_NAME))
                return -EINVAL;
            /* value runs to end of line; ':' inside belongs to it */
            name_vs = vs;
            name_le = le;
        } else if (line_value(p, le, "Pid", &vs)) {
            if (seen_field(&have, F_PID))
                return -EINVAL;
            if (span_u64_line(vs, le, &out->pid) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "PPid", &vs)) {
            if (seen_field(&have, F_PPID))
                return -EINVAL;
            if (span_u64_line(vs, le, &out->ppid) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "State", &vs)) {
            if (seen_field(&have, F_STATE))
                return -EINVAL;
            const char *q = span_skip_ws(vs, le);
            if (q >= le)
                return -EINVAL;
            /* R/S/D/T/Z are the five task states; '?' is the kernel's
             * defensive fallback (procfs_snap.c snap_state_char) for
             * state bits matching none of them — accept it and keep
             * the row; display decides how to render it. */
            if (*q != 'R' && *q != 'S' && *q != 'D' &&
                *q != 'T' && *q != 'Z' && *q != '?')
                return -EINVAL;
            if (span_skip_ws(q + 1, le) != le)
                return -EINVAL;      /* exactly one state letter */
            out->state = *q;
        } else if (line_value(p, le, "CPU", &vs)) {
            if (seen_field(&have, F_CPU))
                return -EINVAL;
            if (span_u64_line(vs, le, &cpu_val) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "BirthId", &vs)) {
            if (seen_field(&have, F_BIRTHID))
                return -EINVAL;
            if (span_u64_line(vs, le, &out->birth_id) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "StartNs", &vs)) {
            if (seen_field(&have, F_STARTNS))
                return -EINVAL;
            if (span_u64_line(vs, le, &out->start_ns) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "SampleNs", &vs)) {
            if (seen_field(&have, F_SAMPLENS))
                return -EINVAL;
            if (span_u64_line(vs, le, &out->sampled_ns) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "RuntimeTicks", &vs)) {
            if (seen_field(&have, F_RTIMETICKS))
                return -EINVAL;
            if (span_u64_line(vs, le, &out->runtime_ticks) != 0)
                return -EINVAL;
        }
        /* Priority and anything else: unknown field, ignored; a
         * non-blank line without any ':' is not a field -> invalid */
        else if (!line_ignorable(p, le)) {
            return -EINVAL;
        }

        p = nl ? nl + 1 : end;
    }

    if (!(have & F_VERSION) || !(have & F_NAME) || !(have & F_PID) ||
        !(have & F_PPID) || !(have & F_STATE) || !(have & F_CPU) ||
        !(have & F_BIRTHID) || !(have & F_STARTNS) ||
        !(have & F_SAMPLENS) || !(have & F_RTIMETICKS))
        return -EINVAL;

    if (cpu_val > UINT32_MAX)
        return -EINVAL;
    out->last_cpu = (uint32_t)cpu_val;

    if (decode_name(name_vs, name_le, out->comm) != 0)
        return -EINVAL;

    return 0;
}

/* ── /proc/thermal ────────────────────────────────────────────────── */

static int span_state(const char *vs, const char *le,
                      enum monitor_thermal_state *out)
{
    if (span_is(vs, le, "ok")) {
        *out = MONITOR_THERMAL_STATE_OK;
    } else if (span_is(vs, le, "unsupported")) {
        *out = MONITOR_THERMAL_STATE_UNSUPPORTED;
    } else if (span_is(vs, le, "invalid")) {
        *out = MONITOR_THERMAL_STATE_INVALID;
    } else if (span_is(vs, le, "read_error")) {
        *out = MONITOR_THERMAL_STATE_READ_ERROR;
    } else if (span_is(vs, le, "stale")) {
        *out = MONITOR_THERMAL_STATE_STALE;
    } else if (span_is(vs, le, "unknown")) {
        *out = MONITOR_THERMAL_STATE_UNKNOWN;
    } else {
        return -EINVAL;      /* not a state the fixed table defines */
    }
    return 0;
}

int monitor_parse_thermal(const char *text, size_t len,
                          struct monitor_thermal_sample *out)
{
    if (!text || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));

    const char *p = text;
    const char *end = text + len;
    unsigned have = 0;
    uint64_t version = 0;

    out->state = MONITOR_THERMAL_STATE_UNKNOWN;
    out->reason = MONITOR_THERMAL_REASON_UNKNOWN;
    out->source = MONITOR_THERMAL_SOURCE_UNKNOWN;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *vs = NULL;
        int rc;

        if (line_value(p, le, "Version", &vs)) {
            rc = take_version(&have, vs, le, &version);
            if (rc != 0)
                return rc;
        } else if (line_value(p, le, "State", &vs)) {
            if (seen_field(&have, F_STATE))
                return -EINVAL;
            if (span_state(vs, le, &out->state) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "Reason", &vs)) {
            if (seen_field(&have, F_REASON))
                return -EINVAL;
            /* fixed table per Task 4; anything else keeps UNKNOWN */
            if (span_is(vs, le, "none"))
                out->reason = MONITOR_THERMAL_REASON_NONE;
            else if (span_is(vs, le, "backend_unavailable"))
                out->reason = MONITOR_THERMAL_REASON_BACKEND_UNAVAILABLE;
            else if (span_is(vs, le, "hypervisor"))
                out->reason = MONITOR_THERMAL_REASON_HYPERVISOR;
            else if (span_is(vs, le, "vendor"))
                out->reason = MONITOR_THERMAL_REASON_VENDOR;
            else if (span_is(vs, le, "no_dts"))
                out->reason = MONITOR_THERMAL_REASON_NO_DTS;
            else if (span_is(vs, le, "model"))
                out->reason = MONITOR_THERMAL_REASON_MODEL;
            else if (span_is(vs, le, "not_ready"))
                out->reason = MONITOR_THERMAL_REASON_NOT_READY;
            else if (span_is(vs, le, "worker_start"))
                out->reason = MONITOR_THERMAL_REASON_WORKER_START;
            else if (span_is(vs, le, "msr"))
                out->reason = MONITOR_THERMAL_REASON_MSR;
            else if (span_is(vs, le, "target"))
                out->reason = MONITOR_THERMAL_REASON_TARGET;
            else if (span_is(vs, le, "reading"))
                out->reason = MONITOR_THERMAL_REASON_READING;
            else
                out->reason = MONITOR_THERMAL_REASON_UNKNOWN;
        } else if (line_value(p, le, "Source", &vs)) {
            if (seen_field(&have, F_SOURCE))
                return -EINVAL;
            if (span_is(vs, le, "none"))
                out->source = MONITOR_THERMAL_SOURCE_NONE;
            else if (span_is(vs, le, "core_dts"))
                out->source = MONITOR_THERMAL_SOURCE_CORE_DTS;
            else if (span_is(vs, le, "package_dts"))
                out->source = MONITOR_THERMAL_SOURCE_PACKAGE_DTS;
            else
                out->source = MONITOR_THERMAL_SOURCE_UNKNOWN;
        } else if (line_value(p, le, "SampleNs", &vs)) {
            if (seen_field(&have, F_SAMPLENS))
                return -EINVAL;
            if (span_u64_line(vs, le, &out->sampled_ns) != 0)
                return -EINVAL;
        } else if (line_value(p, le, "TempMilliC", &vs)) {
            if (seen_field(&have, F_TEMP))
                return -EINVAL;
            if (span_i32(&vs, le, &out->temp_mc) != 0)
                return -EINVAL;
            if (span_skip_ws(vs, le) != le)
                return -EINVAL;
        } else if (line_value(p, le, "CpuId", &vs)) {
            if (seen_field(&have, F_CPUID))
                return -EINVAL;
            if (span_u32_line(vs, le, &out->cpu_id) != 0)
                return -EINVAL;
        } else if (!line_ignorable(p, le)) {
            return -EINVAL;              /* garbage line, not a field */
        }

        p = nl ? nl + 1 : end;
    }

    if (!(have & F_VERSION) || !(have & F_STATE) ||
        !(have & F_REASON) || !(have & F_SOURCE) || !(have & F_SAMPLENS))
        return -EINVAL;

    /* state=ok must carry the value and the owning CPU */
    if (out->state == MONITOR_THERMAL_STATE_OK &&
        (!(have & F_TEMP) || !(have & F_CPUID)))
        return -EINVAL;

    return 0;
}

/* ── Collection ───────────────────────────────────────────────────── */

/* Read path into buf (capacity cap+1).  Returns the byte count, or -1
 * if the file could not be read, failed mid-way, or is longer than cap
 * (overlong files are invalid, never truncated). */
static int64_t read_file_bounded(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    size_t total = 0;
    int failed = 0;
    int64_t n;

    for (;;) {
        n = read(fd, buf + total, cap + 1 - total);
        if (n < 0) {
            failed = 1;
            break;
        }
        if (n == 0)
            break;                       /* stable EOF */
        total += (size_t)n;
        if (total > cap) {
            failed = 1;                  /* longer than the protocol cap */
            break;
        }
    }

    close(fd);
    return failed ? -1 : (int64_t)total;
}

static int pid_name_to_u64(const char *s, uint64_t *out)
{
    if (*s == '\0')
        return -EINVAL;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -EINVAL;              /* all-decimal only */
        unsigned d = (unsigned)(*s - '0');
        if (v > (UINT64_MAX - d) / 10)
            return -EINVAL;              /* not representable */
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

static int already_have_pid(const struct monitor_sample *out, uint64_t pid)
{
    for (uint32_t i = 0; i < out->nprocesses; i++)
        if (out->processes[i].pid == pid)
            return 1;
    return 0;
}

static int process_array_push(struct monitor_sample *out,
                              const struct monitor_process_sample *ps)
{
    if (out->nprocesses == out->process_cap) {
        uint32_t ncap = out->process_cap ? out->process_cap * 2 : 16;
        struct monitor_process_sample *np =
            malloc((size_t)ncap * sizeof(*np));
        if (!np)
            return -ENOMEM;
        memcpy(np, out->processes,
               (size_t)out->nprocesses * sizeof(*np));
        free(out->processes);
        out->processes = np;
        out->process_cap = ncap;
    }
    out->processes[out->nprocesses++] = *ps;
    return 0;
}

void monitor_sample_destroy(struct monitor_sample *out)
{
    if (!out)
        return;
    free(out->processes);
    memset(out, 0, sizeof(*out));
}

int monitor_collect(struct monitor_sample *out)
{
    if (!out)
        return -EINVAL;

    /* release any previous round first; the struct always ends either
     * fully populated or destroyed */
    monitor_sample_destroy(out);
    memset(out, 0, sizeof(*out));

    /* one scratch buffer, sized for the largest cap + overlong probe */
    char buf[MONITOR_STATUS_CAP + 1];
    int64_t n;

    n = read_file_bounded("/proc/stat", buf, MONITOR_STAT_CAP);
    if (n >= 0 &&
        monitor_parse_stat(buf, (size_t)n, &out->stat) == 0)
        out->stat_valid = 1;

    n = read_file_bounded("/proc/thermal", buf, MONITOR_THERMAL_CAP);
    if (n >= 0 &&
        monitor_parse_thermal(buf, (size_t)n, &out->thermal) == 0)
        out->thermal_valid = 1;

    n = read_file_bounded("/proc/meminfo", buf, MONITOR_MEMINFO_CAP);
    if (n >= 0 &&
        monitor_parse_meminfo(buf, (size_t)n, &out->mem) == 0)
        out->mem_valid = 1;

    /* per-PID status: open -> read -> close before the next open; at
     * most the directory handle plus one status fd are ever held */
    DIR *d = opendir("/proc");
    if (!d)
        return 0;                /* no process list: not a round failure */

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        uint64_t pid;
        if (pid_name_to_u64(de->d_name, &pid) != 0)
            continue;            /* only clean decimal PIDs */
        if (already_have_pid(out, pid))
            continue;            /* readdir may repeat: deduplicate */

        char path[64];
        snprintf(path, sizeof(path), "/proc/%llu/status",
                 (unsigned long long)pid);

        n = read_file_bounded(path, buf, MONITOR_STATUS_CAP);
        if (n < 0)
            continue;            /* vanished / unreadable / overlong */

        struct monitor_process_sample ps;
        if (monitor_parse_status(buf, (size_t)n, &ps) != 0)
            continue;            /* one bad file only drops one line */
        if (ps.pid != pid)
            continue;            /* incoherent record: the /proc entry IS
                                    the task identity (spec §5.2); never
                                    key history on a mismatched Pid */

        if (process_array_push(out, &ps) != 0) {
            /* OOM: clean the round's array and report -ENOMEM */
            closedir(d);
            monitor_sample_destroy(out);
            return -ENOMEM;
        }
    }

    closedir(d);
    return 0;
}
