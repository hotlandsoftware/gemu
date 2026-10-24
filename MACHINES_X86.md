# X86 machines emulated by GEMU

## CPUs

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `8086` | Intel 8086 | imperfect | full instruction set incl. undocumented aliases; instruction-table timing, no prefetch queue |
| `8088` | Intel 8088 | imperfect | as 8086, plus the 8-bit bus's extra cycles per word access |

## Machines

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `ibm5150` | IBM 5150 (1981) | imperfect | boots the 19OCT81 BIOS from diskette or into Cassette BASIC; CGA or MDA (`-vga mda`), or both on two monitors (`-device`), keyboard, speaker, floppy drives. No cassette, printer, serial yet |

## IBM 5150 hardware

| Chip | Status |
|------|--------|
| 8253 PIT | all six modes, gates, latching; IRQ0, DRAM refresh and speaker wired up |
| 8259 PIC | edge/level triggering, masking, EOI, auto-EOI, IRR/ISR/poll reads |
| 8237 DMA | registers and page registers; channel 0 refresh cycles |
| 8255 PPI | 5150 wiring: SW1/SW2 DIP switches, keyboard handshake and reset, speaker |
| CGA | 40/80-column text, 320x200 4-color, 640x200 2-color; retrace timing |
| MDA | 80x25 monochrome text in 9x14 cells (720x350, 4:3); monitor phosphor `-vga mda,phosphor=green|amber|white` (default green, the IBM 5151); underline/intense/reverse/blink attributes; 50 Hz timing |
| uPD765 FDC | diskette adapter with up to 4 drives via `-drive if=floppy` / `-fda` (see docs/drive.md); DMA channel 2, IRQ6 |

The DIP switches follow `-m` (64K default). The 19OCT81 BIOS can use up to
544K, but POST's memory test then takes about 50 seconds; Cassette BASIC
only uses 64K either way.

```
./bin/gemu -M ibm5150 -rom roms/
```

Two monitors: like the real machine, the 5150 can have a CGA and an MDA at
once - they use different memory (B8000 vs B0000) and ports (3Dx vs 3Bx).
`-vga` picks the primary card (the DIP switches, so POST starts on it) and
`-device` adds the other:

```
./bin/gemu -M ibm5150 -rom roms/ -vga cga -device mda,phosphor=amber
```

The window shows one monitor at a time: **View → Monitor 1 / Monitor 2**, or
Ctrl+Alt+1 / Ctrl+Alt+2 as in QEMU.  **View → Detach ... to new window** puts
the current monitor in a window of its own, so both are on screen at once;
close that window (or View → Close ... window) to bring it back.  DOS's `MODE MONO` / `MODE CO80` move the
console between them.
