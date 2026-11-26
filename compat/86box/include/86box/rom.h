/*
 * 86Box v6.0's rom.h, the part sound cards use, for GEMU's 86Box layer
 * (compat/86box).  GEMU doesn't have 86Box's ROM set: rom_present() says
 * no, so a device that needs ROM files isn't available, and rom_fopen()
 * gives NULL.  (A port that needs ROMs will find them GEMU's way.)
 *
 * From 86Box (src/include/86box/rom.h), GPLv2 or later.
 */
#ifndef EMU_ROM_H
#define EMU_ROM_H
#include <stdio.h>

extern FILE *rom_fopen(const char *fn, char *mode);
extern int   rom_present(const char *fn);

#endif /*EMU_ROM_H*/
