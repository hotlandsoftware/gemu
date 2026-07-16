# IA-64 machines emulated by GEMU

## CPUs
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| merced | `Intel Itanium (Merced)` | skeleton | no core yet - machine idles at the reset vector |

## Machines
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| i2000 | `HP i2000 (2001)` | skeleton | RAM + firmware flash mapped, nothing executes yet |

## Hardware (HP i2000)
- CPU: Intel Itanium (Merced) 733/800 MHz, 2/4 MB L3
- Chipset: Intel 460GX (SAC/SDC + PXB PCI bridges, GXB AGP, IFB legacy I/O)
- RAM: 4 DIMM slots, up to 2 GiB SDRAM
- Firmware: 4 MiB flash (PAL + SAL + EFI), image `bios130.BIN` (rev 1.30)

## Physical memory map (as modeled so far)
| Range | What |
|-------|------|
| `0x00000000` .. ram_size | SDRAM (default 512 MiB, `-m` up to 2G) |
| `0xFFC00000` .. `0xFFFFFFFF` | firmware flash, top-aligned |
| everything else | open bus (reads `0xFF`) |

Architected entry points live at the top of the flash: PALE entry bundles at
`0xFFFFFF80..0xFFFFFFB0` (reset fetch starts at IP `0xFFFFFFB0`), firmware
pointer slots below `0xFFFFFFF0`, IA-32 compat reset vector at `0xFFFFFFF0`.

## Roadmap
1. Boot the firmware to the EFI shell: Merced core (IA-64 bundles, PAL
   traps), 460GX SAC decode, legacy I/O (serial console first - the i2000
   firmware talks on COM1)
2. Boot Linux/ia64
3. Boot Windows (XP 64-Bit Edition / Server 2003 for Itanium)

## Usage
```
./bin/gemu -M i2000 -rom roms/bios130.BIN
./bin/gemu -M i2000 -rom roms/ -m 1G
```
