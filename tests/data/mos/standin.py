#!/usr/bin/env python3
"""Stand-in programs for the mos family's smaller machines (stage M).

    standin.py OUTDIR        writes the images below into OUTDIR
    standin.py --header H    writes them as C arrays to H

The assembler is tests/data/atari/standin.py's.

  generic.rom   4K at $F000 for the generic 6502 (-M mos -rom 0xF000:...):
                the timer at $DF00 interrupting every 4,000 cycles (auto
                reload), each IRQ printing a digit on the serial port
                ($DF10) and every 8th a letter on the debug port ($F001);
                the main loop sums RAM into $0200 and walks a pattern
                through $0300-$03FF.
  apple1.rom    256 bytes at $FF00 for the Apple I: sets the PIA up as the
                Woz monitor does, prints a banner, then echoes keys and
                prints a counter character every 512 passes of its loop.
  5clown-*      5 Clown's regions (prg encrypted as the board's is, audiocpu,
                gfxbanks, proms; no OKI ROM): the main CPU's NMI (the CRTC's
                vsync) counts frames in NVRAM, fills video and colour RAM,
                writes the sound latch and pulses PIA1 port A, which NMIs the
                sound CPU; that stores the latch in its RAM.  Both idle
                between NMIs.
  magicfly-*    Magic Fly's regions (maincpu at $C000, gfx0-2): the NMI fills
                video and colour RAM and reads the input port; the idle loop
                toggles the DAC bit (no RAM).
  um6578.rom    32K for the UM6578 (maincpu): palette, VRAM through $2006/7,
                an APU tone, PPU NMI on; the NMI scrolls and moves the tone.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "atari"))
from standin import Asm, c_array   # noqa: E402


def build_generic():
    a = Asm(0xF000)
    a.label('reset')
    a('SEI'); a('CLD'); a('LDX', 0xFF, 'imm'); a('TXS')
    a('LDA', 0xA0, 'imm'); a('STA', 0xDF00, 'abs')      # reload 4000 = $0FA0
    a('LDA', 0x0F, 'imm'); a('STA', 0xDF01, 'abs')
    a('LDA', 0x07, 'imm'); a('STA', 0xDF02, 'abs')      # enable, IRQ, auto-reload
    a('LDA', 0, 'imm'); a('STA', 0x10); a('STA', 0x11); a('STA', 0x12)
    a('CLI')
    a.label('main')
    # sum $0000-$00FF into $0200
    a('LDA', 0, 'imm'); a('TAX'); a('CLC')
    a.label('sum'); a('ADC', 0x00, 'zpx'); a('INX'); a('BNE', 'sum')
    a('STA', 0x0200, 'abs')
    # a pattern through $0300-$03FF
    a('INC', 0x10); a('LDX', 0x10); a('LDA', 0x11); a('EOR', 0x10); a('STA', 0x0300, 'absx')
    a('JMP', 'main')

    a.label('irq')
    a('PHA'); a('TXA'); a('PHA')
    a('INC', 0x11)
    a('LDA', 0x80, 'imm'); a('ORA', 0x07, 'imm'); a('STA', 0xDF02, 'abs')   # clear pending
    a('LDA', 0x11); a('AND', 0x07, 'imm'); a('ORA', 0x30, 'imm'); a('STA', 0xDF10, 'abs')
    a('LDA', 0x11); a('AND', 0x07, 'imm'); a('BNE', 'irq1')
    a('INC', 0x12); a('LDA', 0x12); a('AND', 0x0F, 'imm'); a('ORA', 0x40, 'imm')
    a('STA', 0xF001, 'abs')
    a.label('irq1')
    a('PLA'); a('TAX'); a('PLA'); a('RTI')
    a.label('nmi'); a('RTI')
    a.at(0xFFFA)
    a.word('nmi'); a.word('reset'); a.word('irq')
    rom = a.done()
    assert len(rom) == 0x1000
    return rom


KBD, KBDCR, DSP, DSPCR = 0xD010, 0xD011, 0xD012, 0xD013


def build_apple1():
    a = Asm(0xFF00)
    a.label('reset')
    a('CLD'); a('CLI'); a('LDX', 0xFF, 'imm'); a('TXS')
    a('LDY', 0x7F, 'imm'); a('STY', DSP, 'abs')        # DDRB: PB0-6 out
    a('LDA', 0xA7, 'imm'); a('STA', KBDCR, 'abs'); a('STA', DSPCR, 'abs')
    a('LDX', 0, 'imm')
    a.label('ban')
    a('LDA', 'msg', 'absx'); a('BEQ', 'loop'); a('JSR', 'echo'); a('INX'); a('BNE', 'ban')
    a.label('loop')
    a('LDA', KBDCR, 'abs'); a('BPL', 'nokey')
    a('LDA', KBD, 'abs'); a('JSR', 'echo')
    a.label('nokey')
    a('INC', 0x20); a('BNE', 'loop')
    a('INC', 0x21); a('LDA', 0x21); a('AND', 0x01, 'imm'); a('BNE', 'loop')
    a('LDA', 0x21); a('LSR'); a('AND', 0x1F, 'imm'); a('ORA', 0xC0, 'imm'); a('JSR', 'echo')
    a('JMP', 'loop')
    a.label('echo')
    a('BIT', DSP, 'abs'); a('BMI', 'echo'); a('STA', DSP, 'abs'); a('RTS')
    a.label('msg')
    for ch in b"\rGEMU STAND-IN MONITOR\r":
        a.byte(ch | 0x80)
    a.byte(0)
    a.label('nmi'); a('RTI')
    a.at(0xFFFA)
    a.word('nmi'); a.word('reset'); a.word('nmi')
    rom = a.done()
    assert len(rom) == 0x100
    return rom


def build_5clown():
    a = Asm(0xE000)                                   # main CPU, $E000-$FFFF
    a.label('reset')
    a('SEI'); a('CLD'); a('LDX', 0xFF, 'imm'); a('TXS')
    a('LDA', 0, 'imm'); a('STA', 0x0849, 'abs')       # PIA1 CRA: DDRA
    a('LDA', 0xFF, 'imm'); a('STA', 0x0848, 'abs')
    a('LDA', 0x04, 'imm'); a('STA', 0x0849, 'abs')    # ORA
    a('LDA', 0, 'imm'); a('STA', 0x084B, 'abs')       # CRB: DDRB
    a('LDA', 0xFF, 'imm'); a('STA', 0x084A, 'abs')
    a('LDA', 0x04, 'imm'); a('STA', 0x084B, 'abs')
    a('LDA', 0xEF, 'imm'); a('STA', 0x084A, 'abs')    # input group 0 (inverted: $10)
    a('LDA', 0x04, 'imm'); a('STA', 0x0845, 'abs')    # PIA0 CRA: ORA
    a.label('idle'); a('JMP', 'idle')
    a.label('nmi')
    a('PHA'); a('TXA'); a('PHA')
    a('INC', 0x00)
    a('LDX', 0, 'imm')
    a.label('fill')
    a('TXA'); a('CLC'); a('ADC', 0x00); a('STA', 0x1000, 'absx'); a('EOR', 0x55, 'imm')
    a('STA', 0x1800, 'absx'); a('INX'); a('CPX', 64, 'imm'); a('BNE', 'fill')
    a('LDA', 0x00); a('STA', 0xD800, 'abs')           # the sound latch
    a('LDA', 0x07, 'imm'); a('STA', 0x0848, 'abs')    # PIA1 PA: the sound CPU's NMI
    a('LDA', 0x00, 'imm'); a('STA', 0x0848, 'abs')
    a('LDA', 0x0844, 'abs'); a('STA', 0x10)           # inputs
    a('LDA', 0xC400, 'abs'); a('STA', 0x11)           # SW1
    a('LDA', 0x0A, 'imm'); a('STA', 0x0800, 'abs'); a('LDA', 0x00); a('STA', 0x0801, 'abs')
    a('PLA'); a('TAX'); a('PLA'); a('RTI')
    a.label('irq'); a('RTI')
    a.at(0xFFFA); a.word('nmi'); a.word('reset'); a.word('irq')
    top = a.done()
    prg = bytes(0x6000) + top                          # $2000-$7FFF unused
    prg = bytes(b ^ 0x20 for b in prg)                 # as the board stores it

    s = Asm(0xE000)                                   # sound CPU
    s.label('reset')
    s('SEI'); s('CLD'); s('LDX', 0xFF, 'imm'); s('TXS'); s('LDA', 0, 'imm'); s('STA', 0x00)
    s.label('idle'); s('JMP', 'idle')
    s.label('nmi')
    s('PHA'); s('TXA'); s('PHA')
    s('LDX', 0x00); s('LDA', 0x0E06, 'abs'); s('STA', 0x0200, 'absx'); s('INX'); s('STX', 0x00)
    s('LDA', 0xC0, 'imm'); s('STA', 0x0A02, 'abs'); s('LDA', 0x00); s('STA', 0x0800, 'abs')
    s('PLA'); s('TAX'); s('PLA'); s('RTI')
    s.label('irq'); s('RTI')
    s.at(0xFFFA); s.word('nmi'); s.word('reset'); s.word('irq')
    audio = s.done()

    gfx = bytearray(((i * 13) ^ (i >> 3)) & 0xFF for i in range(0x8000))
    for lo, hi, k in ((0x4000, 0x5000, 0x22), (0x6000, 0x7000, 0x3F), (0x7000, 0x8000, 0x22)):
        for i in range(lo, hi):
            gfx[i] ^= k                                # encrypted as the board's
    proms = bytes((i * 7 + 3) & 0x0F for i in range(256))
    return {'5clown-prg.rom': prg, '5clown-audio.rom': audio, '5clown-gfx.rom': bytes(gfx),
            '5clown-proms.rom': proms}


def build_magicfly():
    a = Asm(0xC000)
    a.label('reset')
    a('SEI'); a('CLD'); a('LDX', 0xFF, 'imm'); a('TXS'); a('LDX', 0, 'imm')
    a.label('idle')                                    # the DAC: no RAM touched
    a('TXA'); a('EOR', 0x80, 'imm'); a('TAX'); a('STA', 0x3000, 'abs')
    a('LDY', 20, 'imm')
    a.label('dly'); a('DEY'); a('BNE', 'dly')
    a('JMP', 'idle')
    a.label('nmi')
    a('PHA'); a('TXA'); a('PHA')
    a('INC', 0x00)
    a('LDX', 0, 'imm')
    a.label('fill')
    a('TXA'); a('CLC'); a('ADC', 0x00); a('STA', 0x1000, 'absx'); a('EOR', 0x33, 'imm')
    a('STA', 0x1800, 'absx'); a('INX'); a('CPX', 64, 'imm'); a('BNE', 'fill')
    a('LDA', 0x2800, 'abs'); a('STA', 0x11)
    a('LDA', 0x0A, 'imm'); a('STA', 0x0800, 'abs'); a('LDA', 0x00); a('STA', 0x0801, 'abs')
    a('PLA'); a('TAX'); a('PLA'); a('RTI')
    a.label('irq'); a('RTI')
    a.at(0xFFFA); a.word('nmi'); a.word('reset'); a.word('irq')
    prg = a.done()
    assert len(prg) == 0x4000
    gfx = [bytes(((i * (5 + k)) ^ (i >> (2 + k))) & 0xFF for i in range(0x2000)) for k in range(3)]
    return {'magicfly-prg.rom': prg, 'magicfly-gfx0.rom': gfx[0], 'magicfly-gfx1.rom': gfx[1],
            'magicfly-gfx2.rom': gfx[2]}


def build_um6578():
    a = Asm(0x8000)
    a.label('reset')
    a('SEI'); a('CLD'); a('LDX', 0xFF, 'imm'); a('TXS')
    a('LDX', 0, 'imm')
    a.label('pal'); a('TXA'); a('ASL'); a('STA', 0x2040, 'absx'); a('INX'); a('CPX', 0x40, 'imm')
    a('BNE', 'pal')
    a('LDA', 0x00, 'imm'); a('STA', 0x2006, 'abs'); a('STA', 0x2006, 'abs')
    a('LDY', 0x20, 'imm')                               # 8 KB of VRAM: patterns, names
    a.label('vo'); a('LDX', 0, 'imm')
    a.label('vi'); a('TXA'); a('EOR', 0x20); a('STA', 0x2007, 'abs'); a('INX'); a('BNE', 'vi')
    a('INC', 0x20); a('DEY'); a('BNE', 'vo')
    a('LDA', 0x01, 'imm'); a('STA', 0x4015, 'abs')      # pulse 1
    a('LDA', 0xBF, 'imm'); a('STA', 0x4000, 'abs')
    a('LDA', 0x80, 'imm'); a('STA', 0x4002, 'abs')
    a('LDA', 0x01, 'imm'); a('STA', 0x4003, 'abs')
    a('LDA', 0x1E, 'imm'); a('STA', 0x2001, 'abs')
    a('LDA', 0x80, 'imm'); a('STA', 0x2000, 'abs')      # NMI on
    a.label('idle'); a('JMP', 'idle')
    a.label('nmi')
    a('PHA')
    a('INC', 0x00); a('LDA', 0x00); a('STA', 0x2005, 'abs'); a('STA', 0x2005, 'abs')
    a('STA', 0x4002, 'abs'); a('LDA', 0x2002, 'abs')
    a('PLA'); a('RTI')
    a.label('irq'); a('RTI')
    a.at(0xFFFA); a.word('nmi'); a.word('reset'); a.word('irq')
    rom = a.done()
    assert len(rom) == 0x8000
    return {'um6578.rom': rom}


def main():
    files = {'generic.rom': build_generic(), 'apple1.rom': build_apple1()}
    files.update(build_5clown())
    files.update(build_magicfly())
    files.update(build_um6578())
    if len(sys.argv) == 3 and sys.argv[1] == '--header':
        out = ["/* Generated by tests/data/mos/standin.py --header; do not edit. */\n"]
        for name, data in files.items():
            out.append(c_array('mos_standin_' + name.replace('.rom', '').replace('-', '_'), data))
        Path(sys.argv[2]).write_text("\n".join(out))
        return
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    d = Path(sys.argv[1])
    d.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (d / name).write_bytes(data)


if __name__ == '__main__':
    main()
