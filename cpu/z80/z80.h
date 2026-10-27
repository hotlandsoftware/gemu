#pragma once
/*
 * Zilog Z80 CPU.
 *
 * Every opcode, documented or not: the IX/IY halves (IXH, IXL...), DDCB/FDCB
 * results copied into registers, SLL, IN (C) / OUT (C),0, the ED NOPs and
 * repeated prefixes.  Flags include the undocumented X and Y bits (5 and 3)
 * with their quirks: MEMPTR (WZ) leaking into BIT n,(HL); the Q latch behind
 * SCF/CCF; the block instructions' X/Y/H/P rules, including when they
 * repeat; LD A,I/R's parity flag reflecting IFF2.
 *
 * Timing is per T-state: cycles advances as each bus access happens (opcode
 * fetch 4, memory 3, I/O 4, plus the internal cycles), so a machine can
 * sync its peripherals from inside the read/write/in/out callbacks.
 * Interrupts: NMI, and INT in modes 0 (the data bus supplies an RST, as on
 * nearly every board), 1 and 2.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* F register bits */
#define Z80_CF 0x01u
#define Z80_NF 0x02u
#define Z80_PF 0x04u   /* parity / overflow */
#define Z80_XF 0x08u   /* undocumented: bit 3 */
#define Z80_HF 0x10u
#define Z80_YF 0x20u   /* undocumented: bit 5 */
#define Z80_ZF 0x40u
#define Z80_SF 0x80u

typedef struct Z80 {
    uint8_t  a, f, b, c, d, e, h, l;
    uint16_t af_, bc_, de_, hl_;   /* alternate set */
    uint16_t ix, iy, sp, pc;
    uint16_t wz;                   /* MEMPTR */
    uint8_t  i, r;
    uint8_t  im;
    bool     iff1, iff2;
    bool     halted;
    bool     ei;      /* EI was the last instruction: no INT before the next */
    bool     p;       /* LD A,I / LD A,R was the last instruction */
    uint8_t  q;       /* flags the last instruction set (0 if it set none) */
    bool     q_set;   /* internal: this instruction wrote F */

    bool     int_line;   /* /INT asserted (level) */
    bool     nmi_line;   /* /NMI: set to request, cleared when taken (edge) */
    uint8_t  int_data;   /* data bus during INT acknowledge (IM 0 opcode, IM 2 vector) */

    uint64_t cycles;     /* T-states */

    uint8_t (*read) (void *ud, uint16_t addr);
    void    (*write)(void *ud, uint16_t addr, uint8_t val);
    uint8_t (*in)   (void *ud, uint16_t port);
    void    (*out)  (void *ud, uint16_t port, uint8_t val);
    void    *ud;
} Z80;

void z80_init (Z80 *cpu);
void z80_reset(Z80 *cpu);
/* One instruction, or an interrupt acknowledge.  Returns T-states used. */
int  z80_step (Z80 *cpu);
/* "info cpu" line. */
void z80_format_state(const Z80 *cpu, char *buf, size_t len);

/* Disassembler (z80_dis.c): returns the instruction's length. */
typedef uint8_t (*Z80PeekFn)(void *ud, uint16_t addr);
int  z80_disasm(Z80PeekFn peek, void *ud, uint16_t pc, char *buf, size_t len);
