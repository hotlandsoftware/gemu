/*
 * Address spaces: memory and I/O maps.  (.work/modular-devices, 2.2)
 *
 * A space has 1-64 address bits and an endianness.  Mappings put RAM/ROM (a
 * host pointer) or handlers on a range of it.  Reads and writes are decoded
 * separately: a mapping takes part in a direction when it can do it (RAM
 * unless WRITEONLY/READONLY; handlers when they have a read/write function
 * of any width).  So a ROM over RAM takes the reads and lets the writes
 * through to the RAM below (shadowing), and a write-only register mapping
 * can sit on top of ROM (bank-switch registers).
 *
 * Where mappings overlap, the highest priority wins.  At equal priority the
 * most recently added non-shared mapping wins, and every SHARED mapping
 * there takes part as well: writes go to all of them, reads are ANDed (open
 * bus floats high).  That is 86Box's stacked io_sethandler(); remove one by
 * identity with gemu_space_unmap() or by handle with gemu_mapping_remove().
 *
 * Mirrors: gemu_space_mirror(s, base, size, mask) makes every address A in
 * [base, base+size) decode as A & mask (the most recent mirror covering A
 * applies).  The 5150's 10-bit I/O decode is mirror(io, 0, 0x10000, 0x3FF).
 *
 * Accesses of 8, 16, 32 and 64 bits.  A handler mapping that lacks the
 * width asked for: a wider access is made of narrower ones (in the space's
 * byte order); a narrower read reads the aligned wider unit and takes its
 * part; a narrower write with no narrower handler is dropped.  An access
 * that straddles mappings is split the same way.  Handlers get the offset
 * from the mapping's base.
 *
 * Unmapped reads return the space's unmapped value (all ones by default)
 * and call its unmapped hooks if set.
 *
 * Peeks have no side effects: RAM is read directly, handler mappings only
 * through their peek8 hook; anything else (and unmapped) can't be peeked.
 *
 * Implementation: a sparse page table (4 KiB pages; 256 bytes in spaces of
 * 16 address bits or fewer).  A page that one RAM mapping covers whole is a
 * direct pointer - the fast path, and what gemu_space_ptr() gives CPUs and
 * JITs; anything else is a short list of segments.  Changing the mappings
 * rebuilds the pages they cover (and the mirrors) and bumps the space's
 * generation; gemu_mapping_set_ptr() (bank switching) only updates the
 * direct pointers.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gemu/dev/device.h"

typedef struct GemuSpace   GemuSpace;
typedef struct GemuMapping GemuMapping;

typedef enum { GEMU_LITTLE_ENDIAN, GEMU_BIG_ENDIAN } GemuEndian;

/* Any subset; widths missing are made from the others (see above). */
typedef struct GemuHandlers {
    uint8_t  (*r8) (void *ud, uint64_t off);
    uint16_t (*r16)(void *ud, uint64_t off);
    uint32_t (*r32)(void *ud, uint64_t off);
    uint64_t (*r64)(void *ud, uint64_t off);
    void (*w8) (void *ud, uint64_t off, uint8_t v);
    void (*w16)(void *ud, uint64_t off, uint16_t v);
    void (*w32)(void *ud, uint64_t off, uint32_t v);
    void (*w64)(void *ud, uint64_t off, uint64_t v);
    /* Side-effect-free read for the monitor; false = can't tell. */
    bool (*peek8)(void *ud, uint64_t off, uint8_t *v);
} GemuHandlers;

enum {
    GEMU_MAP_READONLY  = 1 << 0,   /* RAM: reads only (ROM) */
    GEMU_MAP_WRITEONLY = 1 << 1,   /* RAM: writes only */
    GEMU_MAP_SHARED    = 1 << 2,   /* stack with others at its priority */
};

/* Read-only to users; change it through the functions below. */
struct GemuMapping {
    GemuSpace   *space;
    uint64_t     base, size;
    uint8_t     *ram;              /* RAM/ROM mapping: host memory for base */
    GemuHandlers h;                /* handler mapping: a copy of the handlers */
    void        *ud;
    int          priority;
    unsigned     flags;
    bool         enabled;
    char         name[32];         /* for listings; "" if not set */
    uint64_t     seq;              /* order added (later wins ties) */
    GemuMapping *next;
};

/* ── Spaces ───────────────────────────────────────────────────────────────── */

/* m may be NULL for a space on its own (free it with gemu_space_free);
 * otherwise the machine frees it, after its devices. */
GemuSpace  *gemu_space_new(GemuMachine *m, const char *name,
                           unsigned addr_bits, unsigned data_bits, GemuEndian e);
void        gemu_space_free(GemuSpace *s);
GemuSpace  *gemu_space_find(GemuMachine *m, const char *name);
const char *gemu_space_name(const GemuSpace *s);
unsigned    gemu_space_addr_bits(const GemuSpace *s);
unsigned    gemu_space_data_bits(const GemuSpace *s);
GemuEndian  gemu_space_endian(const GemuSpace *s);
/* Bumped by every change to the map (including gemu_mapping_set_ptr):
 * pointers from gemu_space_ptr() are good while it stays the same. */
uint64_t    gemu_space_generation(const GemuSpace *s);

/* What unmapped reads return (truncated to the access width), and hooks
 * called on unmapped accesses (NULL for none). */
void gemu_space_set_unmapped(GemuSpace *s, uint64_t value,
        void (*on_read)(void *ud, uint64_t addr, unsigned bytes),
        void (*on_write)(void *ud, uint64_t addr, unsigned bytes, uint64_t v),
        void *ud);

/* ── Mappings ─────────────────────────────────────────────────────────────── */
/* base and size must lie inside the space and size be > 0; NULL otherwise. */

GemuMapping *gemu_space_map_ram(GemuSpace *s, uint64_t base, uint64_t size,
                                uint8_t *ptr, unsigned flags, int priority);
GemuMapping *gemu_space_map(GemuSpace *s, uint64_t base, uint64_t size,
                            const GemuHandlers *h, void *ud, int priority,
                            unsigned flags);
void gemu_mapping_remove(GemuMapping *mp);
/* Remove the mappings at exactly base/size with these handlers (compared
 * by content) and ud - 86Box's io_removehandler.  Returns how many. */
int  gemu_space_unmap(GemuSpace *s, uint64_t base, uint64_t size,
                      const GemuHandlers *h, void *ud);
bool gemu_mapping_move(GemuMapping *mp, uint64_t base, uint64_t size);
void gemu_mapping_enable(GemuMapping *mp, bool on);
/* A RAM mapping's host memory (bank switching): cheap, no rebuild. */
void gemu_mapping_set_ptr(GemuMapping *mp, uint8_t *ptr);
void gemu_mapping_set_name(GemuMapping *mp, const char *name);

/* Mirrors are permanent. */
bool gemu_space_mirror(GemuSpace *s, uint64_t base, uint64_t size, uint64_t mask);

/* The mappings, most recently added first. */
GemuMapping *gemu_space_mappings(GemuSpace *s);

/* ── Access ───────────────────────────────────────────────────────────────── */
/* Addresses are truncated to the space's width. */

uint8_t  gemu_space_read8 (GemuSpace *s, uint64_t addr);
uint16_t gemu_space_read16(GemuSpace *s, uint64_t addr);
uint32_t gemu_space_read32(GemuSpace *s, uint64_t addr);
uint64_t gemu_space_read64(GemuSpace *s, uint64_t addr);
void gemu_space_write8 (GemuSpace *s, uint64_t addr, uint8_t v);
void gemu_space_write16(GemuSpace *s, uint64_t addr, uint16_t v);
void gemu_space_write32(GemuSpace *s, uint64_t addr, uint32_t v);
void gemu_space_write64(GemuSpace *s, uint64_t addr, uint64_t v);

bool gemu_space_peek8(GemuSpace *s, uint64_t addr, uint8_t *v);

/* Host memory behind addr, when one RAM mapping covers its whole page in
 * that direction; *avail = bytes usable from there (to the page end).
 * NULL otherwise (use the access functions). */
uint8_t *gemu_space_ptr(GemuSpace *s, uint64_t addr, bool write, uint64_t *avail);
