#!/usr/bin/env python3
"""A stand-in Atari 400 OS, an 8K cartridge and an ATR disk, and two 2600
test cartridges, for testing.

    standin.py OUTDIR        writes os.rom ($D800-$FFFF), cart.rom, disk.atr,
                             test4k.a26 and testf8.a26 (build_2600())
    standin.py --header H    writes them as C arrays to H (tests/atari_core.c)

The real OS ROMs (co12399b, co12499b, co14599b) aren't distributed with GEMU,
so the machine's checks run this instead.  It uses what the machine models:

  - a display list at $1000: blank lines, 12 lines of mode 2 text from $2000
    (LMS), 2 of mode 6, 16 of mode $D bitmap from $2400 (LMS), JVB;
  - a character set of its own at $E000 (CHBASE $E0);
  - the VBI NMI (NMIEN $40): counts frames at $80/$81 and cycles COLBK;
  - POKEY IRQs: keyboard (stores KBCODE at $84) and timer 1;
  - WSYNC in the main loop, VCOUNT, the joystick port (PIA), TRIG0, CONSOL;
  - the cartridge: if $BFFC is 0 and the init address at $BFFE is in
    $A000-$FFFF (48K of RAM reads 0 there too), JSR to it;
  - Axlon banks: writes $CFFF = 1 and 2 with a marker each at $4000, then
    reads banks 0, 1, 2 back onto the screen;
  - an SIO read of sector 1 through the trap at $E459, onto the screen.

Each frame the main loop puts the frame count, VCOUNT, the inputs and a
checksum of zero page on the screen and draws into the bitmap.  When the
frame count reaches the 16-bit target at $0600/$0601 (0 = never; poke it
from the monitor) it calls CHECK ($F000), an RTS, so a breakpoint there
stops the machine at a known frame.
"""
import sys
from pathlib import Path

OPS = {
    # name: {mode: opcode}
    'LDA': dict(imm=0xA9, zp=0xA5, abs=0xAD, absx=0xBD, absy=0xB9, indy=0xB1, zpx=0xB5),
    'LDX': dict(imm=0xA2, zp=0xA6, abs=0xAE, absy=0xBE),
    'LDY': dict(imm=0xA0, zp=0xA4, abs=0xAC, absx=0xBC),
    'STA': dict(zp=0x85, abs=0x8D, absx=0x9D, absy=0x99, indy=0x91, zpx=0x95),
    'STX': dict(zp=0x86, abs=0x8E),
    'STY': dict(zp=0x84, abs=0x8C),
    'INC': dict(zp=0xE6, abs=0xEE),
    'DEC': dict(zp=0xC6, abs=0xCE),
    'CMP': dict(imm=0xC9, zp=0xC5, abs=0xCD),
    'CPX': dict(imm=0xE0), 'CPY': dict(imm=0xC0),
    'AND': dict(imm=0x29, zp=0x25), 'ORA': dict(imm=0x09, zp=0x05, abs=0x0D),
    'EOR': dict(imm=0x49, zp=0x45, absx=0x5D),
    'ADC': dict(imm=0x69, zp=0x65, zpx=0x75), 'SBC': dict(imm=0xE9),
    'BIT': dict(abs=0x2C),
    'JMP': dict(abs=0x4C, ind=0x6C), 'JSR': dict(abs=0x20),
    'BNE': dict(rel=0xD0), 'BEQ': dict(rel=0xF0), 'BCC': dict(rel=0x90), 'BCS': dict(rel=0xB0),
    'BPL': dict(rel=0x10), 'BMI': dict(rel=0x30),
}
IMPLIED = dict(RTS=0x60, RTI=0x40, PHA=0x48, PLA=0x68, SEI=0x78, CLI=0x58, CLD=0xD8, CLC=0x18,
               SEC=0x38, TXS=0x9A, TAX=0xAA, TAY=0xA8, TXA=0x8A, TYA=0x98, INX=0xE8, INY=0xC8,
               DEX=0xCA, DEY=0x88, LSR=0x4A, ASL=0x0A, NOP=0xEA)


class Asm:
    def __init__(self, org):
        self.org, self.code, self.labels, self.fix = org, bytearray(), {}, []

    @property
    def pc(self):
        return self.org + len(self.code)

    def label(self, name):
        self.labels[name] = self.pc

    def at(self, addr):
        """Pad with $FF up to addr."""
        assert addr >= self.pc, hex(addr)
        self.code += b'\xff' * (addr - self.pc)

    def byte(self, *b):
        self.code += bytes(b)

    def word(self, v):
        if isinstance(v, str):
            self.fix.append((len(self.code), v, 'abs'))
            v = 0
        self.code += bytes((v & 0xFF, v >> 8))

    def __call__(self, op, arg=None, mode=None):
        if op in IMPLIED:
            self.byte(IMPLIED[op])
            return
        modes = OPS[op]
        if mode is None:
            if 'rel' in modes:
                mode = 'rel'
            elif isinstance(arg, str) or arg > 0xFF:
                mode = 'abs'
            else:
                mode = 'zp'
        self.byte(modes[mode])
        if mode == 'rel':
            self.fix.append((len(self.code), arg, 'rel'))
            self.byte(0)
        elif mode in ('abs', 'absx', 'absy', 'ind'):
            self.word(arg)
        else:
            self.byte(arg)

    def done(self):
        for off, name, kind in self.fix:
            v = self.labels[name]
            if kind == 'abs':
                self.code[off:off + 2] = bytes((v & 0xFF, v >> 8))
            else:
                d = v - (self.org + off + 1)
                assert -128 <= d < 128, (name, d)
                self.code[off] = d & 0xFF
        return bytes(self.code)


# Hardware registers
COLPF1, COLPF2, COLBK, TRIG0, CONSOL = 0xD017, 0xD018, 0xD01A, 0xD010, 0xD01F
AUDF1, AUDCTL, STIMER, KBCODE, IRQEN, SKCTL = 0xD200, 0xD208, 0xD209, 0xD209, 0xD20E, 0xD20F
PORTA = 0xD300
DMACTL, DLISTL, DLISTH, WSYNC, VCOUNT, CHBASE, NMIEN, NMIRES = \
    0xD400, 0xD402, 0xD403, 0xD40A, 0xD40B, 0xD409, 0xD40E, 0xD40F

# RAM
FRAME, LAST, IRQS, KEY, TMR, PTR, TARGET = 0x80, 0x82, 0x83, 0x84, 0x85, 0x86, 0x0600
DL, SCREEN, BITMAP, SIOBUF = 0x1000, 0x2000, 0x2400, 0x3000


def build_os():
    a = Asm(0xD800)
    a.at(0xE000)
    # character set at $E000: 128 characters, each a pattern from its code
    for ch in range(128):
        for row in range(8):
            v = ((ch * 37 + row * 11) ^ (ch << row)) & 0xFF
            if ch == 0:
                v = 0
            a.byte(v)
    # the SIO vector: without a disk the trap doesn't fire, so "timeout"
    a.at(0xE459)
    a('LDY', 0x8A, 'imm')
    a('RTS')

    a.at(0xE800)
    a.label('reset')
    a('SEI'); a('CLD'); a('LDX', 0xFF, 'imm'); a('TXS')
    # clear zero page and pages 2-3 (DCB) and the screen
    a('LDA', 0, 'imm'); a('TAX')
    a.label('clr')
    a('STA', 0x00, 'zpx'); a('STA', 0x0200, 'absx'); a('STA', 0x0300, 'absx')
    for p in range(SCREEN, SCREEN + 0x800, 0x100):
        a('STA', p, 'absx')
    a('INX'); a('BNE', 'clr')
    # display list
    dl = [0x70, 0x70, 0x70, 0x42, SCREEN & 0xFF, SCREEN >> 8] + [0x02] * 11 + [0x06, 0x06] + \
         [0x4D, BITMAP & 0xFF, BITMAP >> 8] + [0x0D] * 15 + [0x41, DL & 0xFF, DL >> 8]
    for i, b in enumerate(dl):
        a('LDA', b, 'imm'); a('STA', DL + i, 'abs')
    # a line of text: screen codes 1..40
    a('LDX', 0, 'imm')
    a.label('txt')
    a('TXA'); a('ADC', 1, 'imm'); a('STA', SCREEN, 'absx'); a('INX'); a('CPX', 40, 'imm'); a('BNE', 'txt')
    # colours, character set, DMA, NMI, POKEY
    a('LDA', 0x0E, 'imm'); a('STA', COLPF1, 'abs')
    a('LDA', 0x94, 'imm'); a('STA', COLPF2, 'abs')
    a('LDA', 0x00, 'imm'); a('STA', COLBK, 'abs')
    a('LDA', 0xE0, 'imm'); a('STA', CHBASE, 'abs')
    a('LDA', DL & 0xFF, 'imm'); a('STA', DLISTL, 'abs')
    a('LDA', DL >> 8, 'imm'); a('STA', DLISTH, 'abs')
    a('LDA', 0x22, 'imm'); a('STA', DMACTL, 'abs')
    a('LDA', 0x40, 'imm'); a('STA', NMIEN, 'abs')
    a('LDA', 0x03, 'imm'); a('STA', SKCTL, 'abs')
    a('LDA', 0x00, 'imm'); a('STA', AUDCTL, 'abs')
    a('LDA', 0x80, 'imm'); a('STA', AUDF1, 'abs')
    a('LDA', 0xC1, 'imm'); a('STA', IRQEN, 'abs')   # BREAK, key, timer 1
    a('STA', STIMER, 'abs')
    a('CLI')
    # the cartridge, if $BFFC is 0
    a('LDA', 0xBFFC, 'abs'); a('BNE', 'nocart')
    a('LDA', 0xBFFF, 'abs'); a('CMP', 0xA0, 'imm'); a('BCC', 'nocart')
    a('JSR', 'cartinit')
    a.label('nocart')
    # Axlon: banks 1 and 2 get markers at $4000, then 0, 1, 2 read back
    a('LDA', 0x11, 'imm'); a('STA', 0x4000, 'abs')
    for bank, mark in ((1, 0xA5), (2, 0x5A)):
        a('LDA', bank, 'imm'); a('STA', 0xCFFF, 'abs')
        a('LDA', mark, 'imm'); a('STA', 0x4000, 'abs')
    for bank in (0, 1, 2):
        a('LDA', bank, 'imm'); a('STA', 0xCFFF, 'abs')
        a('LDA', 0x4000, 'abs'); a('STA', SCREEN + 80 + bank, 'abs')
    a('LDA', 0, 'imm'); a('STA', 0xCFFF, 'abs')
    # SIO: read sector 1 into SIOBUF, status to the screen, 40 bytes after it
    for addr, v in ((0x300, 0x31), (0x301, 1), (0x302, 0x52), (0x304, SIOBUF & 0xFF),
                    (0x305, SIOBUF >> 8), (0x308, 0x80), (0x309, 0), (0x30A, 1), (0x30B, 0)):
        a('LDA', v, 'imm'); a('STA', addr, 'abs')
    a('JSR', 0xE459)
    a('STY', SCREEN + 84, 'abs')
    a('LDX', 0, 'imm')
    a.label('siocp')
    a('LDA', SIOBUF, 'absx'); a('STA', SCREEN + 120, 'absx'); a('INX'); a('CPX', 40, 'imm')
    a('BNE', 'siocp')

    # main loop: once a frame
    a.label('main')
    a('LDA', FRAME); a.label('wait'); a('CMP', FRAME); a('BEQ', 'wait')
    # WSYNC a few times, then VCOUNT
    a('STA', WSYNC, 'abs'); a('STA', WSYNC, 'abs'); a('STA', WSYNC, 'abs')
    a('LDA', VCOUNT, 'abs'); a('STA', SCREEN + 160, 'abs')
    # frame count, inputs, IRQs seen, last key, timer IRQs
    a('LDA', FRAME); a('STA', SCREEN + 161, 'abs')
    a('LDA', FRAME + 1); a('STA', SCREEN + 162, 'abs')
    a('LDA', PORTA, 'abs'); a('STA', SCREEN + 163, 'abs')
    a('LDA', TRIG0, 'abs'); a('STA', SCREEN + 164, 'abs')
    a('LDA', CONSOL, 'abs'); a('STA', SCREEN + 165, 'abs')
    a('LDA', IRQS); a('STA', SCREEN + 166, 'abs')
    a('LDA', KEY); a('STA', SCREEN + 167, 'abs')
    a('LDA', TMR); a('STA', SCREEN + 168, 'abs')
    # checksum of zero page $80-$8F
    a('LDA', 0, 'imm'); a('LDX', 0, 'imm'); a('CLC')
    a.label('sum'); a('ADC', 0x80, 'zpx'); a('INX'); a('CPX', 16, 'imm'); a('BNE', 'sum')
    a('STA', SCREEN + 169, 'abs')
    # the bitmap: the frame count at a moving offset
    a('LDA', FRAME); a('AND', 0xFF, 'imm'); a('TAY')
    a('LDA', BITMAP & 0xFF, 'imm'); a('STA', PTR); a('LDA', BITMAP >> 8, 'imm'); a('STA', PTR + 1)
    a('LDA', FRAME); a('EOR', 0x5A, 'imm'); a('STA', PTR, 'indy')
    # the target frame?
    a('LDA', FRAME); a('CMP', TARGET); a('BNE', 'main')
    a('LDA', FRAME + 1); a('CMP', TARGET + 1); a('BNE', 'main')
    a('LDA', TARGET); a('ORA', TARGET + 1); a('BEQ', 'main')
    a('JSR', 'check')
    a('JMP', 'main')

    a.label('cartinit')
    a('JMP', 0xBFFE, 'ind')

    # VBI: count, NMIRES, COLBK
    a.label('nmi')
    a('PHA'); a('INC', FRAME); a('BNE', 'nmi1'); a('INC', FRAME + 1)
    a.label('nmi1')
    a('STA', NMIRES, 'abs')
    a('LDA', FRAME); a('AND', 0xF0, 'imm'); a('STA', COLBK, 'abs')
    a('PLA'); a('RTI')

    # IRQ: count; keyboard -> KEY; timer 1 -> TMR; re-arm what fired
    a.label('irq')
    a('PHA'); a('INC', IRQS)
    a('LDA', IRQEN, 'abs')                     # IRQST: 0 = pending
    a('AND', 0x40, 'imm'); a('BNE', 'irq1')
    a('LDA', KBCODE, 'abs'); a('STA', KEY)
    a.label('irq1')
    a('LDA', IRQEN, 'abs'); a('AND', 0x01, 'imm'); a('BNE', 'irq2')
    a('INC', TMR)
    a.label('irq2')
    a('LDA', 0x00, 'imm'); a('STA', IRQEN, 'abs')
    a('LDA', 0xC1, 'imm'); a('STA', IRQEN, 'abs')
    a('PLA'); a('RTI')

    a.at(0xF000)
    a.label('check')
    a('RTS')

    a.at(0xFFFA)
    a.word('nmi'); a.word('reset'); a.word('irq')
    rom = a.done()
    assert len(rom) == 0x2800, len(rom)
    return rom


def build_cart():
    """8K at $A000: init writes a signature line onto the screen and returns."""
    a = Asm(0xA000)
    a.label('init')
    a('LDX', 0, 'imm')
    a.label('loop')
    a('TXA'); a('EOR', 0x30, 'imm'); a('STA', SCREEN + 40, 'absx'); a('INX'); a('CPX', 40, 'imm')
    a('BNE', 'loop')
    a('LDA', 0x5C, 'imm'); a('STA', SCREEN + 86, 'abs')
    a('RTS')
    a.at(0xBFFA)
    a.word('init')              # run address (unused here)
    a.byte(0, 0)                # $BFFC: 0 = cartridge present; $BFFD flags
    a.word('init')              # $BFFE: init address
    rom = a.done()
    assert len(rom) == 0x2000
    return rom


def build_atr():
    """A 720-sector single-density ATR; sector n holds n and a pattern."""
    sectors = 720
    data = bytearray()
    for n in range(1, sectors + 1):
        data += bytes(((n * 7 + i * 3) ^ (i << 1)) & 0xFF if i else n & 0xFF for i in range(128))
    paras = len(data) // 16
    head = bytes((0x96, 0x02, paras & 0xFF, (paras >> 8) & 0xFF, 128, 0,
                  (paras >> 16) & 0xFF, (paras >> 24) & 0xFF)) + bytes(8)
    return head + bytes(data)


# ── The 2600 test cartridge ──────────────────────────────────────────────────
VSYNC, VBLANK, WSYNC26, COLUP0, COLUPF, COLUBK, CTRLPF, PF1, RESP0 = 0, 1, 2, 6, 8, 9, 0x0A, 0x0E, 0x10
AUDC0, AUDC1, AUDF0, AUDF1, AUDV0, AUDV1, GRP0, HMP0, HMOVE = 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x20, 0x2A
INPT4, SWCHA, SWCHB, INTIM, TIM64T = 0x0C, 0x280, 0x282, 0x284, 0x296


def build_2600(banked):
    """4K (or F8 8K: two banks of the same code, a different playfield colour
    in each, switching every frame).  Each frame: VSYNC, a RIOT-timed VBLANK,
    192 lines with the background, playfield and player 0 changing, an
    overscan; two tones whose pitch follows the frame count; SWCHA, INPT4
    and SWCHB copied to RAM ($81-$83)."""
    a = Asm(0xF000)
    a.label('reset')
    a('SEI'); a('CLD'); a('LDX', 0xFF, 'imm'); a('TXS')
    a('LDA', 0, 'imm'); a('TAX')
    a.label('clr'); a('STA', 0x00, 'zpx'); a('INX'); a('BNE', 'clr')
    a('LDA', 0x04, 'imm'); a('STA', AUDC0); a('LDA', 0x0C, 'imm'); a('STA', AUDC1)
    a('LDA', 0x0F, 'imm'); a('STA', AUDV0); a('LDA', 0x08, 'imm'); a('STA', AUDV1)
    a('LDA', 0x01, 'imm'); a('STA', CTRLPF)
    a.label('frame')
    a('LDA', 2, 'imm'); a('STA', VSYNC)
    a('STA', WSYNC26); a('STA', WSYNC26); a('STA', WSYNC26)
    a('LDA', 0, 'imm'); a('STA', VSYNC)
    a('LDA', 43, 'imm'); a('STA', TIM64T, 'abs')
    a('INC', 0x80)
    a('LDA', 0x80); a('LSR'); a('LSR'); a('AND', 0x1F, 'imm'); a('STA', AUDF0)
    a('EOR', 0x0F, 'imm'); a('STA', AUDF1)
    a('LDA', SWCHA, 'abs'); a('STA', 0x81)
    a('LDA', INPT4); a('STA', 0x82)
    a('LDA', SWCHB, 'abs'); a('STA', 0x83)
    a('LDA', 0x80); a('ORA', 0x0E, 'imm'); a('STA', COLUP0)
    a.label('bankmark')
    a('LDA', 0x44, 'imm'); a('STA', COLUPF)
    a('STA', WSYNC26); a('STA', RESP0)
    a('LDA', 0x10, 'imm'); a('STA', HMP0); a('STA', WSYNC26); a('STA', HMOVE)
    a.label('vbl'); a('LDA', INTIM, 'abs'); a('BNE', 'vbl')
    a('STA', WSYNC26); a('LDA', 0, 'imm'); a('STA', VBLANK)
    a('LDX', 192, 'imm')
    a.label('line')
    a('STA', WSYNC26)
    a('TXA'); a('CLC'); a('ADC', 0x80); a('STA', COLUBK)
    a('TXA'); a('EOR', 0x80); a('STA', GRP0)
    a('TXA'); a('AND', 0xF0, 'imm'); a('STA', PF1)
    a('DEX'); a('BNE', 'line')
    a('LDA', 2, 'imm'); a('STA', VBLANK)
    a('LDX', 30, 'imm')
    a.label('over'); a('STA', WSYNC26); a('DEX'); a('BNE', 'over')
    if banked:
        a('LDA', 0x80); a('AND', 1, 'imm'); a('BEQ', 'b0')
        a('LDA', 0xFFF9, 'abs'); a('JMP', 'frame')
        a.label('b0'); a('LDA', 0xFFF8, 'abs')
    a('JMP', 'frame')
    a.at(0xFFFC)
    a.word('reset'); a.word('reset')
    rom = a.done()
    assert len(rom) == 0x1000
    if not banked:
        return rom
    mark = a.labels['bankmark'] - 0xF000 + 1
    bank1 = bytearray(rom)
    bank1[mark] = 0xC6
    return rom + bytes(bank1)


def c_array(name, data):
    lines = [f"static const unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    files = {'os.rom': build_os(), 'cart.rom': build_cart(), 'disk.atr': build_atr(),
             'test4k.a26': build_2600(False), 'testf8.a26': build_2600(True)}
    if len(sys.argv) == 3 and sys.argv[1] == '--header':
        out = ["/* Generated by tests/data/atari/standin.py --header; do not edit. */\n"]
        for name, data in (('a400_standin_os', files['os.rom']), ('a400_standin_cart', files['cart.rom']),
                           ('a400_standin_atr', files['disk.atr']), ('a2600_test_4k', files['test4k.a26']),
                           ('a2600_test_f8', files['testf8.a26'])):
            out.append(c_array(name, data))
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
