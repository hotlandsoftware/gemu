/* Address spaces: see gemu/dev/space.h. */
#include "dev_priv.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "space.c assumes a little-endian host"
#endif

enum { RD = 0, WR = 1 };

/* A run of addresses in a page that decode the same way: effective address
 * = addr & mask (mirrors; all ones otherwise), served by maps[0..n) (oldest
 * first; more than one only for SHARED stacks). */
typedef struct {
    uint64_t      lo, hi;          /* space addresses, inclusive */
    uint64_t      mask;
    uint32_t      n;
    GemuMapping **maps;
} Seg;

/* One page in one direction: a direct pointer, or segments (gaps between
 * them are unmapped). */
typedef struct {
    uint8_t     *fast;             /* host memory for the page's first byte */
    GemuMapping *fast_map;         /* the mapping fast points into */
    uint32_t     nseg;
    Seg         *seg;              /* one block: the Segs, then the map pointers */
} Res;

typedef struct { Res dir[2]; } Page;

typedef struct Mirror {
    uint64_t lo, hi, mask;
    struct Mirror *next;           /* newest first */
} Mirror;

#define LEAF_BITS 10u
#define NODE_BITS 10u

struct GemuSpace {
    GemuMachine *machine;
    GemuSpace   *next;
    char         name[32];
    unsigned     addr_bits, data_bits;
    GemuEndian   endian;
    uint64_t     mask;             /* the address mask */
    unsigned     page_bits;
    uint64_t     page_mask;        /* offset within a page */
    unsigned     leaf_bits, levels, top_bits;
    void        *root;             /* inner nodes (void *[]) down to leaves (Page[]) */
    GemuMapping *maps;             /* newest first */
    uint64_t     seq, generation;
    Mirror      *mirrors;
    uint64_t     unmapped;
    void (*on_read)(void *ud, uint64_t addr, unsigned bytes);
    void (*on_write)(void *ud, uint64_t addr, unsigned bytes, uint64_t v);
    void        *unmapped_ud;
    /* scratch for building a page */
    Seg         *tseg;
    size_t       tseg_cap;
    GemuMapping **tmap;
    size_t       tmap_len, tmap_cap;
    GemuMapping **cand;
    size_t       cand_len, cand_cap;
};

static uint64_t ones(unsigned bytes) {
    return bytes >= 8 ? ~0ull : (1ull << (8 * bytes)) - 1;
}

/* ── The page table ───────────────────────────────────────────────────────── */

static unsigned level_bits(const GemuSpace *s, unsigned i) {
    return i == 0 ? s->top_bits : NODE_BITS;
}
static unsigned level_shift(const GemuSpace *s, unsigned i) {
    return s->leaf_bits + (s->levels - 1 - i) * NODE_BITS;
}

static inline Page *page_get(const GemuSpace *s, uint64_t addr) {
    uint64_t pn = addr >> s->page_bits;
    void *node = s->root;
    for (unsigned i = 0; i < s->levels; i++) {
        if (!node)
            return NULL;
        unsigned bits = level_bits(s, i);
        node = ((void **)node)[(pn >> level_shift(s, i)) & ((1ull << bits) - 1)];
    }
    if (!node)
        return NULL;
    return &((Page *)node)[pn & ((1ull << s->leaf_bits) - 1)];
}

static Page *page_make(GemuSpace *s, uint64_t addr) {
    uint64_t pn = addr >> s->page_bits;
    void **slot = &s->root;
    for (unsigned i = 0; i < s->levels; i++) {
        unsigned bits = level_bits(s, i);
        if (!*slot && !(*slot = calloc((size_t)1 << bits, sizeof(void *))))
            abort();
        slot = &((void **)*slot)[(pn >> level_shift(s, i)) & ((1ull << bits) - 1)];
    }
    if (!*slot && !(*slot = calloc((size_t)1 << s->leaf_bits, sizeof(Page))))
        abort();
    return &((Page *)*slot)[pn & ((1ull << s->leaf_bits) - 1)];
}

static void free_tree(GemuSpace *s, void *node, unsigned level) {
    if (!node)
        return;
    if (level == s->levels) {
        Page *leaf = node;
        for (size_t i = 0; i < ((size_t)1 << s->leaf_bits); i++) {
            free(leaf[i].dir[RD].seg);
            free(leaf[i].dir[WR].seg);
        }
    } else {
        void **n = node;
        for (size_t i = 0; i < ((size_t)1 << level_bits(s, level)); i++)
            free_tree(s, n[i], level + 1);
    }
    free(node);
}

/* ── Building pages ───────────────────────────────────────────────────────── */

static bool has_reads(const GemuHandlers *h) { return h->r8 || h->r16 || h->r32 || h->r64; }
static bool has_writes(const GemuHandlers *h) { return h->w8 || h->w16 || h->w32 || h->w64; }

static bool takes_part(const GemuMapping *m, int dir) {
    if (!m->enabled)
        return false;
    if (m->ram)
        return dir == RD ? !(m->flags & GEMU_MAP_WRITEONLY) : !(m->flags & GEMU_MAP_READONLY);
    return dir == RD ? has_reads(&m->h) : has_writes(&m->h);
}

static uint64_t last_of(const GemuMapping *m) { return m->base + m->size - 1; }

static void grow(void **p, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap)
        return;
    size_t n = *cap ? *cap * 2 : 16;
    while (n < need)
        n *= 2;
    void *q = realloc(*p, n * elem);
    if (!q)
        abort();
    *p = q;
    *cap = n;
}

/* The mirror deciding addr (the newest covering it), or NULL. */
static const Mirror *mirror_at(const GemuSpace *s, uint64_t addr) {
    for (const Mirror *mi = s->mirrors; mi; mi = mi->next)
        if (mi->lo <= addr && addr <= mi->hi)
            return mi;
    return NULL;
}

/* Decode the page [pa, pe] in one direction into r (the old contents are
 * freed by the caller).  The candidates are s->cand. */
static void build_res(GemuSpace *s, uint64_t pa, uint64_t pe, int dir, Res *r) {
    size_t nseg = 0;
    s->tmap_len = 0;
    uint64_t a = pa;
    for (;;) {
        /* A run [a, run_end] whose effective addresses are contiguous. */
        const Mirror *mi = mirror_at(s, a);
        uint64_t mask = mi ? (mi->mask & s->mask) : s->mask;
        uint64_t run_end = pe;
        if (mi) {
            uint64_t period = ~mask & (mask + 1);        /* lowest 0 bit of mask */
            if (period && (a | (period - 1)) < run_end)
                run_end = a | (period - 1);
            if (mi->hi < run_end)
                run_end = mi->hi;
        }
        for (const Mirror *o = s->mirrors; o; o = o->next)
            if (o->lo > a && o->lo - 1 < run_end)
                run_end = o->lo - 1;

        uint64_t eff_lo = a & mask, eff_hi = eff_lo + (run_end - a);
        uint64_t e = eff_lo;
        for (;;) {
            /* The mappings covering e, and where that next changes. */
            uint64_t end = eff_hi;
            int top = INT_MIN;
            GemuMapping *winner = NULL;
            size_t first_map = s->tmap_len;
            for (size_t i = 0; i < s->cand_len; i++) {
                GemuMapping *m = s->cand[i];
                if (!takes_part(m, dir))
                    continue;
                uint64_t ml = last_of(m);
                if (m->base <= e && e <= ml) {
                    if (ml < end)
                        end = ml;
                    if (m->priority > top) {
                        top = m->priority;
                        winner = NULL;
                    }
                } else if (m->base > e && m->base - 1 < end) {
                    end = m->base - 1;
                }
            }
            /* At the top priority: every SHARED one, and the newest other
             * one.  Candidates are newest first; collect oldest first. */
            if (top != INT_MIN) {
                for (size_t i = s->cand_len; i-- > 0;) {
                    GemuMapping *m = s->cand[i];
                    if (!takes_part(m, dir) || m->priority != top ||
                        m->base > e || e > last_of(m))
                        continue;
                    if (!(m->flags & GEMU_MAP_SHARED)) {
                        winner = m;      /* the newest wins: keep the last seen */
                        continue;
                    }
                    grow((void **)&s->tmap, &s->tmap_cap, s->tmap_len + 1, sizeof *s->tmap);
                    s->tmap[s->tmap_len++] = m;
                }
                if (winner) {
                    /* in seq order among the shared ones */
                    grow((void **)&s->tmap, &s->tmap_cap, s->tmap_len + 1, sizeof *s->tmap);
                    size_t at = s->tmap_len;
                    while (at > first_map && s->tmap[at - 1]->seq > winner->seq) {
                        s->tmap[at] = s->tmap[at - 1];
                        at--;
                    }
                    s->tmap[at] = winner;
                    s->tmap_len++;
                }
            }
            uint32_t n = (uint32_t)(s->tmap_len - first_map);
            if (n) {
                uint64_t lo = a + (e - eff_lo), hi = a + (end - eff_lo);
                Seg *prev = nseg ? &s->tseg[nseg - 1] : NULL;
                /* tmap offsets are stored in maps until packing */
                if (prev && prev->hi + 1 == lo && prev->mask == mask && prev->n == n &&
                    ((prev->hi & mask) + 1) == (lo & mask) &&
                    !memcmp(&s->tmap[(uintptr_t)prev->maps], &s->tmap[first_map], n * sizeof(GemuMapping *))) {
                    prev->hi = hi;
                    s->tmap_len = first_map;
                } else {
                    grow((void **)&s->tseg, &s->tseg_cap, nseg + 1, sizeof *s->tseg);
                    s->tseg[nseg++] = (Seg){ lo, hi, mask, n, (GemuMapping **)(uintptr_t)first_map };
                }
            }
            if (end == eff_hi)
                break;
            e = end + 1;
        }
        if (run_end == pe)
            break;
        a = run_end + 1;
    }

    r->fast = NULL;
    r->fast_map = NULL;
    r->nseg = (uint32_t)nseg;
    r->seg = NULL;
    if (!nseg)
        return;
    Seg *blk = malloc(nseg * sizeof(Seg) + s->tmap_len * sizeof(GemuMapping *));
    if (!blk)
        abort();
    GemuMapping **mp = (GemuMapping **)(blk + nseg);
    memcpy(mp, s->tmap, s->tmap_len * sizeof *mp);
    for (size_t i = 0; i < nseg; i++) {
        blk[i] = s->tseg[i];
        blk[i].maps = mp + (uintptr_t)s->tseg[i].maps;
    }
    r->seg = blk;
    if (nseg == 1 && blk[0].lo == pa && blk[0].hi == pe && blk[0].n == 1 && blk[0].maps[0]->ram) {
        GemuMapping *m = blk[0].maps[0];
        r->fast_map = m;
        r->fast = m->ram + ((pa & blk[0].mask) - m->base);
    }
}

static bool overlaps(uint64_t lo1, uint64_t hi1, uint64_t lo2, uint64_t hi2) {
    return lo1 <= hi2 && lo2 <= hi1;
}

/* Rebuild the pages covering [lo, hi]. */
static void rebuild_pages(GemuSpace *s, uint64_t lo, uint64_t hi) {
    lo &= ~s->page_mask;            /* whole pages are rebuilt */
    hi |= s->page_mask;
    /* Candidates: the mappings in the range, or all of them when a mirror
     * can bring in addresses from elsewhere. */
    bool mirrored = false;
    for (Mirror *mi = s->mirrors; mi; mi = mi->next)
        if (overlaps(mi->lo, mi->hi, lo, hi))
            mirrored = true;
    s->cand_len = 0;
    for (GemuMapping *m = s->maps; m; m = m->next)
        if (m->enabled && (mirrored || overlaps(m->base, last_of(m), lo, hi))) {
            grow((void **)&s->cand, &s->cand_cap, s->cand_len + 1, sizeof *s->cand);
            s->cand[s->cand_len++] = m;
        }

    uint64_t pn = lo >> s->page_bits, pn_last = hi >> s->page_bits;
    for (;; pn++) {
        uint64_t pa = pn << s->page_bits, pe = pa | s->page_mask;
        Res r[2];
        build_res(s, pa, pe, RD, &r[RD]);
        build_res(s, pa, pe, WR, &r[WR]);
        Page *p = (r[RD].nseg || r[WR].nseg) ? page_make(s, pa) : page_get(s, pa);
        if (p) {
            free(p->dir[RD].seg);
            free(p->dir[WR].seg);
            p->dir[RD] = r[RD];
            p->dir[WR] = r[WR];
        }
        if (pn == pn_last)
            break;
    }
}

/* After a change to the mapping at [lo, hi]: its pages, and every mirror
 * (one may show those addresses elsewhere). */
static void changed(GemuSpace *s, uint64_t lo, uint64_t hi) {
    rebuild_pages(s, lo, hi);
    for (Mirror *mi = s->mirrors; mi; mi = mi->next)
        rebuild_pages(s, mi->lo, mi->hi);
    s->generation++;
}

/* ── Spaces ───────────────────────────────────────────────────────────────── */

GemuSpace *gemu_space_new(GemuMachine *m, const char *name,
                          unsigned addr_bits, unsigned data_bits, GemuEndian e) {
    if (addr_bits < 1 || addr_bits > 64 || !name ||
        (data_bits != 8 && data_bits != 16 && data_bits != 32 && data_bits != 64))
        return NULL;
    if (m && gemu_space_find(m, name))
        return NULL;
    GemuSpace *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    snprintf(s->name, sizeof s->name, "%s", name);
    s->machine = m;
    s->addr_bits = addr_bits;
    s->data_bits = data_bits;
    s->endian = e;
    s->mask = addr_bits == 64 ? ~0ull : (1ull << addr_bits) - 1;
    s->page_bits = addr_bits > 16 ? 12 : addr_bits > 8 ? 8 : addr_bits;
    s->page_mask = (1ull << s->page_bits) - 1;
    unsigned pn_bits = addr_bits - s->page_bits;
    s->leaf_bits = pn_bits < LEAF_BITS ? pn_bits : LEAF_BITS;
    unsigned inner = pn_bits - s->leaf_bits;
    s->levels = (inner + NODE_BITS - 1) / NODE_BITS;
    s->top_bits = s->levels ? inner - (s->levels - 1) * NODE_BITS : 0;
    s->unmapped = ~0ull;
    if (m) {
        s->next = m->spaces;
        m->spaces = s;
    }
    return s;
}

void gemu_space_free(GemuSpace *s) {
    if (!s)
        return;
    if (s->machine) {
        GemuSpace **pp = &s->machine->spaces;
        while (*pp && *pp != s)
            pp = &(*pp)->next;
        if (*pp)
            *pp = s->next;
    }
    free_tree(s, s->root, 0);
    for (GemuMapping *m = s->maps, *n; m; m = n) {
        n = m->next;
        free(m);
    }
    for (Mirror *mi = s->mirrors, *n; mi; mi = n) {
        n = mi->next;
        free(mi);
    }
    free(s->tseg);
    free(s->tmap);
    free(s->cand);
    free(s);
}

void gemu_space_free_all(GemuMachine *m) {
    while (m->spaces)
        gemu_space_free(m->spaces);
}

GemuSpace *gemu_space_find(GemuMachine *m, const char *name) {
    for (GemuSpace *s = m->spaces; s; s = s->next)
        if (!strcmp(s->name, name))
            return s;
    return NULL;
}

const char *gemu_space_name(const GemuSpace *s) { return s->name; }
unsigned gemu_space_addr_bits(const GemuSpace *s) { return s->addr_bits; }
unsigned gemu_space_data_bits(const GemuSpace *s) { return s->data_bits; }
GemuEndian gemu_space_endian(const GemuSpace *s) { return s->endian; }
uint64_t gemu_space_generation(const GemuSpace *s) { return s->generation; }
GemuMapping *gemu_space_mappings(GemuSpace *s) { return s->maps; }

void gemu_space_set_unmapped(GemuSpace *s, uint64_t value,
        void (*on_read)(void *ud, uint64_t addr, unsigned bytes),
        void (*on_write)(void *ud, uint64_t addr, unsigned bytes, uint64_t v),
        void *ud) {
    s->unmapped = value;
    s->on_read = on_read;
    s->on_write = on_write;
    s->unmapped_ud = ud;
}

/* ── Mappings ─────────────────────────────────────────────────────────────── */

static bool in_space(const GemuSpace *s, uint64_t base, uint64_t size) {
    return size && base <= s->mask && size - 1 <= s->mask - base;
}

static GemuMapping *add_mapping(GemuSpace *s, uint64_t base, uint64_t size, uint8_t *ram,
                                const GemuHandlers *h, void *ud, int priority, unsigned flags) {
    if (!s || !in_space(s, base, size))
        return NULL;
    GemuMapping *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->space = s;
    m->base = base;
    m->size = size;
    m->ram = ram;
    if (h)
        m->h = *h;
    m->ud = ud;
    m->priority = priority;
    m->flags = flags;
    m->enabled = true;
    m->seq = ++s->seq;
    m->next = s->maps;
    s->maps = m;
    changed(s, base, last_of(m));
    return m;
}

GemuMapping *gemu_space_map_ram(GemuSpace *s, uint64_t base, uint64_t size,
                                uint8_t *ptr, unsigned flags, int priority) {
    if (!ptr)
        return NULL;
    return add_mapping(s, base, size, ptr, NULL, NULL, priority, flags);
}

GemuMapping *gemu_space_map(GemuSpace *s, uint64_t base, uint64_t size,
                            const GemuHandlers *h, void *ud, int priority, unsigned flags) {
    if (!h)
        return NULL;
    return add_mapping(s, base, size, NULL, h, ud, priority,
                       flags & ~(unsigned)(GEMU_MAP_READONLY | GEMU_MAP_WRITEONLY));
}

void gemu_mapping_remove(GemuMapping *mp) {
    if (!mp)
        return;
    GemuSpace *s = mp->space;
    GemuMapping **pp = &s->maps;
    while (*pp && *pp != mp)
        pp = &(*pp)->next;
    if (!*pp)
        return;
    *pp = mp->next;
    changed(s, mp->base, last_of(mp));
    free(mp);
}

int gemu_space_unmap(GemuSpace *s, uint64_t base, uint64_t size,
                     const GemuHandlers *h, void *ud) {
    int n = 0;
    for (GemuMapping *m = s->maps, *next; m; m = next) {
        next = m->next;
        if (!m->ram && m->base == base && m->size == size && m->ud == ud &&
            !memcmp(&m->h, h, sizeof *h)) {
            gemu_mapping_remove(m);
            n++;
        }
    }
    return n;
}

bool gemu_mapping_move(GemuMapping *mp, uint64_t base, uint64_t size) {
    GemuSpace *s = mp->space;
    if (!in_space(s, base, size))
        return false;
    uint64_t olo = mp->base, ohi = last_of(mp);
    mp->base = base;
    mp->size = size;
    rebuild_pages(s, olo, ohi);
    changed(s, base, last_of(mp));
    return true;
}

void gemu_mapping_enable(GemuMapping *mp, bool on) {
    if (mp->enabled == on)
        return;
    mp->enabled = on;
    changed(mp->space, mp->base, last_of(mp));
}

/* Repoint the direct pointers into mp over [lo, hi]. */
static void repoint(GemuSpace *s, GemuMapping *mp, uint64_t lo, uint64_t hi) {
    uint64_t pn = lo >> s->page_bits, pn_last = hi >> s->page_bits;
    for (;; pn++) {
        uint64_t pa = pn << s->page_bits;
        Page *p = page_get(s, pa);
        for (int d = 0; p && d < 2; d++) {
            Res *r = &p->dir[d];
            if (r->fast_map == mp)
                r->fast = mp->ram + ((pa & r->seg[0].mask) - mp->base);
        }
        if (pn == pn_last)
            break;
    }
}

void gemu_mapping_set_ptr(GemuMapping *mp, uint8_t *ptr) {
    if (!mp->ram || !ptr)
        return;            /* not a RAM mapping */
    GemuSpace *s = mp->space;
    mp->ram = ptr;
    if (mp->enabled) {
        repoint(s, mp, mp->base, last_of(mp));
        for (Mirror *mi = s->mirrors; mi; mi = mi->next)
            repoint(s, mp, mi->lo, mi->hi);
    }
    s->generation++;
}

void gemu_mapping_set_name(GemuMapping *mp, const char *name) {
    snprintf(mp->name, sizeof mp->name, "%s", name ? name : "");
}

bool gemu_space_mirror(GemuSpace *s, uint64_t base, uint64_t size, uint64_t mask) {
    if (!in_space(s, base, size))
        return false;
    Mirror *mi = calloc(1, sizeof *mi);
    if (!mi)
        return false;
    mi->lo = base;
    mi->hi = base + size - 1;
    mi->mask = mask;
    mi->next = s->mirrors;
    s->mirrors = mi;
    changed(s, mi->lo, mi->hi);
    return true;
}

/* ── Access ───────────────────────────────────────────────────────────────── */

static inline uint64_t load(const uint8_t *p, unsigned n, GemuEndian e) {
    switch (n) {
    case 1: return *p;
    case 2: { uint16_t v; memcpy(&v, p, 2); return e == GEMU_BIG_ENDIAN ? __builtin_bswap16(v) : v; }
    case 4: { uint32_t v; memcpy(&v, p, 4); return e == GEMU_BIG_ENDIAN ? __builtin_bswap32(v) : v; }
    default: { uint64_t v; memcpy(&v, p, 8); return e == GEMU_BIG_ENDIAN ? __builtin_bswap64(v) : v; }
    }
}

static inline void store(uint8_t *p, unsigned n, GemuEndian e, uint64_t v) {
    switch (n) {
    case 1: *p = (uint8_t)v; break;
    case 2: { uint16_t t = (uint16_t)v; if (e == GEMU_BIG_ENDIAN) t = __builtin_bswap16(t); memcpy(p, &t, 2); break; }
    case 4: { uint32_t t = (uint32_t)v; if (e == GEMU_BIG_ENDIAN) t = __builtin_bswap32(t); memcpy(p, &t, 4); break; }
    default: { uint64_t t = v; if (e == GEMU_BIG_ENDIAN) t = __builtin_bswap64(t); memcpy(p, &t, 8); break; }
    }
}

/* Combine two halves of `half` bytes: lo_v is at the lower address. */
static inline uint64_t join(const GemuSpace *s, uint64_t lo_v, uint64_t hi_v, unsigned half) {
    return s->endian == GEMU_LITTLE_ENDIAN ? lo_v | (hi_v << (8 * half))
                                           : (lo_v << (8 * half)) | hi_v;
}
static inline uint64_t part_lo(const GemuSpace *s, uint64_t v, unsigned half) {
    return (s->endian == GEMU_LITTLE_ENDIAN ? v : v >> (8 * half)) & ones(half);
}
static inline uint64_t part_hi(const GemuSpace *s, uint64_t v, unsigned half) {
    return (s->endian == GEMU_LITTLE_ENDIAN ? v >> (8 * half) : v) & ones(half);
}

static bool has_read(const GemuHandlers *h, unsigned n) {
    switch (n) { case 1: return h->r8; case 2: return h->r16; case 4: return h->r32; default: return h->r64; }
}
static bool has_write(const GemuHandlers *h, unsigned n) {
    switch (n) { case 1: return h->w8; case 2: return h->w16; case 4: return h->w32; default: return h->w64; }
}
static bool read_below(const GemuHandlers *h, unsigned n) {
    for (unsigned w = 1; w < n; w *= 2)
        if (has_read(h, w)) return true;
    return false;
}
static bool write_below(const GemuHandlers *h, unsigned n) {
    for (unsigned w = 1; w < n; w *= 2)
        if (has_write(h, w)) return true;
    return false;
}

static uint64_t map_read(GemuSpace *s, GemuMapping *m, uint64_t off, unsigned n) {
    if (m->ram)
        return load(m->ram + off, n, s->endian);
    const GemuHandlers *h = &m->h;
    switch (has_read(h, n) ? n : 0) {
    case 1: return h->r8(m->ud, off);
    case 2: return h->r16(m->ud, off);
    case 4: return h->r32(m->ud, off);
    case 8: return h->r64(m->ud, off);
    }
    if (read_below(h, n)) {
        unsigned half = n / 2;
        uint64_t a = map_read(s, m, off, half);
        uint64_t b = map_read(s, m, off + half, half);
        return join(s, a, b, half);
    }
    if (n < 8) {
        /* the aligned wider unit, and our part of it */
        unsigned w = 2 * n;
        uint64_t al = off & ~(uint64_t)(w - 1);
        uint64_t v = map_read(s, m, al, w);
        unsigned pos = (unsigned)(off - al);
        unsigned sh = s->endian == GEMU_LITTLE_ENDIAN ? pos : w - n - pos;
        return (v >> (8 * sh)) & ones(n);
    }
    return ones(n);
}

static void map_write(GemuSpace *s, GemuMapping *m, uint64_t off, unsigned n, uint64_t v) {
    if (m->ram) {
        store(m->ram + off, n, s->endian, v);
        return;
    }
    const GemuHandlers *h = &m->h;
    switch (has_write(h, n) ? n : 0) {
    case 1: h->w8(m->ud, off, (uint8_t)v); return;
    case 2: h->w16(m->ud, off, (uint16_t)v); return;
    case 4: h->w32(m->ud, off, (uint32_t)v); return;
    case 8: h->w64(m->ud, off, v); return;
    }
    if (write_below(h, n)) {
        unsigned half = n / 2;
        map_write(s, m, off, half, part_lo(s, v, half));
        map_write(s, m, off + half, half, part_hi(s, v, half));
    }
    /* no handler this narrow: dropped */
}

/* The segment covering addr; else NULL and *gap_end = the last address of
 * the unmapped gap from addr (within the page). */
static const Seg *find_seg(const GemuSpace *s, const Page *p, int dir, uint64_t addr,
                           uint64_t *gap_end) {
    *gap_end = addr | s->page_mask;
    if (!p)
        return NULL;
    const Res *r = &p->dir[dir];
    for (uint32_t i = 0; i < r->nseg; i++) {
        if (addr < r->seg[i].lo) {
            *gap_end = r->seg[i].lo - 1;
            return NULL;
        }
        if (addr <= r->seg[i].hi)
            return &r->seg[i];
    }
    return NULL;
}

static uint64_t read_n(GemuSpace *s, uint64_t addr, unsigned n);
static void write_n(GemuSpace *s, uint64_t addr, unsigned n, uint64_t v);

static uint64_t slow_read(GemuSpace *s, uint64_t addr, unsigned n) {
    uint64_t gap_end, last = addr + n - 1;
    const Seg *g = find_seg(s, page_get(s, addr), RD, addr, &gap_end);
    bool fits = last >= addr && last <= s->mask;
    if (g && fits && last <= g->hi) {
        uint64_t eff = addr & g->mask, v = ones(n);
        for (uint32_t i = 0; i < g->n; i++)
            v &= map_read(s, g->maps[i], eff - g->maps[i]->base, n);
        return v;
    }
    if (!g && fits && last <= gap_end) {
        if (s->on_read)
            s->on_read(s->unmapped_ud, addr, n);
        return s->unmapped & ones(n);
    }
    unsigned half = n / 2;
    uint64_t a = read_n(s, addr, half);
    uint64_t b = read_n(s, (addr + half) & s->mask, half);
    return join(s, a, b, half);
}

static void slow_write(GemuSpace *s, uint64_t addr, unsigned n, uint64_t v) {
    uint64_t gap_end, last = addr + n - 1;
    const Seg *g = find_seg(s, page_get(s, addr), WR, addr, &gap_end);
    bool fits = last >= addr && last <= s->mask;
    if (g && fits && last <= g->hi) {
        uint64_t eff = addr & g->mask;
        for (uint32_t i = 0; i < g->n; i++)
            map_write(s, g->maps[i], eff - g->maps[i]->base, n, v);
        return;
    }
    if (!g && fits && last <= gap_end) {
        if (s->on_write)
            s->on_write(s->unmapped_ud, addr, n, v);
        return;
    }
    unsigned half = n / 2;
    write_n(s, addr, half, part_lo(s, v, half));
    write_n(s, (addr + half) & s->mask, half, part_hi(s, v, half));
}

static inline uint64_t read_n(GemuSpace *s, uint64_t addr, unsigned n) {
    addr &= s->mask;
    const Page *p = page_get(s, addr);
    uint64_t off = addr & s->page_mask;
    if (p && p->dir[RD].fast && off + n - 1 <= s->page_mask)
        return load(p->dir[RD].fast + off, n, s->endian);
    return slow_read(s, addr, n);
}

static inline void write_n(GemuSpace *s, uint64_t addr, unsigned n, uint64_t v) {
    addr &= s->mask;
    const Page *p = page_get(s, addr);
    uint64_t off = addr & s->page_mask;
    if (p && p->dir[WR].fast && off + n - 1 <= s->page_mask) {
        store(p->dir[WR].fast + off, n, s->endian, v);
        return;
    }
    slow_write(s, addr, n, v);
}

uint8_t  gemu_space_read8 (GemuSpace *s, uint64_t a) { return (uint8_t)read_n(s, a, 1); }
uint16_t gemu_space_read16(GemuSpace *s, uint64_t a) { return (uint16_t)read_n(s, a, 2); }
uint32_t gemu_space_read32(GemuSpace *s, uint64_t a) { return (uint32_t)read_n(s, a, 4); }
uint64_t gemu_space_read64(GemuSpace *s, uint64_t a) { return read_n(s, a, 8); }
void gemu_space_write8 (GemuSpace *s, uint64_t a, uint8_t v)  { write_n(s, a, 1, v); }
void gemu_space_write16(GemuSpace *s, uint64_t a, uint16_t v) { write_n(s, a, 2, v); }
void gemu_space_write32(GemuSpace *s, uint64_t a, uint32_t v) { write_n(s, a, 4, v); }
void gemu_space_write64(GemuSpace *s, uint64_t a, uint64_t v) { write_n(s, a, 8, v); }

bool gemu_space_peek8(GemuSpace *s, uint64_t addr, uint8_t *v) {
    addr &= s->mask;
    const Page *p = page_get(s, addr);
    if (p && p->dir[RD].fast) {
        *v = p->dir[RD].fast[addr & s->page_mask];
        return true;
    }
    uint64_t gap_end;
    const Seg *g = find_seg(s, p, RD, addr, &gap_end);
    if (!g)
        return false;
    uint64_t eff = addr & g->mask;
    uint8_t acc = 0xFF;
    for (uint32_t i = 0; i < g->n; i++) {
        GemuMapping *m = g->maps[i];
        uint8_t b;
        if (m->ram)
            b = m->ram[eff - m->base];
        else if (!m->h.peek8 || !m->h.peek8(m->ud, eff - m->base, &b))
            return false;
        acc &= b;
    }
    *v = acc;
    return true;
}

uint8_t *gemu_space_ptr(GemuSpace *s, uint64_t addr, bool write, uint64_t *avail) {
    addr &= s->mask;
    const Page *p = page_get(s, addr);
    if (!p || !p->dir[write ? WR : RD].fast)
        return NULL;
    uint64_t off = addr & s->page_mask;
    if (avail)
        *avail = s->page_mask - off + 1;
    return p->dir[write ? WR : RD].fast + off;
}
