#pragma once

/* Shared by rage128.c (bus interface, registers, display) and rage128_gui.c
 * (the 2D drawing engine). Nothing else includes it. */

#include "rage128.h"
#include "rage128_regs.h"

#include <stdbool.h>
#include <stdint.h>

/* An unmodelled behaviour, named on stderr once per key (capped): the list
 * of what the model still lacks. Never silent. */
void r128_unimpl(const char *key, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* The 2D engine's registers (0x1400-0x1FFF): true if `off` is one of them
 * (handled, whether modelled or reported). `mask` = bytes written. */
bool r128_gui_write(Rage128 *r, uint32_t off, uint32_t v, uint32_t mask);
bool r128_gui_read(Rage128 *r, uint32_t off, uint32_t *v);
void r128_gui_reset(Rage128 *r);

static inline uint32_t r128_vram_bytes(const Rage128 *r) {
    return (uint32_t)r->vga->vram_size;
}
