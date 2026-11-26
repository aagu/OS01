#!/usr/bin/env python3
"""Exercise the production FDT parser with real, bounded binary DTBs."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def cells(*values):
    return struct.pack('>' + 'I' * len(values), *values)


def string(value):
    return value.encode() + b'\0'


def node(name, props=(), children=()):
    return name, list(props), list(children)


def fixture(cpus=(0,), address_cells=1, method='hvc', psci=True,
            disabled=(), enable='psci', missing=None, gicc=0x08010000,
            uart_size=0x1000,
            timer_irqs=(1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4)):
    children = []
    for cpu in cpus:
        reg = cells(cpu) if address_cells == 1 else cells(cpu >> 32, cpu & 0xffffffff)
        props = [('device_type', string('cpu')), ('reg', reg)]
        if enable is not None:
            props.append(('enable-method', string(enable)))
        if cpu in disabled:
            props.append(('status', string('disabled')))
        children.append(node(f'cpu@{cpu:x}', props))
    devices = [
        node('cpus', [('#address-cells', cells(address_cells)), ('#size-cells', cells(0))], children),
        node('intc@8000000', [('compatible', string('arm,cortex-a15-gic')),
             ('reg', cells(0, 0x08000000, 0, 0x10000, 0, gicc, 0, 0x10000))]),
        node('pl011@9000000', [('compatible', string('arm,pl011') + string('arm,primecell')),
             ('reg', cells(0, 0x09000000, 0, uart_size))]),
        node('timer', [('compatible', string('arm,armv8-timer')),
             ('interrupts', cells(*timer_irqs))]),
    ]
    if psci:
        devices.append(node('psci', [('compatible', string('vendor,ignored') + string('arm,psci-0.2')),
                                    ('method', string(method))]))
    if missing:
        devices = [n for n in devices if not n[0].startswith(missing)]
    return node('', [('#address-cells', cells(2)), ('#size-cells', cells(2))], devices)


def flatten(tree):
    names = bytearray()
    offsets = {}
    structure = bytearray()

    def emit(n):
        name, props, children = n
        structure.extend(cells(1) + string(name))
        structure.extend(b'\0' * (-len(structure) % 4))
        for key, data in props:
            if key not in offsets:
                offsets[key] = len(names)
                names.extend(string(key))
            structure.extend(cells(3, len(data), offsets[key]) + data)
            structure.extend(b'\0' * (-len(structure) % 4))
        for child in children:
            emit(child)
        structure.extend(cells(2))

    emit(tree)
    structure.extend(cells(9))
    # Complete v17 header and terminated reservation map.
    header = cells(0xd00dfeed, 56 + len(structure) + len(names), 56,
                   56 + len(structure), 40, 17, 16, 0, len(names), len(structure))
    return bytearray(header + bytes(16) + structure + names)


RUNNER = r'''
#include <arch/aarch64/dtb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 3 && argc != 5) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f || fseek(f, 0, SEEK_END)) return 2;
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET)) return 2;
    unsigned char *blob = malloc((size_t)size + 1);
    if (!blob || fread(blob, 1, (size_t)size, f) != (size_t)size) return 2;
    fclose(f);
    struct aarch64_platform_info out;
    if (aarch64_dtb_parse(blob, (uint32_t)size, 0, NULL) != -1) return 4;
    memset(&out, 0xa5, sizeof(out));
    if (aarch64_dtb_parse(NULL, 0, 0, &out) != -1) return 4;
    for (size_t i = 0; i < sizeof(out); ++i)
        if (((unsigned char *)&out)[i]) return 4;
    memset(&out, 0xa5, sizeof(out));
    int rc;
    if (argc == 5) {
        uint64_t cpa = strtoull(argv[3], 0, 0);
        int fw = atoi(argv[4]);
        rc = aarch64_dtb_parse_for_console(blob, (uint32_t)size,
                                           strtoull(argv[2], 0, 0),
                                           cpa, fw != 0, &out);
        printf("%d %llx %u %u\n", rc,
               (unsigned long long)out.pl011_base,
               out.pl011_clock, out.pl011_spi);
        free(blob);
        return 0;
    }
    rc = aarch64_dtb_parse(blob, (uint32_t)size, strtoull(argv[2], 0, 0), &out);
    printf("%d %u %d %d %llx %llx %llx %u %u", rc, out.topology.cpu_count,
           out.topology.conduit, out.topology.psci_compatible,
           (unsigned long long)out.gicd_base, (unsigned long long)out.gicc_base,
           (unsigned long long)out.pl011_base, out.cntp_ppi, out.pl011_spi);
    for (unsigned i = 0; i < out.topology.cpu_count; ++i)
        printf(" %llx", (unsigned long long)out.topology.mpidr[i]);
    puts("");
    if (rc) {
        for (size_t i = 0; i < sizeof(out); ++i)
            if (((unsigned char *)&out)[i]) return 3;
    }
    free(blob);
    return 0;
}
'''


def main():
    # Each case catches a distinct semantic or boundary regression; expected
    # CPU arrays are literals, especially Aff3 where truncation can hide a CPU.
    cases = [
        ('one_cpu_no_psci', fixture(psci=False, enable=None), 0, 0, [0]),
        ('four_cpu_hvc', fixture(cpus=(0, 1, 2, 3)), 0, 0, [0, 1, 2, 3]),
        ('two_cells_affinity_0x100', fixture(cpus=(0, 0x100), address_cells=2), 0, 0, [0, 0x100]),
        ('aff3_0x100000000', fixture(cpus=(0, 0x100000000), address_cells=2), 0, 0, [0, 0x100000000]),
        ('bsp_not_first', fixture(cpus=(1, 0, 2)), 0x80000000, 0, [0, 1, 2]),
        ('missing_device', fixture(missing='pl011'), 0, -4, []),
        ('missing_gic', fixture(missing='intc'), 0, -4, []),
        ('missing_timer', fixture(missing='timer'), 0, -4, []),
        ('custom_gicc', fixture(gicc=0x08020000), 0, 0, [0]),
        ('zero_gicc', fixture(gicc=0x0), 0, -4, []),
        ('short_uart_reg', fixture(uart_size=0x40), 0, -4, []),
        ('disabled_cpu_ignored', fixture(cpus=(0, 1), disabled=(1,)), 0, 0, [0]),
        ('duplicate_mpidr', fixture(cpus=(0, 0)), 0, -2, []),
        ('capacity_plus_one', fixture(cpus=tuple(range(9))), 0, -2, []),
        ('missing_bsp', fixture(cpus=(1,)), 0, -2, []),
        ('missing_bsp_without_enable', fixture(cpus=(1, 2), enable=None), 0, -2, []),
        ('invalid_method', fixture(method='hvc-extra'), 0, -3, []),
        ('multi_cpu_without_psci', fixture(cpus=(0, 1), psci=False), 0, -3, []),
        ('wrong_enable_method', fixture(cpus=(0, 1), enable='spin-table'), 0, -3, []),
        ('missing_enable_method', fixture(cpus=(0, 1), enable=None), 0, -3, []),
        ('smc_conduit', fixture(method='smc'), 0, 0, [0]),
        ('affinity_normalized_duplicate', fixture(cpus=(0, 0x80000000)), 0, -2, []),
        ('capacity_exact', fixture(cpus=tuple(range(8))), 0, 0, list(range(8))),
        ('timer_omits_secure_ppi', fixture(timer_irqs=(1, 14, 4, 1, 11, 4, 1, 10, 4)), 0, 0, [0]),
        ('timer_single_cntp_ppi', fixture(timer_irqs=(1, 14, 4)), 0, 0, [0]),
        ('timer_missing_cntp_ppi', fixture(timer_irqs=(1, 13, 4, 1, 11, 4)), 0, -4, []),
        ('timer_bad_interrupts_len', fixture(timer_irqs=(1, 14)), 0, -4, []),
    ]
    one_cell_devices = fixture()
    one_cell_devices[1][:] = [('#address-cells', cells(1)), ('#size-cells', cells(1))]
    one_cell_devices[2][1][1][1] = ('reg', cells(0x08000000, 0x10000, 0x08010000, 0x10000))
    one_cell_devices[2][2][1][1] = ('reg', cells(0x09000000, 0x1000))
    cases.append(('one_cell_device_addresses', one_cell_devices, 0, 0, [0]))

    # Nested 1-cell/2-cell bus with empty ranges and translated bus
    nested = fixture(missing='pl011')
    nested[2].append(node('bus', [
        ('#address-cells', cells(1)), ('#size-cells', cells(1)),
        ('ranges', cells(0x0, 0x0, 0x09000000, 0x2000)),
    ], [
        node('sub', [
            ('#address-cells', cells(1)), ('#size-cells', cells(1)),
            ('ranges', b''),
        ], [
            node('pl011@0', [
                ('compatible', string('arm,pl011')),
                ('reg', cells(0x0, 0x1000)),
            ]),
        ]),
    ]))
    cases.append(('nested_empty_and_translated_ranges', nested, 0, 0, [0]))

    # Intermediate bus missing ranges -> PL011 unreachable (-4)
    no_ranges_bus = fixture(missing='pl011')
    no_ranges_bus[2].append(node('bus', [
        ('#address-cells', cells(2)), ('#size-cells', cells(2)),
    ], [
        node('pl011@9000000', [
            ('compatible', string('arm,pl011')),
            ('reg', cells(0, 0x09000000, 0, 0x1000)),
        ]),
    ]))
    cases.append(('missing_ranges_bus', no_ranges_bus, 0, -4, []))

    # Reg extends past ranges endpoint -> unreachable (-4)
    partial_ranges_bus = fixture(missing='pl011')
    partial_ranges_bus[2].append(node('bus', [
        ('#address-cells', cells(1)), ('#size-cells', cells(1)),
        ('ranges', cells(0x0, 0x0, 0x09000000, 0x0800)),
    ], [
        node('pl011@0', [
            ('compatible', string('arm,pl011')),
            ('reg', cells(0x0, 0x1000)),
        ]),
    ]))
    cases.append(('reg_crosses_ranges_end', partial_ranges_bus, 0, -4, []))

    def with_pl011_irq(irq_cells):
        t = fixture()
        t[2][2][1].append(('interrupts', cells(*irq_cells)))
        return t

    pl011_spi_expected = {
        'pl011_spi_1_level_high': 33,
        'pl011_spi_0': 32,
        'pl011_spi_edge_trigger': 37,
        'pl011_multi_interrupts': 34,
        'pl011_4cell_degrades': 0,
        'pl011_non_spi_degrades': 0,
    }
    cases.extend([
        ('pl011_spi_1_level_high', with_pl011_irq((0, 1, 4)), 0, 0, [0]),
        ('pl011_spi_0', with_pl011_irq((0, 0, 4)), 0, 0, [0]),
        ('pl011_spi_edge_trigger', with_pl011_irq((0, 5, 1)), 0, 0, [0]),
        ('pl011_multi_interrupts', with_pl011_irq((0, 2, 4, 0, 3, 4)), 0, 0, [0]),
        ('pl011_4cell_degrades', with_pl011_irq((0, 1, 4, 0)), 0, 0, [0]),
        ('pl011_non_spi_degrades', with_pl011_irq((1, 1, 4)), 0, 0, [0]),
    ])
    encoded = [(n, flatten(t), bsp, rc, ids) for n, t, bsp, rc, ids in cases]
    bad = flatten(fixture())
    struct.pack_into('>I', bad, 68, 0xffffffff)  # first property's length
    encoded.append(('truncated_property', bad, 0, -1, []))
    bad = flatten(fixture())
    struct.pack_into('>I', bad, 72, 0xffffffff)  # property name offset
    encoded.append(('nameoff_outside_strings', bad, 0, -1, []))
    bad = flatten(fixture())
    bad[-1] = ord('X')  # final property name lacks a NUL in strings block
    encoded.append(('unterminated_string', bad, 0, -1, []))
    bad = flatten(fixture(method='hvc'))
    offset = bad.index(b'hvc\0')
    bad[offset + 3] = ord('X')
    encoded.append(('unterminated_property_string', bad, 0, -1, []))
    bad = flatten(fixture())
    struct.pack_into('>I', bad, 36, 10)  # token with only two remaining bytes
    encoded.append(('partial_token', bad, 0, -1, []))
    bad = flatten(fixture())
    struct.pack_into('>I', bad, 36, 16)  # property header missing nameoff
    encoded.append(('partial_property_header', bad, 0, -1, []))
    bad = flatten(fixture())
    struct.pack_into('>I', bad, 8, 0xfffffff0)
    encoded.append(('struct_range_overflow', bad, 0, -1, []))
    bad = flatten(fixture())
    struct.pack_into('>I', bad, 36, struct.unpack_from('>I', bad, 36)[0] - 4)
    encoded.append(('missing_end', bad, 0, -1, []))
    deep = fixture()
    for _ in range(32):
        deep = node('', children=[deep])
    encoded.append(('depth_overflow', flatten(deep), 0, -1, []))
    encoded.append(('short_header', flatten(fixture())[:39], 0, -1, []))
    encoded.append(('truncated_blob', flatten(fixture())[:-1], 0, -1, []))

    with tempfile.TemporaryDirectory(prefix='aarch64-dtb-') as tmp:
        tmp = Path(tmp)
        runner_c = tmp / 'runner.c'
        runner_c.write_text(RUNNER)
        runner = tmp / 'runner'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Ikernel/include', 'kernel/arch/aarch64/platform/dtb_parse.c', str(runner_c),
                        '-o', str(runner)], cwd=ROOT, check=True)
        for name, data, bsp, expected_rc, expected_ids in encoded:
            path = tmp / (name + '.dtb')
            path.write_bytes(data)
            result = subprocess.run([str(runner), str(path), hex(bsp)], check=True,
                                    text=True, capture_output=True).stdout.split()
            assert int(result[0]) == expected_rc, (name, result)
            if expected_rc == 0:
                assert int(result[1]) == len(expected_ids), (name, result)
                assert [int(x, 16) for x in result[9:]] == expected_ids, (name, result)
                expected_gicc = '8020000' if name == 'custom_gicc' else '8010000'
                assert result[4:8] == ['8000000', expected_gicc, '9000000', '30'], (name, result)
                assert int(result[8]) == pl011_spi_expected.get(name, 0), (name, result)
                conduit = {'one_cpu_no_psci': ['0', '0'], 'smc_conduit': ['1', '1']}
                assert result[2:4] == conduit.get(name, ['2', '1']), (name, result)
            print('PASS', name)

        # Console-selection parser tests (Task 3)
        two_uarts = fixture()
        two_uarts[2][2][1].extend([
            ('clock-frequency', cells(24000000)),
            ('interrupts', cells(0, 1, 4)),
        ])
        two_uarts[2].append(node('pl011@9020000', [
            ('compatible', string('arm,pl011')),
            ('reg', cells(0, 0x09020000, 0, 0x1000)),
            ('clock-frequency', cells(48000000)),
            ('interrupts', cells(0, 3, 4)),
        ]))
        p_two = tmp / 'two_uarts.dtb'
        p_two.write_bytes(flatten(two_uarts))
        res = subprocess.run([str(runner), str(p_two), '0', '0x09020000', '0'],
                             check=True, text=True, capture_output=True).stdout.split()
        assert res == ['0', '9020000', '48000000', '35'], res
        res = subprocess.run([str(runner), str(p_two), '0', '0x09040000', '0'],
                             check=True, text=True, capture_output=True).stdout.split()
        assert res[0] == '-4', res

        dup_uarts = fixture()
        dup_uarts[2].append(node('pl011@9000000-dup', [
            ('compatible', string('arm,pl011')),
            ('reg', cells(0, 0x09000000, 0, 0x1000)),
        ]))
        p_dup = tmp / 'dup_uarts.dtb'
        p_dup.write_bytes(flatten(dup_uarts))
        res = subprocess.run([str(runner), str(p_dup), '0', '0x09000000', '0'],
                             check=True, text=True, capture_output=True).stdout.split()
        assert res[0] == '-4', res

        no_uart = fixture(missing='pl011')
        p_nouart = tmp / 'no_uart.dtb'
        p_nouart.write_bytes(flatten(no_uart))
        res = subprocess.run([str(runner), str(p_nouart), '0', '0x1f00030000', '1'],
                             check=True, text=True, capture_output=True).stdout.split()
        assert res == ['0', '1f00030000', '0', '0'], res
        print('PASS console_selection_cases')

        # virtio-mmio DTB validation cases (Task 4)
        vio_overflow = fixture()
        vio_overflow[2].append(node('virtio_mmio@fffffffffffff000', [
            ('compatible', string('virtio,mmio')),
            ('reg', cells(0xffffffff, 0xfffff000, 0, 0x2000)),
        ]))
        p_vo = tmp / 'vio_overflow.dtb'
        p_vo.write_bytes(flatten(vio_overflow))
        res = subprocess.run([str(runner), str(p_vo), '0'],
                             check=True, text=True, capture_output=True).stdout.split()
        assert res[0] == '-4', res

        vio_short = fixture()
        vio_short[2].append(node('virtio_mmio@a000000', [
            ('compatible', string('virtio,mmio')),
            ('reg', cells(0, 0x0a000000, 0, 0x100)),
        ]))
        p_vs = tmp / 'vio_short.dtb'
        p_vs.write_bytes(flatten(vio_short))
        res = subprocess.run([str(runner), str(p_vs), '0'],
                             check=True, text=True, capture_output=True).stdout.split()
        assert res[0] == '-4', res

        vio_unaligned = fixture()
        vio_unaligned[2].append(node('virtio_mmio@a000002', [
            ('compatible', string('virtio,mmio')),
            ('reg', cells(0, 0x0a000002, 0, 0x200)),
        ]))
        p_vu = tmp / 'vio_unaligned.dtb'
        p_vu.write_bytes(flatten(vio_unaligned))
        res = subprocess.run([str(runner), str(p_vu), '0'],
                             check=True, text=True, capture_output=True).stdout.split()
        assert res[0] == '-4', res

        vio_cross_ranges = fixture()
        vio_cross_ranges[2].append(node('bus', [
            ('#address-cells', cells(1)), ('#size-cells', cells(1)),
            ('ranges', cells(0x1000, 0x0, 0x0a001000, 0x100)),
        ], [
            node('virtio_mmio@1000', [
                ('compatible', string('virtio,mmio')),
                ('reg', cells(0x1000, 0x200)),
            ]),
        ]))
        p_vcr = tmp / 'vio_cross_ranges.dtb'
        p_vcr.write_bytes(flatten(vio_cross_ranges))
        res = subprocess.run([str(runner), str(p_vcr), '0'],
                             check=True, text=True, capture_output=True).stdout.split()
        assert res[0] == '-4', res
        print('PASS virtio_mmio_cases')
    print(f'{len(encoded)} DTB cases passed')


if __name__ == '__main__':
    main()
