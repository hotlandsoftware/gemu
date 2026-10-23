#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Intel 8086/8088 interpreter. Meant to grow in place through later
 * generations (186/286/386/486/Pentium) by feature-gating on `type`
 * rather than forking into new files - see merced.c's history for the
 * shape this is expected to take once protected mode shows up. */

typedef enum {
    X86_CPU_8086,
    X86_CPU_8088,
} X86CpuType;

/* FLAGS register bit positions */
#define X86_FLAG_CF (1u << 0)
#define X86_FLAG_PF (1u << 2)
#define X86_FLAG_AF (1u << 4)
#define X86_FLAG_ZF (1u << 6)
#define X86_FLAG_SF (1u << 7)
#define X86_FLAG_TF (1u << 8)
#define X86_FLAG_IF (1u << 9)
#define X86_FLAG_DF (1u << 10)
#define X86_FLAG_OF (1u << 11)
/* Fixed FLAGS bits on the 8086/8088: bit 1 and bits 12-15 always read as
 * 1 (software tells an 8086 from a 286 this way), bits 3 and 5 as 0. */
#define X86_FLAGS_FIXED_ON  0xF002u
#define X86_FLAGS_FIXED_OFF 0x0028u

typedef union {
    uint16_t x;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    struct { uint8_t hi, lo; } b;
#else
    struct { uint8_t lo, hi; } b;
#endif
} X86Reg16;

typedef struct X86Cpu {
    X86CpuType type;

    X86Reg16 ax, bx, cx, dx;
    uint16_t sp, bp, si, di;
    uint16_t cs, ds, es, ss;
    uint16_t ip;
    uint16_t flags;

    bool halted;

    /* Set by x86_request_irq(); consumed at the top of the next x86_step()
     * once IF allows it. Software INT/exceptions bypass this and deliver
     * immediately via the same internal path. */
    bool    irq_pending;
    uint8_t irq_vector;

    /* Interrupts aren't recognized for one instruction after STI, MOV SS
     * or POP SS (so SS:SP can be loaded as a pair). */
    bool    int_inhibit;

    /* Maskable interrupt line (INTR), sampled at every instruction
     * boundary while IF is set: intr_pending reports whether the PIC has
     * a deliverable request, and intr_ack performs the INTA cycle and
     * returns the vector.  Both optional; also wakes the CPU from HLT. */
    bool    (*intr_pending)(void *ud);
    uint8_t (*intr_ack)(void *ud);

    uint64_t insn_count;

    /* 20-bit physical memory bus: phys = ((seg << 4) + off) & 0xFFFFF.
     * That wraparound at the 1M boundary is real 8086 behavior (software
     * of the era relies on it) - deliberately not "fixed" here; the A20
     * gate is a 286+ concern for a later pass. */
    uint8_t (*mem_read8)(uint32_t addr, void *ud);
    void    (*mem_write8)(uint32_t addr, uint8_t val, void *ud);
    /* 16-bit port I/O space, always split into two 8-bit bus cycles - the
     * 8088's actual bus, and behaviorally identical to the 8086 for this
     * purpose since every PC/XT peripheral is 8-bit anyway. */
    uint8_t (*io_read8)(uint16_t port, void *ud);
    void    (*io_write8)(uint16_t port, uint8_t val, void *ud);
    void    *bus_ud;

    /* Called for an opcode byte (post-prefixes) decode doesn't recognize.
     * Caller is expected to log it once per distinct byte (mirroring
     * machine_i2000.c's mmio_log() first-seen pattern) and decide whether
     * to treat it as fatal - this function does not itself halt the CPU. */
    void (*on_unknown_opcode)(struct X86Cpu *cpu, uint8_t opcode, void *ud);
} X86Cpu;

void x86_reset(X86Cpu *cpu);

/* Executes one instruction (or services a pending HLT/interrupt) and
 * returns the clock cycles it took, from the 8086/8088 instruction timing
 * tables (including effective-address time and the 8088's extra 4 cycles
 * per word memory access, but not prefetch-queue or wait-state effects). */
unsigned x86_step(X86Cpu *cpu);

/* Raises a maskable interrupt request; delivered once IF is set, at an
 * instruction boundary. Call once per PIC INTR assertion (level-triggered
 * callers should keep calling until the PIC sees INTA). */
void x86_request_irq(X86Cpu *cpu, uint8_t vector);

/* Physical address helper, exposed so devices/monitor code can resolve a
 * segment:offset pair without duplicating the wraparound rule. */
uint32_t x86_linear(uint16_t seg, uint16_t off);

/* Register dump for the monitor's "info cpu". */
void x86_format_state(const X86Cpu *cpu, char *buf, size_t len);
