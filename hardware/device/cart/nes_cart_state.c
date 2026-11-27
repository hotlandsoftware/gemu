/* The NES cartridge's save state: see nes_cart_state.h. */
#include "cart/nes_cart_state.h"
#include "cart/nes_cart_internal.h"
#include "gemu/dev/state.h"
#include <stdio.h>
#include <string.h>

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* ── "info": which cartridge this is ──
 * The header's fields and chr_is_ram, plus the PRG and CHR image sizes
 * (0 for an empty slot).  The check refuses a state from a cartridge with
 * a different mapper, PRG or CHR size, or CHR ROM/RAM (what
 * nes_snapshot_load checked); the load then writes the fields back (the
 * same values, from the same header). */

typedef struct {
    uint8_t  prg_banks, chr_banks, mapper, mirror, has_battery, chr_is_ram;
    uint32_t prg_size, chr_size;
} CartId;

static void id_of(const NesCart *c, CartId *id) {
    id->prg_banks   = c->info.prg_banks;
    id->chr_banks   = c->info.chr_banks;
    id->mapper      = c->info.mapper;
    id->mirror      = c->info.mirror;
    id->has_battery = c->info.has_battery;
    id->chr_is_ram  = c->chr_is_ram;
    id->prg_size    = (uint32_t)nes_cart_prg_size(c);
    id->chr_size    = (uint32_t)nes_cart_chr_size(c);
}
static void info_save(void *ud, GemuStateWriter *w) {
    CartId id;
    id_of(ud, &id);
    uint8_t b[6] = { id.prg_banks, id.chr_banks, id.mapper, id.mirror, id.has_battery,
                     id.chr_is_ram };
    gemu_state_write(w, b, sizeof b);
    gemu_state_write_u32(w, id.prg_size);
    gemu_state_write_u32(w, id.chr_size);
}
static bool info_read(GemuStateReader *r, CartId *id) {
    uint8_t b[6];
    if (gemu_state_left(r) != sizeof b + 8 || !gemu_state_read(r, b, sizeof b) ||
        !gemu_state_read_u32(r, &id->prg_size) || !gemu_state_read_u32(r, &id->chr_size)) {
        gemu_state_refuse(r, "nes cartridge: bad info item");
        return false;
    }
    id->prg_banks = b[0]; id->chr_banks = b[1]; id->mapper = b[2]; id->mirror = b[3];
    id->has_battery = b[4]; id->chr_is_ram = b[5];
    return true;
}
static bool info_check(void *ud, GemuStateReader *r) {
    CartId s, h;
    if (!info_read(r, &s))
        return false;
    id_of(ud, &h);
    if (s.mapper != h.mapper || s.prg_size != h.prg_size || s.chr_size != h.chr_size ||
        (s.chr_is_ram != 0) != (h.chr_is_ram != 0)) {
        gemu_state_refuse(r, "nes cartridge: the state is for mapper %u, %u KB PRG, %u KB CHR %s;"
                          " the slot holds mapper %u, %u KB PRG, %u KB CHR %s",
                          s.mapper, (unsigned)(s.prg_size / 1024), (unsigned)(s.chr_size / 1024),
                          s.chr_is_ram ? "RAM" : "ROM",
                          h.mapper, (unsigned)(h.prg_size / 1024), (unsigned)(h.chr_size / 1024),
                          h.chr_is_ram ? "RAM" : "ROM");
        return false;
    }
    return true;
}
static bool info_load(void *ud, GemuStateReader *r) {
    NesCart *c = ud;
    CartId s;
    if (!info_read(r, &s))
        return false;
    c->info.prg_banks   = s.prg_banks;
    c->info.chr_banks   = s.chr_banks;
    c->info.mapper      = s.mapper;
    c->info.mirror      = s.mirror;
    c->info.has_battery = s.has_battery != 0;
    c->chr_is_ram       = s.chr_is_ram != 0;
    return true;
}

/* ── "chr_ram": the CHR RAM (all of CHR on a CHR-RAM cartridge, the 8 KB
 * after the ROM on TQROM; none otherwise - nes_cart_chr_ram) ── */

static void chr_save(void *ud, GemuStateWriter *w) {
    size_t len;
    uint8_t *p = nes_cart_chr_ram(ud, &len);
    gemu_state_write_u32(w, (uint32_t)len);
    if (len)
        gemu_state_write(w, p, len);
}
static bool chr_check(void *ud, GemuStateReader *r) {
    size_t len;
    uint32_t n;
    nes_cart_chr_ram(ud, &len);
    if (!gemu_state_read_u32(r, &n) || gemu_state_left(r) != n) {
        gemu_state_refuse(r, "nes chr_ram: bad size");
        return false;
    }
    if (n != len) {
        gemu_state_refuse(r, "nes chr_ram: %u bytes in the state, the cartridge has %u",
                          (unsigned)n, (unsigned)len);
        return false;
    }
    return true;
}
static bool chr_load(void *ud, GemuStateReader *r) {
    size_t len;
    uint32_t n;
    uint8_t *p = nes_cart_chr_ram(ud, &len);
    if (!gemu_state_read_u32(r, &n) || n != len)
        return false;
    return !len || gemu_state_read(r, p, len);
}

/* ── "board": the board registers in the union m ──
 * Only one view of the union is live, picked by the board (ops, which the
 * mapper decides: the info check has already refused a different one).
 * The live view's fields are saved packed - no padding, nothing of the
 * other views.  Boards that keep nothing in m save nothing; a board not
 * listed in board_view (one added later) saves the whole union as bytes,
 * so it is never silently lost - add it to board_view. */

#define F(type, member) GEMU_STATE_FIELD(type, member)
static const GemuStateField board_fields[] = {
    F(NesBoard, regs), F(NesBoard, cmd), F(NesBoard, chr), F(NesBoard, nt),
    F(NesBoard, chr_disabled), F(NesBoard, prg6_rom), F(NesBoard, prg6_ram),
    F(NesBoard, prg6_offset),
    F(NesBoard, irq_counter), F(NesBoard, irq_latch), F(NesBoard, irq_enabled),
    F(NesBoard, irq_counting), F(NesBoard, irq_after), F(NesBoard, irq_mode),
    F(NesBoard, irq_reload), F(NesBoard, irq_prescaler),
    F(NesBoard, n163_ram), F(NesBoard, n163_addr),
};
static const GemuStateField mmc1_fields[] = {
    F(NesMmc1, shift), F(NesMmc1, shift_count), F(NesMmc1, ctrl),
    F(NesMmc1, chr0), F(NesMmc1, chr1), F(NesMmc1, prg),
};
static const GemuStateField mmc3_fields[] = {
    F(NesMmc3, bank_sel), F(NesMmc3, regs), F(NesMmc3, irq_latch), F(NesMmc3, irq_counter),
    F(NesMmc3, irq_reload), F(NesMmc3, irq_enabled),
};
static const GemuStateField mmc5_fields[] = {
    F(NesMmc5, prg_mode), F(NesMmc5, chr_mode), F(NesMmc5, exram_mode), F(NesMmc5, nt_map),
    F(NesMmc5, fill_tile), F(NesMmc5, fill_attr), F(NesMmc5, prg_regs), F(NesMmc5, chr_regs),
    F(NesMmc5, chr_hi), F(NesMmc5, irq_target), F(NesMmc5, irq_enabled),
    F(NesMmc5, irq_pending), F(NesMmc5, in_frame), F(NesMmc5, irq_counter), F(NesMmc5, mul),
    F(NesMmc5, last_chr_bg), F(NesMmc5, exattr_latch), F(NesMmc5, prg_is_ram),
    F(NesMmc5, exram),
};
static const GemuStateField mmc2_fields[] = {
    F(NesMmc2, prg_reg), F(NesMmc2, chr_lfd), F(NesMmc2, chr_lfe), F(NesMmc2, chr_rfd),
    F(NesMmc2, chr_rfe), F(NesMmc2, latch_l), F(NesMmc2, latch_r),
};
static const GemuStateField vrc6_fields[] = {
    F(NesVrc6, irq_latch), F(NesVrc6, irq_counter), F(NesVrc6, irq_enabled),
    F(NesVrc6, irq_after), F(NesVrc6, irq_mode), F(NesVrc6, prescaler),
    F(NesVrc6, p1_ctrl), F(NesVrc6, p1_period), F(NesVrc6, p1_en), F(NesVrc6, p1_timer),
    F(NesVrc6, p1_seq),
    F(NesVrc6, p2_ctrl), F(NesVrc6, p2_period), F(NesVrc6, p2_en), F(NesVrc6, p2_timer),
    F(NesVrc6, p2_seq),
    F(NesVrc6, saw_rate), F(NesVrc6, saw_period), F(NesVrc6, saw_en), F(NesVrc6, saw_timer),
    F(NesVrc6, saw_accum), F(NesVrc6, saw_step), F(NesVrc6, halt),
};
static const GemuStateField m178_fields[] = {
    F(NesM178, mode), F(NesM178, prg_lo), F(NesM178, prg_hi),
};
#undef F
/* A board this file doesn't know (added after it): the whole union, raw. */
static const GemuStateField whole_union[] = { { 0, sizeof(((NesCart *)0)->m) } };

typedef struct {
    const GemuStateField *f;
    size_t n;
} BoardView;
#define VIEW(fields) ((BoardView){ fields, sizeof fields / sizeof fields[0] })

/* The view of m the board uses (see each board file's c->m.<view>).
 * The boards of nes_simple.c but m178 (NROM, UxROM, CNROM, AxROM, GxROM,
 * Action 52, the VS. System) keep everything in the generic windows. */
static BoardView board_view(const NesCart *c) {
    const NesMapper *o = c->ops;
    if (!o)                                                  return (BoardView){ NULL, 0 };
    if (o == &nes_mapper_mmc1)                               return VIEW(mmc1_fields);
    if (o == &nes_mapper_mmc3 || o == &nes_mapper_tqrom)     return VIEW(mmc3_fields);
    if (o == &nes_mapper_mmc5)                               return VIEW(mmc5_fields);
    if (o == &nes_mapper_mmc2)                               return VIEW(mmc2_fields);
    if (o == &nes_mapper_vrc6a || o == &nes_mapper_vrc6b)    return VIEW(vrc6_fields);
    if (o == &nes_mapper_m178)                               return VIEW(m178_fields);
    for (int i = 0; nes_boards[i]; i++)
        if (o == nes_boards[i])                              return VIEW(board_fields);
    if (o == &nes_mapper_nrom || o == &nes_mapper_uxrom || o == &nes_mapper_cnrom ||
        o == &nes_mapper_axrom || o == &nes_mapper_gxrom || o == &nes_mapper_m228 ||
        o == &nes_mapper_vs)                                 return (BoardView){ NULL, 0 };
    return VIEW(whole_union);
}
static size_t view_size(BoardView v) {
    size_t n = 0;
    for (size_t i = 0; i < v.n; i++)
        n += v.f[i].size;
    return n;
}

static void board_save(void *ud, GemuStateWriter *w) {
    NesCart *c = ud;
    BoardView v = board_view(c);
    const uint8_t *base = (const uint8_t *)&c->m;
    for (size_t i = 0; i < v.n; i++)
        gemu_state_write(w, base + v.f[i].offset, v.f[i].size);
}
static bool board_check(void *ud, GemuStateReader *r) {
    size_t want = view_size(board_view(ud));
    if (gemu_state_left(r) != want) {
        gemu_state_refuse(r, "nes board: %llu bytes of board registers, %llu expected",
                          (unsigned long long)gemu_state_left(r), (unsigned long long)want);
        return false;
    }
    return true;
}
static bool board_load(void *ud, GemuStateReader *r) {
    NesCart *c = ud;
    BoardView v = board_view(c);
    if (gemu_state_left(r) != view_size(v))
        return false;
    uint8_t *base = (uint8_t *)&c->m;
    for (size_t i = 0; i < v.n; i++)
        if (!gemu_state_read(r, base + v.f[i].offset, v.f[i].size))
            return false;
    return true;
}

/* NesCart, in order.  Excluded:
 *   info, chr_is_ram   which cartridge it is: custom item "info", which
 *                      refuses a state from a different cartridge
 *   ops                the board (a host pointer; follows from the mapper)
 *   prg, chr           the PRG and CHR images (heap, from the file): not
 *                      state - except the CHR RAM in chr, custom item
 *                      "chr_ram" (nes_cart_chr_ram's bytes)
 *   ppu, irq_changed, ud   the wiring
 *   m                  a union: custom item "board", the live view's fields
 *                      packed (board_view: picked by ops)
 * Registered as members: prg_off, chr_off, chr_wmask, irq, prg_ram. */
bool nes_cart_state_add(GemuDevice *d, const char *prefix, NesCart *x) {
    bool ok = true;
    char name[64];
    const char *p = prefix ? prefix : "";
    snprintf(name, sizeof name, "%sinfo", p);
    ok &= gemu_state_add_custom(d, name, info_save, info_check, info_load, x);
    snprintf(name, sizeof name, "%schr_ram", p);
    ok &= gemu_state_add_custom(d, name, chr_save, chr_check, chr_load, x);
    M(prg_off); M(chr_off); M(chr_wmask); M(irq); M(prg_ram);
    snprintf(name, sizeof name, "%sboard", p);
    ok &= gemu_state_add_custom(d, name, board_save, board_check, board_load, x);
    return ok;
}
