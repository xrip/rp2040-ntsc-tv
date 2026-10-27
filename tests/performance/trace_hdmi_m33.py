#!/usr/bin/env python3
"""Execute linked RP2350/Cortex-M33 HDMI code and count instructions.

This is functional emulation, not a cycle-accurate RP2350 timing model. It
reports executed instructions and memory operations, never M0+ cycle prices or
a CPU-usage percentage. DMA, PIO, bus arbitration, caches, exception entry/exit
and competing interrupt/lock users are not modeled.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import statistics
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_SP
import unicorn.arm_const as arm_registers

from trace_hdmi_cycles import Trace, RETURN, VRAM


LOCKS = 0x200d0000


class TraceM33(Trace):
    # M33 supports ordinary unaligned loads/stores with CCR.UNALIGN_TRP clear.
    # Unicorn enforces instructions that still require alignment, such as LDM.
    allow_unaligned = True

    @staticmethod
    def create_machine():
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
        return uc

    def on_code(self, uc, address, size, data):
        self.min_sp = min(self.min_sp, uc.reg_read(UC_ARM_REG_SP))
        if address == RETURN:
            uc.emu_stop()
            return
        if address in self.code_hooks:
            self.code_hooks[address](self)
        if address not in self.instructions:
            self.instructions[address] = next(self.cs.disasm(bytes(uc.mem_read(address, size)), address))
        instruction = self.instructions[address]
        self.counts[instruction.mnemonic] += 1
        self.hotspots[address] += 1
        self.instruction_bytes += size
        if 0x10000000 <= address < 0x11000000:
            self.flash_instructions += 1

    def on_read(self, uc, access, address, size, value, data):
        self.read_bytes += size
        self.unaligned_reads += int(size in (2, 4) and bool(address % size))
        span = getattr(self, 'kernel_input_range', None)
        if span is not None and VRAM - 16 <= address < VRAM + 16384:
            if not (span[0] <= address and address + size <= span[1]):
                raise AssertionError(f'Kernel read beyond its input span: {address:#x}/{size}')
        return super().on_read(uc, access, address, size, value, data)

    def on_write(self, uc, access, address, size, value, data):
        self.write_bytes += size
        self.unaligned_writes += int(size in (2, 4) and bool(address % size))
        return super().on_write(uc, access, address, size, value, data)

    def call(self, name, args=()):
        self.instruction_bytes = self.read_bytes = self.write_bytes = 0
        self.unaligned_reads = self.unaligned_writes = 0
        saved = {}
        if name in ('hdmi_text_span_80', 'hdmi_text_span_40'):
            for index in range(4, 12):
                register = getattr(arm_registers, 'UC_ARM_REG_R' + str(index))
                saved[register] = 0xa5342100 + index * 0x010203
                self.uc.reg_write(register, saved[register])
        result = super().call(name, args)
        for register, expected in saved.items():
            if self.uc.reg_read(register) != expected:
                raise AssertionError(f'Kernel clobbered callee-saved register {register}')
        if saved:
            result['r4_r11_preserved'] = True
        del result['ideal_m0plus_cycles']
        result.update(
            executed_instruction_bytes=self.instruction_bytes,
            data_read_bytes=self.read_bytes,
            data_write_bytes=self.write_bytes,
            unaligned_data_reads=self.unaligned_reads,
            unaligned_data_writes=self.unaligned_writes,
        )
        for symbol in ('hdmi_text_span_80', 'hdmi_text_span_40'):
            first = self.symbols.get(symbol, 0) & ~1
            end = first + self.symbol_sizes.get(symbol, 0)
            result[symbol + '_instructions'] = sum(
                count for address, count in self.hotspots.items() if first <= address < end)
        return result

    def configure(self, wrap_first_cells=None, **options):
        super().configure(**options)
        if wrap_first_cells is not None:
            columns = self.options['columns']
            first = columns - 1 if wrap_first_cells == -1 else wrap_first_cells
            if not self.options['wrap'] or not 1 <= first < columns:
                raise ValueError('wrap_first_cells must select a nonempty split row')
            start = 8192 - 2 * first
            self.options['display_start'] = start
            self.write(self.symbols['text_config'] + 16, start)
            self.write(self.symbols['text_config'] + 20, (start + 30) % 8192)
        # Current RP2350 SDK uses SRAM byte spinlocks (LDAEXB/STREXB/STLB).
        # Configure uncontended locks rather than the RP2040 SIO read-to-claim
        # registers. These fixtures are outside the application's linked RAM.
        self.uc.mem_write(LOCKS, bytes(8))
        self.variable('text_lock', LOCKS)
        self.variable('hdmi_lock', LOCKS + 4, required=False)


VARIANTS = (
    ('normal', {}),
    ('msb', {'msb': True}),
    ('blink_on', {'blink': True}),
    ('blink_hidden', {'blink': True, 'hidden': True}),
    ('cursor', {'cursor': True}),
    ('wrap', {'wrap': True}),
    ('all', {'msb': True, 'blink': True, 'cursor': True, 'wrap': True}),
    ('hidden', {'msb': True, 'blink': True, 'cursor': True, 'wrap': True, 'hidden': True}),
    ('font8', {'font_height': 8, 'font_stride': 8}),
    ('font14', {'font_height': 14, 'font_stride': 14}),
    ('font_stride32', {'font_stride': 32}),
    ('unaligned', {'unaligned': True}),
    ('wrap_first1', {'wrap': True, 'wrap_first_cells': 1}),
    ('wrap_first2', {'wrap': True, 'wrap_first_cells': 2}),
    ('wrap_first3', {'wrap': True, 'wrap_first_cells': 3}),
    ('wrap_last', {'wrap': True, 'wrap_first_cells': -1}),
    ('all_odd_tail', {'msb': True, 'blink': True, 'cursor': True,
                      'wrap': True, 'wrap_first_cells': 3, 'unaligned': True}),
)


def initialized_trace(path):
    trace = TraceM33(path)
    trace.transport = 'commands'
    trace.call('hdmi_change_workspace_mode', (1,))
    return trace


def run_cases(path, verify):
    initial = initialized_trace(path)
    image = bytes(initial.uc.mem_read(initial.symbols['workspace'], 4896))

    def new_trace():
        trace = TraceM33(path)
        trace.transport = 'commands'
        trace.uc.mem_write(trace.symbols['workspace'], image)
        return trace

    records = {}
    for columns, rows in ((80, 30), (80, 25), (40, 25)):
        for variant, options in VARIANTS:
            trace = new_trace()
            trace.configure(columns=columns, rows=rows, **options)
            height = options.get('font_height', 16)
            line = (480 - rows * height * (2 if height <= 8 else 1)) // 2
            result = trace.scanline(line, irq=True)
            if verify:
                result['output_verified'] = trace.check_text_line(line, transport='commands')
            records[f'{columns}x{rows}/{variant}'] = result
    for name, line in (('text/vblank', 480), ('text/vsync', 490), ('text/border', 0)):
        trace = new_trace()
        trace.configure(rows=25)
        records[name] = trace.scanline(line, irq=True)
        if verify:
            records[name]['output_verified'] = trace.check_text_line(line, transport='commands')
    for name, line in (('graphics/even', 0), ('graphics/odd', 1), ('graphics/vblank', 480)):
        trace = new_trace()
        trace.configure()
        trace.variable('text_mode', 0, 1)
        if line == 1:
            trace.scanline(0, irq=True, buffer=1)
        records[name] = trace.scanline(line, irq=True)
        if verify:
            records[name]['output_verified'] = trace.check_graphics_line(line)
    for columns in (80, 40):
        trace = new_trace()
        for step, (blink, hidden) in enumerate(((False, False), (True, False),
                                               (True, True), (False, True),
                                               (True, True), (True, False),
                                               (False, False))):
            trace.configure(columns=columns, rows=25, msb=True, wrap=True,
                            cursor=True, blink=blink, hidden=hidden)
            state = (3 if hidden else 1) if blink else 0
            for repeat in range(2):
                result = trace.scanline(40, irq=True, buffer=repeat)
                if verify:
                    result['output_verified'] = trace.check_text_line(40, buffer=repeat, transport='commands')
                actual_state = int.from_bytes(trace.uc.mem_read(trace.symbols['workspace'] + 3424, 4), 'little')
                if actual_state != state:
                    raise AssertionError(f'Attribute lookup state {actual_state}, expected {state}')
                result['lookup_state'] = actual_state
                records[f'transition/{columns}cols/step{step}-state{state}/' +
                        ('first' if repeat == 0 else 'steady')] = result
    return {
        'elf': str(path), 'elf_sha256': initial.elf_sha256,
        'target': 'RP2350 Cortex-M33',
        'method': 'executed instruction and memory-operation counts, not timing',
        'scope': 'Software IRQ body/prologue/epilogue; uncontended SRAM byte locks; no hardware exception entry/exit',
        'all_pixels_verified': verify,
        'records': records,
    }


def run_frame(path, columns, rows, variant, verify):
    trace = initialized_trace(path)
    trace.configure(columns=columns, rows=rows, **dict(VARIANTS)[variant])
    totals = Counter()
    lines = []
    max_stack = abi_checked = guards_checked = 0
    for line in range(525):
        result = trace.scanline(line, irq=True, buffer=line & 1)
        if verify:
            trace.check_text_line(line, buffer=line & 1, transport='commands')
        max_stack = max(max_stack, result.pop('max_software_stack_bytes'))
        abi_checked += int(result.pop('r4_r11_preserved', False))
        guards_checked += int(result.pop('workspace_and_input_guards_passed', False))
        totals.update(result)
        lines.append(result['executed_instructions'])
    return {
        'elf': str(path), 'elf_sha256': trace.elf_sha256,
        'target': 'RP2350 Cortex-M33', 'mode': f'{columns}x{rows}',
        'variant': variant, 'all_pixels_verified': verify,
        'method': 'executed instruction and memory-operation counts, not timing',
        'scope': '525 software IRQs; excludes hardware exception entry/return, bus/peripheral/lock contention, caches and other IRQs',
        'totals': dict(totals),
        'max_software_stack_bytes': max_stack,
        'irqs_with_r4_r11_preservation_verified': abi_checked,
        'irqs_with_workspace_and_input_guards_verified': guards_checked,
        'line_min_instructions': min(lines), 'line_max_instructions': max(lines),
        'line_mean_instructions': statistics.mean(lines),
        'line_instructions': lines,
    }


def run_kernels(path, verify):
    """Exercise every span length and both possible STRD output alignments."""
    records = {}
    descriptor = LOCKS + 64
    for columns in (80, 40):
        for msb in (False, True):
            for unaligned in (False, True):
                trace = initialized_trace(path)
                trace.configure(columns=columns, rows=25, msb=msb, unaligned=unaligned)
                glyph_line, font_stride = 3, 16
                cells = bytes(trace.uc.mem_read(trace.vram_address, columns * 2))
                font = bytes(trace.uc.mem_read(trace.font_address, trace.font_size))
                for output_mod8 in (0, 4):
                    output = trace.symbols['workspace'] + 2624 + 72 + output_mod8
                    for count in range(columns + 1):
                        size = count * (4 if columns == 80 else 8)
                        text = struct.pack('<8I4B', trace.vram_address, trace.font_address,
                                           0, count * 2, columns, glyph_line, font_stride,
                                           0xffffffff, msb, 0, 0, 0)
                        trace.uc.mem_write(descriptor, text)
                        trace.uc.mem_write(trace.symbols['workspace'] + 2624, bytes([0xa5]) * 800)
                        trace.active_line_range = (output, output + size)
                        trace.kernel_input_range = (trace.vram_address, trace.vram_address + count * 2)
                        trace.check_scanline_writes = True
                        try:
                            result = trace.call('hdmi_text_span_' + str(columns),
                                                (output, trace.vram_address, count, descriptor))
                        finally:
                            trace.check_scanline_writes = False
                            trace.kernel_input_range = None
                        if result['font_data_reads'] != count:
                            raise AssertionError('Kernel must read exactly one font byte per cell')
                        if verify:
                            raw = bytes(trace.uc.mem_read(output, size)) if size else b''
                            actual = bytearray()
                            for (command,) in struct.iter_unpack('<I', raw):
                                if command & 0x070000e0:
                                    raise AssertionError('Kernel set command padding bits')
                                fg, bg = command & 31, command >> 27
                                for bit in range(8):
                                    pair = (command >> (8 + 2 * bit)) & 3
                                    actual.append((fg if pair & 1 else bg) + 16 * (pair >> 1))
                            expected = bytearray()
                            for cell in range(count):
                                character, attribute = cells[cell * 2:cell * 2 + 2]
                                glyph = font[character * font_stride + glyph_line]
                                for bit in range(8):
                                    ink = glyph & (128 >> bit if msb else 1 << bit)
                                    color = attribute & 15 if ink else attribute >> 4
                                    for _ in range(2 if columns == 40 else 1):
                                        expected.append(color + 16 * (len(expected) & 1))
                            if actual != expected:
                                raise AssertionError('Kernel output differs from scalar glyph reference')
                            result['output_verified'] = True
                        result['workspace_and_input_guards_passed'] = True
                        records[f'{columns}cols/msb{int(msb)}/odd_input{int(unaligned)}/out_mod8_{output_mod8}/count{count}'] = result
    return {
        'elf': str(path), 'elf_sha256': trace.elf_sha256,
        'target': 'RP2350 Cortex-M33',
        'method': 'direct linked-kernel execution; instructions counted, not timed',
        'scope': 'All lengths0..80/40, both font bit orders, aligned/odd input pointers, output address0/4mod8',
        'all_pixels_verified': verify,
        'records': records,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--frame', action='store_true', help='Trace one full frame instead of the case suite')
    mode.add_argument('--kernels', action='store_true', help='Test every direct-kernel span length and alignment')
    parser.add_argument('--columns', type=int, choices=(40, 80), default=80)
    parser.add_argument('--rows', type=int, choices=(25, 30), default=30)
    parser.add_argument('--variant', choices=[name for name, _ in VARIANTS], default='normal')
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--json', type=Path, required=True)
    args = parser.parse_args()
    if args.frame:
        report = run_frame(args.elf, args.columns, args.rows, args.variant, args.verify)
    elif args.kernels:
        report = run_kernels(args.elf, args.verify)
    else:
        report = run_cases(args.elf, args.verify)
    args.json.write_text(json.dumps(report, indent=2) + '\n')
    if args.frame:
        del report['line_instructions']
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
