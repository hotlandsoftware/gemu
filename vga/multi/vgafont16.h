/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Standard IBM VGA 8x16 ROM font, imported from real QEMU's ui/vgafont.c.
 */
#pragma once
#include <stdint.h>

#define FONT_WIDTH 8
#define FONT_HEIGHT 16

extern const uint8_t vgafont16[256 * FONT_HEIGHT];
