/*
 * 86Box v6.0's rom.h, the part devices use, for GEMU's 86Box layer
 * (compat/86box).
 *
 * rom_init() loads an image into a buffer the layer owns (freed with the
 * device, after its close - most v6.0 drivers never free it) and maps it
 * read-only (mem.h).  ROM files are found as named: a path, or "roms/..."
 * from the current directory - GEMU's -rom search isn't 86Box's ROM set.
 * rom_present() says whether the file is there.
 *
 * From 86Box (src/include/86box/rom.h, src/mem/rom.c), GPLv2 or later:
 *   Copyright 2018-2019 Fred N. van Kempen (rom.h); Copyright 2008-2019
 *   Sarah Walker, 2016-2019 Miran Grca, 2018-2019 Fred N. van Kempen (rom.c).
 */
#ifndef EMU_ROM_H
#define EMU_ROM_H
#include <stdint.h>
#include <stdio.h>
#include <86box/mem.h>

typedef struct rom_t {
    uint8_t      *rom;
    int           sz;
    uint32_t      mask;
    mem_mapping_t mapping;
} rom_t;

#ifdef __cplusplus
extern "C" {
#endif

extern FILE    *rom_fopen(const char *fn, char *mode);
extern int      rom_present(const char *fn);

extern uint8_t  rom_read(uint32_t addr, void *priv);
extern uint16_t rom_readw(uint32_t addr, void *priv);
extern uint32_t rom_readl(uint32_t addr, void *priv);
extern void     rom_write(uint32_t addr, uint8_t val, void *priv);
extern void     rom_writew(uint32_t addr, uint16_t val, void *priv);
extern void     rom_writel(uint32_t addr, uint32_t val, void *priv);

extern int      rom_load_linear(const char *fn, uint32_t addr, int sz, int off, uint8_t *ptr);
extern int      rom_init(rom_t *rom, const char *fn, uint32_t addr, int sz, int mask, int off, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif /*EMU_ROM_H*/
