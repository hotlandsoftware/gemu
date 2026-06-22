#include "cdp1802.h"
#include <string.h>

/* ── Memory helpers ──────────────────────────────────────────────────────── */

static inline uint8_t mr(Cdp1802 *c, uint16_t a) {
    c->memory_addr = a;
    if (c->mem_read)
        return c->mem_read(a, c->io_ud);
    return c->mem[a & c->mem_mask];
}
static inline void mw(Cdp1802 *c, uint16_t a, uint8_t v) {
    c->memory_addr = a;
    if (c->mem_write) {
        c->mem_write(a, v, c->io_ud);
        return;
    }
    c->mem[a & c->mem_mask] = v;
}

static void cdp1802_panic(Cdp1802 *c, const char *reason) {
    if (c->panic)
        c->panic(c, reason, c->panic_ud);
}

/* Short-branch: replace low byte of R[P] with the next memory byte */
static inline void sbranch(Cdp1802 *c) {
    c->R[c->P] = (c->R[c->P] & 0xFF00u) | mr(c, c->R[c->P]);
}

/* ── Carry / borrow helpers ──────────────────────────────────────────────── */

/* a + b (+ cin): returns 8-bit sum; sets c->DF = carry-out */
static inline uint8_t add8(Cdp1802 *cpu, uint8_t a, uint8_t b, uint8_t cin) {
    uint16_t r = (uint16_t)a + b + cin;
    cpu->DF = (r > 0xFFu) ? 1u : 0u;
    return (uint8_t)r;
}

/* a - b (- bin): returns 8-bit result; sets c->DF = 1 if no borrow (a >= b+bin) */
static inline uint8_t sub8(Cdp1802 *cpu, uint8_t a, uint8_t b, uint8_t bin) {
    uint16_t r = (uint16_t)a - b - bin;
    cpu->DF = (r <= 0xFFu) ? 1u : 0u;
    return (uint8_t)r;
}

/* ── Request queuing ─────────────────────────────────────────────────────── */

void cdp1802_request_dma_out(Cdp1802 *c, uint16_t count,
                              void (*cb)(uint8_t*, void*), void *ud) {
    if (!c->dma_out_pending) {
        if ((uint32_t)c->dma_count + count > CDP1802_DMA_QUEUE_CAP) {
            cdp1802_panic(c, "DMA-out queue overflow");
            return;
        }
        c->dma_out_pending   = true;
        c->next_dma_out.count = count;
        c->next_dma_out.cb    = cb;
        c->next_dma_out.ud    = ud;
    }
}

void cdp1802_request_dma_in(Cdp1802 *c, uint16_t count,
                             void (*cb)(uint8_t*, void*), void *ud) {
    if (!c->dma_in_pending) {
        if ((uint32_t)c->dma_count + count > CDP1802_DMA_QUEUE_CAP) {
            cdp1802_panic(c, "DMA-in queue overflow");
            return;
        }
        c->dma_in_pending   = true;
        c->next_dma_in.count = count;
        c->next_dma_in.cb    = cb;
        c->next_dma_in.ud    = ud;
    }
}

void cdp1802_request_irq(Cdp1802 *c) {
    c->irq_pending = true;
}

/* Push N pending-request entries into the DMA ring then set state. */
static void flush_requests(Cdp1802 *c) {
    if (c->dma_in_pending) {
        c->dma_in_pending = false;
        for (uint16_t i = 0; i < c->next_dma_in.count; i++) {
            unsigned idx = (c->dma_head + c->dma_count) & CDP1802_DMA_QUEUE_MASK;
            c->dma_queue[idx] = (Cdp1802DmaEntry){
                .is_out = false,
                .cb = c->next_dma_in.cb,
                .ud = c->next_dma_in.ud,
            };
            c->dma_count++;
        }
        c->state = CDP1802_S_DMA;
    } else if (c->dma_out_pending) {
        c->dma_out_pending = false;
        for (uint16_t i = 0; i < c->next_dma_out.count; i++) {
            unsigned idx = (c->dma_head + c->dma_count) & CDP1802_DMA_QUEUE_MASK;
            c->dma_queue[idx] = (Cdp1802DmaEntry){
                .is_out = true,
                .cb = c->next_dma_out.cb,
                .ud = c->next_dma_out.ud,
            };
            c->dma_count++;
        }
        c->state = CDP1802_S_DMA;
    } else if (c->irq_pending) {
        c->irq_pending = false;
        c->state = c->IE ? CDP1802_S_INTERRUPT : CDP1802_S_FETCH;
    } else {
        c->state = CDP1802_S_FETCH;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void cdp1802_init(Cdp1802 *c, uint8_t *mem, uint32_t mem_size) {
    memset(c, 0, sizeof(*c));
    c->mem      = mem;
    c->mem_mask = (uint32_t)(mem_size - 1u);
    c->IE       = 1;
    c->state    = CDP1802_S_EXECUTE;
    c->init_pending = true;
    for (int i = 0; i < 4; i++)
        c->EF[i] = true;
}

void cdp1802_reset(Cdp1802 *c) {
    c->I = 0; c->N = 0;
    c->Q = 0; c->IE = 1;
    c->state       = CDP1802_S_EXECUTE;
    c->exec_left   = 0;
    c->idle        = false;
    c->init_pending  = true;
    c->dma_in_pending  = false;
    c->dma_out_pending = false;
    c->irq_pending     = false;
    c->dma_head = c->dma_count = 0;
    for (int i = 0; i < 4; i++)
        c->EF[i] = true;
    if (c->q_out) c->q_out(0, c->io_ud);
}

/* Long-branch helper: two-cycle fetch of 16-bit target. */
static inline void lbranch(Cdp1802 *c, bool take) {
    if (take) {
        if (c->exec_left > 1) {
            c->B = mr(c, c->R[c->P]);
            c->R[c->P]++;
        } else {
            c->R[c->P] = ((uint16_t)c->B << 8) | mr(c, c->R[c->P]);
        }
    } else {
        c->R[c->P]++;   /* skip one byte per execute cycle → +2 total */
    }
}

void cdp1802_step(Cdp1802 *c) {
    c->cycle_count++;

    if (c->on_sync) c->on_sync(c->io_ud);

    switch (c->state) {

    /* ── Fetch ── */
    case CDP1802_S_FETCH: {
        uint8_t op = mr(c, c->R[c->P]);
        c->I = op >> 4;
        c->N = op & 0xF;
        c->exec_left = (op >= 0xC0 && op <= 0xCF) ? 2 : 1;
        c->idle      = (op == 0x00);
        c->R[c->P]++;
        c->state = CDP1802_S_EXECUTE;
        break;
    }

    /* ── Execute ── */
    case CDP1802_S_EXECUTE: {
        /* Reset initialisation: sets X=0, P=0, R[0]=0 and starts fetching. */
        if (c->init_pending) {
            c->X = 0; c->P = 0; c->R[0] = 0;
            c->state = CDP1802_S_FETCH;
            c->init_pending = false;
            break;
        }

        /* IDLE: stall until DMA or interrupt breaks out. */
        if (c->idle) {
            if (c->dma_in_pending || c->dma_out_pending) {
                c->idle = false;
                flush_requests(c);
            } else if (c->irq_pending) {
                c->idle        = false;
                c->irq_pending = false;
                c->state = c->IE ? CDP1802_S_INTERRUPT : CDP1802_S_FETCH;
            }
            break;
        }

        uint8_t op = (uint8_t)((c->I << 4) | c->N);

        switch (op) {

        /* 0x00 IDLE — handled above */

        /* 0x01-0x0F  LDN Rn */
        case 0x01: case 0x02: case 0x03: case 0x04:
        case 0x05: case 0x06: case 0x07: case 0x08:
        case 0x09: case 0x0A: case 0x0B: case 0x0C:
        case 0x0D: case 0x0E: case 0x0F:
            c->D = mr(c, c->R[c->N]); break;

        /* 0x10-0x1F  INC Rn */
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:
            c->R[c->N]++; break;

        /* 0x20-0x2F  DEC Rn */
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            c->R[c->N]--; break;

        /* Short branches: replace low byte of R[P] if taken, else skip */
        case 0x30: sbranch(c); break;                                        /* BR  */
        case 0x31: if (c->Q)      sbranch(c); else c->R[c->P]++; break;     /* BQ  */
        case 0x32: if (!c->D)     sbranch(c); else c->R[c->P]++; break;     /* BZ  */
        case 0x33: if (c->DF)     sbranch(c); else c->R[c->P]++; break;     /* BDF */
        case 0x34: if (!c->EF[0]) sbranch(c); else c->R[c->P]++; break;     /* B1  */
        case 0x35: if (!c->EF[1]) sbranch(c); else c->R[c->P]++; break;     /* B2  */
        case 0x36: if (!c->EF[2]) sbranch(c); else c->R[c->P]++; break;     /* B3  */
        case 0x37: if (!c->EF[3]) sbranch(c); else c->R[c->P]++; break;     /* B4  */
        case 0x38: c->R[c->P]++; break;                                      /* SKP */
        case 0x39: if (!c->Q)     sbranch(c); else c->R[c->P]++; break;     /* BNQ */
        case 0x3A: if (c->D)      sbranch(c); else c->R[c->P]++; break;     /* BNZ */
        case 0x3B: if (!c->DF)    sbranch(c); else c->R[c->P]++; break;     /* BNF */
        case 0x3C: if (c->EF[0])  sbranch(c); else c->R[c->P]++; break;     /* BN1 */
        case 0x3D: if (c->EF[1])  sbranch(c); else c->R[c->P]++; break;     /* BN2 */
        case 0x3E: if (c->EF[2])  sbranch(c); else c->R[c->P]++; break;     /* BN3 */
        case 0x3F: if (c->EF[3])  sbranch(c); else c->R[c->P]++; break;     /* BN4 */

        /* 0x40-0x4F  LDA Rn */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            c->D = mr(c, c->R[c->N]); c->R[c->N]++; break;

        /* 0x50-0x5F  STR Rn */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            mw(c, c->R[c->N], c->D); break;

        case 0x60: c->R[c->X]++; break;   /* IRX */

        /* 0x61-0x67  OUT N (read M[R[X]], post-increment R[X], send to port) */
        case 0x61: case 0x62: case 0x63: case 0x64:
        case 0x65: case 0x66: case 0x67: {
            uint16_t a = c->R[c->X];
            uint8_t v = mr(c, a);
            c->R[c->X]++;
            c->memory_addr = a;
            if (c->io_out) c->io_out(c->N, v, c->io_ud);
            break;
        }

        /* 0x69-0x6F  INP N (read from port, write to M[R[X]] and D) */
        case 0x69: case 0x6A: case 0x6B: case 0x6C:
        case 0x6D: case 0x6E: case 0x6F: {
            uint8_t v = c->io_in ? c->io_in(c->N - 8u, c->io_ud) : 0xFFu;
            mw(c, c->R[c->X], v);
            c->D = v;
            break;
        }

        case 0x70: {   /* RET: pop X,P; enable IE */
            uint8_t b = mr(c, c->R[c->X]); c->R[c->X]++;
            c->X = b >> 4; c->P = b & 0xF; c->IE = 1;
            break;
        }
        case 0x71: {   /* DIS: pop X,P; disable IE */
            uint8_t b = mr(c, c->R[c->X]); c->R[c->X]++;
            c->X = b >> 4; c->P = b & 0xF; c->IE = 0;
            break;
        }
        case 0x72: c->D = mr(c, c->R[c->X]); c->R[c->X]++; break;   /* LDXA */
        case 0x73: mw(c, c->R[c->X], c->D); c->R[c->X]--; break;    /* STXD */

        case 0x74: c->D = add8(c, mr(c,c->R[c->X]), c->D, c->DF); break; /* ADC  */
        case 0x75: c->D = sub8(c, mr(c,c->R[c->X]), c->D, c->DF^1u); break; /* SDB */
        case 0x76: {   /* RSHR */
            uint8_t lsb = c->D & 1u;
            c->D = (c->D >> 1) | (c->DF << 7);
            c->DF = lsb; break;
        }
        case 0x77: c->D = sub8(c, c->D, mr(c,c->R[c->X]), c->DF^1u); break; /* SMB */
        case 0x78: mw(c, c->R[c->X], c->T); break;   /* SAV */
        case 0x79: {   /* MARK */
            c->T = (uint8_t)((c->X << 4) | c->P);
            mw(c, c->R[2], c->T);
            c->X = c->P;
            c->R[2]--;
            break;
        }
        case 0x7A:   /* REQ */
            c->Q = 0;
            if (c->q_out) c->q_out(0, c->io_ud);
            break;
        case 0x7B:   /* SEQ */
            c->Q = 1;
            if (c->q_out) c->q_out(1, c->io_ud);
            break;
        case 0x7C: {   /* ADCI */
            uint8_t imm = mr(c, c->R[c->P]); c->R[c->P]++;
            c->D = add8(c, imm, c->D, c->DF); break;
        }
        case 0x7D: {   /* SDBI */
            uint8_t imm = mr(c, c->R[c->P]); c->R[c->P]++;
            c->D = sub8(c, imm, c->D, c->DF^1u); break;
        }
        case 0x7E: {   /* RSHL */
            uint8_t msb = (c->D >> 7) & 1u;
            c->D = (c->D << 1) | c->DF;
            c->DF = msb; break;
        }
        case 0x7F: {   /* SMBI */
            uint8_t imm = mr(c, c->R[c->P]); c->R[c->P]++;
            c->D = sub8(c, c->D, imm, c->DF^1u); break;
        }

        /* 0x80-0x8F  GLO Rn */
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            c->D = (uint8_t)(c->R[c->N]); break;

        /* 0x90-0x9F  GHI Rn */
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            c->D = (uint8_t)(c->R[c->N] >> 8); break;

        /* 0xA0-0xAF  PLO Rn */
        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            c->R[c->N] = (c->R[c->N] & 0xFF00u) | c->D; break;

        /* 0xB0-0xBF  PHI Rn */
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            c->R[c->N] = (c->R[c->N] & 0x00FFu) | ((uint16_t)c->D << 8); break;

        /* Long branches / skips — all 0xC0..0xCF, exec_left cycles from 2→1 */
        case 0xC0: lbranch(c, true);        break;   /* LBR  */
        case 0xC1: lbranch(c, c->Q);        break;   /* LBQ  */
        case 0xC2: lbranch(c, !c->D);       break;   /* LBZ  */
        case 0xC3: lbranch(c, c->DF);       break;   /* LBDF */
        case 0xC4: break;                            /* NOP  */
        case 0xC5: if (!c->Q)   c->R[c->P]++; break; /* LSNQ  (skip if Q=0) */
        case 0xC6: if (c->D)    c->R[c->P]++; break; /* LSNZ  */
        case 0xC7: if (!c->DF)  c->R[c->P]++; break; /* LSNF  */
        case 0xC8: c->R[c->P]++; break;              /* LSKP  */
        case 0xC9: lbranch(c, !c->Q);       break;  /* LBNQ */
        case 0xCA: lbranch(c, c->D != 0);   break;  /* LBNZ */
        case 0xCB: lbranch(c, !c->DF);      break;  /* LBNF */
        case 0xCC: if (c->IE)   c->R[c->P]++; break; /* LSIE  */
        case 0xCD: if (c->Q)    c->R[c->P]++; break; /* LSQ   */
        case 0xCE: if (!c->D)   c->R[c->P]++; break; /* LSZ   */
        case 0xCF: if (c->DF)   c->R[c->P]++; break; /* LSDF  */

        /* 0xD0-0xDF  SEP Rn */
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            c->P = c->N; break;

        /* 0xE0-0xEF  SEX Rn */
        case 0xE0: case 0xE1: case 0xE2: case 0xE3:
        case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB:
        case 0xEC: case 0xED: case 0xEE: case 0xEF:
            c->X = c->N; break;

        case 0xF0: c->D  = mr(c, c->R[c->X]); break;                 /* LDX  */
        case 0xF1: c->D |= mr(c, c->R[c->X]); break;                 /* OR   */
        case 0xF2: c->D &= mr(c, c->R[c->X]); break;                 /* AND  */
        case 0xF3: c->D ^= mr(c, c->R[c->X]); break;                 /* XOR  */
        case 0xF4: c->D = add8(c, mr(c,c->R[c->X]), c->D, 0); break; /* ADD  */
        case 0xF5: c->D = sub8(c, mr(c,c->R[c->X]), c->D, 0); break; /* SD   */
        case 0xF6: { uint8_t b = c->D&1u; c->DF=b; c->D>>=1; break; } /* SHR */
        case 0xF7: c->D = sub8(c, c->D, mr(c,c->R[c->X]), 0); break; /* SM   */
        case 0xF8: c->D  = mr(c, c->R[c->P]); c->R[c->P]++; break;   /* LDI  */
        case 0xF9: c->D |= mr(c, c->R[c->P]); c->R[c->P]++; break;   /* ORI  */
        case 0xFA: c->D &= mr(c, c->R[c->P]); c->R[c->P]++; break;   /* ANI  */
        case 0xFB: c->D ^= mr(c, c->R[c->P]); c->R[c->P]++; break;   /* XRI  */
        case 0xFC: { uint8_t i=mr(c,c->R[c->P]); c->R[c->P]++; c->D=add8(c,i,c->D,0); break; } /* ADI */
        case 0xFD: { uint8_t i=mr(c,c->R[c->P]); c->R[c->P]++; c->D=sub8(c,i,c->D,0); break; } /* SDI */
        case 0xFE: { uint8_t b=(c->D>>7)&1u; c->DF=b; c->D<<=1; break; } /* SHL */
        case 0xFF: { uint8_t i=mr(c,c->R[c->P]); c->R[c->P]++; c->D=sub8(c,c->D,i,0); break; } /* SMI */

        default: cdp1802_panic(c, "unsupported opcode"); break;
        }

        c->exec_left--;
        if (!c->exec_left) {
            c->insn_count++;
            flush_requests(c);
        }
        break;
    }

    /* ── DMA ── */
    case CDP1802_S_DMA: {
        Cdp1802DmaEntry *e = &c->dma_queue[c->dma_head];
        uint8_t byte = 0;
        if (e->is_out) {
            byte = mr(c, c->R[0]);
            e->cb(&byte, e->ud);
        } else {
            e->cb(&byte, e->ud);
            mw(c, c->R[0], byte);
            c->D = byte;
        }
        c->R[0]++;
        c->dma_head = (c->dma_head + 1u) & CDP1802_DMA_QUEUE_MASK;
        c->dma_count--;

        if (!c->dma_count)
            flush_requests(c);
        break;
    }

    /* ── Interrupt ── */
    case CDP1802_S_INTERRUPT: {
        c->T  = (uint8_t)((c->X << 4) | c->P);
        c->X  = 2;
        c->P  = 1;
        c->IE = 0;
        c->state = CDP1802_S_FETCH;
        break;
    }
    default:
        cdp1802_panic(c, "invalid CPU cycle state");
        break;
    }
}
