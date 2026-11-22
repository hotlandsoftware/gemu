#pragma once

#include "gemu/display.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Generic Itanium PC - a from-scratch GEMU test platform, not modeling any
 * real machine. Its firmware is our own (reference/gemu-efi), written and
 * grown alongside the emulator, so each new CPU/device feature can be
 * exercised by a small purpose-built test before being trusted against
 * unmodified, undocumented real-hardware firmware (HP i2000's).
 *
 * Physical memory map:
 *
 *   0x0000000000000000 +--------------------------+
 *                      | SDRAM (default 512 MiB)  |
 *          ram_size    +--------------------------+
 *                      | open bus                 |
 *   0x00000000000A0000 +--------------------------+
 *                      | VGA VRAM aperture         | (standard legacy
 *   0x00000000000C0000 +--------------------------+  0xA0000-0xBFFFF hole,
 *                      | open bus                 |   same convention real
 *   0x00000000C0000000 +--------------------------+  PC firmware expects)
 *                      | VGA control registers,   |
 *                      | memory-mapped 1:1 by     |
 *                      | legacy port number - see |
 *                      | GENERIC_VGA_IO_BASE      |
 *   0x00000000C0000030 +--------------------------+
 *                      | open bus                 |
 *   0x00000000FFF00000 +--------------------------+
 *                      | firmware ROM (1 MiB,     |
 *                      | top-aligned so its last  |
 *                      | bundle lands at the      |
 *                      | architected reset vector)|
 *   0x0000000100000000 +--------------------------+
 *
 * Unlike i2000, there's no legacy IA-32 real-mode reset path to model: our
 * own firmware starts directly in native IA-64 mode at the reset vector,
 * and VGA is exposed as ordinary memory-mapped registers rather than the
 * sparse legacy-I/O port encoding real x86-derived firmware expects.
 *
 * The keyboard and CD-ROM controllers are likewise deliberately NOT modeled
 * on real PC hardware (no i8042 scancode protocol, no ATAPI/IDE task file
 * registers) - our own firmware is the only code that will ever touch them
 * directly, matching how a real EFI system works anyway: OS loaders go
 * through firmware-provided protocols (EFI_SIMPLE_TEXT_INPUT_PROTOCOL,
 * EFI_BLOCK_IO_PROTOCOL), never raw controller registers. Modeling the
 * simplest thing that could work leaves more of the session for the boot
 * manager logic itself instead of protocol-accuracy busywork we don't need
 * yet.
 */

#define GENERIC_RAM_MAX      0x80000000ull                       /* 2 GiB */
#define GENERIC_ROM_SIZE      0x400000u                          /* 4 MiB, matches -h's "top-aligned, max 4 MiB" */
#define GENERIC_ROM_BASE      (0x100000000ull - GENERIC_ROM_SIZE)
#define GENERIC_VGA_IO_BASE   0x00000000C0000000ull
#define GENERIC_VGA_IO_SIZE   0x30u                    /* ports 3B0h-3DFh */

/* Fixed linear framebuffer used by qemu-system-ia64's EFI firmware.  Its
 * GOP mode is 640x400x32, matching the generic display surface exactly. */
#define GENERIC_QEMU_VGA_FB_BASE 0x00000000C4000000ull
#define GENERIC_QEMU_VGA_FB_SIZE (640u * 400u * 4u)
#define GENERIC_QEMU_VGA_MMIO_BASE 0x00000000C8000000ull
#define GENERIC_QEMU_VGA_MMIO_SIZE 0x10000u
#define GENERIC_QEMU_PCI_ECAM_BASE 0x0000007FF0000000ull
#define GENERIC_QEMU_PCI_ECAM_SIZE 0x10000000ull
#define GENERIC_QEMU_NVRAM_BASE 0x00000000FFF00000ull
#define GENERIC_QEMU_NVRAM_SIZE 0x10000u

#define GENERIC_QEMU_UART_BASE 0x00000047F0000000ull
#define GENERIC_QEMU_UART_SIZE 0x8u

/* Minimal Intel 82093-style I/O SAPIC.  IA-64 Windows requires one during
 * HAL phase one even before any routed PCI interrupt is enabled. */
#define GENERIC_IOSAPIC_BASE  0x0000000080110000ull
#define GENERIC_IOSAPIC_SIZE  0x100u
#define GENERIC_ACPI_PM_BASE   0x0000000080112000ull
#define GENERIC_ACPI_PM_SIZE   0x10u
#define GENERIC_LEGACY_IO_BASE 0x000000800010000000ull
/* Real IA-64 HAL never addresses this window with a plain linear port
 * offset - it uses the architectural "sparse I/O space" encoding (see
 * sparse_io_decode() in machine_generic_ia64.c), which needs ~4 bytes of
 * address space per port bit rather than 1. Sized to comfortably cover
 * every port we implement (COM1, ATAPI, PCI config) once sparse-encoded,
 * matching reference/qemu-system-ia64's IA64_PCI_IO_SIZE convention. */
#define GENERIC_LEGACY_IO_SPARSE_SIZE 0x1000000ull

typedef struct {
    uint64_t        ram_size;
    GemuDisplayType display_type;
    int             display_scale;
    bool            no_shutdown;
    /* Disks come from the shared drive table (gemu/drive.h): the HDD is
     * ide0-hd0 (-hda), the CD-ROM ide1-cd0 (-cdrom). */
    const char     *cpu;        /* NULL or "merced" (default), "mckinley" */
    /* -serial SPEC: attach a 16550-style COM1 UART (port 0x3F8, inside
     * the legacy I/O window - see GENERIC_LEGACY_IO_BASE) to a host
     * transport, for a real Windows kernel debugger (WinDbg/KD) to
     * attach to over a serial transport. NULL = no UART. SPEC is one of:
     *   "stdio"          host's own stdin/stdout, raw byte mode (shares
     *                     stdout with the VGA-mirror text echo - casual
     *                     use only, not a clean KD wire)
     *   "tcp:HOST:PORT"  a dedicated listening socket, one reconnectable
     *                     client at a time - the one to point a real
     *                     debugger at */
    const char     *serial_spec;
    bool            menu_disabled; /* machine XML's menu="disabled" */
    bool            jit;           /* -accel jit (this machine steps the
                                    * CPU slot by slot: always interprets) */
} GenericConfig;

typedef struct Ia64GenericState Ia64GenericState;

Ia64GenericState *ia64_generic_create(const GenericConfig *cfg);
void               ia64_generic_run(Ia64GenericState *s, const GenericConfig *cfg);
void               ia64_generic_destroy(Ia64GenericState *s);

bool ia64_generic_load_firmware(Ia64GenericState *s, const char *path);
