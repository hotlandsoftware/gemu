/*
 * Merced IA-32 compatibility execution.
 *
 * The architectural state mapping and decoder behavior follow HP Ski's
 * ia_state.h, ia_decoder.tmpl.c, ia_read.c, ia_write.c and ia_exec.c.  This
 * implementation is intentionally integrated with GEMU's Merced MMU/bus
 * instead of importing Ski's process-global simulator state.
 */
#include "merced.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { X_ES, X_CS, X_SS, X_DS, X_FS, X_GS };
enum { FL_CF = 1u << 0, FL_PF = 1u << 2, FL_AF = 1u << 4,
       FL_ZF = 1u << 6, FL_SF = 1u << 7, FL_IF = 1u << 9,
       FL_DF = 1u << 10, FL_OF = 1u << 11, FL_VM = 1u << 17 };

typedef struct {
    Merced *m;
    uint32_t start, pc;
    unsigned op32, addr32;
    int seg_override;
    unsigned rep;
} X86;

typedef struct {
    bool is_reg;
    unsigned reg;
    uint32_t addr;
} RM;

static MercedStatus xhalt(X86 *x, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->m->halt_msg, sizeof(x->m->halt_msg), fmt, ap);
    va_end(ap);
    x->m->halt_ip = x->start;
    return MERCED_HALT_UNIMPL;
}

static uint64_t gr(X86 *x, unsigned r) {
    return merced_ia32_gr_read(x->m, r);
}
static void setgr(X86 *x, unsigned r, uint64_t v) {
    merced_ia32_gr_write(x->m, r, v);
}
static uint32_t xr(X86 *x, unsigned r, unsigned size) {
    uint64_t v = gr(x, 8 + (r & 7));
    if (size == 1 && r >= 4) return (uint8_t)(gr(x, 8 + r - 4) >> 8);
    if (size == 1) return (uint8_t)v;
    if (size == 2) return (uint16_t)v;
    return (uint32_t)v;
}
static void setxr(X86 *x, unsigned r, unsigned size, uint32_t v) {
    unsigned ar = r;
    unsigned shift = 0;
    if (size == 1 && r >= 4) { ar = r - 4; shift = 8; }
    uint64_t old = gr(x, 8 + ar), mask;
    if (size == 4) mask = UINT32_MAX;
    else mask = ((1ull << (size * 8)) - 1) << shift;
    setgr(x, 8 + ar, (old & ~mask) | (((uint64_t)v << shift) & mask));
}
static uint16_t sel(X86 *x, unsigned s) { return (uint16_t)gr(x, 16 + s); }
static uint32_t sbase(X86 *x, unsigned s) { return (uint32_t)gr(x, 24 + s); }
static void setseg_real(X86 *x, unsigned s, uint16_t v) {
    setgr(x, 16 + s, (gr(x, 16 + s) & ~UINT64_C(0xffff)) | v);
    uint64_t d = gr(x, 24 + s);
    d = (d & ~UINT64_C(0x000fffffffffffff)) | ((uint32_t)v << 4) |
        (UINT64_C(0xffff) << 32);
    setgr(x, 24 + s, d);
}
static uint32_t eflags(X86 *x) { return (uint32_t)x->m->ar[24]; }
static void setflags(X86 *x, uint32_t f) {
    x->m->ar[24] = (x->m->ar[24] & ~UINT64_C(0xffffffff)) | f | 2;
}

static bool rb(X86 *x, uint32_t a, unsigned size, bool fetch, uint32_t *v) {
    uint64_t q;
    if (!merced_ia32_read(x->m, a, size, fetch, &q)) return false;
    *v = (uint32_t)q;
    return true;
}
static bool wb(X86 *x, uint32_t a, unsigned size, uint32_t v) {
    return merced_ia32_write(x->m, a, size, v);
}
static bool fetch(X86 *x, unsigned size, uint32_t *v) {
    if (!rb(x, x->pc, size, true, v)) return false;
    x->pc += size;
    return true;
}

static uint32_t stack_off(X86 *x) {
    return (gr(x, 26) >> 62) & 1 ? xr(x, 4, 4) : xr(x, 4, 2);
}
static void set_stack_off(X86 *x, uint32_t v) {
    setxr(x, 4, ((gr(x, 26) >> 62) & 1) ? 4 : 2, v);
}
static bool push(X86 *x, unsigned size, uint32_t v) {
    uint32_t sp = stack_off(x) - size;
    set_stack_off(x, sp);
    return wb(x, sbase(x, X_SS) + sp, size, v);
}
static bool pop(X86 *x, unsigned size, uint32_t *v) {
    uint32_t sp = stack_off(x);
    if (!rb(x, sbase(x, X_SS) + sp, size, false, v)) return false;
    set_stack_off(x, sp + size);
    return true;
}

static bool decode_rm(X86 *x, uint8_t modrm, RM *rm) {
    unsigned mod = modrm >> 6, r = modrm & 7;
    if (mod == 3) { rm->is_reg = true; rm->reg = r; return true; }
    rm->is_reg = false;
    unsigned seg = x->seg_override >= 0 ? (unsigned)x->seg_override : X_DS;
    int32_t disp = 0;
    if (!x->addr32) {
        static const int8_t base[8] = {3,3,5,5,6,7,-1,3};
        static const int8_t index[8] = {6,7,6,7,-1,-1,-1,-1};
        if ((r >= 2 && r <= 3) || (r == 6 && mod != 0)) seg = X_SS;
        uint32_t off = base[r] < 0 ? 0 : xr(x, base[r], 2);
        if (index[r] >= 0) off += xr(x, index[r], 2);
        uint32_t q;
        if (mod == 0 && r == 6) { if (!fetch(x, 2, &q)) return false; off = q; }
        else if (mod == 1) { if (!fetch(x, 1, &q)) return false; disp = (int8_t)q; }
        else if (mod == 2) { if (!fetch(x, 2, &q)) return false; disp = (int16_t)q; }
        rm->addr = sbase(x, seg) + (uint16_t)(off + disp);
        return true;
    }
    uint32_t off = 0, q;
    if (r == 4) {
        if (!fetch(x, 1, &q)) return false;
        unsigned ss = q >> 6, idx = (q >> 3) & 7, b = q & 7;
        if (idx != 4) off += xr(x, idx, 4) << ss;
        if (b == 5 && mod == 0) { if (!fetch(x, 4, &off)) return false; }
        else { off += xr(x, b, 4); if (b == 4 || b == 5) seg = X_SS; }
    } else if (r == 5 && mod == 0) { if (!fetch(x, 4, &off)) return false; }
    else { off = xr(x, r, 4); if (r == 4 || r == 5) seg = X_SS; }
    if (mod == 1) { if (!fetch(x, 1, &q)) return false; disp = (int8_t)q; }
    else if (mod == 2) { if (!fetch(x, 4, &q)) return false; disp = (int32_t)q; }
    rm->addr = sbase(x, seg) + off + disp;
    return true;
}
static bool rmread(X86 *x, RM rm, unsigned size, uint32_t *v) {
    return rm.is_reg ? (*v = xr(x, rm.reg, size), true)
                     : rb(x, rm.addr, size, false, v);
}
static bool rmwrite(X86 *x, RM rm, unsigned size, uint32_t v) {
    if (rm.is_reg) { setxr(x, rm.reg, size, v); return true; }
    return wb(x, rm.addr, size, v);
}

static void logic_flags(X86 *x, uint32_t v, unsigned size) {
    uint32_t f = eflags(x) & ~(FL_CF|FL_PF|FL_AF|FL_ZF|FL_SF|FL_OF);
    uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
    v &= mask;
    if (!v) f |= FL_ZF;
    if (v & (1u << (size * 8 - 1))) f |= FL_SF;
    if (!(__builtin_parity(v & 0xff))) f |= FL_PF;
    setflags(x, f);
}

static void sub_flags(X86 *x, uint32_t a, uint32_t b, uint32_t v,
                      unsigned size) {
    uint64_t mask = size == 4 ? UINT32_MAX : (UINT64_C(1) << (size * 8)) - 1;
    uint32_t sign = 1u << (size * 8 - 1);
    uint32_t f = eflags(x) & ~(FL_CF|FL_PF|FL_AF|FL_ZF|FL_SF|FL_OF);
    a &= mask; b &= mask; v &= mask;
    if (a < b) f |= FL_CF;
    if ((a ^ b) & (a ^ v) & sign) f |= FL_OF;
    if ((a ^ b ^ v) & 0x10) f |= FL_AF;
    if (!v) f |= FL_ZF;
    if (v & sign) f |= FL_SF;
    if (!__builtin_parity(v & 0xff)) f |= FL_PF;
    setflags(x, f);
}

static void add_flags(X86 *x, uint32_t a, uint32_t b, uint32_t v,
                      unsigned size) {
    uint64_t mask = size == 4 ? UINT32_MAX : (UINT64_C(1) << (size * 8)) - 1;
    uint32_t sign = 1u << (size * 8 - 1);
    uint32_t f = eflags(x) & ~(FL_CF|FL_PF|FL_AF|FL_ZF|FL_SF|FL_OF);
    a &= mask; b &= mask; v &= mask;
    if ((uint64_t)a + b > mask) f |= FL_CF;
    if (~(a ^ b) & (a ^ v) & sign) f |= FL_OF;
    if ((a ^ b ^ v) & 0x10) f |= FL_AF;
    if (!v) f |= FL_ZF;
    if (v & sign) f |= FL_SF;
    if (!__builtin_parity(v & 0xff)) f |= FL_PF;
    setflags(x, f);
}

static bool condition(X86 *x, unsigned c) {
    uint32_t f = eflags(x);
    bool cf=f&FL_CF, pf=f&FL_PF, zf=f&FL_ZF, sf=f&FL_SF, of=f&FL_OF;
    switch (c & 15) {
    case 0: return of;       case 1: return !of;
    case 2: return cf;       case 3: return !cf;
    case 4: return zf;       case 5: return !zf;
    case 6: return cf||zf;   case 7: return !cf&&!zf;
    case 8: return sf;       case 9: return !sf;
    case 10:return pf;       case 11:return !pf;
    case 12:return sf!=of;   case 13:return sf==of;
    case 14:return zf||sf!=of; default:return !zf&&sf==of;
    }
}

MercedStatus merced_ia32_step(Merced *m) {
    X86 x = {.m=m, .start=(uint32_t)m->ip, .pc=(uint32_t)m->ip,
             .seg_override=-1};
    unsigned d = (unsigned)((gr(&x, 25) >> 62) & 1);
    x.op32 = x.addr32 = d;
    uint32_t q;
    uint8_t op;
    for (;;) {
        if (!fetch(&x, 1, &q)) {
            /* The shared Merced MMU has already delivered a successful
             * IA-32 translation fault into the IA-64 IVT. */
            if (!(m->psr & (UINT64_C(1) << 34))) return MERCED_OK;
            return xhalt(&x, "undeliverable IA-32 fetch fault at %08X", x.pc);
        }
        op = (uint8_t)q;
        if (op == 0x66) { x.op32 ^= 1; continue; }
        if (op == 0x67) { x.addr32 ^= 1; continue; }
        if (op == 0x26) { x.seg_override=X_ES; continue; }
        if (op == 0x2e) { x.seg_override=X_CS; continue; }
        if (op == 0x36) { x.seg_override=X_SS; continue; }
        if (op == 0x3e) { x.seg_override=X_DS; continue; }
        if (op == 0x64) { x.seg_override=X_FS; continue; }
        if (op == 0x65) { x.seg_override=X_GS; continue; }
        if (op == 0xf0) continue;
        if (op == 0xf2 || op == 0xf3) { x.rep=op; continue; }
        break;
    }
    unsigned size = x.op32 ? 4 : 2;
    bool branch = false;

    unsigned hi = m->trace_history_next++ % MERCED_TRACE_HISTORY;
    m->trace_history[hi].ip = x.start;
    m->trace_history[hi].raw = op;
    m->trace_history[hi].src2 = gr(&x, 8);
    m->trace_history[hi].src3 = eflags(&x);
    m->trace_history[hi].r25 = gr(&x, 25);
    m->trace_history[hi].b0 = m->br[0];
    m->trace_history[hi].unit = 'x';
    m->trace_history[hi].qp = 0;

    if (op == 0x90) { /* nop */ }
    else if ((op <= 0x03) || (op >= 0x08 && op <= 0x0b) ||
             (op >= 0x20 && op <= 0x23) || (op >= 0x28 && op <= 0x2b) ||
             (op >= 0x30 && op <= 0x33) || (op >= 0x38 && op <= 0x3b)) {
        unsigned n = (op & 1) ? size : 1;
        bool reg_dest = (op & 2) != 0;
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if (!decode_rm(&x,mr,&rm)) goto fault;
        unsigned r=(mr>>3)&7; uint32_t rv=xr(&x,r,n), mv;
        if (!rmread(&x,rm,n,&mv)) goto fault;
        uint32_t a=reg_dest?rv:mv, b=reg_dest?mv:rv, v;
        unsigned family=op&0xf8;
        if (family==0x00) { v=a+b; add_flags(&x,a,b,v,n); }
        else if (family==0x08) { v=a|b; logic_flags(&x,v,n); }
        else if (family==0x20) { v=a&b; logic_flags(&x,v,n); }
        else if (family==0x28 || family==0x38) { v=a-b; sub_flags(&x,a,b,v,n); }
        else { v=a^b; logic_flags(&x,v,n); }
        if (family!=0x38) {
            if (reg_dest) setxr(&x,r,n,v);
            else if (!rmwrite(&x,rm,n,v)) goto fault;
        }
    }
    else if (op >= 0xb8 && op <= 0xbf) {
        if (!fetch(&x, size, &q)) goto fault;
        setxr(&x, op - 0xb8, size, q);
    } else if (op >= 0x50 && op <= 0x57) {
        if (!push(&x, size, xr(&x, op - 0x50, size))) goto fault;
    } else if (op >= 0x58 && op <= 0x5f) {
        if (!pop(&x, size, &q)) goto fault;
        setxr(&x, op - 0x58, size, q);
    } else if (op == 0x06 || op == 0x0e || op == 0x16 || op == 0x1e) {
        unsigned s = op == 0x06 ? X_ES : op == 0x0e ? X_CS : op == 0x16 ? X_SS : X_DS;
        if (!push(&x, 2, sel(&x, s))) goto fault;
    } else if (op == 0x07 || op == 0x17 || op == 0x1f) {
        unsigned s = op == 0x07 ? X_ES : op == 0x17 ? X_SS : X_DS;
        if (!pop(&x, 2, &q)) goto fault;
        setseg_real(&x, s, q);
    } else if (op == 0x60 || op == 0x61) {
        uint32_t oldsp = stack_off(&x), vals[8];
        if (op == 0x60) {
            for (unsigned i=0;i<8;i++) vals[i]=xr(&x,i,size);
            for (int i=0;i<8;i++) if (!push(&x,size,i==4?oldsp:vals[i])) goto fault;
        } else {
            for (int i=7;i>=0;i--) if (!pop(&x,size,&vals[i])) goto fault;
            for (unsigned i=0;i<8;i++) if (i!=4) setxr(&x,i,size,vals[i]);
        }
    } else if (op == 0x9c) {
        if (!push(&x, size, eflags(&x))) goto fault;
    } else if (op == 0x9d) {
        if (!pop(&x, size, &q)) goto fault;
        setflags(&x, q);
    } else if (op == 0xfc) setflags(&x, eflags(&x) & ~FL_DF);
    else if (op == 0xfd) setflags(&x, eflags(&x) | FL_DF);
    else if (op >= 0x70 && op <= 0x7f) {
        if (!fetch(&x,1,&q)) goto fault;
        if (condition(&x,op&15)) { x.pc += (int8_t)q; branch=true; }
    } else if (op == 0xeb || op == 0xe9) {
        if (!fetch(&x, op==0xeb?1:size, &q)) goto fault;
        int32_t rel = op==0xeb ? (int8_t)q : size==2 ? (int16_t)q : (int32_t)q;
        x.pc += rel; branch=true;
    } else if (op == 0xe8) {
        if (!fetch(&x,size,&q) || !push(&x,size,x.pc-sbase(&x,X_CS))) goto fault;
        x.pc += size==2?(int16_t)q:(int32_t)q; branch=true;
    } else if (op == 0xea) {
        uint32_t off, cs;
        if (!fetch(&x,size,&off) || !fetch(&x,2,&cs)) goto fault;
        setseg_real(&x,X_CS,cs); x.pc=sbase(&x,X_CS)+(off&(size==2?0xffff:UINT32_MAX)); branch=true;
    } else if (op == 0xc3 || op == 0xcb) {
        uint32_t off, cs;
        if (!pop(&x,size,&off)) goto fault;
        if (op==0xcb) { if (!pop(&x,2,&cs)) goto fault; setseg_real(&x,X_CS,cs); }
        x.pc=sbase(&x,X_CS)+(off&(size==2?0xffff:UINT32_MAX)); branch=true;
    } else if (op == 0x8b || op == 0x89 || op == 0x8c || op == 0x8e) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if (!decode_rm(&x,mr,&rm)) goto fault;
        unsigned reg=(mr>>3)&7;
        if (op==0x8b) { if(!rmread(&x,rm,size,&q))goto fault; setxr(&x,reg,size,q); }
        else if(op==0x89) { if(!rmwrite(&x,rm,size,xr(&x,reg,size)))goto fault; }
        else if(op==0x8c) { if(reg>5||!rmwrite(&x,rm,2,sel(&x,reg)))goto fault; }
        else { if(reg>5||!rmread(&x,rm,2,&q))goto fault; setseg_real(&x,reg,q); }
    } else if (op == 0x3c) {
        if (!fetch(&x,1,&q)) goto fault;
        uint32_t a=xr(&x,0,1), v=a-(uint8_t)q;
        sub_flags(&x,a,(uint8_t)q,v,1);
    } else if (op == 0xc6 || op == 0xc7) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm;
        if (((mr>>3)&7)!=0 || !decode_rm(&x,mr,&rm)) goto fault;
        unsigned n=op==0xc6?1:size;
        if (!fetch(&x,n,&q) || !rmwrite(&x,rm,n,q)) goto fault;
    } else if (op == 0x81 || op == 0x83) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm;
        if(!decode_rm(&x,mr,&rm))goto fault;
        unsigned sub=(mr>>3)&7;
        uint32_t a,imm; if(!rmread(&x,rm,size,&a)||!fetch(&x,op==0x83?1:size,&imm))goto fault;
        if(op==0x83) imm=(uint32_t)(int32_t)(int8_t)imm;
        uint32_t v; if(sub==0)v=a+imm; else if(sub==5)v=a-imm;
        else return xhalt(&x,"IA-32 group1 /%u unimplemented at %08X",sub,x.start);
        if(!rmwrite(&x,rm,size,v))goto fault;
        if(sub==5) sub_flags(&x,a,imm,v,size); else add_flags(&x,a,imm,v,size);
    } else if (op == 0xa4 || op == 0xa5) {
        unsigned n=op==0xa4?1:size; uint32_t count=x.rep?xr(&x,1,x.addr32?4:2):1;
        int step=(eflags(&x)&FL_DF)?-(int)n:(int)n;
        while(count--){uint32_t v,si=xr(&x,6,x.addr32?4:2),di=xr(&x,7,x.addr32?4:2);
            unsigned sg=x.seg_override>=0?(unsigned)x.seg_override:X_DS;
            if(!rb(&x,sbase(&x,sg)+si,n,false,&v)||!wb(&x,sbase(&x,X_ES)+di,n,v))goto fault;
            setxr(&x,6,x.addr32?4:2,si+step);setxr(&x,7,x.addr32?4:2,di+step);}
        if(x.rep)setxr(&x,1,x.addr32?4:2,0);
    } else if (op == 0xff) {
        if(!fetch(&x,1,&q))goto fault;
        uint8_t mr=q;RM rm;
        if(!decode_rm(&x,mr,&rm))goto fault;
        unsigned sub=(mr>>3)&7;
        if(sub==3 && !rm.is_reg){uint32_t off,cs;if(!rb(&x,rm.addr,size,false,&off)||!rb(&x,rm.addr+size,2,false,&cs))goto fault;
            if(!push(&x,2,sel(&x,X_CS))||!push(&x,size,x.pc-sbase(&x,X_CS)))goto fault;
            setseg_real(&x,X_CS,cs);x.pc=sbase(&x,X_CS)+(off&(size==2?0xffff:UINT32_MAX));branch=true;}
        else return xhalt(&x,"IA-32 FF /%u unimplemented at %08X",sub,x.start);
    } else if (op == 0x0f) {
        uint32_t op2;if(!fetch(&x,1,&op2))goto fault;
        if(op2==0x00){uint32_t mr;if(!fetch(&x,1,&mr))goto fault;
            if(((mr>>3)&7)==6){RM rm;if(!decode_rm(&x,mr,&rm)||!rmread(&x,rm,size,&q))goto fault;
                /* JMPE: leave IA-32 mode and resume at an aligned IA-64 IP. */
                m->ip=(sbase(&x,X_CS)+q)&UINT32_C(0xfffffff0);m->psr&=~(UINT64_C(1)<<34);m->psr&=~(UINT64_C(3)<<41);m->taken=1;m->ninsts++;return MERCED_OK;}
        }
        return xhalt(&x,"IA-32 0F %02X unimplemented at %08X",op2,x.start);
    } else return xhalt(&x,"IA-32 opcode %02X unimplemented at %08X",op,x.start);

    (void)branch;
    m->ip=x.pc; m->ninsts++; return MERCED_OK;
fault:
    if (!(m->psr & (UINT64_C(1) << 34))) return MERCED_OK;
    return xhalt(&x,"IA-32 memory fault at %08X (instruction %08X)",x.pc,x.start);
}
