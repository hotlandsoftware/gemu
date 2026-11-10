# IA-64 machines emulated by GEMU

## CPUs

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `merced` | Intel Itanium (Merced) | preliminary | Executes IA-64 and IA-32 code and implements the PAL, SAL, RSE, interruption, and virtual-memory paths needed by i2000 firmware. |

## Machines

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `i2000` | HP i2000 (2001) | preliminary | Boots supported firmware to EFI, exposes an ATAPI CD-ROM, starts the Windows XP IA-64 CD loader |

## HP i2000 hardware

- CPU: Intel Itanium (Merced), including IA-32 compatibility mode
- Chipset: Intel 460GX (SAC/SDC, PXB PCI bridges, GXB AGP, and IFB legacy I/O)
- RAM: 2 GiB by default; smaller matched DIMM populations are supported
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

Use `-m 512m` for 512 MiB. The eight DIMM sockets are populated in groups
of four matching modules. Supported RAM sizes are one or two power-of-two
groups of 64, 128, 256, 512, or 1024 MiB, up to 2 GiB total. Unrepresentable
sizes are rejected rather than silently advertising less RAM.

The EFI shell normally maps a bootable ISO as `fs0:`. To start its default
IA-64 boot loader manually:

```text
Shell> fs0:
fs0:\> .\efi\boot\bootia64.efi
```

Seeing `blk0` and `blk1` in addition to `fs0:` is normal: EFI publishes both
the ATAPI block devices and the filesystem found on the CD.

`-cdrom FILE` is sugar for QEMU-style `-drive file=FILE,if=ide,media=cdrom`;
a raw read-write HDD image (`-hda FILE`) is likewise `-drive file=FILE,if=ide,
media=disk`. i2000 exposes exactly one IDE drive per channel, so `-drive`
only accepts `index=0` - there's no slave/secondary device to attach a
second image to.

The monitor's `sendkey` types into the guest: QEMU key names (`ret`, `spc`,
`esc`, `tab`, `up`, `f8`, ...) or plain text, e.g. `sendkey ret` at the
"Press any key to boot from CD-ROM" prompt.

The monitor supports `savevm PATH` and `loadvm PATH`. Keep the same `-rom` and
`-cdrom`/`-drive` arguments when restoring a checkpoint; explicitly configured
CD media is retained across the restore.

Cold boot after updating from versions with the incorrect EFI RAM map.
Existing snapshots retain their old firmware memory descriptors and cannot
pick up the RAM-discovery fix merely by being loaded into a newer emulator.
Checkpoints made before the register-stack fixes also retain old backing-store
and NaT state. Firmware PCI bus numbers and resource assignments likewise
remain in old snapshots; the corrected chipset registers do not repair those
assignments on restore. Use a cold boot when validating these fixes.

NVRAM is stored per firmware image under the GEMU configuration directory.
If firmware state becomes unusable after changing ROMs, remove only the
corresponding `i2000-<firmware-hash>.nvram` file and cold boot.

## Current limitations

- Windows XP IA-64 Setup boots from CD and reaches **Setup is starting
  Windows**. The RAM-map, CPU, and chipset fixes let it progress into PCI
  enumeration, IDE-controller attachment, and CD reads through the Windows
  driver. Interactive Setup has not yet been reached: later kernel/filesystem
  initialization remains blocked. The latest 2 GiB cold run stops at bugcheck
  `0x50` in `cdfs.sys`. Completing installation remains unverified.
- The Merced core and 460GX platform model remain incomplete; other operating
  systems and firmware revisions may encounter unimplemented instructions or
  devices.
- Emulation is instruction-heavy and substantially slower than native i2000
  hardware.

## Roadmap

1. Resolve the remaining HAL/kernel initialization failures in Windows Setup.
2. Complete the remaining Merced, PAL, and 460GX behavior.
3. Boot Windows XP/Server 2003 for Itanium beyond text-mode Setup.
4. Add and validate Linux/IA-64 boot support.

## Memory and boot regression checks

AMI firmware reads DIMM SPD through the SAC's indirect I2C interface in
memory-card PCI configuration space, rather than through IFB SMBus.
The emulation routes those reads to the selected DIMM, honors the row
multiplexer, and supplies supported SDRAM geometries. Returning ordinary
configuration bytes here previously produced the bogus RAM limit
`0x00000E0080000000`.

The regression suite checks RAM discovery, the EFI interrupt and CPU-revision
handoff boundary, reserved-entry rejection by the hardware VHPT walker, and
the floating-point check-load instructions used by Windows HAL. It also
checks that faulting integer loads preserve their destination and address
registers until the fault handler returns and the instruction retries, and
that `tbit/tnat .unc` instructions with a false qualifying predicate clear
their output predicates.

Platform checks cover PCI discovery of the relocated chipset bus, the SAC's
four physical expander links, link-probe completion, and bridge identities
and bus capacity. The modeled topology provides one PXB bus, two WXB buses,
and one AGP bus. These checks also cover IDE and IFB BAR sizing and I/O
decoding, and the IFB ACPI enable/disable handshake. Correcting the expander
count prevents firmware from assigning buses to nonexistent links. The ACPI
register window follows its programmed base independently of GPIO straps.
The empty UHCI controller supports reset, run/stop status, disconnected root
ports, and relocatable I/O registers. Version 26 snapshots preserve its state
along with the ACPI state added in version 25.

ATAPI interrupt checks cover IOSAPIC routing, interrupt masking, status
acknowledgement, device reset, and separate IDENTIFY and PACKET completion
behavior. Version 27 snapshots preserve the interrupt and command-phase
state; regression checks also load snapshots from versions 24 through 26.

Integer `mux1` checks cover every byte permutation, including the low-byte
broadcast used by Windows to fill allocation bitmaps, with NaT propagation
and predication.

Floating-point `fmix` and `fsxt` checks cover lane selection, source order,
sign extension, output format, NaT propagation, and predication. Register-stack
checks cover automatic fills after a backing-store switch without `loadrs`,
preserving dirty and current-frame registers, RNAT collection boundaries and
byte order, and fill faults that retry at the return target without popping
the frame twice. CPU checks exercise both slow and fast execution paths.

Build and run these ROM-independent checks with:

```sh
./configure --target-list=ia64 --build-dir=build/ia64 --bindir=bin
ninja
python3 tools/ia64/run_i2000_memory_test.py
python3 tools/ia64/run_i2000_interrupt_test.py
```
