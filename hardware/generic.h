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
 *   0x00000000C0001000 +--------------------------+
 *                      | keyboard controller      | see GENERIC_KBD_IO_BASE
 *   0x00000000C0001008 +--------------------------+
 *                      | open bus                 |
 *   0x00000000C0002000 +--------------------------+
 *                      | CD-ROM controller        | see GENERIC_CDROM_IO_BASE
 *   0x00000000C0002010 +--------------------------+
 *                      | open bus                 |
 *   0x00000000C0003000 +--------------------------+
 *                      | CD-ROM path buffer,      | see GENERIC_CDROM_PATH_BASE
 *                      | write-only, 128 bytes    |
 *   0x00000000C0003080 +--------------------------+
 *                      | open bus                 |
 *   0x00000000C0100000 +--------------------------+
 *                      | CD-ROM sector/chunk      | last sector or file
 *                      | buffer, read-only,       | chunk read by the
 *                      | 2048 bytes               | CD-ROM controller
 *   0x00000000C0100800 +--------------------------+
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
 * sparse_io_decode() in machine_generic.c), which needs ~4 bytes of
 * address space per port bit rather than 1. Sized to comfortably cover
 * every port we implement (COM1, ATAPI, PCI config) once sparse-encoded,
 * matching reference/qemu-system-ia64's IA64_PCI_IO_SIZE convention. */
#define GENERIC_LEGACY_IO_SPARSE_SIZE 0x1000000ull

/* Keyboard controller: two byte registers.
 *   +0x0  STATUS (read-only)  bit 0 = a key code is waiting
 *   +0x4  DATA   (read-only)  pop the next key code (0 if none waiting);
 *                             1 = Up, 2 = Down, 3 = Enter
 * Arrow/Enter presses are the only things the boot manager needs to
 * navigate menus, so those are the only codes that exist. */
#define GENERIC_KBD_IO_BASE   0x00000000C0001000ull
#define GENERIC_KBD_IO_SIZE   0x8u
#define GENERIC_KBD_KEY_UP    1u
#define GENERIC_KBD_KEY_DOWN  2u
#define GENERIC_KBD_KEY_ENTER 3u

/* CD-ROM controller: sector-at-a-time raw block access, plus a
 * file-by-path interface for the embedded El Torito EFI boot image
 * (see cdrom_open_file() in machine_generic.c - it's a FAT12 floppy
 * image, standard for Microsoft's IA-64 EFI install media of this era;
 * ISO 9660 + El Torito + FAT12 parsing all happen host-side in C, same
 * "our own firmware is the only client" reasoning as the rest of this
 * device: real EFI abstracts filesystem access behind a protocol too,
 * rather than making every OS loader speak FAT itself).
 *   +0x0  STATUS    (read-only)   bit 0 = media present
 *   +0x4  LBA       (write, 4B LE) sector number for CMD_READ_SECTOR
 *   +0x8  CMD       (write-only)  1 = read 2048 bytes at LBA into the
 *                                     buffer window (raw ISO sector)
 *                                 2 = open the file whose path was
 *                                     written to GENERIC_CDROM_PATH_BASE
 *                                     (null-terminated, e.g. "SETUPLDR.EFI")
 *                                 3 = read the next chunk (up to 2048
 *                                     bytes) of the file opened by CMD 2
 *                                     into the buffer window
 *   +0xC  RESULT    (read-only)   0 = last command ok, 1 = error
 *   +0x10 FILE_SIZE  (read-only, 4B) total size of the file opened by
 *                                    the last successful CMD 2
 *   +0x14 CHUNK_SIZE (read-only, 4B) valid byte count in the buffer
 *                                    window after the last CMD 3 (less
 *                                    than 2048 only for the final chunk)
 *   +0x18 PE_SRC     (write, 4B)   RAM address of an already-loaded raw
 *                                  PE32+/IA-64 file, for CMD 4
 *   +0x1C PE_DST     (write, 4B)   RAM address to place the PE image's
 *                                  sections at (firmware passes the
 *                                  image's own preferred ImageBase here,
 *                                  so no relocation is needed - see
 *                                  cdrom_load_pe_image() for why that's
 *                                  a safe simplification)
 *   +0x20 PE_ENTRY_RVA (read-only, 4B) RVA of the entry-point plabel,
 *                                      valid after a successful CMD 4
 * CMD 4 = parse the PE headers/section table at PE_SRC (already-loaded
 * raw file bytes) and copy each section to PE_DST + VirtualAddress,
 * zero-padding out to VirtualSize. This is "LoadImage()"-ish section
 * placement, not disk access, but it's the same kind of file-format
 * mechanics as the FAT12 work above, and far more reliable done once in
 * tested C than as a hand-unrolled assembly loop (see the whole point of
 * this device's design in the block comment above hardware/generic.h).
 * The result of the last READ/OPEN/chunk-read is memory-mapped
 * read-only at GENERIC_CDROM_BUF_BASE. */
/* Hard disk: a raw, read-write image in 512-byte sectors (create one with
 * `qemu-img create -f raw disk.img 5G`, or just truncate(1) a file).  Same
 * "our own firmware is the only client" reasoning as the CD-ROM above - this
 * is deliberately not an ATA/ATAPI task-file register set, so an OS driver
 * will NOT find it; it exists so the firmware can read and write a disk.
 *   +0x0  STATUS  (ro)        bit 0 = an image is attached
 *   +0x4  LBA     (wo, 4B)    512-byte sector number for the next command
 *   +0x8  CMD     (wo)        1 = read that sector into the buffer window
 *                             2 = write the buffer window to that sector
 *   +0xC  RESULT  (ro)        0 = last command ok, 1 = error
 *   +0x10 SECTORS (ro, 4B)    image size in 512-byte sectors
 * The sector buffer is mapped read-write at GENERIC_HDD_BUF_BASE. */
#define GENERIC_HDD_IO_BASE         0x00000000C0004000ull
#define GENERIC_HDD_IO_SIZE         0x14u
#define GENERIC_HDD_CMD_READ        1u
#define GENERIC_HDD_CMD_WRITE       2u
#define GENERIC_HDD_BUF_BASE        0x00000000C0005000ull
#define GENERIC_HDD_SECTOR_SIZE     512u

#define GENERIC_CDROM_IO_BASE       0x00000000C0002000ull
#define GENERIC_CDROM_IO_SIZE       0x2Cu
#define GENERIC_CDROM_CMD_READ      1u
#define GENERIC_CDROM_CMD_OPEN      2u
#define GENERIC_CDROM_CMD_READ_NEXT 3u
#define GENERIC_CDROM_CMD_LOAD_PE   4u
#define GENERIC_CDROM_CMD_READ_DMA  5u
#define GENERIC_CDROM_BUF_BASE      0x00000000C0100000ull
#define GENERIC_CDROM_BUF_SIZE      0x800u                        /* 2048 */
#define GENERIC_CDROM_SECTOR_SIZE   2048u
#define GENERIC_CDROM_PATH_BASE     0x00000000C0003000ull
#define GENERIC_CDROM_PATH_SIZE     0x80u                          /* 128 */

typedef struct {
    uint64_t        ram_size;
    GemuDisplayType display_type;
    int             display_scale;
    bool            no_shutdown;
    const char     *cdrom_path;
    /* -hda FILE: raw read-write disk image, 512-byte sectors. NULL = none. */
    const char     *hda_path;
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
    /* -vnc ADDR: accept RFB keyboard input (arrow/enter, for the boot
     * menu, and raw ASCII for qemu_firmware's UART ConIn path) over a
     * VNC connection - the framebuffer itself is NOT exported this way
     * (gemu_vnc_update() only understands 8bpp palette/mono surfaces,
     * not this machine's native 32-bit ARGB one); use the monitor's
     * `screendump` command for visual feedback instead. NULL = no VNC
     * server. Format: "host:display" or ":display" (port = 5900+display). */
    const char     *vnc_addr;
} GenericConfig;

typedef struct Ia64GenericState Ia64GenericState;

Ia64GenericState *ia64_generic_create(const GenericConfig *cfg);
void               ia64_generic_run(Ia64GenericState *s, const GenericConfig *cfg);
void               ia64_generic_destroy(Ia64GenericState *s);

bool ia64_generic_load_firmware(Ia64GenericState *s, const char *path);
