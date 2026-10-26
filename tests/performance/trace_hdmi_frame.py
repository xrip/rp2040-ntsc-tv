#!/usr/bin/env python3
"""Trace all 525 software IRQ calls of one frame in a linked RP2040 ELF.

Same ideal instruction model/limitations as trace_hdmi_cycles.py. Neither tool
models PIO, DMA or bus/cache contention. Run on an unstripped firmware ELF.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import statistics

from trace_hdmi_cycles import Trace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--columns', type=int, choices=(40, 80), default=80)
    parser.add_argument('--rows', type=int, choices=(25, 30), default=30)
    parser.add_argument('--transport', choices=('bytes', 'commands'), default='bytes')
    parser.add_argument('--variant', choices=('normal', 'msb', 'all', 'hidden'), default='normal')
    parser.add_argument('--dirty-divider', action='store_true')
    parser.add_argument('--initialize', action='store_true')
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--json', type=Path, required=True)
    args = parser.parse_args()
    trace = Trace(args.elf, args.dirty_divider)
    trace.transport = args.transport
    if args.initialize:
        trace.call('hdmi_change_workspace_mode', (1,))
    options = {}
    if args.variant == 'msb':
        options['msb'] = True
    elif args.variant == 'all':
        options.update(msb=True, blink=True, cursor=True, wrap=True)
    elif args.variant == 'hidden':
        options.update(msb=True, blink=True, cursor=True, wrap=True, hidden=True)
    trace.configure(columns=args.columns, rows=args.rows, **options)
    totals = Counter()
    lines = []
    max_stack = 0
    abi_checked = 0
    guards_checked = 0
    for line in range(525):
        # Model an interrupted divider user on every line when requested.
        trace.dirty = args.dirty_divider
        result = trace.scanline(line, irq=True, buffer=line & 1)
        if args.verify:
            trace.check_text_line(line, buffer=line & 1, transport=args.transport)
        max_stack = max(max_stack, result.pop('max_software_stack_bytes'))
        abi_checked += int(result.pop('r4_r11_preserved', False))
        guards_checked += int(result.pop('workspace_and_input_guards_passed', False))
        totals.update(result)
        lines.append(result['ideal_m0plus_cycles'])
    report = {
        'elf': str(args.elf), 'elf_sha256': trace.elf_sha256,
        'mode': f'{args.columns}x{args.rows}',
        'transport': args.transport, 'variant': args.variant,
        'dirty_divider_each_irq': args.dirty_divider,
        'all_pixels_verified': args.verify,
        'scope': '525 software IRQ calls; excludes hardware exception entry/return, DMA/CPU contention, stalls',
        'totals': dict(totals),
        'max_software_stack_bytes': max_stack,
        'irqs_with_r4_r11_preservation_verified': abi_checked,
        'irqs_with_workspace_and_input_guards_verified': guards_checked,
        'ideal_software_irq_fraction_378mhz_60hz': totals['ideal_m0plus_cycles'] / 6300000,
        'line_min_cycles': min(lines), 'line_max_cycles': max(lines),
        'line_mean_cycles': statistics.mean(lines),
        'line_cycles': lines,
    }
    args.json.write_text(json.dumps(report, indent=2) + '\n')
    del report['line_cycles']
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
