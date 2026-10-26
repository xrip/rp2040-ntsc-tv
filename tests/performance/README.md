# Linked ARM instruction trace for the HDMI text renderer

This benchmark executes the actual **linked RP2040 Cortex-M0+ machine code**
in Unicorn, records every executed instruction and prices that trace using
Arm's M0+ instruction timing table. It also compares every output pixel with
an independent scalar text/glyph/blink/cursor reference.

It is **not a hardware measurement or cycle-accurate RP2040 simulator**.
It deliberately excludes SRAM arbitration with DMA/the other core, PIO/DMA
execution, cache misses, higher-priority IRQs and lock contention. The cycle
number therefore estimates ideal core work and is useful for a like-for-like
comparison of two implementations. The frame percentage is for one core at
378 MHz, 60 Hz, 525 lines. Do not apply M0+ timing numbers to RP2350/M33.

## Run

Use Python 3.10+ and an unstripped firmware ELF with its local symbols intact.
The benchmark does not communicate with a board or change firmware/source.
If Unicorn crashes while mapping memory on Windows, run these trace tools
under WSL/Linux. The host C and PIO tests can still run on Windows.

```sh
python -m venv .venv
.venv/bin/pip install -r requirements.txt

# Previous byte-index text transport.
.venv/bin/python trace_hdmi_cycles.py old.elf --irq --verify --json old-cases.json
.venv/bin/python trace_hdmi_frame.py old.elf --columns 80 --rows 30 \
  --verify --json old-frame.json

# New command transport. Initialize the real linked LUT/palette routine,
# then exclude its startup work from the timed IRQ calls.
.venv/bin/python trace_hdmi_cycles.py fast.elf --irq --verify \
  --transport commands --initialize --json fast-cases.json
.venv/bin/python trace_hdmi_frame.py fast.elf --columns 80 --rows 30 \
  --transport commands --initialize --verify --json fast-frame.json
```

Use `--rows 25` and optionally `--columns 40` for the other geometries.
`trace_hdmi_frame.py --variant all` adds a wrapped display start, MSB font,
attribute blinking and a cursor; `--variant hidden` chooses the hidden blink
phase. `--dirty-divider` starts each sampled IRQ with the SIO divider in use,
exercising SDK save/restore. Frame tracing checks every physical pixel of all
525 lines, including margins and sync/blank control indices.

## Model

The pricing follows Arm DDI 0484B, *Cortex-M0+ Technical Reference Manual*,
table 3-1: <https://documentation-service.arm.com/static/60411742ee937942ba3016f2>.
Ordinary ALU instructions cost one cycle; SRAM/AHB loads and stores two;
SIO single-cycle-port accesses one. A taken conditional branch costs two,
an untaken one costs one, BL costs three, BX costs two. Multi-register
transfers, MRS/MSR, barriers and return POPs use their table entries.
The harness fails on an unpriced instruction or an unaligned halfword/word
access instead of silently assuming a price. Every software IRQ begins with
distinct values in r4..r11 and must preserve them and SP on return. The peak
software stack depth is recorded separately from the hardware exception frame.

The model maps a larger virtual SRAM region solely to keep synthetic text,
font and stack allocations separate from the loaded ELF. Those addresses
are test fixtures, not board memory-layout recommendations. Font and VRAM
are filled with deterministic varied bytes; text descriptors, locks and DMA
status are initialized explicitly. SIO divider arithmetic and uncontended
spinlock reads are emulated, but competing users are not. Full IRQ tracing
executes the IRQ's software prologue/epilogue and renderer, while hardware
exception entry/return and the SDK vector dispatcher are outside the trace.

Flash instruction fetches and flash data reads are counted separately.
For all hot scanline paths these counters should remain zero; startup LUT
initialization may use flash and is not part of those counters. Actual flash
font use would add XIP reads and needs separate hardware evaluation.

The command oracle decodes 100 32-bit commands into 800 physical pixels.
Each command has foreground bits 0..4, background bits 27..31 and eight
interleaved glyph/phase pairs in bits 8..23. It validates command padding,
palette control indices, phase alternation and scalar glyph colors. PIO
instruction behavior/timing is tested separately by `test_hdmi_pio.py`.

Memory hooks reject font/VRAM reads outside the configured caller allocation,
scanout writes to those allocations, writes into the other DMA line buffer,
and writes into lookup/font/palette storage other than the permitted attribute
table update routine. The observed font read count is one per cell per glyph
line. An additional persistent-workspace test drives blink lookup states
0→1→3→0→3→1→0 in both widths, verifies first and steady-state calls, and checks
all background colors are restored after disabling blinking. Graphics cases
verify the retained indexed scanline output and odd-line duplication as well.

An oscilloscope/logic-analyzer measurement of the optional profiling GPIO
under the real emulator remains the final check of actual IRQ occupation
and worst-line duration, including DMA and other-core contention.
