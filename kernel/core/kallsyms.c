#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct symbol_entry {
    uint64_t address;
    char type;
    int priority;
    char *symbol;
    size_t symbol_length;
};

static struct symbol_entry *table;
static size_t capacity, count;

static uint64_t trace_text_start, trace_text_end;
static uint64_t trace_ltext_start, trace_ltext_end;
static bool has_text_start, has_text_end;
static bool has_ltext_start, has_ltext_end;

static int symbol_priority(char type)
{
    switch (type) {
    case 'T':
        return 3;
    case 't':
        return 2;
    case 'W':
    case 'w':
        return 1;
    default:
        return 0;
    }
}

static bool is_range_marker(const char *name)
{
    static const char *const markers[] = {
        "_text",
        "_etext",
        "_text_start",
        "_text_end",
        "_ltext",
        "_eltext",
        "__trace_text_start",
        "__trace_text_end",
        "__trace_ltext_start",
        "__trace_ltext_end",
    };
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        if (strcmp(name, markers[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_excluded_symbol(const char *name)
{
    if (name[0] == '\0' || name[0] == '$' || name[0] == '.') {
        return true;
    }
    return is_range_marker(name);
}

static int read_symbol(FILE *filp, struct symbol_entry *sym_entry)
{
    char line[1024];
    char name[512];
    uint64_t addr = 0;
    char type = '\0';

    if (!fgets(line, sizeof(line), filp)) {
        return -1;
    }
    if (sscanf(line, "%" SCNx64 " %c %511s", &addr, &type, name) != 3) {
        return -1;
    }

    sym_entry->address = addr;
    sym_entry->type = type;
    sym_entry->priority = symbol_priority(type);
    sym_entry->symbol = strdup(name);
    if (!sym_entry->symbol) {
        fprintf(stderr, "kallsyms: out of memory\n");
        exit(1);
    }
    sym_entry->symbol_length = strlen(name) + 1;
    return 0;
}

static void read_map(FILE *filp)
{
    struct symbol_entry tmp;

    while (read_symbol(filp, &tmp) == 0 || !feof(filp)) {
        if (tmp.symbol == NULL) {
            continue;
        }
        if (strcmp(tmp.symbol, "__trace_text_start") == 0) {
            trace_text_start = tmp.address;
            has_text_start = true;
        } else if (strcmp(tmp.symbol, "__trace_text_end") == 0) {
            trace_text_end = tmp.address;
            has_text_end = true;
        } else if (strcmp(tmp.symbol, "__trace_ltext_start") == 0) {
            trace_ltext_start = tmp.address;
            has_ltext_start = true;
        } else if (strcmp(tmp.symbol, "__trace_ltext_end") == 0) {
            trace_ltext_end = tmp.address;
            has_ltext_end = true;
        }

        if (count >= capacity) {
            capacity = capacity ? capacity * 2 : 256;
            table = realloc(table, sizeof(*table) * capacity);
            if (!table) {
                fprintf(stderr, "kallsyms: out of memory\n");
                exit(1);
            }
        }
        table[count++] = tmp;
        memset(&tmp, 0, sizeof(tmp));
    }
}

static bool in_trace_ranges(uint64_t addr)
{
    if (has_text_start && has_text_end && trace_text_start < trace_text_end) {
        if (addr >= trace_text_start && addr < trace_text_end) {
            return true;
        }
    }
    if (has_ltext_start && has_ltext_end && trace_ltext_start < trace_ltext_end) {
        if (addr >= trace_ltext_start && addr < trace_ltext_end) {
            return true;
        }
    }
    return false;
}

static bool symbol_valid(const struct symbol_entry *sym)
{
    if (sym->priority == 0) {
        return false;
    }
    if (is_excluded_symbol(sym->symbol)) {
        return false;
    }
    return in_trace_ranges(sym->address);
}

static int cmp_symbol(const void *pa, const void *pb)
{
    const struct symbol_entry *a = pa;
    const struct symbol_entry *b = pb;

    if (a->address < b->address) {
        return -1;
    }
    if (a->address > b->address) {
        return 1;
    }
    if (a->priority != b->priority) {
        return (a->priority > b->priority) ? -1 : 1;
    }
    return strcmp(a->symbol, b->symbol);
}

static void write_src(void)
{
    struct symbol_entry *filtered = malloc(sizeof(*filtered) * (count ? count : 1));
    size_t valid = 0;

    if (!filtered) {
        fprintf(stderr, "kallsyms: out of memory\n");
        exit(1);
    }

    for (size_t i = 0; i < count; i++) {
        if (symbol_valid(&table[i])) {
            filtered[valid++] = table[i];
        }
    }

    if (valid > 1) {
        qsort(filtered, valid, sizeof(*filtered), cmp_symbol);
    }

    size_t unique = 0;
    for (size_t i = 0; i < valid; i++) {
        if (unique > 0 && filtered[i].address == filtered[unique - 1].address) {
            continue;
        }
        filtered[unique++] = filtered[i];
    }

    printf(".section .rodata\n\n");
    printf(".globl kallsyms_addresses\n");
    printf(".align 8\n\n");
    printf("kallsyms_addresses:\n");
    for (size_t i = 0; i < unique; i++) {
        printf("\t.quad\t0x%" PRIx64 "\n", filtered[i].address);
    }
    putchar('\n');

    printf(".globl kallsyms_syms_num\n");
    printf(".align 8\n\n");
    printf("kallsyms_syms_num:\n");
    printf("\t.quad\t%zu\n", unique);
    putchar('\n');

    printf(".globl kallsyms_index\n");
    printf(".align 8\n\n");
    printf("kallsyms_index:\n");
    size_t position = 0;
    for (size_t i = 0; i < unique; i++) {
        printf("\t.quad\t%zu\n", position);
        position += filtered[i].symbol_length;
    }
    putchar('\n');

    printf(".globl kallsyms_names\n");
    printf(".align 8\n\n");
    printf("kallsyms_names:\n");
    for (size_t i = 0; i < unique; i++) {
        printf("\t.asciz\t\"%s\"\n", filtered[i].symbol);
    }
    putchar('\n');

    free(filtered);
}

int main(void)
{
    read_map(stdin);
    write_src();
    return 0;
}
