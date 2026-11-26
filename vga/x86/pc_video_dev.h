#pragma once
/* The PC's display adapters as core devices, each with a screen (stage D4):
 *   "cga"      IBM Color Graphics Adapter      ports 3D0h-3DFh, VRAM at B8000h
 *   "mda"      IBM Monochrome Display Adapter  ports 3B0h-3BBh, VRAM at B0000h
 *   "hercules" Hercules Graphics Card          ports 3B0h-3BBh and 3BFh
 * MDA and Hercules take phosphor=green|amber|white (the monitor).
 *
 * The cards keep their own beam counters, ticked in CPU clocks (that's what
 * software polls through the status ports): gemu_*_tick() advances them and
 * begins the screen's next frame when the card does (device-timed screens).
 * The screens: CGA 640x400 (scan lines doubled; already the picture's
 * shape), MDA and Hercules 720x350 shown at 4:3.  The machine decodes
 * video memory itself (cards can share B8000h) through gemu_cga() and
 * friends. */
#include "cga.h"
#include "hercules.h"
#include "mda.h"
#include "gemu/dev/device.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_cga_type, gemu_mda_type, gemu_hercules_type;
/* ud = the device; offsets from 3D0h (CGA), 3B0h (MDA, Hercules), 3BFh
 * (Hercules' configuration switch) */
extern const GemuHandlers gemu_cga_io, gemu_mda_io, gemu_hercules_io, gemu_hercules_cfg_io;

CgaDevice      *gemu_cga(GemuDevice *d);
MdaDevice      *gemu_mda(GemuDevice *d);
HerculesDevice *gemu_hercules(GemuDevice *d);
GemuScreen     *gemu_pc_video_screen(GemuDevice *d);   /* any of the three */

/* Advance by `clocks` CPU clocks; true when the card began a frame. */
bool gemu_cga_tick(GemuDevice *d, uint32_t clocks);
bool gemu_mda_tick(GemuDevice *d, uint32_t clocks);
bool gemu_hercules_tick(GemuDevice *d, uint32_t clocks, uint32_t cpu_hz);
