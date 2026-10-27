/*
 * Atari 2600 debugger: the monitor commands and views that make the
 * emulator useful while writing a 2600 program.
 *
 *   disas [addr] [n]     disassembly with TIA/RIOT register names and symbols
 *   tia, riot            chip state: registers, object positions, collisions,
 *                        timer
 *   frame [n]            run n frames, then stop
 *   scanline [n]         run n scanlines, then stop
 *   breakline <n>|off    stop when the beam reaches scanline n
 *   trace <file>|off     log every instruction with frame/scanline/cycle
 *   frameinfo [watch]    scanline counts per frame; warn when they change
 *   debugcolors on|off   fixed colour per object, blanking shown grey
 *   hide <objects>       leave objects out of the picture
 *   reload, autoreload   pick up a fresh build of the cartridge (and symbols)
 *
 * The display gets a second monitor, "Full frame (debug)", showing every
 * color clock of every line: HBLANK, VBLANK, VSYNC and the HMOVE comb are
 * visible, and while stopped the beam's position is marked.
 */
#include "atari2600.h"
#include <SDL2/SDL.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ── Register names ──────────────────────────────────────────────────────── */

static const char *const tia_write_names[0x2D] = {
    "VSYNC", "VBLANK", "WSYNC", "RSYNC", "NUSIZ0", "NUSIZ1", "COLUP0", "COLUP1",
    "COLUPF", "COLUBK", "CTRLPF", "REFP0", "REFP1", "PF0", "PF1", "PF2",
    "RESP0", "RESP1", "RESM0", "RESM1", "RESBL", "AUDC0", "AUDC1", "AUDF0",
    "AUDF1", "AUDV0", "AUDV1", "GRP0", "GRP1", "ENAM0", "ENAM1", "ENABL",
    "HMP0", "HMP1", "HMM0", "HMM1", "HMBL", "VDELP0", "VDELP1", "VDELBL",
    "RESMP0", "RESMP1", "HMOVE", "HMCLR", "CXCLR",
};

static const char *const tia_read_names[14] = {
    "CXM0P", "CXM1P", "CXP0FB", "CXP1FB", "CXM0FB", "CXM1FB", "CXBLPF", "CXPPMM",
    "INPT0", "INPT1", "INPT2", "INPT3", "INPT4", "INPT5",
};

/* Names for the disassembler: TIA and RIOT registers by direction, then
 * the loaded symbols. */
static const char *a26_symbol(void *ud, uint16_t addr, int access) {
    Atari2600State *s = ud;
    if (addr < 0x80 && access != MOS6502_SYM_CODE) {
        if (access == MOS6502_SYM_WRITE) {
            if ((addr & 0x3F) < 0x2D) return tia_write_names[addr & 0x3F];
        } else if ((addr & 0x0F) < 14) {
            return tia_read_names[addr & 0x0F];
        }
    }
    switch (addr) {
    case 0x280: return "SWCHA";
    case 0x281: return "SWACNT";
    case 0x282: return "SWCHB";
    case 0x283: return "SWBCNT";
    case 0x284: return "INTIM";
    case 0x285: return "TIMINT";
    case 0x294: return "TIM1T";
    case 0x295: return "TIM8T";
    case 0x296: return "TIM64T";
    case 0x297: return "T1024T";
    }
    return gemu_monitor_symbol_name(s->monitor, addr);
}

static uint8_t dis_peek(void *ud, uint16_t addr) {
    return a26_peek(ud, addr);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void stop(Atari2600State *s) {
    gemu_monitor_pause(s->monitor);
}

static bool parse_onoff(const char *a, bool *out) {
    if (!strcasecmp(a, "on") || !strcmp(a, "1"))  { *out = true;  return true; }
    if (!strcasecmp(a, "off") || !strcmp(a, "0")) { *out = false; return true; }
    return false;
}

static int64_t file_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_mtime : -1;
}

/* ── disas ───────────────────────────────────────────────────────────────── */

static GemuMonCmd cmd_disas(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    char a1[64] = "", a2[32] = "";
    sscanf(args, "%63s %31s", a1, a2);
    uint64_t addr = s->cpu.PC;
    if (a1[0] && !gemu_monitor_parse_addr(mon, a1, &addr)) {
        gemu_monitor_printf(mon, "disas: bad address '%s'\n", a1);
        return GEMU_MON_NONE;
    }
    int count = a2[0] ? atoi(a2) : 16;
    if (count <= 0 || count > 1000) count = 16;

    char bank[160];
    a26_cart_describe(&s->cart, bank, sizeof(bank));
    gemu_monitor_printf(mon, "; %s\n", bank);
    uint16_t pc = (uint16_t)addr;
    for (int i = 0; i < count; i++) {
        const char *label = gemu_monitor_symbol_name(mon, pc);
        if (label && (pc & 0x1000u)) gemu_monitor_printf(mon, "        %s:\n", label);
        char text[96];
        int len = mos6502_disasm(dis_peek, s, pc, text, sizeof(text), a26_symbol, s);
        char bytes[16] = "";
        for (int b = 0; b < len; b++)
            snprintf(bytes + strlen(bytes), sizeof(bytes) - strlen(bytes), "%02X ",
                     a26_peek(s, (uint16_t)(pc + b)));
        bool here = ((pc ^ s->cpu.PC) & 0x1FFFu) == 0;
        gemu_monitor_printf(mon, "%s%04X  %-9s %s\n", here ? ">" : " ", pc, bytes, text);
        pc = (uint16_t)(pc + len);
    }
    return GEMU_MON_NONE;
}

/* ── tia / riot ──────────────────────────────────────────────────────────── */

static int hm_value(uint8_t hm_clocks) {   /* back to the signed HMxx value */
    return (int)(hm_clocks ^ 8u) >= 8 ? (int)(hm_clocks ^ 8u) - 16 : (int)(hm_clocks ^ 8u);
}

static GemuMonCmd cmd_tia(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    (void)args;
    Tia *t = &s->tia;
    tia_run(t, s->cpu.cycle_count * 3u);
    static const char *const nusiz[8] = {
        "1 copy", "2 close", "2 medium", "3 close", "2 wide", "double", "3 medium", "quad",
    };
    gemu_monitor_printf(mon, "frame %" PRIu64 "  scanline %d  color clock %d (cpu cycle %d)%s%s\n",
                        t->frame_count, t->scanline, t->hctr, t->hctr / 3,
                        t->hblank ? "  HBLANK" : "", t->ext_hblank ? " (HMOVE-extended)" : "");
    gemu_monitor_printf(mon, "last frame: %d lines, VSYNC ended line %d, picture lines %d-%d\n",
                        t->frame_lines, t->done_vsync_end, t->done_first_lit, t->done_last_lit);
    gemu_monitor_printf(mon, "VSYNC=%02X VBLANK=%02X%s%s%s  CTRLPF=%02X%s%s%s ball %dpx\n",
                        t->vsync, t->vblank, (t->vblank & 2) ? " (blank)" : "",
                        (t->vblank & 0x40) ? " (latch inputs)" : "", (t->vblank & 0x80) ? " (dump pots)" : "",
                        t->ctrlpf, (t->ctrlpf & 1) ? " reflect" : "", (t->ctrlpf & 2) ? " score" : "",
                        (t->ctrlpf & 4) ? " priority" : "", t->bl.width);
    gemu_monitor_printf(mon, "COLUP0=%02X COLUP1=%02X COLUPF=%02X COLUBK=%02X\n",
                        t->colup[0], t->colup[1], t->colupf, t->colubk);
    char pf[41];
    for (int c = 0; c < 20; c++) {
        bool bit = c < 4 ? (t->pf[0] >> (4 + c)) & 1 : c < 12 ? (t->pf[1] >> (11 - c)) & 1
                                                           : (t->pf[2] >> (c - 12)) & 1;
        pf[c] = bit ? '#' : '.';
        pf[(t->ctrlpf & 1) ? 39 - c : 20 + c] = bit ? '#' : '.';
    }
    pf[40] = '\0';
    gemu_monitor_printf(mon, "PF0=%02X PF1=%02X PF2=%02X  |%s|\n", t->pf[0], t->pf[1], t->pf[2], pf);
    for (int i = 0; i < 2; i++) {
        const TiaObject *p = &t->p[i];
        gemu_monitor_printf(mon, "P%d: x=%-3d GRP=%02X old=%02X NUSIZ=%02X (%s)%s%s HM=%+d%s\n", i,
                            tia_object_x(t, p, p->width > 1 ? 7 : 6),
                            t->grp_new[i], t->grp_old[i], t->nusiz[i], nusiz[t->nusiz[i] & 7],
                            t->refp[i] ? " REFP" : "", t->vdelp[i] ? " VDEL" : "",
                            hm_value(p->hm_clocks), (t->hide & (TIA_HIDE_P0 << i)) ? " [hidden]" : "");
    }
    for (int i = 0; i < 2; i++) {
        const TiaObject *m = &t->m[i];
        gemu_monitor_printf(mon, "M%d: x=%-3d %s width=%d%s HM=%+d%s\n", i, tia_object_x(t, m, 5),
                            t->enam[i] ? "on " : "off", m->width, t->resmp[i] ? " RESMP" : "",
                            hm_value(m->hm_clocks), (t->hide & (TIA_HIDE_M0 << i)) ? " [hidden]" : "");
    }
    gemu_monitor_printf(mon, "BL: x=%-3d %s (old %s)%s width=%d HM=%+d%s\n", tia_object_x(t, &t->bl, 5),
                        t->enabl_new ? "on " : "off", t->enabl_old ? "on" : "off",
                        t->vdelbl ? " VDEL" : "", t->bl.width, hm_value(t->bl.hm_clocks),
                        (t->hide & TIA_HIDE_BL) ? " [hidden]" : "");
    static const char *const cx[8][2] = {
        { "M0-P1", "M0-P0" }, { "M1-P0", "M1-P1" }, { "P0-PF", "P0-BL" }, { "P1-PF", "P1-BL" },
        { "M0-PF", "M0-BL" }, { "M1-PF", "M1-BL" }, { "BL-PF", NULL }, { "P0-P1", "M0-M1" },
    };
    char hits[160] = "";
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 2; b++)
            if (cx[r][b] && (t->cx[r] & (0x80u >> b)))
                snprintf(hits + strlen(hits), sizeof(hits) - strlen(hits), " %s", cx[r][b]);
    gemu_monitor_printf(mon, "collisions:%s\n", hits[0] ? hits : " none");
    gemu_monitor_printf(mon, "AUDC0=%X AUDF0=%02X AUDV0=%X  AUDC1=%X AUDF1=%02X AUDV1=%X\n",
                        t->sound.ch[0].audc, t->sound.ch[0].audf, t->sound.ch[0].audv,
                        t->sound.ch[1].audc, t->sound.ch[1].audf, t->sound.ch[1].audv);
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_riot(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    (void)args;
    const M6532 *r = &s->riot;
    uint64_t now = s->cpu.cycle_count, d = now - r->timer_start;
    uint64_t under = ((uint64_t)r->timer_value + 1u) << r->timer_shift;
    static const char *const names[] = { "TIM1T", "TIM8T", "TIM64T", "T1024T" };
    int iv = r->timer_shift == 0 ? 0 : r->timer_shift == 3 ? 1 : r->timer_shift == 6 ? 2 : 3;
    gemu_monitor_printf(mon, "INTIM=%02X  last write %s #$%02X %" PRIu64 " cycles ago, ",
                        m6532_timer(r, now), names[iv], r->timer_value, d);
    if (d < under)
        gemu_monitor_printf(mon, "expires in %" PRIu64 " cycles (%.1f scanlines)\n",
                            under - d, (double)(under - d) / 76.0);
    else
        gemu_monitor_printf(mon, "expired %" PRIu64 " cycles ago (counting every cycle)\n", d - under);
    gemu_monitor_printf(mon, "TIMINT=%02X\n", m6532_peek(r, 0x285, now));
    gemu_monitor_printf(mon, "SWCHA=%02X SWACNT=%02X  SWCHB=%02X SWBCNT=%02X\n",
                        m6532_peek(r, 0x280, now), r->ddra, m6532_peek(r, 0x282, now), r->ddrb);
    return GEMU_MON_NONE;
}

/* ── Raster stepping ─────────────────────────────────────────────────────── */

static uint32_t count_arg(const char *args) {
    long n = *args ? strtol(args, NULL, 10) : 1;
    return n < 1 ? 1u : (uint32_t)n;
}

static GemuMonCmd cmd_frame(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    (void)mon;
    s->dbg.stop_frames = count_arg(args);
    return GEMU_MON_CONT;
}

static GemuMonCmd cmd_scanline(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    (void)mon;
    s->dbg.stop_lines = count_arg(args);
    s->dbg.last_line = s->tia.scanline;
    return GEMU_MON_CONT;
}

static GemuMonCmd cmd_breakline(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    if (!*args) {
        if (s->dbg.break_line < 0) gemu_monitor_printf(mon, "breakline: off\n");
        else gemu_monitor_printf(mon, "breakline: scanline %d\n", s->dbg.break_line);
    } else if (!strcasecmp(args, "off")) {
        s->dbg.break_line = -1;
        gemu_monitor_printf(mon, "breakline: off\n");
    } else {
        int n = atoi(args);
        if (n < 0 || n >= TIA_MAX_LINES) {
            gemu_monitor_printf(mon, "usage: breakline <0-%d> | off\n", TIA_MAX_LINES - 1);
            return GEMU_MON_NONE;
        }
        s->dbg.break_line = n;
        s->dbg.last_line = s->tia.scanline;
        gemu_monitor_printf(mon, "breakline: stopping at scanline %d\n", n);
    }
    return GEMU_MON_NONE;
}

/* ── trace ───────────────────────────────────────────────────────────────── */

static void trace_close(Atari2600State *s) {
    if (s->dbg.trace) fclose(s->dbg.trace);
    s->dbg.trace = NULL;
}

static GemuMonCmd cmd_trace(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    if (!*args) {
        if (s->dbg.trace) gemu_monitor_printf(mon, "trace: writing %s\n", s->dbg.trace_path);
        else gemu_monitor_printf(mon, "trace: off\n");
    } else if (!strcasecmp(args, "off")) {
        trace_close(s);
        gemu_monitor_printf(mon, "trace: off\n");
    } else {
        trace_close(s);
        s->dbg.trace = fopen(args, "w");
        if (!s->dbg.trace) {
            gemu_monitor_printf(mon, "trace: cannot write '%s'\n", args);
            return GEMU_MON_NONE;
        }
        snprintf(s->dbg.trace_path, sizeof(s->dbg.trace_path), "%s", args);
        fprintf(s->dbg.trace, "; frame scanline cycle  PC    bytes     instruction          registers\n");
        gemu_monitor_printf(mon, "trace: writing every instruction to %s ('trace off' stops)\n", args);
    }
    return GEMU_MON_NONE;
}

static void trace_insn(Atari2600State *s) {
    const Mos6502 *c = &s->cpu;
    char text[96], bytes[16] = "";
    int len = mos6502_disasm(dis_peek, s, c->PC, text, sizeof(text), a26_symbol, s);
    for (int b = 0; b < len; b++)
        snprintf(bytes + strlen(bytes), sizeof(bytes) - strlen(bytes), "%02X ",
                 a26_peek(s, (uint16_t)(c->PC + b)));
    static const char flags[] = "NV-BDIZC";
    char p[9];
    for (int i = 0; i < 8; i++) p[i] = (c->P & (0x80u >> i)) ? flags[i] : '.';
    p[8] = '\0';
    const char *label = gemu_monitor_symbol_name(s->monitor, c->PC);
    if (label) fprintf(s->dbg.trace, "%s:\n", label);
    fprintf(s->dbg.trace, "%6" PRIu64 " %3d %2d  %04X  %-9s %-20s A=%02X X=%02X Y=%02X S=%02X %s\n",
            s->tia.frame_count, s->tia.scanline, s->tia.hctr / 3, c->PC, bytes, text,
            c->A, c->X, c->Y, c->SP, p);
}

/* ── frameinfo ───────────────────────────────────────────────────────────── */

static int expected_lines(const Atari2600State *s) {
    return s->pal ? 312 : 262;
}

static GemuMonCmd cmd_frameinfo(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    A26Debug *d = &s->dbg;
    if (!strncasecmp(args, "watch", 5)) {
        const char *a = args + 5;
        while (*a == ' ') a++;
        bool on = true;
        if (*a && !parse_onoff(a, &on)) {
            gemu_monitor_printf(mon, "usage: frameinfo watch [on | off]\n");
            return GEMU_MON_NONE;
        }
        d->frame_watch = on;
        gemu_monitor_printf(mon, "frameinfo: %s scanline-count changes\n", on ? "reporting" : "not reporting");
        return GEMU_MON_NONE;
    }
    gemu_monitor_printf(mon, "frame    lines  VSYNC end  picture    cpu cycles   (%s expects %d lines)\n",
                        s->pal ? "PAL" : "NTSC", expected_lines(s));
    for (int i = 0; i < d->hist_n; i++) {
        const A26FrameStats *f = &d->hist[(d->hist_pos - 1 - i + A26_HISTORY) % A26_HISTORY];
        gemu_monitor_printf(mon, "%-8" PRIu64 " %4d%s  %5d     %3d-%-3d   %8" PRIu64 "\n",
                            s->tia.frame_count - (uint64_t)i, f->lines,
                            f->lines != expected_lines(s) ? "!" : " ", f->vsync_end,
                            f->first_lit, f->last_lit, f->cycles);
    }
    if (!d->hist_n) gemu_monitor_printf(mon, "(no frames yet)\n");
    return GEMU_MON_NONE;
}

/* ── Display debugging ───────────────────────────────────────────────────── */

void a26_debug_toggle_colors(void *ud) {
    Atari2600State *s = ud;
    s->tia.debug_colors = !s->tia.debug_colors;
}

static GemuMonCmd cmd_debugcolors(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    bool on = !s->tia.debug_colors;
    if (*args && !parse_onoff(args, &on)) {
        gemu_monitor_printf(mon, "usage: debugcolors [on | off]\n");
        return GEMU_MON_NONE;
    }
    s->tia.debug_colors = on;
    gemu_monitor_printf(mon, "debugcolors: %s%s\n", on ? "on" : "off",
                        on ? " (P0 red, M0 dark red, P1 yellow, M1 orange, BL blue, PF green, VBLANK grey)" : "");
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_hide(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    static const struct { const char *name; uint8_t bit; } objs[] = {
        { "p0", TIA_HIDE_P0 }, { "p1", TIA_HIDE_P1 }, { "m0", TIA_HIDE_M0 },
        { "m1", TIA_HIDE_M1 }, { "bl", TIA_HIDE_BL }, { "pf", TIA_HIDE_PF },
    };
    if (*args) {
        uint8_t mask = 0;
        char buf[128];
        snprintf(buf, sizeof(buf), "%s", args);
        for (char *tok = strtok(buf, " ,"); tok; tok = strtok(NULL, " ,")) {
            if (!strcasecmp(tok, "none")) { mask = 0; continue; }
            bool found = false;
            for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++)
                if (!strcasecmp(tok, objs[i].name)) { mask |= objs[i].bit; found = true; }
            if (!found) {
                gemu_monitor_printf(mon, "hide: unknown object '%s' (p0 p1 m0 m1 bl pf none)\n", tok);
                return GEMU_MON_NONE;
            }
        }
        s->tia.hide = mask;
    }
    char list[64] = "";
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++)
        if (s->tia.hide & objs[i].bit)
            snprintf(list + strlen(list), sizeof(list) - strlen(list), " %s", objs[i].name);
    gemu_monitor_printf(mon, "hidden:%s\n", list[0] ? list : " none");
    return GEMU_MON_NONE;
}

/* ── reload / autoreload ─────────────────────────────────────────────────── */

static void do_reload(Atari2600State *s, GemuMonitor *mon) {
    char err[256] = "";
    if (!a26_reload_cart(s, err, sizeof(err))) {
        gemu_monitor_printf(mon, "reload: %s\n", err);
        return;
    }
    s->dbg.cart_mtime = file_mtime(s->cart_path);
    bool syms = gemu_monitor_reload_symbols_if_changed(s->monitor);
    gemu_monitor_printf(mon, "reload: %s%s\n", s->cart_path, syms ? " (and symbols)" : "");
}

void a26_debug_reload(void *ud) {
    Atari2600State *s = ud;
    do_reload(s, s->monitor);
}

static GemuMonCmd cmd_reload(GemuMonitor *mon, const char *args, void *ud) {
    (void)args;
    do_reload(ud, mon);
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_autoreload(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    bool on = true;
    if (*args && !parse_onoff(args, &on)) {
        gemu_monitor_printf(mon, "usage: autoreload [on | off]\n");
        return GEMU_MON_NONE;
    }
    s->dbg.autoreload = on;
    s->dbg.cart_mtime = file_mtime(s->cart_path);
    gemu_monitor_printf(mon, "autoreload: %s%s%s\n", on ? "watching " : "off",
                        on ? s->cart_path : "", on && s->monitor ? "" : "");
    return GEMU_MON_NONE;
}

/* ── Hooks from the run loop ─────────────────────────────────────────────── */

bool a26_debug_before_step(Atari2600State *s) {
    if (s->dbg.trace) {
        tia_run(&s->tia, s->cpu.cycle_count * 3u);
        trace_insn(s);
    }
    return false;
}

bool a26_debug_after_step(Atari2600State *s) {
    A26Debug *d = &s->dbg;
    if (!d->stop_lines && d->break_line < 0) return false;
    tia_run(&s->tia, s->cpu.cycle_count * 3u);   /* see a WSYNC hold end */
    int line = s->tia.scanline;
    if (line == d->last_line) return false;
    d->last_line = line;
    if (d->stop_lines && --d->stop_lines == 0) { stop(s); return true; }
    if (line == d->break_line) {
        gemu_monitor_printf(s->monitor, "breakline: beam at scanline %d (frame %" PRIu64 ") pc=%04X\n",
                            line, s->tia.frame_count, s->cpu.PC);
        stop(s);
        return true;
    }
    return false;
}

bool a26_debug_frame_done(Atari2600State *s, uint64_t cycles) {
    A26Debug *d = &s->dbg;
    const Tia *t = &s->tia;
    int prev = d->hist_n ? d->hist[(d->hist_pos - 1 + A26_HISTORY) % A26_HISTORY].lines : -1;
    d->hist[d->hist_pos] = (A26FrameStats){
        t->frame_lines, t->done_vsync_end, t->done_first_lit, t->done_last_lit, cycles,
    };
    d->hist_pos = (d->hist_pos + 1) % A26_HISTORY;
    if (d->hist_n < A26_HISTORY) d->hist_n++;
    if (d->frame_watch && prev >= 0 && prev != t->frame_lines)
        gemu_monitor_printf(s->monitor, "frameinfo: frame %" PRIu64 " has %d scanlines (was %d, %s wants %d)\n",
                            t->frame_count, t->frame_lines, prev, s->pal ? "PAL" : "NTSC",
                            expected_lines(s));
    if (d->stop_frames && --d->stop_frames == 0) { stop(s); return true; }
    return false;
}

void a26_debug_poll(Atari2600State *s) {
    A26Debug *d = &s->dbg;
    if (!d->autoreload || !s->cart_path[0]) return;
    uint32_t now = SDL_GetTicks();
    if (now - d->last_check_ms < 500) return;
    d->last_check_ms = now;
    int64_t m = file_mtime(s->cart_path);
    if (m < 0 || m == d->cart_mtime) return;
    do_reload(s, s->monitor);
}

/* Full-frame view: every color clock, blanking shaded, VSYNC lines marked,
 * the visible window's edges drawn, and the beam marked while stopped. */
void a26_debug_present(Atari2600State *s) {
    if (!s->display || !s->dbg.full_fb) return;
    bool shown = gemu_display_head_shown(s->display, 1);
    s->tia.debug_view = shown;
    if (!shown) return;

    const uint32_t *pal = a26_palette(s);
    const Tia *t = &s->tia;
    const uint8_t (*cur)[TIA_LINE_CLOCKS] = tia_dbg_frame(t, true);
    const uint8_t (*old)[TIA_LINE_CLOCKS] = tia_dbg_frame(t, false);
    bool paused = gemu_monitor_is_paused(s->monitor);
    for (int y = 0; y < A26_FULL_H; y++) {
        bool now = y < t->scanline;
        const uint8_t *row = now ? cur[y] : old[y];
        bool vsync = now ? t->dbg_vsync[t->draw][y] : t->dbg_vsync[t->draw ^ 1][y];
        uint32_t *dst = s->dbg.full_fb + y * A26_FULL_W;
        for (int x = 0; x < TIA_LINE_CLOCKS; x++) {
            uint8_t v = row[x];
            uint32_t c = pal[v >> 1];
            if (v & 1) {   /* blanked: show what's there, darkened */
                if ((v >> 1) == 0) c = vsync ? 0xFF501850u : 0xFF182030u;
                else c = 0xFF000000u | ((c >> 1) & 0x7F7F7Fu) | (vsync ? 0x00400040u : 0x00101828u);
            }
            dst[2 * x] = dst[2 * x + 1] = c;
        }
        if (y == s->window_start || y == s->window_start + A2600_FB_H - 1)   /* TV window */
            for (int x = 0; x < TIA_HBLANK * 2; x += 4) dst[x] = 0xFF40C0C0u;
    }
    if (paused && t->scanline < A26_FULL_H) {   /* the beam */
        uint32_t *row = s->dbg.full_fb + t->scanline * A26_FULL_W;
        for (int x = 0; x < A26_FULL_W; x += 2) row[x] ^= 0x00303030u;
        for (int dy = -3; dy <= 3; dy++) {
            int y = t->scanline + dy;
            if (y < 0 || y >= A26_FULL_H) continue;
            for (int dx = -1; dx <= 2; dx++) {
                int x = t->hctr * 2 + dx;
                if (x >= 0 && x < A26_FULL_W) s->dbg.full_fb[y * A26_FULL_W + x] = 0xFFFFFF00u;
            }
        }
    }
    gemu_display_render_head(s->display, 1, s->dbg.full_fb, A26_FULL_W, A26_FULL_H);
}

static GemuMonCmd cmd_view(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    if (!s->display) { gemu_monitor_printf(mon, "view: no display\n"); return GEMU_MON_NONE; }
    if (!strcasecmp(args, "full"))    gemu_display_select_head(s->display, 1);
    else if (!strcasecmp(args, "tv")) gemu_display_select_head(s->display, 0);
    else if (*args) { gemu_monitor_printf(mon, "usage: view [tv | full]\n"); return GEMU_MON_NONE; }
    gemu_monitor_printf(mon, "view: %s\n", gemu_display_head(s->display) == 1 ? "full frame (debug)" : "TV picture");
    return GEMU_MON_NONE;
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

void a26_debug_init(Atari2600State *s) {
    memset(&s->dbg, 0, sizeof(s->dbg));
    s->dbg.break_line = -1;
    s->dbg.last_line = -1;
    s->dbg.full_fb = calloc((size_t)A26_FULL_W * A26_FULL_H, sizeof(uint32_t));
}

void a26_debug_destroy(Atari2600State *s) {
    trace_close(s);
    free(s->dbg.full_fb);
    s->dbg.full_fb = NULL;
}

void a26_debug_bind(Atari2600State *s) {
    GemuMonitor *mon = s->monitor;
    gemu_monitor_bind(mon, "disas",       cmd_disas,       s);
    gemu_monitor_bind(mon, "tia",         cmd_tia,         s);
    gemu_monitor_bind(mon, "riot",        cmd_riot,        s);
    gemu_monitor_bind(mon, "frame",       cmd_frame,       s);
    gemu_monitor_bind(mon, "scanline",    cmd_scanline,    s);
    gemu_monitor_bind(mon, "breakline",   cmd_breakline,   s);
    gemu_monitor_bind(mon, "trace",       cmd_trace,       s);
    gemu_monitor_bind(mon, "frameinfo",   cmd_frameinfo,   s);
    gemu_monitor_bind(mon, "debugcolors", cmd_debugcolors, s);
    gemu_monitor_bind(mon, "hide",        cmd_hide,        s);
    gemu_monitor_bind(mon, "reload",      cmd_reload,      s);
    gemu_monitor_bind(mon, "autoreload",  cmd_autoreload,  s);
    gemu_monitor_bind(mon, "view",        cmd_view,        s);
}
