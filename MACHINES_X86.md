# X86 machines emulated by GEMU

## CPUs

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `8086` | Intel 8086 | imperfect | full instruction set incl. undocumented aliases; instruction-table timing, no prefetch queue |
| `8088` | Intel 8088 | imperfect | as 8086, plus the 8-bit bus's extra cycles per word access |

## Machines

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `ibm5150` | IBM 5150 (1981) | imperfect | boots the 19OCT81 BIOS into Cassette BASIC; CGA text and graphics, keyboard, speaker. No floppy, cassette, printer or serial yet |

## IBM 5150 hardware

| Chip | Status |
|------|--------|
| 8253 PIT | all six modes, gates, latching; IRQ0, DRAM refresh and speaker wired up |
| 8259 PIC | edge/level triggering, masking, EOI, auto-EOI, IRR/ISR/poll reads |
| 8237 DMA | registers and page registers; channel 0 refresh cycles |
| 8255 PPI | 5150 wiring: SW1/SW2 DIP switches, keyboard handshake and reset, speaker |
| CGA | 40/80-column text, 320x200 4-color, 640x200 2-color; retrace timing |

The DIP switches follow `-m` (64K default). The 19OCT81 BIOS can use up to
544K, but POST's memory test then takes about 50 seconds; Cassette BASIC
only uses 64K either way.

```
./bin/gemu -M ibm5150 -rom roms/
```
