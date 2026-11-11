# IA-64 machines emulated by GEMU

## CPUs

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `merced` | Intel Itanium (Merced) | preliminary | Executes IA-64 and IA-32 code and implements the PAL, SAL, RSE, interruption, and virtual-memory paths needed by i2000 firmware. |

## Machines

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `i2000` | HP i2000 (2001) | preliminary | Boots supported firmware to EFI and Windows XP IA-64 text-mode Setup, with ATA disk and ATAPI CD-ROM support |

## HP i2000 hardware

- CPU: Intel Itanium (Merced), including IA-32 compatibility mode
- Chipset: Intel 460GX (SAC/SDC, PXB PCI bridges, GXB AGP, and IFB legacy I/O)
- RAM: 2 GiB by default; smaller matched DIMM populations are supported
- Firmware: 4 MiB flash containing PAL, SAL, and EFI
- Display: emulated VGA/Rage 128 firmware interface
- Storage: primary-master ATA disk backed by a raw image and secondary-master ATAPI CD-ROM backed by a raw ISO image
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

To create a disposable 4 GiB installation disk, run from the repository root:

```sh
qemu-img create -f raw roms/ItaniumTest.img 4G
./bin/gemu -M i2000 -m 512m -rom roms/bios117c.BIN \
  -cdrom "roms/Windows XP 64-Bit Edition Version 2003 (Itanium).iso" \
  -hda roms/ItaniumTest.img
```

Creating the image again erases its contents. VM snapshots do not include disk
contents; preserve a matching disk image when saving an installation checkpoint.

The monitor's `sendkey` types into the guest: QEMU key names (`ret`, `spc`,
`esc`, `tab`, `up`, `f8`, ...) or plain text, e.g. `sendkey ret` at the
"Press any key to boot from CD-ROM" prompt.

Snapshots are manual: use the monitor's `savevm PATH` and `loadvm PATH`.
Pause with `stop` before saving and copying the matching disk image. The
emulator does not create periodic snapshots. Keep the same `-rom` and
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

Older builds incorrectly asserted the board's manufacturing-mode GPIO input,
making EFI discard Windows' boot entry and regenerate its boot list. The normal
input level now preserves the entry, including the original short device path
written by Windows Setup. Existing NVRAM may retain `MfgMode=1`: boot the updated
emulator to the EFI menu or shell once to clear it before restoring a missing
Windows option.
From the EFI shell on the system partition, run `\MSUtil\nvrboot.efi`, use Import
on `\EFI\Microsoft\WINNT50\Boot0003`, then Push the imported entry to the top.
Quit NVRBOOT, then type `reset` at the EFI shell.
The backup's number and filesystem mapping can differ between installations.
Keep the disk; restoring a missing boot entry does not require formatting it.

## Current limitations

- Windows XP IA-64 boots from CD into interactive text-mode Setup with
  512 MiB of RAM. Full NTFS formatting on a 4 GiB raw disk completes and
  Setup copies files into the Windows folders and restarts into EFI.
  The normal EFI boot manager launches the original Windows boot entry after
  repairing entries lost by older builds. Graphical Setup reaches the
  "Installing Windows" stage, and 32-bit RSAENH and DSSENH registration succeeds.
  INITPKI registration reaches an unimplemented x87 `FLDZ` in `MSASN1.dll`;
  the same routine also needs `FLD1` and `FADD`. Installation remains incomplete.
- The Merced core and 460GX platform model remain incomplete; other operating
  systems and firmware revisions may encounter unimplemented instructions or
  devices.
- Emulation is instruction-heavy and substantially slower than native i2000
  hardware.

## Roadmap

1. Complete Windows Setup and boot the installed system from the ATA disk.
2. Complete the remaining Merced, PAL, and 460GX behavior.
3. Validate Windows Server 2003 for Itanium installation and startup.
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
Machine-reset checks preserve memory backing and pending NVRAM updates across
monitor resets, firmware processor resets, and subsequent snapshot save/load.
Reset previously cleared a host memory pointer, causing the next autosave to
crash even when guest execution had resumed successfully.
CF9 hard resets also reset PCI/IDE configuration and interrupt routing, while
retaining the SAC bootstrap latches (CBNR, CCSR, and configuration word 44h).
Keeping the previous OS's device state made restarted EFI lose its disk and
CD-ROM; clearing word 44h instead made initial firmware boot loop on reset.
CPU-only resets leave device configuration intact.

ATA storage checks use a sparse 4 GiB image to verify sector and multiple-sector
PIO transfers through the physical I/O decoder, including 256-sector commands,
different data widths, the 2 GiB boundary, and the end of the disk. Read-back
through the controller and an independently opened backing file must agree.

CPU checks cover `chk.a` recovery targets and interruption returns across
register-stack context switches. An incorrectly decoded `chk.a` target made
Windows' disk driver jump into an unrelated function during formatting. A
private interruption-register cache could also restore an older thread's
registers when two threads resumed at the same instruction. `rfi` now uses the
architectural register stack and backing store established by the handler.

Floating-point `chk.s` checks cover NaTVal recovery, signed targets, predication,
rotating and constant registers, and disabled-register-bank faults in both CPU
execution paths. Windows' first graphical session executes this instruction in
`WIN32K.SYS`; the missing implementation previously halted at the background
screen before graphical Setup appeared. EFI handoff checks also cover utilities
loaded below 64 MiB, where pending interrupts previously stopped `NVRBOOT` before
its first instruction.

IA-32 `CMPXCHG`, `CMPXCHG8B` and `XADD` checks cover operand widths, register
aliases, flags, failed-comparison writeback, and precise read-modify-write faults.
Cross-page accesses check both pages before modifying memory and can resume
through native `rfi` after the fault is repaired. Graphical Setup also exercises
the IA-32 compatibility engine.

`LEAVE`, direct/indirect `JMPE`, and `SCAS` checks cover native-mode transitions,
stack widths, repeated comparisons and precise faults. Repeated MOVS/LODS/STOS
now update the remaining count after each completed element; previously a page
fault could resume from an advanced pointer with the original count and overrun
the destination. Guarded-buffer tests check retry through native `rfi`, including
word/dword elements crossing discontiguous or protected pages.

`CMPS` checks cover both source and destination faults, repeated comparison
conditions, direction, and address widths. IA-32 SIB address decoding retains
the scaled index when no base register is present; losing it made Windows'
loader jump into instruction bytes instead of the selected jump-table entry.
Regressions cover the actual failing loader instruction, every index and scale,
segment overrides, and address wraparound.
Ordinary IA-32 operands and instruction fetches also translate both pages of
a split access before using the bus. Relocation tests check that a DWORD
crossing discontiguous pages updates the intended bytes without corrupting
neighboring physical memory, and that arithmetic retries preserve input flags.
Return instructions fetch their cleanup immediate before popping the stack,
so a split instruction-page fault cannot consume the return address twice.
Double shifts cover both operand widths, immediate and CL counts, aliases,
flags, and protected memory even when the masked count is zero.

IA-32 floating-point control checks cover `FWAIT`, `FNSTCW`, `FLDCW`, and
`FNCLEX`, including disabled hardware, pending exceptions, and preservation
of the SSE controls held in the upper FCR bits. The literal MSVCRT control-word
initialization sequence is checked in both CPU execution paths.
The common-controls display-scaling sequence exercises integer loads, multiply,
binary64 loads/stores, comparison and status transfers on the x87 stack. Its
tests check exact scale values and balanced TOP/empty state for every initial
TOP. Leaving IA-32 marks low floating-point registers modified when DFL is clear,
including the saved context used to resume after a native fault or interrupt.

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
python3 tools/ia64/run_i2000_ata_storage_test.py
```
