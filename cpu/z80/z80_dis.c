/*
 * Z80 disassembler: every opcode, including the undocumented ones (IXH/IXL,
 * SLL, DDCB results copied to a register, IN (C), OUT (C),0), in Zilog
 * syntax.  Decodes with the same x/y/z/p/q split as z80.c.
 */
#include "z80.h"
#include <stdio.h>
#include <string.h>

static const char *const r_names[8]   = { "B", "C", "D", "E", "H", "L", "(HL)", "A" };
static const char *const rp_names[4]  = { "BC", "DE", "HL", "SP" };
static const char *const rp2_names[4] = { "BC", "DE", "HL", "AF" };
static const char *const cc_names[8]  = { "NZ", "Z", "NC", "C", "PO", "PE", "P", "M" };
static const char *const alu_names[8] = { "ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP " };
static const char *const rot_names[8] = { "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL" };
static const char *const acc_names[8] = { "RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF" };
static const char *const im_names[8]  = { "0", "0/1", "1", "2", "0", "0/1", "1", "2" };
static const char *const blk_names[4][4] = {
    { "LDI", "CPI", "INI", "OUTI" }, { "LDD", "CPD", "IND", "OUTD" },
    { "LDIR", "CPIR", "INIR", "OTIR" }, { "LDDR", "CPDR", "INDR", "OTDR" },
};

typedef struct {
    Z80PeekFn peek;
    void     *ud;
    uint16_t  pc;
    int       len;
    int       xm;       /* 0 HL, 1 IX, 2 IY */
    int8_t    disp;
    bool      have_disp;
} Dis;

static uint8_t next(Dis *d) {
    d->len++;
    return d->peek(d->ud, (uint16_t)(d->pc + d->len - 1));
}

static uint16_t next16(Dis *d) {
    uint8_t lo = next(d);
    return (uint16_t)(lo | next(d) << 8);
}

static const char *xhl(const Dis *d) { return d->xm == 0 ? "HL" : d->xm == 1 ? "IX" : "IY"; }

/* 8-bit register r, with H/L/(HL) replaced under DD/FD.  noxy: the other
 * operand is (IX+d), so H and L stay H and L. */
static void reg(Dis *d, int r, bool noxy, char *out, size_t len) {
    if (r == 6 && d->xm) {
        if (!d->have_disp) { d->disp = (int8_t)next(d); d->have_disp = true; }
        snprintf(out, len, "(%s%c%02XH)", xhl(d), d->disp < 0 ? '-' : '+',
                 d->disp < 0 ? -d->disp : d->disp);
    } else if ((r == 4 || r == 5) && d->xm && !noxy) {
        snprintf(out, len, "%s%c", xhl(d), r == 4 ? 'H' : 'L');
    } else {
        snprintf(out, len, "%s", r_names[r]);
    }
}

static const char *rp(const Dis *d, int p)  { return p == 2 ? xhl(d) : rp_names[p]; }
static const char *rp2(const Dis *d, int p) { return p == 2 ? xhl(d) : rp2_names[p]; }

static void dis_cb(Dis *d, char *buf, size_t len) {
    char r[16];
    uint8_t op;
    if (d->xm) {   /* DDCB d op */
        d->disp = (int8_t)next(d);
        d->have_disp = true;
        op = next(d);
    } else {
        op = next(d);
    }
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    reg(d, d->xm ? 6 : z, true, r, sizeof(r));
    char copy[8] = "";
    if (d->xm && z != 6 && x != 1) snprintf(copy, sizeof(copy), ",%s", r_names[z]);
    switch (x) {
    case 0: snprintf(buf, len, "%s %s%s", rot_names[y], r, copy); break;
    case 1: snprintf(buf, len, "BIT %d,%s", y, r); break;
    case 2: snprintf(buf, len, "RES %d,%s%s", y, r, copy); break;
    default: snprintf(buf, len, "SET %d,%s%s", y, r, copy); break;
    }
}

static void dis_ed(Dis *d, char *buf, size_t len) {
    uint8_t op = next(d);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    if (x == 2 && z <= 3 && y >= 4) { snprintf(buf, len, "%s", blk_names[y - 4][z]); return; }
    if (x != 1) { snprintf(buf, len, "NOP*"); return; }
    switch (z) {
    case 0:
        if (y == 6) snprintf(buf, len, "IN (C)");
        else snprintf(buf, len, "IN %s,(C)", r_names[y]);
        break;
    case 1:
        if (y == 6) snprintf(buf, len, "OUT (C),0");
        else snprintf(buf, len, "OUT (C),%s", r_names[y]);
        break;
    case 2: snprintf(buf, len, "%s HL,%s", q ? "ADC" : "SBC", rp_names[p]); break;
    case 3: {
        uint16_t nn = next16(d);
        if (q) snprintf(buf, len, "LD %s,(%04XH)", rp_names[p], nn);
        else   snprintf(buf, len, "LD (%04XH),%s", nn, rp_names[p]);
        break;
    }
    case 4: snprintf(buf, len, y ? "NEG*" : "NEG"); break;
    case 5: snprintf(buf, len, y == 1 ? "RETI" : y ? "RETN*" : "RETN"); break;
    case 6: snprintf(buf, len, "IM %s", im_names[y]); break;
    default: {
        static const char *const n[8] = { "LD I,A", "LD R,A", "LD A,I", "LD A,R", "RRD", "RLD", "NOP*", "NOP*" };
        snprintf(buf, len, "%s", n[y]);
        break;
    }
    }
}

int z80_disasm(Z80PeekFn peek, void *ud, uint16_t pc, char *buf, size_t len) {
    Dis d = { .peek = peek, .ud = ud, .pc = pc };
    char a[16], b[16];
    uint8_t op = next(&d);
    if (op == 0xDD || op == 0xFD) {
        /* followed by another prefix (or ED) it does nothing but take 4 T */
        uint8_t n = peek(ud, (uint16_t)(pc + 1));
        if (n == 0xDD || n == 0xFD || n == 0xED) {
            snprintf(buf, len, "NOP* (%02X prefix)", op);
            return 1;
        }
        d.xm = op == 0xDD ? 1 : 2;
        op = next(&d);
    }
    if (op == 0xCB) { dis_cb(&d, buf, len); return d.len; }
    if (op == 0xED) { dis_ed(&d, buf, len); return d.len; }

    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    switch (x) {
    case 0:
        switch (z) {
        case 0:
            if (y == 0) snprintf(buf, len, "NOP");
            else if (y == 1) snprintf(buf, len, "EX AF,AF'");
            else {
                int8_t e = (int8_t)next(&d);
                uint16_t t = (uint16_t)(pc + d.len + e);
                if (y == 2) snprintf(buf, len, "DJNZ %04XH", t);
                else if (y == 3) snprintf(buf, len, "JR %04XH", t);
                else snprintf(buf, len, "JR %s,%04XH", cc_names[y - 4], t);
            }
            break;
        case 1:
            if (q) snprintf(buf, len, "ADD %s,%s", xhl(&d), rp(&d, p));
            else   snprintf(buf, len, "LD %s,%04XH", rp(&d, p), next16(&d));
            break;
        case 2:
            switch (y) {
            case 0: snprintf(buf, len, "LD (BC),A"); break;
            case 1: snprintf(buf, len, "LD A,(BC)"); break;
            case 2: snprintf(buf, len, "LD (DE),A"); break;
            case 3: snprintf(buf, len, "LD A,(DE)"); break;
            case 4: snprintf(buf, len, "LD (%04XH),%s", next16(&d), xhl(&d)); break;
            case 5: snprintf(buf, len, "LD %s,(%04XH)", xhl(&d), next16(&d)); break;
            case 6: snprintf(buf, len, "LD (%04XH),A", next16(&d)); break;
            default: snprintf(buf, len, "LD A,(%04XH)", next16(&d)); break;
            }
            break;
        case 3: snprintf(buf, len, "%s %s", q ? "DEC" : "INC", rp(&d, p)); break;
        case 4: reg(&d, y, false, a, sizeof(a)); snprintf(buf, len, "INC %s", a); break;
        case 5: reg(&d, y, false, a, sizeof(a)); snprintf(buf, len, "DEC %s", a); break;
        case 6: reg(&d, y, false, a, sizeof(a)); snprintf(buf, len, "LD %s,%02XH", a, next(&d)); break;
        default: snprintf(buf, len, "%s", acc_names[y]); break;
        }
        break;
    case 1:
        if (op == 0x76) { snprintf(buf, len, "HALT"); break; }
        reg(&d, y, z == 6, a, sizeof(a));
        reg(&d, z, y == 6, b, sizeof(b));
        snprintf(buf, len, "LD %s,%s", a, b);
        break;
    case 2:
        reg(&d, z, false, a, sizeof(a));
        snprintf(buf, len, "%s%s", alu_names[y], a);
        break;
    default:
        switch (z) {
        case 0: snprintf(buf, len, "RET %s", cc_names[y]); break;
        case 1:
            if (!q) { snprintf(buf, len, "POP %s", rp2(&d, p)); break; }
            switch (p) {
            case 0: snprintf(buf, len, "RET"); break;
            case 1: snprintf(buf, len, "EXX"); break;
            case 2: snprintf(buf, len, "JP (%s)", xhl(&d)); break;
            default: snprintf(buf, len, "LD SP,%s", xhl(&d)); break;
            }
            break;
        case 2: snprintf(buf, len, "JP %s,%04XH", cc_names[y], next16(&d)); break;
        case 3:
            switch (y) {
            case 0: snprintf(buf, len, "JP %04XH", next16(&d)); break;
            case 2: snprintf(buf, len, "OUT (%02XH),A", next(&d)); break;
            case 3: snprintf(buf, len, "IN A,(%02XH)", next(&d)); break;
            case 4: snprintf(buf, len, "EX (SP),%s", xhl(&d)); break;
            case 5: snprintf(buf, len, "EX DE,HL"); break;
            case 6: snprintf(buf, len, "DI"); break;
            default: snprintf(buf, len, "EI"); break;
            }
            break;
        case 4: snprintf(buf, len, "CALL %s,%04XH", cc_names[y], next16(&d)); break;
        case 5:
            if (!q) snprintf(buf, len, "PUSH %s", rp2(&d, p));
            else    snprintf(buf, len, "CALL %04XH", next16(&d));
            break;
        case 6: snprintf(buf, len, "%s%02XH", alu_names[y], next(&d)); break;
        default: snprintf(buf, len, "RST %02XH", y * 8); break;
        }
        break;
    }
    return d.len;
}
