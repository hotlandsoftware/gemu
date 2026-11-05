#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gemu/display.h"
#include "i8086.h"

typedef enum {
    X86_MACHINE_IBM5150,
    X86_MACHINE_IOPENER, /* data/machine/x86/iopener.xml - not implemented yet */
} X86MachineType;

typedef enum {
    X86_VGA_CGA,
    X86_VGA_MDA,
    X86_VGA_HERCULES,
} X86VgaType;

/* The MDA and Hercules both drive a monochrome monitor from ports
 * 3B0h-3BFh: a PC has one of them, and a CGA beside it. */
static inline bool x86_vga_mono(X86VgaType t) { return t == X86_VGA_MDA || t == X86_VGA_HERCULES; }

#define X86_MAX_ROM_LOADS 8

typedef enum {
    X86_HDC_NONE,
    X86_HDC_IBM,        /* -device ibm-fixed-disk: IBM Fixed Disk Adapter */
} X86HdcType;

typedef struct {
    const char *path;
    const char *region; /* "bios", "basic", ... - NULL for single-image machines */
} X86RomLoad;

typedef struct X86Config {
    X86RomLoad      roms[X86_MAX_ROM_LOADS];
    int             n_roms;
    X86MachineType  machine;
    X86CpuType      cpu;
    X86VgaType      vga;
    const char     *vga_opts;   /* -vga NAME,opts: card options (e.g. phosphor=amber) */
    bool            has_vga2;   /* a second card from -device (the other of CGA/MDA) */
    X86VgaType      vga2;
    const char     *vga2_opts;
    X86HdcType      hdc;        /* hard disk controller (-device) */
    const char     *hdc_opts;   /* its options (romfile=, interleave=) */
    int             mouse_com;  /* -device msmouse: on COM1 or COM2 (0 = none) */
    uint32_t        ram_size; /* bytes */
    GemuDisplayType display_type;
    int             display_scale;
    bool            no_shutdown;
} X86Config;
