/*
 * 6502 disassembler (NMOS: all 256 opcodes, including the undocumented ones
 * under their common names).  Operand addresses go through an optional
 * symbol callback, told whether the address is a jump target, read or
 * written, so a machine can name registers that differ by direction.
 */
#include "mos6502.h"
#include <stdio.h>
#include <string.h>

enum { M_IMP, M_ACC, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_IND, M_IZX, M_IZY, M_REL };

static const struct { const char *mn; uint8_t mode; } ops[256] = {
    {"BRK",M_IMP}, {"ORA",M_IZX}, {"KIL",M_IMP}, {"SLO",M_IZX}, {"NOP",M_ZP}, {"ORA",M_ZP}, {"ASL",M_ZP}, {"SLO",M_ZP},
    {"PHP",M_IMP}, {"ORA",M_IMM}, {"ASL",M_ACC}, {"ANC",M_IMM}, {"NOP",M_ABS}, {"ORA",M_ABS}, {"ASL",M_ABS}, {"SLO",M_ABS},
    {"BPL",M_REL}, {"ORA",M_IZY}, {"KIL",M_IMP}, {"SLO",M_IZY}, {"NOP",M_ZPX}, {"ORA",M_ZPX}, {"ASL",M_ZPX}, {"SLO",M_ZPX},
    {"CLC",M_IMP}, {"ORA",M_ABY}, {"NOP",M_IMP}, {"SLO",M_ABY}, {"NOP",M_ABX}, {"ORA",M_ABX}, {"ASL",M_ABX}, {"SLO",M_ABX},
    {"JSR",M_ABS}, {"AND",M_IZX}, {"KIL",M_IMP}, {"RLA",M_IZX}, {"BIT",M_ZP}, {"AND",M_ZP}, {"ROL",M_ZP}, {"RLA",M_ZP},
    {"PLP",M_IMP}, {"AND",M_IMM}, {"ROL",M_ACC}, {"ANC",M_IMM}, {"BIT",M_ABS}, {"AND",M_ABS}, {"ROL",M_ABS}, {"RLA",M_ABS},
    {"BMI",M_REL}, {"AND",M_IZY}, {"KIL",M_IMP}, {"RLA",M_IZY}, {"NOP",M_ZPX}, {"AND",M_ZPX}, {"ROL",M_ZPX}, {"RLA",M_ZPX},
    {"SEC",M_IMP}, {"AND",M_ABY}, {"NOP",M_IMP}, {"RLA",M_ABY}, {"NOP",M_ABX}, {"AND",M_ABX}, {"ROL",M_ABX}, {"RLA",M_ABX},
    {"RTI",M_IMP}, {"EOR",M_IZX}, {"KIL",M_IMP}, {"SRE",M_IZX}, {"NOP",M_ZP}, {"EOR",M_ZP}, {"LSR",M_ZP}, {"SRE",M_ZP},
    {"PHA",M_IMP}, {"EOR",M_IMM}, {"LSR",M_ACC}, {"ALR",M_IMM}, {"JMP",M_ABS}, {"EOR",M_ABS}, {"LSR",M_ABS}, {"SRE",M_ABS},
    {"BVC",M_REL}, {"EOR",M_IZY}, {"KIL",M_IMP}, {"SRE",M_IZY}, {"NOP",M_ZPX}, {"EOR",M_ZPX}, {"LSR",M_ZPX}, {"SRE",M_ZPX},
    {"CLI",M_IMP}, {"EOR",M_ABY}, {"NOP",M_IMP}, {"SRE",M_ABY}, {"NOP",M_ABX}, {"EOR",M_ABX}, {"LSR",M_ABX}, {"SRE",M_ABX},
    {"RTS",M_IMP}, {"ADC",M_IZX}, {"KIL",M_IMP}, {"RRA",M_IZX}, {"NOP",M_ZP}, {"ADC",M_ZP}, {"ROR",M_ZP}, {"RRA",M_ZP},
    {"PLA",M_IMP}, {"ADC",M_IMM}, {"ROR",M_ACC}, {"ARR",M_IMM}, {"JMP",M_IND}, {"ADC",M_ABS}, {"ROR",M_ABS}, {"RRA",M_ABS},
    {"BVS",M_REL}, {"ADC",M_IZY}, {"KIL",M_IMP}, {"RRA",M_IZY}, {"NOP",M_ZPX}, {"ADC",M_ZPX}, {"ROR",M_ZPX}, {"RRA",M_ZPX},
    {"SEI",M_IMP}, {"ADC",M_ABY}, {"NOP",M_IMP}, {"RRA",M_ABY}, {"NOP",M_ABX}, {"ADC",M_ABX}, {"ROR",M_ABX}, {"RRA",M_ABX},
    {"NOP",M_IMM}, {"STA",M_IZX}, {"NOP",M_IMM}, {"SAX",M_IZX}, {"STY",M_ZP}, {"STA",M_ZP}, {"STX",M_ZP}, {"SAX",M_ZP},
    {"DEY",M_IMP}, {"NOP",M_IMM}, {"TXA",M_IMP}, {"XAA",M_IMM}, {"STY",M_ABS}, {"STA",M_ABS}, {"STX",M_ABS}, {"SAX",M_ABS},
    {"BCC",M_REL}, {"STA",M_IZY}, {"KIL",M_IMP}, {"SHA",M_IZY}, {"STY",M_ZPX}, {"STA",M_ZPX}, {"STX",M_ZPY}, {"SAX",M_ZPY},
    {"TYA",M_IMP}, {"STA",M_ABY}, {"TXS",M_IMP}, {"TAS",M_ABY}, {"SHY",M_ABX}, {"STA",M_ABX}, {"SHX",M_ABY}, {"SHA",M_ABY},
    {"LDY",M_IMM}, {"LDA",M_IZX}, {"LDX",M_IMM}, {"LAX",M_IZX}, {"LDY",M_ZP}, {"LDA",M_ZP}, {"LDX",M_ZP}, {"LAX",M_ZP},
    {"TAY",M_IMP}, {"LDA",M_IMM}, {"TAX",M_IMP}, {"LAX",M_IMM}, {"LDY",M_ABS}, {"LDA",M_ABS}, {"LDX",M_ABS}, {"LAX",M_ABS},
    {"BCS",M_REL}, {"LDA",M_IZY}, {"KIL",M_IMP}, {"LAX",M_IZY}, {"LDY",M_ZPX}, {"LDA",M_ZPX}, {"LDX",M_ZPY}, {"LAX",M_ZPY},
    {"CLV",M_IMP}, {"LDA",M_ABY}, {"TSX",M_IMP}, {"LAS",M_ABY}, {"LDY",M_ABX}, {"LDA",M_ABX}, {"LDX",M_ABY}, {"LAX",M_ABY},
    {"CPY",M_IMM}, {"CMP",M_IZX}, {"NOP",M_IMM}, {"DCP",M_IZX}, {"CPY",M_ZP}, {"CMP",M_ZP}, {"DEC",M_ZP}, {"DCP",M_ZP},
    {"INY",M_IMP}, {"CMP",M_IMM}, {"DEX",M_IMP}, {"SBX",M_IMM}, {"CPY",M_ABS}, {"CMP",M_ABS}, {"DEC",M_ABS}, {"DCP",M_ABS},
    {"BNE",M_REL}, {"CMP",M_IZY}, {"KIL",M_IMP}, {"DCP",M_IZY}, {"NOP",M_ZPX}, {"CMP",M_ZPX}, {"DEC",M_ZPX}, {"DCP",M_ZPX},
    {"CLD",M_IMP}, {"CMP",M_ABY}, {"NOP",M_IMP}, {"DCP",M_ABY}, {"NOP",M_ABX}, {"CMP",M_ABX}, {"DEC",M_ABX}, {"DCP",M_ABX},
    {"CPX",M_IMM}, {"SBC",M_IZX}, {"NOP",M_IMM}, {"ISC",M_IZX}, {"CPX",M_ZP}, {"SBC",M_ZP}, {"INC",M_ZP}, {"ISC",M_ZP},
    {"INX",M_IMP}, {"SBC",M_IMM}, {"NOP",M_IMP}, {"SBC",M_IMM}, {"CPX",M_ABS}, {"SBC",M_ABS}, {"INC",M_ABS}, {"ISC",M_ABS},
    {"BEQ",M_REL}, {"SBC",M_IZY}, {"KIL",M_IMP}, {"ISC",M_IZY}, {"NOP",M_ZPX}, {"SBC",M_ZPX}, {"INC",M_ZPX}, {"ISC",M_ZPX},
    {"SED",M_IMP}, {"SBC",M_ABY}, {"NOP",M_IMP}, {"ISC",M_ABY}, {"NOP",M_ABX}, {"SBC",M_ABX}, {"INC",M_ABX}, {"ISC",M_ABX},
};

static const uint8_t mode_len[] = { 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 2, 2, 2 };

int mos6502_insn_len(uint8_t opcode) {
    return mode_len[ops[opcode].mode];
}

/* How an instruction touches its operand address. */
static int access_of(const char *mn) {
    static const char *const writes[] = {
        "STA", "STX", "STY", "SAX", "SHA", "SHX", "SHY", "TAS",
        "ASL", "LSR", "ROL", "ROR", "INC", "DEC", "SLO", "RLA", "SRE", "RRA", "DCP", "ISC",
    };
    if (!strcmp(mn, "JMP") || !strcmp(mn, "JSR")) return MOS6502_SYM_CODE;
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i++)
        if (!strcmp(mn, writes[i])) return MOS6502_SYM_WRITE;
    return MOS6502_SYM_READ;
}

static void addr_text(char *out, size_t len, uint16_t a, bool zp, int access,
                      Mos6502SymFn sym, void *sym_ud) {
    const char *name = sym ? sym(sym_ud, a, access) : NULL;
    if (name) snprintf(out, len, "%s", name);
    else if (zp) snprintf(out, len, "$%02X", a);
    else         snprintf(out, len, "$%04X", a);
}

int mos6502_disasm(Mos6502PeekFn peek, void *ud, uint16_t pc, char *buf, size_t len,
                   Mos6502SymFn sym, void *sym_ud) {
    uint8_t op = peek(ud, pc);
    const char *mn = ops[op].mn;
    int mode = ops[op].mode;
    uint8_t b1 = peek(ud, (uint16_t)(pc + 1)), b2 = peek(ud, (uint16_t)(pc + 2));
    uint16_t w = (uint16_t)(b1 | (b2 << 8));
    int access = access_of(mn);
    char a[64];

    switch (mode) {
    case M_IMP: snprintf(buf, len, "%s", mn); break;
    case M_ACC: snprintf(buf, len, "%s A", mn); break;
    case M_IMM: snprintf(buf, len, "%s #$%02X", mn, b1); break;
    case M_ZP:  addr_text(a, sizeof(a), b1, true, access, sym, sym_ud); snprintf(buf, len, "%s %s", mn, a); break;
    case M_ZPX: addr_text(a, sizeof(a), b1, true, access, sym, sym_ud); snprintf(buf, len, "%s %s,X", mn, a); break;
    case M_ZPY: addr_text(a, sizeof(a), b1, true, access, sym, sym_ud); snprintf(buf, len, "%s %s,Y", mn, a); break;
    case M_ABS: addr_text(a, sizeof(a), w, false, access, sym, sym_ud); snprintf(buf, len, "%s %s", mn, a); break;
    case M_ABX: addr_text(a, sizeof(a), w, false, access, sym, sym_ud); snprintf(buf, len, "%s %s,X", mn, a); break;
    case M_ABY: addr_text(a, sizeof(a), w, false, access, sym, sym_ud); snprintf(buf, len, "%s %s,Y", mn, a); break;
    case M_IND: addr_text(a, sizeof(a), w, false, MOS6502_SYM_READ, sym, sym_ud); snprintf(buf, len, "%s (%s)", mn, a); break;
    case M_IZX: addr_text(a, sizeof(a), b1, true, MOS6502_SYM_READ, sym, sym_ud); snprintf(buf, len, "%s (%s,X)", mn, a); break;
    case M_IZY: addr_text(a, sizeof(a), b1, true, MOS6502_SYM_READ, sym, sym_ud); snprintf(buf, len, "%s (%s),Y", mn, a); break;
    case M_REL: {
        uint16_t t = (uint16_t)(pc + 2 + (int8_t)b1);
        addr_text(a, sizeof(a), t, false, MOS6502_SYM_CODE, sym, sym_ud);
        snprintf(buf, len, "%s %s", mn, a);
        break;
    }
    }
    return mode_len[mode];
}
