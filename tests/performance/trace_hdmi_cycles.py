#!/usr/bin/env python3
"""Execute a linked RP2040 HDMI scanline and count Cortex-M0+ instructions.

Requires unicorn, capstone, pyelftools. This is NOT a cycle-accurate RP2040
emulator: SRAM/AHB accesses cost two core clocks and SIO accesses one, all
without contention. Flash instruction/data accesses are reported separately.
PIO, DMA, the second core, interrupt arbitration and flash caches are absent.
The computed result is a reproducible ideal instruction-cycle estimate.

Timing reference: Arm DDI 0484B Cortex-M0+ TRM, table 3-1.
https://documentation-service.arm.com/static/60411742ee937942ba3016f2
"""
import argparse
from collections import Counter
import hashlib
import io
import json
from pathlib import Path
import struct
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
from capstone.arm import ARM_OP_MEM, ARM_REG_PC
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import *

SIO = 0xd0000000
SRAM = 0x20000000
RETURN = 0x200ffff0
STACK = 0x200ff000
VRAM = 0x200b0000
FONT = 0x200c0000
FRAMEBUFFER = 0x20080000
CS_TO_UC = {
    'r' + str(i): globals()['UC_ARM_REG_R' + str(i)] for i in range(13)
}
CS_TO_UC.update({'sp': UC_ARM_REG_SP, 'lr': UC_ARM_REG_LR,
                 'pc': UC_ARM_REG_PC, 'ip': UC_ARM_REG_R12,
                 'fp': UC_ARM_REG_R11, 'sl': UC_ARM_REG_R10,
                 'sb': UC_ARM_REG_R9})


class Trace:
    elf_images = {}

    def __init__(self, path, dirty_divider=False):
        self.path = path
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.mem_map(0x10000000, 0x1000000)
        self.uc.mem_map(SRAM, 0x100000)
        self.uc.mem_map(SIO, 0x1000)
        self.uc.mem_map(0x50000000, 0x10000)
        self.uc.mem_map(0xe000e000, 0x2000)
        self.cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        self.cs.detail = True
        self.symbols = {}
        self.symbol_sizes = {}
        image_key = str(path.resolve())
        if image_key not in self.elf_images:
            self.elf_images[image_key] = path.read_bytes()
        self.elf_sha256 = hashlib.sha256(self.elf_images[image_key]).hexdigest()
        with io.BytesIO(self.elf_images[image_key]) as stream:
            elf = ELFFile(stream)
            for section in elf.iter_sections():
                if section['sh_flags'] & 2 and section['sh_size']:
                    addr = section['sh_addr']
                    if 0x10000000 <= addr < 0x11000000 or SRAM <= addr < SRAM + 0x100000:
                        self.uc.mem_write(addr, section.data())
            for sym in elf.get_section_by_name('.symtab').iter_symbols():
                if sym.name:
                    self.symbols[sym.name] = sym['st_value']
                    self.symbol_sizes[sym.name] = sym['st_size']
        self.div = {0x60: 123, 0x64: 7, 0x70: 17, 0x74: 4}
        self.dirty = dirty_divider
        self.instructions = {}
        self.prev = None
        self.cycles = 0
        self.counts = Counter()
        self.hotspots = Counter()
        self.flash_instructions = 0
        self.flash_data_reads = 0
        self.read_count = 0
        self.write_count = 0
        self.min_sp = STACK
        self.transport = 'bytes'
        self.check_scanline_writes = False
        self.configured = False
        self.font_reads = 0
        self.vram_reads = 0
        self.code_hooks = {}
        self.uc.hook_add(UC_HOOK_CODE, self.on_code)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.on_read)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write)
        self.write(SIO + 0x100, 1)
        self.write(SIO + 0x104, 1)

    def write(self, address, value, size=4):
        self.uc.mem_write(address, int(value).to_bytes(size, 'little'))

    def variable(self, name, value, size=4, required=True):
        if name not in self.symbols:
            if required:
                raise RuntimeError('ELF has no symbol ' + name)
            return
        self.write(self.symbols[name], value, size)

    def on_read(self, uc, access, address, size, value, data):
        self.read_count += 1
        if size in (2, 4) and address % size:
            raise RuntimeError(f'Unaligned Cortex-M0+ read: {size} bytes at {address:#x}')
        if self.configured:
            if FONT - 16 <= address < FONT + 16384:
                if not self.font_address <= address or address + size > self.font_address + self.font_size:
                    raise AssertionError(f'Font read outside caller allocation: {address:#x}/{size}')
                self.font_reads += 1
            elif VRAM - 16 <= address < VRAM + 16384:
                if not self.vram_address <= address or address + size > self.vram_address + 8192:
                    raise AssertionError(f'VRAM read outside caller allocation: {address:#x}/{size}')
                self.vram_reads += 1
        if 0x10000000 <= address < 0x11000000:
            self.flash_data_reads += 1
        offset = address - SIO
        if offset == 0x78:
            self.write(address, 1 | (2 if self.dirty else 0))
        elif offset in self.div:
            self.write(address, self.div[offset])
            if offset == 0x70:
                self.dirty = False
        elif 0x100 <= offset < 0x180:
            self.write(address, 1)

    def on_write(self, uc, access, address, size, value, data):
        self.write_count += 1
        if size in (2, 4) and address % size:
            raise RuntimeError(f'Unaligned Cortex-M0+ write: {size} bytes at {address:#x}')
        if self.check_scanline_writes:
            workspace = self.symbols['workspace']
            if workspace <= address < workspace + 4896:
                first, end = self.active_line_range
                allowed = first <= address and address + size <= end
                if self.transport == 'commands':
                    update = self.symbols.get('hdmi_update_text_attributes', 0) & ~1
                    update_size = self.symbol_sizes.get('hdmi_update_text_attributes', 0)
                    pc = uc.reg_read(UC_ARM_REG_PC)
                    if update <= pc < update + update_size:
                        allowed |= (workspace + 1024 <= address and address + size <= workspace + 1536)
                        allowed |= (workspace + 3424 <= address and address + size <= workspace + 3428)
                if not allowed:
                    raise AssertionError(f'Scanline wrote outside its free buffer/LUT: {address:#x}/{size}')
            if FONT - 16 <= address < FONT + 16384 or VRAM - 16 <= address < VRAM + 16384:
                raise AssertionError(f'Scanout wrote caller VRAM/font: {address:#x}/{size}')
        offset = address - SIO
        if offset in self.div:
            self.div[offset] = value
            self.dirty = True
            if offset in (0x60, 0x64):
                a, b = self.div[0x60], self.div[0x64]
                self.div[0x70] = a // b if b else 0xffffffff
                self.div[0x74] = a % b if b else a

    def memory_address(self, instruction):
        for operand in instruction.operands:
            if operand.type == ARM_OP_MEM:
                mem = operand.mem
                def regval(reg):
                    if not reg:
                        return 0
                    if reg == ARM_REG_PC:
                        return (instruction.address + 4) & ~3
                    return self.uc.reg_read(CS_TO_UC[instruction.reg_name(reg)])
                return (regval(mem.base) + regval(mem.index) * mem.scale + mem.disp) & 0xffffffff
        return None

    def price(self, instruction, following, mem_address):
        name = instruction.mnemonic.split('.')[0]
        if name.startswith(('ldr', 'str')):
            return 1 if mem_address is not None and SIO <= mem_address < SIO + 0x1000 else 2
        if name in ('ldm', 'ldmia', 'stm', 'stmia'):
            return len(instruction.operands)  # base operand + N registers
        if name == 'push':
            return 1 + len(instruction.operands)
        if name == 'pop':
            return (3 if 'pc' in instruction.op_str else 1) + len(instruction.operands)
        if name == 'bl':
            return 3
        if name in ('bx', 'blx', 'b'):
            return 2
        if name in ('beq', 'bne', 'bhs', 'bcs', 'blo', 'bcc', 'bmi', 'bpl',
                    'bvs', 'bvc', 'bhi', 'bls', 'bge', 'blt', 'bgt', 'ble'):
            return 1 if following == instruction.address + instruction.size else 2
        if name in ('dmb', 'dsb', 'isb', 'mrs', 'msr'):
            return 3
        if name in ('mov', 'movs', 'add', 'adds') and instruction.op_str.startswith('pc,'):
            return 2
        if name in ('adc', 'adcs', 'add', 'adds', 'adr', 'and', 'ands', 'asr',
                    'asrs', 'bic', 'bics', 'cmn', 'cmp', 'cpsid', 'cpsie',
                    'eor', 'eors', 'lsl', 'lsls', 'lsr', 'lsrs', 'mov', 'movs',
                    'mul', 'muls', 'mvn', 'mvns', 'neg', 'negs', 'nop', 'orr',
                    'orrs', 'rev', 'rev16', 'revsh', 'ror', 'rors', 'rsb',
                    'rsbs', 'sbc', 'sbcs', 'sev', 'sub', 'subs', 'sxtb',
                    'sxth', 'tst', 'uxtb', 'uxth', 'yield'):
            return 1
        raise RuntimeError(f'No M0+ price for {instruction.address:x}: {instruction.mnemonic} {instruction.op_str}')

    def on_code(self, uc, address, size, data):
        if self.prev is not None:
            instruction, mem = self.prev
            cost = self.price(instruction, address, mem)
            self.cycles += cost
            self.hotspots[instruction.address] += cost
            if instruction.mnemonic in ('push', 'pop') or instruction.op_str.startswith('sp,'):
                self.min_sp = min(self.min_sp, uc.reg_read(UC_ARM_REG_SP))
        self.prev = None
        if address == RETURN:
            uc.emu_stop()
            return
        if address in self.code_hooks:
            self.code_hooks[address](self)
        if address not in self.instructions:
            self.instructions[address] = next(self.cs.disasm(bytes(uc.mem_read(address, size)), address))
        instruction = self.instructions[address]
        self.counts[instruction.mnemonic] += 1
        if 0x10000000 <= address < 0x11000000:
            self.flash_instructions += 1
        self.prev = (instruction, self.memory_address(instruction))

    def call(self, name, args=()):
        self.prev = None
        self.cycles = 0
        self.counts.clear()
        self.hotspots.clear()
        self.flash_instructions = self.flash_data_reads = 0
        self.read_count = self.write_count = 0
        self.font_reads = self.vram_reads = 0
        self.min_sp = STACK
        self.uc.reg_write(UC_ARM_REG_SP, STACK)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN | 1)
        self.uc.reg_write(UC_ARM_REG_XPSR, 0x01000000)
        saved_registers = {}
        if name == 'hdmi_scanline_interrupt_handler':
            for index in range(4, 12):
                register = globals()['UC_ARM_REG_R' + str(index)]
                saved_registers[register] = 0xa5342100 + index * 0x010203
                self.uc.reg_write(register, saved_registers[register])
        for index, value in enumerate(args):
            self.uc.reg_write(globals()['UC_ARM_REG_R' + str(index)], value)
        self.uc.emu_start(self.symbols[name] | 1, RETURN + 2, count=1000000)
        if self.uc.reg_read(UC_ARM_REG_PC) != RETURN:
            raise RuntimeError('Function did not return (possible hardware wait): ' + name)
        for register, expected in saved_registers.items():
            actual = self.uc.reg_read(register)
            if actual != expected:
                raise AssertionError(f'IRQ clobbered callee-saved register {register}: {actual:#010x} != {expected:#010x}')
        if self.uc.reg_read(UC_ARM_REG_SP) != STACK:
            raise AssertionError('Function did not restore SP')
        result = {
            'ideal_m0plus_cycles': self.cycles,
            'executed_instructions': sum(self.counts.values()),
            'data_reads': self.read_count,
            'data_writes': self.write_count,
            'flash_instruction_fetches': self.flash_instructions,
            'flash_data_reads': self.flash_data_reads,
            'max_software_stack_bytes': STACK - self.min_sp,
            'font_data_reads': self.font_reads,
            'vram_data_reads': self.vram_reads,
        }
        if saved_registers:
            result['r4_r11_preserved'] = True
        return result

    def scanline(self, line, irq=False, buffer=0):
        text = self.uc.mem_read(self.symbols['text_mode'], 1)[0] != 0
        offset, stride = ((2624, 400) if self.transport == 'commands' else (2048, 800)) if text else (4096, 400)
        first = self.symbols['workspace'] + offset + buffer * stride
        self.active_line_range = (first, first + stride)
        self.check_scanline_writes = True
        try:
            if not irq:
                result = self.call('hdmi_prepare_line', (line, buffer))
            else:
                self.variable('dma_channel_control', 0)
                self.variable('pending_framebuffer', 0)
                self.variable('hdmi_current_scanline', (line - 2) % 525, 2)
                self.write(0x5000040c, 1)
                self.write(0x50000000, self.symbols['dma_buffer_addresses'] + 4 * buffer)
                result = self.call('hdmi_scanline_interrupt_handler')
            result['workspace_and_input_guards_passed'] = True
            return result
        finally:
            self.check_scanline_writes = False

    def check_text_line(self, line, buffer=0, transport='bytes'):
        """Compare the real linked renderer's byte output with a scalar oracle."""
        options = self.options
        columns, rows = options['columns'], options['rows']
        height = options['font_height']
        top = (480 - rows * height * (2 if height <= 8 else 1)) // 2
        blank = 254 if line in (490, 491) else 252
        expected = bytearray([blank + 1] * 96 + [blank] * 704)
        if line < 480:
            active = bytearray([0, 16] * 320)
            if top <= line < top + rows * height * (2 if height <= 8 else 1):
                y = line - top
                cell_height = height * (2 if height <= 8 else 1)
                row, glyph_y = divmod(y, cell_height)
                if height <= 8:
                    glyph_y //= 2
                start = 8192 - columns if options['wrap'] else 0
                row_start = (start + row * columns * 2) % 8192
                cursor_offset = (start + 30) % 8192
                frame = self.uc.mem_read(self.symbols['text_frame'], 1)[0]
                hidden = bool(frame & 16)
                vram = bytes(self.uc.mem_read(self.vram_address, 8192))
                font = bytes(self.uc.mem_read(self.font_address, self.font_size))
                for col in range(columns):
                    off = (row_start + col * 2) % 8192
                    char, attr = vram[off], vram[(off + 1) % 8192]
                    fg, bg = attr & 15, (attr >> 4) & (7 if options['blink'] else 15)
                    glyph = font[char * options['font_stride'] + glyph_y]
                    if options['blink'] and hidden and attr & 128:
                        glyph = 0
                    if options['cursor'] and not hidden and off == cursor_offset:
                        glyph = 255
                    width = 2 if columns == 40 else 1
                    for bit in range(8):
                        ink = bool(glyph & (128 >> bit if options['msb'] else 1 << bit))
                        for copy in range(width):
                            x = (col * 8 + bit) * width + copy
                            active[x] = (fg if ink else bg) | ((x & 1) * 16)
            expected[144:784] = active
        address = self.symbols['workspace'] + (2624 + buffer * 400 if transport == 'commands'
                                               else 2048 + buffer * 800)
        if transport == 'commands':
            commands = struct.unpack('<100I', self.uc.mem_read(address, 400))
            actual = bytearray()
            for command in commands:
                if command & 0x070000e0:
                    raise AssertionError(f'Unexpected command padding bits: {command:#010x}')
                foreground, background = command & 31, command >> 27
                for pixel in range(8):
                    pair = (command >> (8 + 2 * pixel)) & 3
                    color = foreground if pair & 1 else background
                    phase = pair >> 1
                    if color >= 16:
                        if color >= 20:
                            raise AssertionError(f'Invalid control color {color}')
                        actual.append(color - 16 + 252)
                    else:
                        actual.append(color + phase * 16)
            actual = bytes(actual)
        else:
            actual = bytes(self.uc.mem_read(address, 800))
        if actual != expected:
            mismatch = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
            raise AssertionError(f'line {line}, byte {mismatch}: actual {actual[mismatch]} != expected {expected[mismatch]}')
        return True

    def check_graphics_line(self, line, buffer=0):
        blank = 254 if line in (490, 491) else 252
        expected = bytearray([blank + 1] * 48 + [blank] * 352)
        if line < 480:
            source = self.uc.mem_read(FRAMEBUFFER + (line // 2) * 320, 320)
            expected[72:392] = bytes(value if value < 252 else value - 252 + 48 for value in source)
        actual = bytes(self.uc.mem_read(self.symbols['workspace'] + 4096 + buffer * 400, 400))
        if actual != expected:
            mismatch = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
            raise AssertionError(f'graphics line {line}, byte {mismatch}: actual {actual[mismatch]} != expected {expected[mismatch]}')
        return True

    def configure(self, columns=80, rows=30, msb=False, blink=False,
                  hidden=False, cursor=False, wrap=False, font_height=16,
                  font_stride=16, unaligned=False):
        self.options = dict(columns=columns, rows=rows, msb=msb, blink=blink,
                            hidden=hidden, cursor=cursor, wrap=wrap,
                            font_height=font_height, font_stride=font_stride,
                            unaligned=unaligned)
        self.variable('text_mode', 1, 1)
        self.variable('text_columns', columns, 1)
        self.variable('text_rows', rows, 1)
        self.variable('text_frame', 16 if hidden else 0, 1)
        self.variable('text_lock', SIO + 0x100)
        self.variable('hdmi_lock', SIO + 0x104, required=False)
        self.variable('graphics_framebuffer', FRAMEBUFFER, required=False)
        # Deterministic glyphs/attributes span all colours and both blink bits.
        self.font_address = FONT + int(unaligned)
        self.vram_address = VRAM + int(unaligned)
        self.font_size = font_stride * 256
        self.uc.mem_write(self.font_address, bytes((i * 73 + i // font_stride * 13 + 0x59) & 255 for i in range(self.font_size)))
        vram = bytearray(8192)
        for cell in range(len(vram) // 2):
            vram[cell * 2] = (cell * 43 + 19) & 255
            vram[cell * 2 + 1] = (cell * 37 + 7) & 255
        self.uc.mem_write(self.vram_address, bytes(vram))
        self.uc.mem_write(FRAMEBUFFER, bytes(i & 255 for i in range(320 * 240)))
        start = 8192 - columns if wrap else 0
        cursor_offset = (start + 30) % 8192
        config = struct.pack('<6I2H7Bx', self.vram_address, self.font_address,
                             len(vram), self.font_size,
                             start, cursor_offset, columns * 2, font_stride,
                             font_height, 0, font_height - 1,
                             msb, blink, cursor, wrap)
        self.uc.mem_write(self.symbols['text_config'], config)
        self.configured = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--json', type=Path)
    parser.add_argument('--dirty-divider', action='store_true')
    parser.add_argument('--irq', action='store_true', help='Include software IRQ handler (not hardware exception entry/exit)')
    parser.add_argument('--verify', action='store_true', help='Verify full800-byte text output with an independent scalar oracle')
    parser.add_argument('--transport', choices=('bytes', 'commands'), default='bytes')
    parser.add_argument('--initialize', action='store_true', help='Initialize text workspace using the linked hdmi_change_workspace_mode function')
    args = parser.parse_args()
    records = {}
    workspace_image = None
    if args.initialize:
        trace = Trace(args.elf)
        trace.call('hdmi_change_workspace_mode', (1,))
        workspace_image = bytes(trace.uc.mem_read(trace.symbols['workspace'], 4896))
    def new_trace():
        trace = Trace(args.elf, args.dirty_divider)
        trace.transport = args.transport
        if workspace_image is not None:
            trace.uc.mem_write(trace.symbols['workspace'], workspace_image)
        return trace
    for columns, rows in ((80, 30), (80, 25), (40, 25)):
        for variant, options in (
            ('normal', {}),
            ('msb', {'msb': True}),
            ('blink_on', {'blink': True}),
            ('blink_hidden', {'blink': True, 'hidden': True}),
            ('cursor', {'cursor': True}),
            ('wrap', {'wrap': True}),
            ('all', {'msb': True, 'blink': True, 'cursor': True, 'wrap': True}),
            ('font8', {'font_height': 8, 'font_stride': 8}),
            ('font14', {'font_height': 14, 'font_stride': 14}),
            ('font_stride32', {'font_stride': 32}),
            ('unaligned', {'unaligned': True}),
        ):
            trace = new_trace()
            trace.configure(columns=columns, rows=rows, **options)
            height = options.get('font_height', 16)
            line = (480 - rows * height * (2 if height <= 8 else 1)) // 2
            result = trace.scanline(line, args.irq)
            if args.verify:
                result['output_verified'] = trace.check_text_line(line, transport=args.transport)
            records[f'{columns}x{rows}/{variant}'] = result
    for name, line in (('text/vblank', 480), ('text/vsync', 490), ('text/border', 0)):
        trace = new_trace()
        trace.configure(rows=25)
        records[name] = trace.scanline(line, args.irq)
        if args.verify:
            records[name]['output_verified'] = trace.check_text_line(line, transport=args.transport)
    for name, line in (('graphics/even', 0), ('graphics/odd', 1), ('graphics/vblank', 480)):
        trace = new_trace()
        trace.configure()
        trace.variable('text_mode', 0, 1)
        if line == 1:
            trace.scanline(0, args.irq, buffer=1)
        records[name] = trace.scanline(line, args.irq)
        if args.verify:
            records[name]['output_verified'] = trace.check_graphics_line(line)
    if args.transport == 'commands' and args.initialize:
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
                    result = trace.scanline(40, args.irq, buffer=repeat)
                    if args.verify:
                        result['output_verified'] = trace.check_text_line(40, buffer=repeat,
                                                                         transport='commands')
                    actual_state = int.from_bytes(trace.uc.mem_read(trace.symbols['workspace'] + 3424, 4), 'little')
                    if actual_state != state:
                        raise AssertionError(f'Attribute lookup state {actual_state}, expected {state}')
                    result['lookup_state'] = actual_state
                    records[f'transition/{columns}cols/step{step}-state{state}/' +
                            ('first' if repeat == 0 else 'steady')] = result
    result = {
        'elf': str(args.elf), 'elf_sha256': trace.elf_sha256,
        'dirty_divider': args.dirty_divider,
        'transport': args.transport, 'workspace_initialized': args.initialize,
        'scope': ('Software IRQ handler including hdmi_prepare_line; no hardware exception entry/exit' if args.irq else
                  'hdmi_prepare_line only; no hardware IRQ entry/exit or IRQ dispatcher'),
        'assumptions': 'M0+ ideal SRAM/AHB=2 cycles, SIO=1; no contention, flash-cache cost or peripheral stalls',
        'records': records,
    }
    print(json.dumps(result, indent=2))
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
