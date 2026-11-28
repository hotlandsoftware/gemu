#!/usr/bin/env python3
"""Stand-in programs for the rca family (stage R): a small CDP1802 assembler
and one program per machine.

    standin.py OUTDIR        writes the images into OUTDIR
    standin.py --header H    writes them as C arrays to H

Each program sets up the classic 1802 interrupt routine (R1 the entry,
R2 the stack, the exit just before the entry), counts frames in RAM in its
interrupt, points DMA at its display buffer where the machine uses DMA,
and in its main loop draws a moving pattern, reads its inputs and toggles
Q (the speaker).

  vip.rom        at $0000 for the COSMAC VIP (CDP1861): display buffer
                 $0100-$04FF (the 1861's 64x128), the counter at $0600,
                 the keypad read with IN 1 (which also turns the display on).
  studio2.rom    2 KB at $0000 for the Studio II: the same on its 512 bytes
                 of RAM ($0800-$09FF): display at $0900, counter at $0880.
  pecom.rom      16 KB, the ROM at $8000, for the Pecom 32: from the boot mirror at
                 $0000 a long branch to $8003, then OUT 1 to map RAM;
                 fills the CDP1869's page RAM ($F800) and character RAM
                 ($F400); its interrupt (once a frame) counts at $0600.
  destroyer.rom  4 KB at $0000 for Destroyer: as the Pecom's, from $0000,
                 its stack and counter in the RAM at $2000 ($2080), also
                 reading the input port (IN 1) each pass.
"""
import sys
from pathlib import Path

R = {f'R{i}': i for i in range(16)}

SHORT = dict(BR=0x30, BQ=0x31, BZ=0x32, BDF=0x33, B1=0x34, B2=0x35, B3=0x36, B4=0x37,
             BNQ=0x39, BNZ=0x3A, BNF=0x3B, BN1=0x3C, BN2=0x3D, BN3=0x3E, BN4=0x3F)
LONG = dict(LBR=0xC0, LBQ=0xC1, LBZ=0xC2, LBDF=0xC3, LBNQ=0xC9, LBNZ=0xCA, LBNF=0xCB)
REG = dict(LDN=0x00, INC=0x10, DEC=0x20, LDA=0x40, STR=0x50, GLO=0x80, GHI=0x90, PLO=0xA0,
           PHI=0xB0, SEP=0xD0, SEX=0xE0)
IMPLIED = dict(IDL=0x00, IRX=0x60, RET=0x70, DIS=0x71, LDXA=0x72, STXD=0x73, ADC=0x74,
               SAV=0x78, MARK=0x79, REQ=0x7A, SEQ=0x7B, NOP=0xC4, LDX=0xF0, OR=0xF1, AND=0xF2,
               XOR=0xF3, ADD=0xF4, SHR=0xF6, SHL=0xFE)
IMM = dict(LDI=0xF8, ORI=0xF9, ANI=0xFA, XRI=0xFB, ADI=0xFC, SMI=0xFF, ADCI=0x7C)


class Asm:
    def __init__(self, org):
        self.org, self.code, self.labels, self.fix = org, bytearray(), {}, []

    @property
    def pc(self):
        return self.org + len(self.code)

    def label(self, name):
        self.labels[name] = self.pc

    def at(self, addr, fill=0x00):
        assert addr >= self.pc, hex(addr)
        self.code += bytes([fill]) * (addr - self.pc)

    def byte(self, *b):
        self.code += bytes(b)

    def __call__(self, op, arg=None):
        if op in IMPLIED:
            self.byte(IMPLIED[op])
        elif op in REG:
            self.byte(REG[op] | (R[arg] if isinstance(arg, str) else arg))
        elif op == 'OUT':
            self.byte(0x60 | arg)
        elif op == 'INP':
            self.byte(0x68 | arg)
        elif op in IMM:
            self.byte(IMM[op], arg & 0xFF)
        elif op in SHORT:
            self.byte(SHORT[op])
            self.fix.append((len(self.code), arg, 'short', self.pc))
            self.byte(0)
        elif op in LONG:
            self.byte(LONG[op])
            self.fix.append((len(self.code), arg, 'long', self.pc))
            self.byte(0, 0)
        else:
            raise ValueError(op)

    def load16(self, reg, value):
        """R(reg) = value (a number or a label)."""
        if isinstance(value, str):
            self.fix.append((len(self.code) + 1, value, 'hi', 0))
            self.fix.append((len(self.code) + 4, value, 'lo', 0))
            value = 0
        self('LDI', value >> 8); self('PHI', reg)
        self('LDI', value & 0xFF); self('PLO', reg)

    def done(self):
        for off, name, kind, at in self.fix:
            v = self.labels[name] if isinstance(name, str) else name
            if kind == 'short':
                assert (v >> 8) == ((self.org + off) >> 8), f'short branch to {name} leaves the page'
                self.code[off] = v & 0xFF
            elif kind == 'long':
                self.code[off] = v >> 8
                self.code[off + 1] = v & 0xFF
            elif kind == 'hi':
                self.code[off] = v >> 8
            else:
                self.code[off] = v & 0xFF
        return bytes(self.code)


def interrupt(a, display, counter, dma=True):
    """The classic routine: exit (LDXA; RET), then the entry R1 points at."""
    a.label('int_exit')
    a('LDXA'); a('RET')
    a.label('int')
    a('DEC', 'R2'); a('SAV'); a('DEC', 'R2'); a('STR', 'R2')     # save T and D
    if dma:
        a.load16('R0', display)                                    # DMA from the buffer
    a.load16('R7', counter)
    a('LDN', 'R7'); a('ADI', 1); a('STR', 'R7')                    # the frame count
    a('BR', 'int_exit')


def setup(a, stack, counter_reg_value=None):
    a.load16('R1', 'int')
    a.load16('R2', stack)
    a.load16('R3', 'main')
    a('SEP', 'R3')


def main_loop(a, buf_lo, buf_hi_end, inp_port=None, store_inp=None):
    """Draw R5's count through R4 over the buffer, toggle Q every 256."""
    a.label('main')
    a('SEX', 'R2')
    a.load16('R4', buf_lo)
    a.label('loop')
    a('GLO', 'R5'); a('STR', 'R4'); a('INC', 'R4'); a('INC', 'R5')
    a('GHI', 'R4'); a('XRI', buf_hi_end); a('BNZ', 'nowrap')
    a.load16('R4', buf_lo)
    a.label('nowrap')
    if inp_port is not None:
        a.load16('R8', store_inp)
        a('SEX', 'R8'); a('INP', inp_port); a('SEX', 'R2')       # M(R8) = D = the port
    a('GLO', 'R5'); a('BNZ', 'loop')
    a('BQ', 'qoff'); a('SEQ'); a('BR', 'loop')
    a.label('qoff'); a('REQ'); a('LBR', 'loop')


def build_vip():
    a = Asm(0x0000)
    setup(a, 0x07FF)
    a.at(0x0040)
    interrupt(a, 0x0100, 0x0600)
    a.at(0x0080)
    main_loop(a, 0x0100, 0x05, inp_port=1, store_inp=0x0601)
    return a.done()


def build_studio2():
    a = Asm(0x0000)
    setup(a, 0x09FF)
    a.at(0x0040)
    interrupt(a, 0x0900, 0x0880)
    a.at(0x0080)
    main_loop(a, 0x0900, 0x0A, inp_port=1, store_inp=0x0881)
    a.at(0x0800)
    return a.done()


def vis_main(a, inp_port=None, store=None):
    """Fill the CDP1869's page RAM ($F800-$FBFF) and character RAM ($F400-$F7FF)
    once, then the drawing loop on page RAM."""
    a.label('main')
    a('SEX', 'R2')
    a.load16('R4', 0xF400)
    a.label('cfill')
    a('GLO', 'R4'); a('XRI', 0x5A); a('STR', 'R4'); a('INC', 'R4')
    a('GHI', 'R4'); a('XRI', 0xF8); a('BNZ', 'cfill')
    main_loop_body(a, inp_port, store)


def main_loop_body(a, inp_port, store):
    a.load16('R4', 0xF800)
    a.label('loop')
    a('GLO', 'R5'); a('STR', 'R4'); a('INC', 'R4'); a('INC', 'R5')
    a('GHI', 'R4'); a('XRI', 0xFC); a('BNZ', 'nowrap')
    a.load16('R4', 0xF800)
    a.label('nowrap')
    if inp_port is not None:
        a.load16('R8', store)
        a('SEX', 'R8'); a('INP', inp_port); a('SEX', 'R2')
    a('GLO', 'R5'); a('BNZ', 'loop')
    a('BQ', 'qoff'); a('SEQ'); a('BR', 'loop')
    a.label('qoff'); a('REQ'); a('LBR', 'loop')


def build_pecom():
    a = Asm(0x8000)
    # from the boot mirror at $0000: to $8003 first, then OUT 1 (which maps
    # RAM at $0000; with X = P it takes the byte after it as its data)
    a('LBR', 0x8003)
    a('OUT', 1); a.byte(0x00)
    setup(a, 0x07FF)
    a.at(0x8040)
    interrupt(a, 0, 0x0600, dma=False)
    a.at(0x8080)
    vis_main(a)
    a.at(0xC000)
    return a.done()


def build_destroyer():
    a = Asm(0x0000)
    setup(a, 0x20FF)                      # RAM: $2000-$20FF (ROM to $1FFF)
    a.at(0x0040)
    interrupt(a, 0, 0x2080, dma=False)
    a.at(0x0080)
    vis_main(a, inp_port=1, store=0x2081)
    a.at(0x1000)
    return a.done()


def c_array(name, data):
    lines = [f"static const unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    files = {'vip.rom': build_vip(), 'studio2.rom': build_studio2(), 'pecom.rom': build_pecom(),
             'destroyer.rom': build_destroyer()}
    if len(sys.argv) == 3 and sys.argv[1] == '--header':
        out = ["/* Generated by tests/data/rca/standin.py --header; do not edit. */\n"]
        for name, data in files.items():
            out.append(c_array('rca_standin_' + name.replace('.rom', ''), data))
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
