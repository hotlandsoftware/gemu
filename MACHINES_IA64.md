# IA-64 machines emulated by GEMU

## CPUs

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `merced` | Intel Itanium (Merced) | preliminary | Executes IA-64 and IA-32 code and implements the PAL, SAL, RSE, interruption, and virtual-memory paths needed by i2000 firmware. |

## Machines

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `i2000` | HP i2000 (2001) | preliminary | Boots supported firmware to EFI, exposes an ATAPI CD-ROM, starts Windows XP IA-64 Setup |

## HP i2000 hardware

- CPU: Intel Itanium (Merced), including IA-32 compatibility mode
- Chipset: Intel 460GX (SAC/SDC, PXB PCI bridges, GXB AGP, and IFB legacy I/O)
- RAM: 2 GiB by default
- Firmware: 4 MiB flash containing PAL, SAL, and EFI
- Display: emulated VGA/Rage 128 firmware interface
- Storage: secondary-master ATAPI CD-ROM backed by a raw ISO image
- Legacy devices: PIC/IOSAPIC, PIT, RTC/CMOS, keyboard, mouse, serial, and SMBus support
- Persistent, firmware-specific flash/NVRAM and full-machine snapshots

The machine definition recognizes the SDV EFI 0.99 debug image and HP firmware
revisions 1.17c and 1.30. Firmware images are not distributed with GEMU.

## Physical memory map

| Range | What |
|-------|------|
| `0x00000000` through installed RAM | SDRAM and chipset-reserved low-memory windows |
| `0xFFC00000` .. `0xFFFFFFFF` | 4 MiB firmware flash, top-aligned |
| chipset-defined I/O and MMIO windows | 460GX, PCI, VGA, IOSAPIC, and legacy devices |
| everything else | open bus |

Architected entry points live at the top of flash. PALE entry bundles occupy
`0xFFFFFF80` through `0xFFFFFFB0`; reset begins at `0xFFFFFFB0`. Firmware
pointer slots and the IA-32 compatibility reset vector occupy the remaining
top-of-flash locations.

## Usage

Boot firmware with an optional IA-64 installation CD:

```sh
./bin/gemu -M i2000 \
  -rom "reference/fwver130.BIN" \
  -cdrom "roms/windows-ia64.iso" \
  -monitor stdio
```

The EFI shell normally maps a bootable ISO as `fs0:`. To start its default
IA-64 boot loader manually:

```text
Shell> fs0:
fs0:\> .\efi\boot\bootia64.efi
```

Seeing `blk0` and `blk1` in addition to `fs0:` is normal: EFI publishes both
the ATAPI block devices and the filesystem found on the CD.

The monitor supports `savevm PATH` and `loadvm PATH`. Keep the same `-rom` and
`-cdrom` arguments when restoring a checkpoint; explicitly configured CD media
is retained across the restore.

NVRAM is stored per firmware image under the GEMU configuration directory.
If firmware state becomes unusable after changing ROMs, remove only the
corresponding `i2000-<firmware-hash>.nvram` file and cold boot.

## Current limitations

- Windows XP IA-64 Setup boots from CD and reaches **Setup is starting
  Windows**, but currently enters a repeated nested page-not-present/VHPT
  translation path instead of advancing. The recurring address observed in
  the current investigation is `0xE00001060000C000`.
- The Merced core and 460GX platform model remain incomplete; other operating
  systems and firmware revisions may encounter unimplemented instructions or
  devices.
- Emulation is instruction-heavy and substantially slower than native i2000
  hardware.

## Roadmap

1. Correct the nested VHPT/page-fault restart behavior used by Windows Setup.
2. Complete the remaining Merced, PAL, and 460GX behavior.
3. Boot Windows XP/Server 2003 for Itanium beyond text-mode Setup.
4. Add and validate Linux/IA-64 boot support.
