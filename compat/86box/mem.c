/*
 * GEMU's 86Box layer: memory mappings (<86box/mem.h>) as core mappings in
 * the machine's ISA memory space; ROM images (<86box/rom.h>); a device's
 * NVRAM files (<86box/nvr.h>).
 *
 * rom_read/rom_write and rom_init/rom_load_linear follow 86Box's
 * src/mem/rom.c (GPLv2 or later; Sarah Walker, Miran Grca, Fred N. van
 * Kempen).
 */
#include "compat_priv.h"
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/nvr.h>
#include <86box/rom.h>
#include "gemu/dev/space.h"
#include "gemu/util.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct Gemu86BoxMem {
    Gemu86BoxDev  *owner;
    mem_mapping_t *map;
    GemuMapping   *mp;               /* NULL while disabled or empty */
    Gemu86BoxMem  *next;             /* the owner's */
};

/* ── Mappings ─────────────────────────────────────────────────────────────── */

static uint8_t r8(void *ud, uint64_t off) {
    Gemu86BoxMem *r = ud;
    mem_mapping_t *map = r->map;
    Gemu86BoxDev *prev = gemu_86box_enter(r->owner);
    uint8_t v = map->read_b ? map->read_b(map->base + (uint32_t)off, map->priv)
                            : map->exec[off];
    gemu_86box_leave(prev);
    return v;
}
static uint16_t r16(void *ud, uint64_t off) {
    Gemu86BoxMem *r = ud;
    mem_mapping_t *map = r->map;
    Gemu86BoxDev *prev = gemu_86box_enter(r->owner);
    uint16_t v = map->read_w(map->base + (uint32_t)off, map->priv);
    gemu_86box_leave(prev);
    return v;
}
static uint32_t r32(void *ud, uint64_t off) {
    Gemu86BoxMem *r = ud;
    mem_mapping_t *map = r->map;
    Gemu86BoxDev *prev = gemu_86box_enter(r->owner);
    uint32_t v = map->read_l(map->base + (uint32_t)off, map->priv);
    gemu_86box_leave(prev);
    return v;
}
static void w8(void *ud, uint64_t off, uint8_t v) {
    Gemu86BoxMem *r = ud;
    mem_mapping_t *map = r->map;
    Gemu86BoxDev *prev = gemu_86box_enter(r->owner);
    map->write_b(map->base + (uint32_t)off, v, map->priv);
    gemu_86box_leave(prev);
}
static void w16(void *ud, uint64_t off, uint16_t v) {
    Gemu86BoxMem *r = ud;
    mem_mapping_t *map = r->map;
    Gemu86BoxDev *prev = gemu_86box_enter(r->owner);
    map->write_w(map->base + (uint32_t)off, v, map->priv);
    gemu_86box_leave(prev);
}
static void w32(void *ud, uint64_t off, uint32_t v) {
    Gemu86BoxMem *r = ud;
    mem_mapping_t *map = r->map;
    Gemu86BoxDev *prev = gemu_86box_enter(r->owner);
    map->write_l(map->base + (uint32_t)off, v, map->priv);
    gemu_86box_leave(prev);
}
/* The monitor's look: what the mapping's memory holds, when it has some */
static bool peek(void *ud, uint64_t off, uint8_t *v) {
    const mem_mapping_t *map = ((Gemu86BoxMem *)ud)->map;
    if (!map->exec)
        return false;
    *v = map->exec[off];
    return true;
}

static GemuSpace *mem_space(Gemu86BoxDev *b) {
    GemuSpace *s = gemu_space_find(b->dev->machine, "isa-memory");
    if (!s)
        s = gemu_space_find(b->dev->machine, "program");
    if (!s)
        fatal("the machine has no \"isa-memory\" or \"program\" space for 86Box memory\n");
    return s;
}

/* The core mapping again, from the structure: where, whether, which ways. */
static void remap(Gemu86BoxMem *r) {
    mem_mapping_t *map = r->map;
    if (r->mp) {
        gemu_mapping_remove(r->mp);
        r->mp = NULL;
    }
    if (!map->enable || !map->size)
        return;
    GemuHandlers h = {
        .r8 = (map->read_b || map->exec) ? r8 : NULL,
        .r16 = map->read_w ? r16 : NULL, .r32 = map->read_l ? r32 : NULL,
        .w8 = map->write_b ? w8 : NULL, .w16 = map->write_w ? w16 : NULL,
        .w32 = map->write_l ? w32 : NULL,
        .peek8 = map->exec ? peek : NULL,
    };
    if (!h.r8 && !h.r16 && !h.r32 && !h.w8 && !h.w16 && !h.w32)
        return;
    if (!(r->mp = gemu_space_map(mem_space(r->owner), map->base, map->size, &h, r, 0, 0)))
        fatal("mem_mapping: %05X-%05X isn't in the memory space\n", map->base,
              map->base + map->size - 1);
    gemu_mapping_set_name(r->mp, r->owner->dev->path);
}

static Gemu86BoxMem *record(mem_mapping_t *map, const char *what) {
    Gemu86BoxMem *r = map->gemu;
    if (!r)
        fatal("%s: a mapping mem_mapping_add() didn't set up\n", what);
    return r;
}

void mem_mapping_set(mem_mapping_t *map, uint32_t base, uint32_t size,
                     uint8_t (*read_b)(uint32_t, void *), uint16_t (*read_w)(uint32_t, void *),
                     uint32_t (*read_l)(uint32_t, void *),
                     void (*write_b)(uint32_t, uint8_t, void *),
                     void (*write_w)(uint32_t, uint16_t, void *),
                     void (*write_l)(uint32_t, uint32_t, void *),
                     uint8_t *exec, uint32_t flags, void *priv) {
    Gemu86BoxDev *b = gemu_86box_need("mem_mapping_add");
    Gemu86BoxMem *r = map->gemu;
    if (!r) {
        if (!(r = calloc(1, sizeof *r)))
            fatal("mem_mapping_add: out of memory\n");
        r->owner = b;
        r->map = map;
        r->next = b->mem;
        b->mem = r;
    }
    map->enable = size != 0;
    map->base = base;
    map->size = size;
    map->mask = size ? 0xffffffff : 0;
    map->read_b = read_b;
    map->read_w = read_w;
    map->read_l = read_l;
    map->write_b = write_b;
    map->write_w = write_w;
    map->write_l = write_l;
    map->exec = exec;
    map->flags = flags;
    map->priv = priv;
    map->prev = map->next = NULL;
    map->gemu = r;
    remap(r);
}

void mem_mapping_add(mem_mapping_t *map, uint32_t base, uint32_t size,
                     uint8_t (*read_b)(uint32_t, void *), uint16_t (*read_w)(uint32_t, void *),
                     uint32_t (*read_l)(uint32_t, void *),
                     void (*write_b)(uint32_t, uint8_t, void *),
                     void (*write_w)(uint32_t, uint16_t, void *),
                     void (*write_l)(uint32_t, uint32_t, void *),
                     uint8_t *exec, uint32_t flags, void *priv) {
    mem_mapping_set(map, base, size, read_b, read_w, read_l, write_b, write_w, write_l, exec,
                    flags, priv);
}

void mem_mapping_set_handler(mem_mapping_t *map,
                             uint8_t (*read_b)(uint32_t, void *), uint16_t (*read_w)(uint32_t, void *),
                             uint32_t (*read_l)(uint32_t, void *),
                             void (*write_b)(uint32_t, uint8_t, void *),
                             void (*write_w)(uint32_t, uint16_t, void *),
                             void (*write_l)(uint32_t, uint32_t, void *)) {
    map->read_b = read_b;
    map->read_w = read_w;
    map->read_l = read_l;
    map->write_b = write_b;
    map->write_w = write_w;
    map->write_l = write_l;
    remap(record(map, "mem_mapping_set_handler"));
}

void mem_mapping_set_write_handler(mem_mapping_t *map,
                                   void (*write_b)(uint32_t, uint8_t, void *),
                                   void (*write_w)(uint32_t, uint16_t, void *),
                                   void (*write_l)(uint32_t, uint32_t, void *)) {
    map->write_b = write_b;
    map->write_w = write_w;
    map->write_l = write_l;
    remap(record(map, "mem_mapping_set_write_handler"));
}

void mem_mapping_set_p(mem_mapping_t *map, void *priv) { map->priv = priv; }

void mem_mapping_set_addr(mem_mapping_t *map, uint32_t base, uint32_t size) {
    map->enable = 1;
    map->base = base;
    map->size = size;
    remap(record(map, "mem_mapping_set_addr"));
}

void mem_mapping_set_exec(mem_mapping_t *map, uint8_t *exec) {
    map->exec = exec;
    remap(record(map, "mem_mapping_set_exec"));
}

void mem_mapping_disable(mem_mapping_t *map) {
    map->enable = 0;
    remap(record(map, "mem_mapping_disable"));
}

void mem_mapping_enable(mem_mapping_t *map) {
    map->enable = 1;
    remap(record(map, "mem_mapping_enable"));
}

void gemu_86box_mem_post_load(Gemu86BoxDev *b) {
    for (Gemu86BoxMem *r = b->mem; r; r = r->next)
        remap(r);
}

void gemu_86box_mem_free(Gemu86BoxDev *b) {
    while (b->mem) {
        Gemu86BoxMem *r = b->mem;
        b->mem = r->next;
        if (r->mp)
            gemu_mapping_remove(r->mp);
        free(r);
    }
    for (unsigned i = 0; i < b->n_roms; i++)
        free(b->roms[i]);
    free(b->roms);
    b->roms = NULL;
    b->n_roms = 0;
}

/* ── ROM images (86Box's src/mem/rom.c) ───────────────────────────────────── */

FILE *rom_fopen(const char *fn, char *mode) {
    return fn && mode ? fopen(fn, mode) : NULL;
}

int rom_present(const char *fn) {
    struct stat st;
    return fn && stat(fn, &st) == 0 && S_ISREG(st.st_mode);
}

uint8_t rom_read(uint32_t addr, void *priv) {
    const rom_t *rom = priv;
    if (addr < rom->mapping.base || addr >= rom->mapping.base + rom->sz)
        return 0xff;
    return rom->rom[(addr - rom->mapping.base) & rom->mask];
}

uint16_t rom_readw(uint32_t addr, void *priv) {
    const rom_t *rom = priv;
    if (addr < rom->mapping.base - 1 || addr >= rom->mapping.base + rom->sz)
        return 0xffff;
    uint16_t v;
    memcpy(&v, &rom->rom[(addr - rom->mapping.base) & rom->mask], 2);
    return v;
}

uint32_t rom_readl(uint32_t addr, void *priv) {
    const rom_t *rom = priv;
    if (addr < rom->mapping.base - 3 || addr >= rom->mapping.base + rom->sz)
        return 0xffffffff;
    uint32_t v;
    memcpy(&v, &rom->rom[(addr - rom->mapping.base) & rom->mask], 4);
    return v;
}

void rom_write(uint32_t addr, uint8_t val, void *priv) {
    const rom_t *rom = priv;
    if (addr < rom->mapping.base || addr >= rom->mapping.base + rom->sz)
        return;
    rom->rom[(addr - rom->mapping.base) & rom->mask] = val;
}

void rom_writew(uint32_t addr, uint16_t val, void *priv) {
    const rom_t *rom = priv;
    if (addr < rom->mapping.base - 1 || addr >= rom->mapping.base + rom->sz)
        return;
    memcpy(&rom->rom[(addr - rom->mapping.base) & rom->mask], &val, 2);
}

void rom_writel(uint32_t addr, uint32_t val, void *priv) {
    const rom_t *rom = priv;
    if (addr < rom->mapping.base - 3 || addr >= rom->mapping.base + rom->sz)
        return;
    memcpy(&rom->rom[(addr - rom->mapping.base) & rom->mask], &val, 4);
}

int rom_load_linear(const char *fn, uint32_t addr, int sz, int off, uint8_t *ptr) {
    FILE *fp = rom_fopen(fn, "rb");
    if (!fp) {
        pclog("gemu: 86Box layer: ROM image '%s' not found\n", fn ? fn : "(none)");
        return 0;
    }
    /* only the offset within the bottom 256K, as 86Box */
    addr = addr >= 0x40000 ? 0 : (addr & 0x03ffff);
    if (ptr) {
        if (fseek(fp, off, SEEK_SET) == -1)
            fatal("rom_load_linear(): can't seek in '%s'\n", fn);
        (void)!fread(ptr + addr, 1, (size_t)sz, fp);     /* a shorter image leaves FFs */
    }
    fclose(fp);
    return 1;
}

int rom_init(rom_t *rom, const char *fn, uint32_t addr, int sz, int mask, int off, uint32_t flags) {
    Gemu86BoxDev *b = gemu_86box_need("rom_init");
    if (sz <= 0)
        return -1;
    rom->rom = malloc((size_t)sz);
    if (!rom->rom)
        fatal("rom_init: out of memory\n");
    memset(rom->rom, 0xff, (size_t)sz);
    if (!rom_load_linear(fn, addr, sz, off, rom->rom)) {
        free(rom->rom);
        rom->rom = NULL;
        return -1;
    }
    /* the layer's: freed with the device, after its close */
    uint8_t **roms = realloc(b->roms, (b->n_roms + 1) * sizeof *roms);
    if (!roms)
        fatal("rom_init: out of memory\n");
    b->roms = roms;
    b->roms[b->n_roms++] = rom->rom;
    rom->sz = sz;
    rom->mask = (uint32_t)mask;
    mem_mapping_add(&rom->mapping, addr, (uint32_t)sz, rom_read, rom_readw, rom_readl,
                    NULL, NULL, NULL, rom->rom, flags | MEM_MAPPING_ROM_WS, rom);
    return 0;
}

/* ── A device's NVRAM files ───────────────────────────────────────────────── */

FILE *nvr_fopen(char *str, char *mode) {
    char name[512], path[1024], dir[1024];
    snprintf(name, sizeof name, "86box/%s", str);
    gemu_config_path(path, sizeof path, name);
    if (mode && (mode[0] == 'w' || mode[0] == 'a')) {
        gemu_config_path(dir, sizeof dir, "86box");
        gemu_ensure_parent_dir(dir);       /* the settings directory, */
        gemu_ensure_parent_dir(path);      /* then 86box/ in it */
    }
    return fopen(path, mode);
}
