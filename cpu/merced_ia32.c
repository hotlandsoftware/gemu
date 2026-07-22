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
static uint64_t ioaddr(X86 *x, uint16_t port) {
    return x->m->ar[0] | ((uint64_t)(port & 0xfffc) << 10) |
           (port & 0x0fff);
}
static bool ioread(X86 *x, uint16_t port, unsigned size, uint32_t *v) {
    uint64_t q;
    if (!merced_ia32_read(x->m, ioaddr(x, port), size, false, &q)) return false;
    *v = (uint32_t)q;
    return true;
}
static bool iowrite(X86 *x, uint16_t port, unsigned size, uint32_t v) {
    return merced_ia32_write(x->m, ioaddr(x, port), size, v);
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
        static const int8_t base[8] = {3,3,5,5,6,7,5,3};
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

static void incdec_flags(X86 *x, uint32_t a, uint32_t b, uint32_t v,
                         unsigned size, bool sub) {
    uint32_t cf = eflags(x) & FL_CF;
    if (sub) sub_flags(x,a,b,v,size); else add_flags(x,a,b,v,size);
    setflags(x,(eflags(x) & ~FL_CF) | cf);
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

/* Near control transfers update EIP, not the already-segmented linear IP.
 * In a 16-bit code segment the result wraps at 16 bits before CSD.base is
 * added.  Treating x.pc as a flat address made backward calls crossing the
 * segment base jump below CS (for example F000:F50FE -> physical EF019
 * instead of the intended F000:0019). */
static uint32_t near_target(X86 *x, int32_t rel) {
    uint32_t off = x->pc - sbase(x, X_CS);
    off += (uint32_t)rel;
    if (!x->op32) off &= UINT16_MAX;
    return sbase(x, X_CS) + off;
}

/* ---- Minimal INT 10h video-services shim ----
 *
 * Real Itanium firmware runs a legacy x86 video BIOS option ROM through
 * the IA-32 Execution Layer specifically so NT's boot-time text output
 * (including bugcheck/blue-screen text) can call int 10h - a real,
 * documented convention on actual Itanium hardware, not an x86-only
 * quirk. GEMU has no such ROM to run, so int 10h is intercepted directly
 * here (the same "shim the call, don't emulate the real firmware"
 * pattern already used for SAL/PAL) instead of dispatching through the
 * real-mode IVT into handler code that doesn't exist, implementing the
 * handful of functions boot-time text output actually needs against the
 * real VGA registers/memory the rest of this file already talks to.
 *
 * VGA ports live behind the generic machine's own MMIO window
 * (GENERIC_VGA_IO_BASE in hardware/generic.h) rather than the standard
 * IA-64 sparse I/O port space x86 in/out normally routes through - this
 * shim reads/writes them (and the BDA/text VRAM, both addressed exactly
 * like the existing int/iret code already addresses the real-mode IVT:
 * flat, unsegmented) directly for that reason. The field layout used to
 * compute the active screen size (VGA_CRTC_H_DISP=1, VGA_CRTC_OVERFLOW=7,
 * VGA_CRTC_MAX_SCAN=9, VGA_CRTC_V_DISP_END=0x12) matches
 * reference/qemu-system-ia64's hw/display/vga.c vga_get_resolution(),
 * not a hardcoded 80x25. */
#define INT10_VGA_IO_BASE 0xC0000000ull  /* GENERIC_VGA_IO_BASE */
#define INT10_VRAM_BASE   0xB8000ull
#define INT10_BDA_CURSOR  0x450ull       /* word per page: (row<<8)|col */
#define INT10_BDA_MODE    0x449ull

static uint32_t int10_crtc_read(X86 *x, uint8_t index) {
    uint32_t v = 0;
    wb(x, INT10_VGA_IO_BASE + (0x3D4 - 0x3B0), 1, index);
    rb(x, INT10_VGA_IO_BASE + (0x3D5 - 0x3B0), 1, false, &v);
    return v;
}

static void int10_screen_dims(X86 *x, unsigned *cols, unsigned *rows) {
    unsigned h_disp = int10_crtc_read(x, 0x01);
    unsigned max_scan = (int10_crtc_read(x, 0x09) & 0x1F) + 1;
    unsigned ov = int10_crtc_read(x, 0x07);
    unsigned vdisp = int10_crtc_read(x, 0x12) |
                     ((ov & 0x02) << 7) | ((ov & 0x40) << 3);
    *cols = h_disp + 1;
    if (!*cols || *cols > 132) *cols = 80;
    if (!max_scan) max_scan = 16;
    *rows = (vdisp + 1) / max_scan;
    if (!*rows || *rows > 60) *rows = 25;
}

static void int10_get_cursor(X86 *x, unsigned page, unsigned *row, unsigned *col) {
    uint32_t v = 0;
    rb(x, INT10_BDA_CURSOR + page * 2, 2, false, &v);
    *col = v & 0xFF; *row = (v >> 8) & 0xFF;
}
static void int10_set_cursor(X86 *x, unsigned page, unsigned row, unsigned col) {
    wb(x, INT10_BDA_CURSOR + page * 2, 2, ((row & 0xFF) << 8) | (col & 0xFF));
}

static void int10_scroll(X86 *x, uint32_t base, unsigned cols, unsigned rows,
                         uint8_t attr) {
    for (unsigned r = 1; r < rows; r++)
        for (unsigned c = 0; c < cols; c++) {
            uint32_t v = 0;
            rb(x, base + (r * cols + c) * 2, 2, false, &v);
            wb(x, base + ((r - 1) * cols + c) * 2, 2, v);
        }
    for (unsigned c = 0; c < cols; c++)
        wb(x, base + ((rows - 1) * cols + c) * 2, 2,
           ((uint32_t)attr << 8) | ' ');
}

/* Writes one character at the current cursor position (teletype-style:
 * handles CR/LF/BS/bell and end-of-line/end-of-screen wraparound),
 * advances the cursor, and scrolls if it ran off the bottom. with_attr
 * false leaves the existing attribute byte alone (matches real int 10h
 * AH=0Eh, which never touches attributes). */
static void int10_putc(X86 *x, unsigned page, uint8_t ch, uint8_t attr,
                       bool with_attr) {
    unsigned cols, rows, row, col;
    int10_screen_dims(x, &cols, &rows);
    int10_get_cursor(x, page, &row, &col);
    uint32_t base = INT10_VRAM_BASE + page * 0x1000;
    if (ch == '\r') {
        col = 0;
    } else if (ch == '\n') {
        row++;
    } else if (ch == 8) {
        if (col) col--;
    } else if (ch == 7) {
        /* bell: nothing to do without an audio device */
    } else {
        uint32_t off = base + (uint32_t)(row * cols + col) * 2;
        wb(x, off, 1, ch);
        if (with_attr) wb(x, off + 1, 1, attr);
        col++;
        if (col >= cols) { col = 0; row++; }
    }
    if (row >= rows) {
        int10_scroll(x, base, cols, rows, attr);
        row = rows - 1;
    }
    int10_set_cursor(x, page, row, col);
}

static void int10_handler(X86 *x) {
    unsigned ah = xr(x, 4, 1), al = xr(x, 0, 1) & 0xFF;
    static unsigned int10_debug;
    if (int10_debug++ < 40)
        fprintf(stderr, "merced: INT10 ah=%02x al=%02x ip=%08x ninsts=%"
                PRIu64 "\n", ah, al, x->start, x->m->ninsts);
    switch (ah) {
    case 0x0E:                                      /* teletype output */
        int10_putc(x, 0, (uint8_t)al, 0x07, false);
        break;
    case 0x13: {                                    /* write string */
        unsigned mode = al & 3;
        unsigned page = xr(x, 7, 1) & 7;             /* BH */
        uint8_t attr = (uint8_t)xr(x, 3, 1);         /* BL */
        unsigned cnt = xr(x, 1, 2);                  /* CX */
        unsigned row = xr(x, 6, 1), col = xr(x, 2, 1); /* DH, DL */
        uint32_t str_addr = sbase(x, X_ES) + xr(x, 5, 2); /* ES:BP */
        int10_set_cursor(x, page, row, col);
        for (unsigned i = 0; i < cnt; i++) {
            uint32_t ch = 0, at = attr;
            if (!rb(x, str_addr++, 1, false, &ch)) break;
            if (mode & 2) {
                uint32_t a = 0;
                rb(x, str_addr++, 1, false, &a);
                at = a;
            }
            int10_putc(x, page, (uint8_t)ch, (uint8_t)at, true);
        }
        break;
    }
    case 0x02: {                                    /* set cursor position */
        unsigned page = xr(x, 7, 1) & 7;             /* BH */
        int10_set_cursor(x, page, xr(x, 6, 1), xr(x, 2, 1)); /* DH, DL */
        break;
    }
    case 0x03: {                                    /* get cursor position */
        unsigned page = xr(x, 7, 1) & 7, row, col;   /* BH */
        int10_get_cursor(x, page, &row, &col);
        setxr(x, 6, 1, row);                         /* DH */
        setxr(x, 2, 1, col);                          /* DL */
        setxr(x, 1, 2, 0x0607);                       /* CX: cursor shape */
        break;
    }
    case 0x0F: {                                    /* get video mode */
        unsigned cols, rows;
        int10_screen_dims(x, &cols, &rows);
        uint32_t mode = 0;
        rb(x, INT10_BDA_MODE, 1, false, &mode);
        setxr(x, 0, 1, mode ? mode : 3);              /* AL */
        setxr(x, 4, 1, cols);                         /* AH = columns */
        setxr(x, 7, 1, 0);                            /* BH = page 0 */
        break;
    }
    case 0x00:                                       /* set video mode */
        /* Not implemented: the mode software actually wants is already
         * established by the time this shim intercepts int 10h (real
         * mode-set register tables aren't needed for boot-time text
         * output), so this is a deliberate no-op rather than a guess at
         * hardware register values. Revisit if a real mode switch turns
         * out to be load-bearing. */
        wb(x, INT10_BDA_MODE, 1, al);
        break;
    default:
        /* Unimplemented function: no-op rather than halting, so a rare
         * call here doesn't take down an otherwise-working boot. */
        break;
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
    else if (op >= 0x91 && op <= 0x97) {
        unsigned r=op&7; uint32_t a=xr(&x,0,size), b=xr(&x,r,size);
        setxr(&x,0,size,b); setxr(&x,r,size,a);
    }
    else if ((op & 6) == 4 && (op <= 0x0d ||
             (op >= 0x20 && op <= 0x3d))) {
        unsigned n=(op&1)?size:1; uint32_t imm,a=xr(&x,0,n),v;
        if(!fetch(&x,n,&imm))goto fault;
        unsigned family=op&0x38;
        if(family==0x00){v=a+imm;add_flags(&x,a,imm,v,n);}
        else if(family==0x08){v=a|imm;logic_flags(&x,v,n);}
        else if(family==0x20){v=a&imm;logic_flags(&x,v,n);}
        else if(family==0x28||family==0x38){v=a-imm;sub_flags(&x,a,imm,v,n);}
        else {v=a^imm;logic_flags(&x,v,n);}
        if(family!=0x38)setxr(&x,0,n,v);
    }
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
    else if (op >= 0xb0 && op <= 0xb7) {
        if (!fetch(&x, 1, &q)) goto fault;
        setxr(&x, op - 0xb0, 1, q);
    } else if (op >= 0xb8 && op <= 0xbf) {
        if (!fetch(&x, size, &q)) goto fault;
        setxr(&x, op - 0xb8, size, q);
    } else if (op >= 0x50 && op <= 0x57) {
        if (!push(&x, size, xr(&x, op - 0x50, size))) goto fault;
    } else if (op == 0x68 || op == 0x6a) {
        if (!fetch(&x,op==0x68?size:1,&q)) goto fault;
        if (op==0x6a) q=(uint32_t)(int32_t)(int8_t)q;
        if (!push(&x,size,q)) goto fault;
    } else if (op >= 0x58 && op <= 0x5f) {
        if (!pop(&x, size, &q)) goto fault;
        setxr(&x, op - 0x58, size, q);
    } else if (op >= 0x40 && op <= 0x4f) {
        unsigned r=op&7; uint32_t a=xr(&x,r,size);
        bool sub=op>=0x48; uint32_t v=sub?a-1:a+1;
        setxr(&x,r,size,v); incdec_flags(&x,a,1,v,size,sub);
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
        if (condition(&x,op&15)) { x.pc=near_target(&x,(int8_t)q); branch=true; }
    } else if (op >= 0xe0 && op <= 0xe3) {
        if (!fetch(&x,1,&q)) goto fault;
        unsigned n=x.addr32?4:2; uint32_t count=xr(&x,1,n);
        bool take;
        if(op==0xe3) take=count==0;
        else {
            count--; setxr(&x,1,n,count);
            take=count!=0 && (op==0xe2 || (op==0xe1)==!!(eflags(&x)&FL_ZF));
        }
        if (take) {
            x.pc=near_target(&x,(int8_t)q); branch=true;
        }
    } else if (op == 0xeb || op == 0xe9) {
        if (!fetch(&x, op==0xeb?1:size, &q)) goto fault;
        int32_t rel = op==0xeb ? (int8_t)q : size==2 ? (int16_t)q : (int32_t)q;
        x.pc=near_target(&x,rel); branch=true;
    } else if (op == 0xe8) {
        if (!fetch(&x,size,&q) || !push(&x,size,x.pc-sbase(&x,X_CS))) goto fault;
        x.pc=near_target(&x,size==2?(int16_t)q:(int32_t)q); branch=true;
    } else if (op == 0xea) {
        uint32_t off, cs;
        if (!fetch(&x,size,&off) || !fetch(&x,2,&cs)) goto fault;
        setseg_real(&x,X_CS,cs); x.pc=sbase(&x,X_CS)+(off&(size==2?0xffff:UINT32_MAX)); branch=true;
    } else if (op == 0xc3 || op == 0xcb) {
        uint32_t off, cs;
        if (!pop(&x,size,&off)) goto fault;
        if (op==0xcb) { if (!pop(&x,2,&cs)) goto fault; setseg_real(&x,X_CS,cs); }
        x.pc=sbase(&x,X_CS)+(off&(size==2?0xffff:UINT32_MAX)); branch=true;
    } else if (op == 0xc4 || op == 0xc5) {          /* les / lds */
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if(!decode_rm(&x,mr,&rm)||rm.is_reg)goto fault;
        uint32_t off,seg;
        if(!rb(&x,rm.addr,size,false,&off)||!rb(&x,rm.addr+size,2,false,&seg))goto fault;
        setxr(&x,(mr>>3)&7,size,off);
        setseg_real(&x,op==0xc4?X_ES:X_DS,(uint16_t)seg);
    } else if (op == 0xcd || op == 0xcc) {           /* int imm8 / int3 */
        uint32_t vec = 3;
        if (op == 0xcd && !fetch(&x,1,&vec)) goto fault;
        if (op == 0xcd && vec == 0x10) {
            /* No real video BIOS ROM behind the IVT to jump to - service
             * the call directly instead (see int10_handler()'s comment). */
            int10_handler(&x);
        } else {
            uint32_t ip_lo, cs_lo;
            if (!push(&x,2,(uint16_t)eflags(&x)) ||
                !push(&x,2,sel(&x,X_CS)) ||
                !push(&x,2,(uint16_t)(x.pc-sbase(&x,X_CS))) ||
                !rb(&x,vec*4,2,false,&ip_lo) || !rb(&x,vec*4+2,2,false,&cs_lo))
                goto fault;
            setseg_real(&x,X_CS,(uint16_t)cs_lo);
            x.pc = sbase(&x,X_CS) + ip_lo;
            setflags(&x, eflags(&x) & ~(FL_IF));
            branch = true;
        }
    } else if (op == 0xcf) {                        /* iret */
        uint32_t ip_r, cs_r, fl_r;
        if (!pop(&x,2,&ip_r) || !pop(&x,2,&cs_r) || !pop(&x,2,&fl_r)) goto fault;
        setseg_real(&x,X_CS,(uint16_t)cs_r);
        x.pc = sbase(&x,X_CS) + ip_r;
        setflags(&x, fl_r);
        branch = true;
    } else if (op == 0xfa) setflags(&x, eflags(&x) & ~FL_IF);   /* cli */
    else if (op == 0xfb) setflags(&x, eflags(&x) | FL_IF);      /* sti */
    else if (op == 0xe4 || op == 0xe5 || op == 0xe6 || op == 0xe7 ||
             op == 0xec || op == 0xed || op == 0xee || op == 0xef) {
        bool out = op == 0xe6 || op == 0xe7 || op == 0xee || op == 0xef;
        unsigned n = (op & 1) ? size : 1;
        uint16_t port;
        if (op >= 0xec) port = (uint16_t)xr(&x,2,2);
        else { if (!fetch(&x,1,&q)) goto fault; port = (uint8_t)q; }
        if (out) {
            if (!iowrite(&x,port,n,xr(&x,0,n))) goto fault;
        } else {
            if (!ioread(&x,port,n,&q)) goto fault;
            setxr(&x,0,n,q);
        }
    } else if (op == 0x86 || op == 0x87) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if (!decode_rm(&x,mr,&rm)) goto fault;
        unsigned n=op==0x86?1:size, reg=(mr>>3)&7; uint32_t a,b;
        if(!rmread(&x,rm,n,&a))goto fault;
        b=xr(&x,reg,n);
        if(!rmwrite(&x,rm,n,b))goto fault;
        setxr(&x,reg,n,a);
    } else if (op == 0x8f) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm;
        if (((mr>>3)&7)!=0 || !decode_rm(&x,mr,&rm)) goto fault;
        if (!pop(&x,size,&q) || !rmwrite(&x,rm,size,q)) goto fault;
    } else if (op == 0x88 || op == 0x8a || op == 0x8b || op == 0x89 ||
               op == 0x8c || op == 0x8e) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if (!decode_rm(&x,mr,&rm)) goto fault;
        unsigned reg=(mr>>3)&7;
        if (op==0x8a || op==0x8b) { unsigned n=op==0x8a?1:size; if(!rmread(&x,rm,n,&q))goto fault; setxr(&x,reg,n,q); }
        else if(op==0x88 || op==0x89) { unsigned n=op==0x88?1:size; if(!rmwrite(&x,rm,n,xr(&x,reg,n)))goto fault; }
        else if(op==0x8c) { if(reg>5||!rmwrite(&x,rm,2,sel(&x,reg)))goto fault; }
        else { if(reg>5||!rmread(&x,rm,2,&q))goto fault; setseg_real(&x,reg,q); }
    } else if (op == 0x69 || op == 0x6b) {           /* imul r, r/m, imm */
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if(!decode_rm(&x,mr,&rm)) goto fault;
        unsigned reg=(mr>>3)&7;
        uint32_t a, imm;
        if(!rmread(&x,rm,size,&a)) goto fault;
        if (op==0x69) { if(!fetch(&x,size,&imm)) goto fault; }
        else { if(!fetch(&x,1,&imm)) goto fault; imm=(uint32_t)(int32_t)(int8_t)imm; }
        int64_t sa = size==2 ? (int16_t)a : (int32_t)a;
        int64_t si = size==2 ? (int16_t)imm : (int32_t)imm;
        int64_t full = sa*si;
        uint32_t v = size==2 ? (uint32_t)(uint16_t)full : (uint32_t)full;
        int64_t sv = size==2 ? (int16_t)v : (int32_t)v;
        uint32_t f = eflags(&x) & ~(FL_CF|FL_OF);
        if (full != sv) f |= FL_CF|FL_OF;
        setflags(&x, f);
        setxr(&x, reg, size, v);
    } else if (op >= 0xa0 && op <= 0xa3) {
        uint32_t off; if(!fetch(&x,x.addr32?4:2,&off))goto fault;
        unsigned seg=x.seg_override>=0?(unsigned)x.seg_override:X_DS;
        uint32_t addr=sbase(&x,seg)+(x.addr32?off:(uint16_t)off);
        unsigned n=(op&1)?size:1;
        if(op<0xa2){if(!rb(&x,addr,n,false,&q))goto fault;setxr(&x,0,n,q);}
        else if(!wb(&x,addr,n,xr(&x,0,n)))goto fault;
    } else if (op == 0x2f) {                        /* das */
        uint32_t f = eflags(&x);
        uint32_t al = xr(&x, 0, 1);
        uint32_t old_al = al;
        bool old_cf = f & FL_CF, af = f & FL_AF, cf;
        if ((al & 0xf) > 9 || af) { al = (al - 6) & 0xff; af = true; }
        else af = false;
        if (old_al > 0x99 || old_cf) { al = (al - 0x60) & 0xff; cf = true; }
        else cf = false;
        f &= ~(FL_CF|FL_AF|FL_ZF|FL_SF|FL_PF);
        if (cf) f |= FL_CF;
        if (af) f |= FL_AF;
        if (!al) f |= FL_ZF;
        if (al & 0x80) f |= FL_SF;
        if (!__builtin_parity(al)) f |= FL_PF;
        setflags(&x, f);
        setxr(&x, 0, 1, al);
    } else if (op == 0x3c) {
        if (!fetch(&x,1,&q)) goto fault;
        uint32_t a=xr(&x,0,1), v=a-(uint8_t)q;
        sub_flags(&x,a,(uint8_t)q,v,1);
    } else if (op == 0xc0 || op == 0xc1 ||
               op == 0xd0 || op == 0xd1 || op == 0xd2 || op == 0xd3) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if(!decode_rm(&x,mr,&rm))goto fault;
        unsigned sub=(mr>>3)&7, n=(op&1)?size:1, count;
        uint32_t v, old;
        if(!rmread(&x,rm,n,&v))goto fault;
        old=v;
        if(op==0xc0||op==0xc1){if(!fetch(&x,1,&q))goto fault;count=q&31;}
        else if(op==0xd0||op==0xd1)count=1;
        else count=xr(&x,1,1)&31;
        if(count){
            uint32_t cf=0, sign=1u<<(n*8-1);
            if(sub!=4&&sub!=5&&sub!=7)
                return xhalt(&x,"IA-32 shift /%u unimplemented at %08X",sub,x.start);
            for(unsigned i=0;i<count;i++){
                if(sub==4){cf=!!(v&sign);v<<=1;}
                else {cf=v&1;
                    if(sub==5)v>>=1;
                    else if(n==1)v=(uint8_t)((int8_t)v>>1);
                    else if(n==2)v=(uint16_t)((int16_t)v>>1);
                    else v=(uint32_t)((int32_t)v>>1);
                }
            }
            if(n<4)v&=(1u<<(n*8))-1;
            logic_flags(&x,v,n);
            uint32_t f=eflags(&x)&~(FL_CF|FL_OF);
            if(cf)f|=FL_CF;
            if(count==1){
                if(sub==4&&((!!(v&sign))!=cf))f|=FL_OF;
                else if(sub==5&&(old&sign))f|=FL_OF;
            }
            setflags(&x,f);
            if(!rmwrite(&x,rm,n,v))goto fault;
        }
    } else if (op == 0xc6 || op == 0xc7) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm;
        if (((mr>>3)&7)!=0 || !decode_rm(&x,mr,&rm)) goto fault;
        unsigned n=op==0xc6?1:size;
        if (!fetch(&x,n,&q) || !rmwrite(&x,rm,n,q)) goto fault;
    } else if (op == 0x80 || op == 0x81 || op == 0x83) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm;
        if(!decode_rm(&x,mr,&rm))goto fault;
        unsigned sub=(mr>>3)&7;
        unsigned n=op==0x80?1:size;
        uint32_t a,imm; if(!rmread(&x,rm,n,&a)||!fetch(&x,op==0x83?1:n,&imm))goto fault;
        if(op==0x83) imm=(uint32_t)(int32_t)(int8_t)imm;
        uint32_t v; if(sub==0)v=a+imm;
        else if(sub==1)v=a|imm;
        else if(sub==4)v=a&imm;
        else if(sub==6)v=a^imm;
        else if(sub==5||sub==7)v=a-imm;
        else return xhalt(&x,"IA-32 group1 /%u unimplemented at %08X",sub,x.start);
        if(sub!=7&&!rmwrite(&x,rm,n,v))goto fault;
        if(sub==5||sub==7)sub_flags(&x,a,imm,v,n);
        else if(sub==0)add_flags(&x,a,imm,v,n);
        else logic_flags(&x,v,n);
    } else if (op == 0xa4 || op == 0xa5) {
        unsigned n=op==0xa4?1:size; uint32_t count=x.rep?xr(&x,1,x.addr32?4:2):1;
        int step=(eflags(&x)&FL_DF)?-(int)n:(int)n;
        while(count--){uint32_t v,si=xr(&x,6,x.addr32?4:2),di=xr(&x,7,x.addr32?4:2);
            unsigned sg=x.seg_override>=0?(unsigned)x.seg_override:X_DS;
            if(!rb(&x,sbase(&x,sg)+si,n,false,&v)||!wb(&x,sbase(&x,X_ES)+di,n,v))goto fault;
            setxr(&x,6,x.addr32?4:2,si+step);setxr(&x,7,x.addr32?4:2,di+step);}
        if(x.rep)setxr(&x,1,x.addr32?4:2,0);
    } else if (op == 0xac || op == 0xad) {
        unsigned n=op==0xac?1:size; uint32_t count=x.rep?xr(&x,1,x.addr32?4:2):1;
        int step=(eflags(&x)&FL_DF)?-(int)n:(int)n;
        unsigned sg=x.seg_override>=0?(unsigned)x.seg_override:X_DS;
        while(count--){uint32_t v,si=xr(&x,6,x.addr32?4:2);
            if(!rb(&x,sbase(&x,sg)+si,n,false,&v))goto fault;
            setxr(&x,0,n,v);setxr(&x,6,x.addr32?4:2,si+step);}
        if(x.rep)setxr(&x,1,x.addr32?4:2,0);
    } else if (op == 0xaa || op == 0xab) {
        unsigned n=op==0xaa?1:size; uint32_t count=x.rep?xr(&x,1,x.addr32?4:2):1;
        int step=(eflags(&x)&FL_DF)?-(int)n:(int)n;
        uint32_t val=xr(&x,0,n);
        while(count--){uint32_t di=xr(&x,7,x.addr32?4:2);
            if(!wb(&x,sbase(&x,X_ES)+di,n,val))goto fault;
            setxr(&x,7,x.addr32?4:2,di+step);}
        if(x.rep)setxr(&x,1,x.addr32?4:2,0);
    } else if (op == 0xf6 || op == 0xf7) {
        if(!fetch(&x,1,&q))goto fault;
        uint8_t mr=q;RM rm;if(!decode_rm(&x,mr,&rm))goto fault;
        unsigned sub=(mr>>3)&7,n=op==0xf6?1:size;uint32_t src;
        if(!rmread(&x,rm,n,&src))goto fault;
        if(sub==0){uint32_t imm;if(!fetch(&x,n,&imm))goto fault;
            logic_flags(&x,src&imm,n);
        } else if(sub==4){
            if(n==1){uint16_t v=(uint16_t)xr(&x,0,1)*(uint8_t)src;
                setxr(&x,0,2,v);setflags(&x,(eflags(&x)&~(FL_CF|FL_OF))|((v>>8)?FL_CF|FL_OF:0));}
            else {uint64_t v=(uint64_t)xr(&x,0,n)*src;
                setxr(&x,0,n,(uint32_t)v);setxr(&x,2,n,(uint32_t)(v>>(n*8)));
                setflags(&x,(eflags(&x)&~(FL_CF|FL_OF))|((v>>(n*8))?FL_CF|FL_OF:0));}
        } else return xhalt(&x,"IA-32 group3 /%u unimplemented at %08X",sub,x.start);
    } else if (op == 0xfe || op == 0xff) {
        if(!fetch(&x,1,&q))goto fault;
        uint8_t mr=q;RM rm;
        if(!decode_rm(&x,mr,&rm))goto fault;
        unsigned sub=(mr>>3)&7;
        unsigned n=op==0xfe?1:size;
        if(op==0xfe && sub<=1){uint32_t a,v;if(!rmread(&x,rm,1,&a))goto fault;v=sub?a-1:a+1;
            if(!rmwrite(&x,rm,1,v))goto fault;
            incdec_flags(&x,a,1,v,1,sub!=0);}
        else if(op==0xff && sub==4){uint32_t off;if(!rmread(&x,rm,n,&off))goto fault;
            x.pc=sbase(&x,X_CS)+(off&(n==2?UINT16_MAX:UINT32_MAX));branch=true;}
        else if(op==0xff && sub==3 && !rm.is_reg){uint32_t off,cs;if(!rb(&x,rm.addr,size,false,&off)||!rb(&x,rm.addr+size,2,false,&cs))goto fault;
            if(!push(&x,2,sel(&x,X_CS))||!push(&x,size,x.pc-sbase(&x,X_CS)))goto fault;
            setseg_real(&x,X_CS,cs);x.pc=sbase(&x,X_CS)+(off&(size==2?0xffff:UINT32_MAX));branch=true;}
        else return xhalt(&x,"IA-32 FF /%u unimplemented at %08X",sub,x.start);
    } else if (op == 0x0f) {
        uint32_t op2;if(!fetch(&x,1,&op2))goto fault;
        if(op2==0xb6 || op2==0xb7 || op2==0xbe || op2==0xbf){
            uint32_t mr;if(!fetch(&x,1,&mr))goto fault;RM rm;
            if(!decode_rm(&x,mr,&rm))goto fault;
            unsigned n=(op2&1)?2:1;uint32_t v;if(!rmread(&x,rm,n,&v))goto fault;
            if(op2>=0xbe)v=n==1?(uint32_t)(int32_t)(int8_t)v:(uint32_t)(int32_t)(int16_t)v;
            setxr(&x,(mr>>3)&7,size,v);
        } else if(op2==0x00){uint32_t mr;if(!fetch(&x,1,&mr))goto fault;
            if(((mr>>3)&7)==6){RM rm;if(!decode_rm(&x,mr,&rm)||!rmread(&x,rm,size,&q))goto fault;
                /* JMPE: leave IA-32 mode and resume at an aligned IA-64 IP.
                 * Per the Itanium SDM's JMPE operation: "GR[1] = EIP +
                 * AR[CSD].base" (the next sequential instruction address
                 * following JMPE), zero-extended - IA-64 code on the other
                 * side of the mode switch is architecturally entitled to
                 * rely on r1 holding this. Leaving it unset let whatever
                 * stale IA-32-mode value sat there leak into IA-64
                 * execution as a bogus address (seen corrupting a later
                 * TLB-miss lookup). x.pc here is already the linear address
                 * right after this instruction's operand, matching the
                 * formula exactly. */
                m->gr_static[1] = x.pc;
                m->nat_static[1] = 0;
                m->ip=(sbase(&x,X_CS)+q)&UINT32_C(0xfffffff0);m->psr&=~(UINT64_C(1)<<34);m->psr&=~(UINT64_C(3)<<41);m->taken=1;m->ninsts++;return MERCED_OK;}
            return xhalt(&x,"IA-32 0F %02X unimplemented at %08X",op2,x.start);
        } else if(op2==0xa2){
            /* CPUID. Only leaf 0 (vendor string + max leaf) and leaf 1
             * (family/model/stepping + feature flags) are modeled; any
             * other leaf falls back to the leaf-1 shape rather than
             * halting. Feature flags advertise FPU only - RDTSC,
             * CMPXCHG8B and friends aren't implemented in this IA-32
             * layer, so claiming them would just trade this halt for a
             * later "unimplemented opcode" one when firmware acts on
             * them. */
            uint32_t a,b,c,d;
            if(xr(&x,0,4)==0){a=1;b=0x756e6547;d=0x49656e69;c=0x6c65746e;}
            else{a=0x00000601;b=0;c=0;d=0x00000001;}
            setxr(&x,0,4,a);setxr(&x,3,4,b);setxr(&x,1,4,c);setxr(&x,2,4,d);
        } else return xhalt(&x,"IA-32 0F %02X unimplemented at %08X",op2,x.start);
    } else return xhalt(&x,"IA-32 opcode %02X unimplemented at %08X",op,x.start);

    (void)branch;
    m->ip=x.pc; m->ninsts++; return MERCED_OK;
fault:
    if (!(m->psr & (UINT64_C(1) << 34))) return MERCED_OK;
    return xhalt(&x,"IA-32 memory fault at %08X (instruction %08X)",x.pc,x.start);
}
