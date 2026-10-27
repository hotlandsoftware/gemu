/*
 * TIA - see tia.h for the model.  Register numbers come from the Stella
 * Programmer's Guide; object timing follows Andrew Towers' TIA hardware
 * notes as worked out (and verified against real consoles) in Stella's
 * TIA core: the counter values RESx loads, the render delays, the copy
 * decodes, the HMOVE clocks and their "starfield" side effects, NUSIZ
 * changes during a draw and the per-register write delays.
 */
#include "tia.h"
#include <string.h>

enum {
    VSYNC = 0x00, VBLANK, WSYNC, RSYNC, NUSIZ0, NUSIZ1, COLUP0, COLUP1,
    COLUPF, COLUBK, CTRLPF, REFP0, REFP1, PF0, PF1, PF2,
    RESP0, RESP1, RESM0, RESM1, RESBL, AUDC0, AUDC1, AUDF0,
    AUDF1, AUDV0, AUDV1, GRP0, GRP1, ENAM0, ENAM1, ENABL,
    HMP0, HMP1, HMM0, HMM1, HMBL, VDELP0, VDELP1, VDELBL,
    RESMP0, RESMP1, HMOVE, HMCLR, CXCLR,
};

/* Write delays in color clocks (registers not listed act immediately). */
static uint8_t write_delay(uint8_t reg) {
    switch (reg) {
    case VBLANK: case REFP0: case REFP1: case GRP0: case GRP1:
    case ENAM0: case ENAM1: case ENABL:                      return 1;
    case PF0: case PF1: case PF2:
    case HMP0: case HMP1: case HMM0: case HMM1: case HMBL:
    case HMCLR:                                              return 2;
    case HMOVE:                                              return 6;
    default:                                                 return 0;
    }
}

/* ── Object start decodes ────────────────────────────────────────────────── *
 *
 * Indexed by NUSIZ bits 0-2: the copy number (1 = main) at counter values
 * where a copy starts.  156 starts the main copy; the others sit 16, 32 or
 * 64 clocks on. */
static uint8_t decodes[8][TIA_WIDTH];

static void build_decodes(void) {
    memset(decodes, 0, sizeof(decodes));
    for (int n = 0; n < 8; n++) decodes[n][156] = 1;
    decodes[1][12] = 2;
    decodes[2][28] = 2;
    decodes[3][12] = 2; decodes[3][28] = 3;
    decodes[4][60] = 2;
    decodes[6][28] = 2; decodes[6][60] = 3;
}

#define PLAYER_OFFSET  (-5)   /* render counter at a player's start decode */
#define MISSILE_OFFSET (-4)   /* ...a missile's or the ball's */

/* RESPx/RESMx/RESBL load the counter so the object is next drawn a fixed
 * distance on: 157 in the visible area, 159 in HBLANK, 158 late in an
 * HMOVE-extended HBLANK. */
static uint8_t resx_counter(const Tia *t) {
    if (!t->hblank) return 157;
    return t->hctr >= 73 ? 158 : 159;
}

/* ── Players ─────────────────────────────────────────────────────────────── */

static uint8_t player_pattern(const Tia *t, int i) {
    return t->vdelp[i] ? t->grp_old[i] : t->grp_new[i];
}

static void player_set_divider(TiaObject *p, uint8_t d) {
    p->width = d;
    p->trip = d == 1 ? 0 : 1;
}

static void player_tick(Tia *t, int i) {
    TiaObject *p = &t->p[i];
    uint8_t pat = player_pattern(t, i);
    int bit = t->refp[i] ? p->sample : 7 - p->sample;
    p->on = p->rendering && p->render >= p->trip && ((pat >> bit) & 1u);

    if (decodes[p->decode][p->counter]) {
        p->rendering = true;
        p->sample = 0;
        p->render = PLAYER_OFFSET;
        p->copy = decodes[p->decode][p->counter];
    } else if (p->rendering) {
        p->render++;
        if (p->width == 1) {
            if (p->render > 0) p->sample++;
            if (p->render >= 0 && p->divider_change >= 0 && p->divider_change-- == 0)
                player_set_divider(p, p->divider_pending);
        } else {
            if (p->render > 1 && ((p->render - 1) & (p->width - 1)) == 0) p->sample++;
            if (p->render > 0 && p->divider_change >= 0 && p->divider_change-- == 0)
                player_set_divider(p, p->divider_pending);
        }
        if (p->sample > 7) p->rendering = false;
    }
    if (++p->counter >= TIA_WIDTH) p->counter = 0;
}

static void player_nusiz(Tia *t, int i, uint8_t val) {
    TiaObject *p = &t->p[i];
    uint8_t old_decode = p->decode;
    p->decode = val & 7u;
    p->divider_pending = p->decode == 5 ? 2 : p->decode == 7 ? 4 : 1;

    /* A decode can trigger on the counter value just passed. */
    uint8_t prev = (uint8_t)((p->counter + TIA_WIDTH - 1) % TIA_WIDTH);
    if (!p->rendering && decodes[p->decode][prev]) {
        p->rendering = true;
        p->sample = 0;
        p->render = PLAYER_OFFSET;
        p->copy = decodes[p->decode][prev];
    }
    if (p->decode != old_decode && p->rendering && p->render - PLAYER_OFFSET < 2 &&
        !decodes[p->decode][(p->counter - p->render + PLAYER_OFFSET + TIA_WIDTH - 1) % TIA_WIDTH])
        p->rendering = false;

    if (p->divider_pending == p->width) return;
    if (!p->rendering) { player_set_divider(p, p->divider_pending); return; }

    /* Size changes during a draw take effect part-way through. */
    bool hblank = t->hblank;
    int delta = p->render - PLAYER_OFFSET;
    switch ((p->width << 4) | p->divider_pending) {
    case 0x12: case 0x14:
        if (hblank) {
            if (delta < 4) player_set_divider(p, p->divider_pending);
            else p->divider_change = delta < 5 ? 1 : 0;
        } else {
            if (delta < 3) player_set_divider(p, p->divider_pending);
            else p->divider_change = 1;
        }
        break;
    case 0x21: case 0x41:
        if (delta < (hblank ? 4 : 3)) {
            player_set_divider(p, p->divider_pending);
        } else if (delta < (hblank ? 6 : 5)) {
            player_set_divider(p, p->divider_pending);
            p->render--;
        } else {
            p->divider_change = hblank ? 0 : 1;
        }
        break;
    case 0x42: case 0x24:
        if (p->render < 1 || (hblank && p->render % p->width == 1))
            player_set_divider(p, p->divider_pending);
        else
            p->divider_change = (int8_t)(p->width - (p->render - 1) % p->width);
        break;
    default:
        player_set_divider(p, p->divider_pending);
        break;
    }
}

static void player_resp(Tia *t, int i) {
    TiaObject *p = &t->p[i];
    uint8_t c = resx_counter(t);
    p->counter = c;
    if (p->rendering && p->render - PLAYER_OFFSET < 4)
        p->render = (int8_t)(PLAYER_OFFSET + (c - 157));
}

/* Where RESMPx puts the missile: the player's centre. */
static uint8_t player_resp_clock(const TiaObject *p) {
    int back = p->width == 1 ? 5 : p->width == 2 ? 8 : 12;
    return (uint8_t)((p->counter + TIA_WIDTH - back) % TIA_WIDTH);
}

/* ── Missiles ────────────────────────────────────────────────────────────── */

static void missile_tick(Tia *t, int i, bool regular) {
    TiaObject *m = &t->m[i];
    int hc = t->hctr;
    bool visible = m->rendering &&
        (m->render >= 0 || (m->moving && regular && m->render == -1 && m->width < 4 &&
                            (hc + 1) % 4 == 3));
    m->on = visible && t->enam[i] && !t->resmp[i];

    if (decodes[m->decode][m->counter] && !t->resmp[i]) {
        m->rendering = true;
        m->render = MISSILE_OFFSET;
        m->copy = decodes[m->decode][m->counter];
    } else if (m->rendering) {
        if (m->render == -1) {
            if (m->moving && regular) {   /* HMOVE clock merging: "starfield" */
                switch ((hc + 1) % 4) {
                case 3:
                    m->eff_width = m->width == 1 ? 2 : m->width;
                    if (m->width < 4) m->render++;
                    break;
                case 2:  m->eff_width = 0; break;
                default: m->eff_width = m->width; break;
                }
            } else {
                m->eff_width = m->width;
            }
        }
        if (++m->render >= (m->moving ? m->eff_width : m->width)) m->rendering = false;
    }
    if (++m->counter >= TIA_WIDTH) m->counter = 0;
}

static void missile_resm(Tia *t, int i) {
    TiaObject *m = &t->m[i];
    uint8_t c = resx_counter(t);
    m->counter = c;
    if (!m->rendering) return;
    if (m->render < 0) {
        m->render = (int8_t)(MISSILE_OFFSET + (c - 157));
        return;
    }
    switch (m->width) {   /* RESMx during a draw */
    case 8: m->render = (int8_t)((c - 157) + (m->render >= 4 ? 4 : 0)); break;
    case 4: m->render = (int8_t)(c - 157); break;
    case 2:
        if (t->hblank) m->rendering = m->render > 1;
        else if (m->render == 0) m->render++;
        break;
    default:
        if (t->hblank) m->rendering = m->render > 0;
        break;
    }
}

/* ── Ball ────────────────────────────────────────────────────────────────── */

static void ball_tick(Tia *t, bool regular) {
    TiaObject *b = &t->bl;
    bool enabled = t->vdelbl ? t->enabl_old : t->enabl_new;
    b->on = b->rendering && b->render >= 0 && enabled;

    bool starfield = b->moving && regular;
    if (b->counter == 156) {
        b->rendering = true;
        b->render = MISSILE_OFFSET;
        uint8_t delta = (uint8_t)((b->counter + TIA_WIDTH - b->last_move) % 4);
        if (starfield && delta == 3 && b->width < 4) b->render++;
        b->eff_width = delta == 3 ? (b->width == 1 ? 2 : b->width) : delta == 2 ? 0 : b->width;
    } else if (b->rendering && ++b->render >= (starfield ? b->eff_width : b->width)) {
        b->rendering = false;
    }
    if (++b->counter >= TIA_WIDTH) b->counter = 0;
}

/* RESBL also starts a draw straight away (the ball shows on this line). */
static void ball_resbl(Tia *t) {
    uint8_t c = resx_counter(t);
    t->bl.counter = c;
    t->bl.rendering = true;
    t->bl.render = (int8_t)(MISSILE_OFFSET + (c - 157));
}

/* ── HMOVE ───────────────────────────────────────────────────────────────── */

static void set_hm(TiaObject *o, uint8_t val) {
    o->hm_clocks = (uint8_t)((val >> 4) ^ 0x08u);
}

/* Every 4th color clock while HMOVE is running: each object takes extra
 * clocks until the ripple counter reaches its HMxx value.  In the visible
 * area the extra clock merges with the regular one instead. */
static void movement_tick(Tia *t) {
    uint8_t clk = t->movement_clock > 15 ? 0 : t->movement_clock;
    bool hb = t->hblank;
    t->bl.last_move = t->bl.counter;
    TiaObject *objs[5] = { &t->m[0], &t->m[1], &t->p[0], &t->p[1], &t->bl };
    for (int i = 0; i < 5; i++) {
        TiaObject *o = objs[i];
        if (!o->moving) continue;
        if (clk == o->hm_clocks) { o->moving = false; continue; }
        if (!hb) continue;
        switch (i) {
        case 0: missile_tick(t, 0, false); break;
        case 1: missile_tick(t, 1, false); break;
        case 2: player_tick(t, 0); break;
        case 3: player_tick(t, 1); break;
        case 4: ball_tick(t, false); break;
        }
    }
    t->movement = t->m[0].moving || t->m[1].moving || t->p[0].moving ||
                  t->p[1].moving || t->bl.moving;
    t->movement_clock++;
}

static void start_hmove(Tia *t) {
    t->movement = true;
    t->movement_clock = 0;
    if (t->hblank) t->ext_hblank = true;
    t->m[0].moving = t->m[1].moving = t->p[0].moving = t->p[1].moving = t->bl.moving = true;
}

/* ── Register writes ─────────────────────────────────────────────────────── */

static void end_frame(Tia *t) {
    if (t->debug_view)   /* clear what the new frame doesn't reach */
        for (int y = t->scanline; y < TIA_MAX_LINES; y++) {
            memset(t->dbg[t->draw][y], 0x01, TIA_LINE_CLOCKS);
            t->dbg_vsync[t->draw][y] = 0;
        }
    t->frame_lines    = t->scanline;
    t->done_first_lit = t->first_lit;
    t->done_last_lit  = t->last_lit;
    t->done_vsync_end = t->vsync_end_line;
    t->draw ^= 1;
    memset(t->fb[t->draw], 0, sizeof(t->fb[t->draw]));
    t->frame_ready = true;
    t->frame_count++;
    t->scanline = 0;
    t->first_lit = t->last_lit = -1;
    t->vsync_end_line = 0;
}

static void apply_write(Tia *t, uint8_t reg, uint8_t val) {
    switch (reg) {
    case VSYNC: {
        bool on = val & 0x02u;
        if (on && !(t->vsync & 0x02u) && t->scanline > 10) end_frame(t);
        if (!on && (t->vsync & 0x02u)) t->vsync_end_line = t->scanline;
        t->vsync = val;
        break;
    }
    case VBLANK:
        if ((t->vblank & 0x80u) && !(val & 0x80u)) t->pot_start = t->clock;
        if ((val & 0x40u) && !t->latch_inputs) t->latch[0] = t->latch[1] = true;
        t->latch_inputs = val & 0x40u;
        t->vblank = val;
        break;
    case RSYNC:  t->hctr = TIA_LINE_CLOCKS - 3; break;
    case NUSIZ0: case NUSIZ1: {
        int i = reg - NUSIZ0;
        static const uint8_t mwidth[4] = { 1, 2, 4, 8 };
        t->nusiz[i] = val;
        TiaObject *m = &t->m[i];
        m->decode = val & 7u;
        m->width = mwidth[(val >> 4) & 3u];
        if (m->rendering && m->render >= m->width) m->rendering = false;
        player_nusiz(t, i, val);
        break;
    }
    case COLUP0: case COLUP1: t->colup[reg - COLUP0] = val & 0xFEu; break;
    case COLUPF: t->colupf = val & 0xFEu; break;
    case COLUBK: t->colubk = val & 0xFEu; break;
    case CTRLPF: {
        static const uint8_t bwidth[4] = { 1, 2, 4, 8 };
        t->ctrlpf = val;
        t->bl.width = bwidth[(val >> 4) & 3u];
        break;
    }
    case REFP0: case REFP1: t->refp[reg - REFP0] = val & 0x08u; break;
    case PF0: case PF1: case PF2: t->pf[reg - PF0] = val; break;
    case RESP0: case RESP1: player_resp(t, reg - RESP0); break;
    case RESM0: case RESM1: missile_resm(t, reg - RESM0); break;
    case RESBL: ball_resbl(t); break;
    case AUDC0: case AUDC1: case AUDF0: case AUDF1: case AUDV0: case AUDV1:
        tia_sound_write(&t->sound, reg, val);
        break;
    case GRP0:   /* writing GRP0 moves GRP1 to its delayed copy */
        t->grp_new[0] = val;
        t->grp_old[1] = t->grp_new[1];
        break;
    case GRP1:   /* ...and GRP1 moves GRP0 and ENABL */
        t->grp_new[1] = val;
        t->grp_old[0] = t->grp_new[0];
        t->enabl_old  = t->enabl_new;
        break;
    case ENAM0: case ENAM1: t->enam[reg - ENAM0] = val & 0x02u; break;
    case ENABL:  t->enabl_new = val & 0x02u; break;
    case HMP0: case HMP1: set_hm(&t->p[reg - HMP0], val); break;
    case HMM0: case HMM1: set_hm(&t->m[reg - HMM0], val); break;
    case HMBL:   set_hm(&t->bl, val); break;
    case VDELP0: case VDELP1: t->vdelp[reg - VDELP0] = val & 0x01u; break;
    case VDELBL: t->vdelbl = val & 0x01u; break;
    case RESMP0: case RESMP1: t->resmp[reg - RESMP0] = val & 0x02u; break;
    case HMOVE:  start_hmove(t); break;
    case HMCLR:
        set_hm(&t->p[0], 0); set_hm(&t->p[1], 0);
        set_hm(&t->m[0], 0); set_hm(&t->m[1], 0); set_hm(&t->bl, 0);
        break;
    case CXCLR:  memset(t->cx, 0, sizeof(t->cx)); break;
    default: break;
    }
}

bool tia_write(Tia *t, uint16_t addr, uint8_t val) {
    uint8_t reg = addr & 0x3Fu;
    if (reg == WSYNC) return true;
    uint8_t d = write_delay(reg);
    if (d == 0 || t->dq_n >= TIA_DELAY_QUEUE) {
        apply_write(t, reg, val);
    } else {
        t->dq[t->dq_n++] = (TiaDelayedWrite){ t->clock + d, reg, val };
    }
    return false;
}

/* ── Reads ───────────────────────────────────────────────────────────────── */

static uint8_t input_bits(const Tia *t, int r) {
    if (r < 8) return t->cx[r];
    if (r < 12) {   /* INPT0-3: paddle capacitor charged? */
        if (t->vblank & 0x80u) return 0;   /* dumped to ground */
        uint32_t need = t->pot_charge[r - 8];
        return need != UINT32_MAX && t->clock - t->pot_start >= need ? 0x80u : 0;
    }
    if (r < 14) {   /* INPT4-5: fire buttons, active low */
        int i = r - 12;
        bool high = !t->fire[i];
        if (t->latch_inputs) high = high && t->latch[i];
        return high ? 0x80u : 0;
    }
    return 0;
}

uint8_t tia_read(Tia *t, uint16_t addr, uint8_t bus) {
    int r = addr & 0x0F;
    if ((r == 12 || r == 13) && t->latch_inputs && t->fire[r - 12])
        t->latch[r - 12] = false;
    return (uint8_t)((input_bits(t, r) & 0xC0u) | (bus & 0x3Fu));
}

uint8_t tia_peek(const Tia *t, uint16_t addr) {
    return input_bits(t, addr & 0x0F);
}

/* ── The color clock ─────────────────────────────────────────────────────── */

static bool pf_bit_at(const Tia *t, int x) {
    int col = x >> 2;
    if (col >= 20) col = t->pf_reflect ? 39 - col : col - 20;
    if (col < 4)  return (t->pf[0] >> (4 + col)) & 1u;
    if (col < 12) return (t->pf[1] >> (11 - col)) & 1u;
    return (t->pf[2] >> (col - 12)) & 1u;
}

/* Fixed colours for debugcolors (NTSC values). */
#define DBG_P0 0x46u
#define DBG_M0 0x42u
#define DBG_P1 0x1Eu
#define DBG_M1 0x18u
#define DBG_BL 0x9Eu
#define DBG_PF 0xC6u
#define DBG_BK 0x00u

static uint8_t pixel_color(const Tia *t, int x, bool p0, bool p1, bool m0, bool m1,
                           bool bl, bool pf) {
    uint8_t c0 = t->colup[0], c1 = t->colup[1], cpf = t->colupf, cbl = t->colupf, cbk = t->colubk;
    uint8_t cm0 = c0, cm1 = c1, cpfs = (t->ctrlpf & 0x02u) ? t->colup[x >= 80] : cpf;
    if (t->debug_colors) {
        c0 = DBG_P0; cm0 = DBG_M0; c1 = DBG_P1; cm1 = DBG_M1;
        cpf = cpfs = DBG_PF; cbl = DBG_BL; cbk = DBG_BK;
    }
    if (t->ctrlpf & 0x04u)          /* playfield priority: PF/BL, P0/M0, P1/M1 */
        return pf ? cpf : bl ? cbl : p0 ? c0 : m0 ? cm0 : p1 ? c1 : m1 ? cm1 : cbk;
    if (t->ctrlpf & 0x02u)          /* score mode: PF in the players' colours */
        return p0 ? c0 : m0 ? cm0 : pf ? cpfs : p1 ? c1 : m1 ? cm1 : bl ? cbl : cbk;
    return p0 ? c0 : m0 ? cm0 : p1 ? c1 : m1 ? cm1 : pf ? cpf : bl ? cbl : cbk;
}

static void draw_pixel(Tia *t, int x) {
    bool p0 = t->p[0].on, p1 = t->p[1].on, m0 = t->m[0].on, m1 = t->m[1].on;
    bool bl = t->bl.on, pf = t->pf_bit;
    bool blank = t->vblank & 0x02u;

    if (t->debug_view && t->scanline < TIA_MAX_LINES) {
        uint8_t h = t->hide;
        uint8_t c = pixel_color(t, x, p0 && !(h & TIA_HIDE_P0), p1 && !(h & TIA_HIDE_P1),
                                m0 && !(h & TIA_HIDE_M0), m1 && !(h & TIA_HIDE_M1),
                                bl && !(h & TIA_HIDE_BL), pf && !(h & TIA_HIDE_PF));
        t->dbg[t->draw][t->scanline][TIA_HBLANK + x] = (uint8_t)(c | (blank ? 1u : 0u));
    }

    if (blank) {   /* VBLANK blanks the picture and the collision latches */
        if (t->scanline < TIA_MAX_LINES)
            t->fb[t->draw][t->scanline][x] = t->debug_colors ? 0x04u : 0;
        return;
    }

    if (m0) {
        if (p1) t->cx[0] |= 0x80u;
        if (p0) t->cx[0] |= 0x40u;
        if (pf) t->cx[4] |= 0x80u;
        if (bl) t->cx[4] |= 0x40u;
        if (m1) t->cx[7] |= 0x40u;
    }
    if (m1) {
        if (p0) t->cx[1] |= 0x80u;
        if (p1) t->cx[1] |= 0x40u;
        if (pf) t->cx[5] |= 0x80u;
        if (bl) t->cx[5] |= 0x40u;
    }
    if (p0) {
        if (pf) t->cx[2] |= 0x80u;
        if (bl) t->cx[2] |= 0x40u;
        if (p1) t->cx[7] |= 0x80u;
    }
    if (p1) {
        if (pf) t->cx[3] |= 0x80u;
        if (bl) t->cx[3] |= 0x40u;
    }
    if (bl && pf) t->cx[6] |= 0x80u;

    t->line_lit = true;
    uint8_t h = t->hide;
    uint8_t color = pixel_color(t, x, p0 && !(h & TIA_HIDE_P0), p1 && !(h & TIA_HIDE_P1),
                                m0 && !(h & TIA_HIDE_M0), m1 && !(h & TIA_HIDE_M1),
                                bl && !(h & TIA_HIDE_BL), pf && !(h & TIA_HIDE_PF));
    if (t->scanline < TIA_MAX_LINES) t->fb[t->draw][t->scanline][x] = color;
}

static void new_line(Tia *t) {
    if (t->line_lit) {
        if (t->first_lit < 0) t->first_lit = t->scanline;
        t->last_lit = t->scanline;
    }
    t->line_lit = false;
    t->hctr = 0;
    t->hblank = true;
    t->ext_hblank = false;
    if (++t->scanline >= TIA_MAX_LINES) end_frame(t);   /* no VSYNC: the TV rolls */
}

static void tick(Tia *t) {
    /* Delayed register writes due now */
    for (int i = 0; i < t->dq_n; ) {
        if (t->dq[i].when <= t->clock) {
            TiaDelayedWrite w = t->dq[i];
            memmove(&t->dq[i], &t->dq[i + 1], (size_t)(--t->dq_n - i) * sizeof(w));
            apply_write(t, w.reg, w.val);
        } else {
            i++;
        }
    }

    tia_sound_clock(&t->sound);

    if (t->movement && (t->hctr & 3) == 0) movement_tick(t);

    if (t->hblank) {
        if (t->hctr == (t->ext_hblank ? TIA_HBLANK + 7 : TIA_HBLANK - 1)) t->hblank = false;
        int x = t->hctr - TIA_HBLANK;
        if (x >= 0 && t->scanline < TIA_MAX_LINES)
            t->fb[t->draw][t->scanline][x] = 0;   /* the HMOVE comb */
        if (t->debug_view && t->scanline < TIA_MAX_LINES) {
            t->dbg[t->draw][t->scanline][t->hctr] = 0x01u;
            if (t->hctr == 0) t->dbg_vsync[t->draw][t->scanline] = 0;
            if (t->vsync & 0x02u) t->dbg_vsync[t->draw][t->scanline] = 1;
        }
    } else {
        int x = t->hctr - TIA_HBLANK;
        if (x == 0 || x == 79) t->pf_reflect = t->ctrlpf & 0x01u;
        if ((x & 3) == 0) t->pf_bit = pf_bit_at(t, x);
        player_tick(t, 0);
        player_tick(t, 1);
        missile_tick(t, 0, true);
        if (t->resmp[0] && t->p[0].rendering && t->p[0].sample == 4 && t->p[0].copy == 1)
            t->m[0].counter = player_resp_clock(&t->p[0]);
        missile_tick(t, 1, true);
        if (t->resmp[1] && t->p[1].rendering && t->p[1].sample == 4 && t->p[1].copy == 1)
            t->m[1].counter = player_resp_clock(&t->p[1]);
        ball_tick(t, true);
        draw_pixel(t, x);
    }

    t->clock++;
    if (++t->hctr >= TIA_LINE_CLOCKS) new_line(t);
}

int tia_object_x(const Tia *t, const TiaObject *o, int start_delay) {
    /* Counter ticks happen on visible clocks; the main copy's decode is at
     * 156 and its first pixel start_delay ticks later. */
    int x0 = t->hctr - TIA_HBLANK;
    if (x0 < 0 || t->hblank) x0 = t->ext_hblank ? 8 : 0;
    int ticks = (156 - o->counter + TIA_WIDTH) % TIA_WIDTH;
    return (x0 + ticks + start_delay) % TIA_WIDTH;
}

void tia_run(Tia *t, uint64_t target) {
    while (t->clock < target) tick(t);
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void tia_reset(Tia *t) {
    TiaSound sound = t->sound;
    uint64_t clock = t->clock, frames = t->frame_count;
    uint32_t pots[4];
    memcpy(pots, t->pot_charge, sizeof(pots));
    bool fire0 = t->fire[0], fire1 = t->fire[1];
    memset(t, 0, sizeof(*t));
    t->sound = sound;
    tia_sound_reset(&t->sound);
    t->clock = clock;
    t->frame_count = frames;
    memcpy(t->pot_charge, pots, sizeof(pots));
    t->fire[0] = fire0; t->fire[1] = fire1;
    t->hblank = true;
    t->first_lit = t->last_lit = t->done_first_lit = t->done_last_lit = -1;
    t->frame_lines = 262;
    t->latch[0] = t->latch[1] = true;
    TiaObject *objs[5] = { &t->p[0], &t->p[1], &t->m[0], &t->m[1], &t->bl };
    for (int i = 0; i < 5; i++) {
        set_hm(objs[i], 0);
        objs[i]->width = 1;
        objs[i]->eff_width = 1;
        objs[i]->divider_change = -1;
        objs[i]->copy = 1;
    }
    player_set_divider(&t->p[0], 1);
    player_set_divider(&t->p[1], 1);
    t->p[0].divider_pending = t->p[1].divider_pending = 1;
}

void tia_init(Tia *t) {
    static bool built;
    if (!built) { build_decodes(); built = true; }
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 4; i++) t->pot_charge[i] = UINT32_MAX;
    tia_reset(t);
}
