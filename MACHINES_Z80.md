# Z80 machines emulated by GEMU

## CPUs

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `z80` | Zilog Z80 (NMOS) | working | every opcode incl. undocumented (IXH/IXL, SLL, DDCB register copies, `IN (C)`, `OUT (C),0`); X/Y flags, MEMPTR, Q (SCF/CCF), block-instruction repeat flags; per-T-state bus timing; NMI and IM 0/1/2 |

The core (`cpu/z80/`) passes all 1,604,000 [SingleStepTests Z80](https://github.com/SingleStepTests/z80)
cases (registers, flags, WZ, Q, R, memory, I/O and T-state counts) and zexall:

```
ninja z80-sst && ./bin/z80-sst roms/tests/z80          # the suite's v1/*.json, gzipped
./bin/gemu -M z80 -cpm zexall.com -display none -monitor none
```

## Machines

| Name | Full name | Status | Notes |
|------|-----------|--------|-------|
| `z80` | Generic Z80 machine | preliminary | 64 KB RAM, `-rom ADDR:FILE`, timer and serial console in I/O space, `-cpm FILE` for CP/M test programs |
| `lagonda` | Javelina AUDIT, Aston Martin Lagonda (1985) | imperfect | the whole instrument computer: three VDP screens, speech, clock, A/D, switches; the CPU clock and some board wiring are inferred (below) |

## Generic machine

The ports are the generic 6502 machine's peripherals, in I/O space:

| Port | Register |
|------|----------|
| 00h/01h | timer reload low/high (T-states; 0 = 65536) |
| 02h | timer control: bit 0 enable, 1 interrupt enable, 2 auto-reload, 7 pending (write 1 to clear) |
| 10h | serial data: write to stdout, read from stdin |
| 11h | serial status: bit 0 RX ready, bit 1 TX ready |
| 12h | serial control: bit 0 RX interrupt enable |

Interrupts put FFh on the data bus (RST 38h in IM 0/1, vector I:FFh in IM 2).
`-cpm FILE` loads a .COM file at 0100h with BDOS console output (functions 2
and 9) and ends the run on a warm boot; it runs unthrottled.  The monitor's
`disas [addr] [count]` disassembles.

## Aston Martin Lagonda (Javelina AUDIT)

```
./bin/gemu -M lagonda -rom roms/lagonda
```

The real firmware runs on emulated hardware.  The program, graphics and
speech EPROMs are identified by checksum.

| Card | Chips | Emulation |
|------|-------|-----------|
| AUDIT CPU | Z80A, 2732 program (0000h) and graphics (1000h) EPROMs, 2 KB RAM (2000h, 3000h) | Z80 core above |
| CVDG-48K | 3 x TMS9128 + 16 KB each (ports 80h-85h) | `vga/multi/tms9918.c`: all modes, sprites, status flags |
| ATI | ADC0809 (08h-0Fh), LM2907 x2, 4518 x2 trip counter (10h, 18h) | conversion timing, output latch; trip counter in tenths of a mile |
| DSI | 8255 (00h-02h), MM58167 RTC (40h-5Fh) | generic 8255; RTC with every register, started at the host's time |
| ASP32K | MSM5205, five 2732 speech EPROMs, 4040 counter (command latch at FEh) | OKI ADPCM at 4 kHz, the board's message sequencing |

The window's head 0 shows the three CRTs as they sit in the car: the
tachometer (VDP 2) on the left, the centre display (VDP 0) and the
speedometer (VDP 1) on the right.  View >
shows or detaches each one on its own.

### The car

The firmware reads sensors and switches, so GEMU supplies the car around
it.  You can set them from the keyboard or with the monitor's `sensor`
command (`sensor` alone lists everything):

| Keys | Input |
|------|-------|
| Up / Down | speed (ADC ch1: 1 count = 1 mph; the firmware shows 0-160) |
| Right / Left | engine rpm (ADC ch0: 1 count = 50 rpm) |
| PgUp / PgDn | fuel (ch2; "low fuel" below 1/8) |
| Home / End | water temperature (ch3 counts; the gauge spans 75-139) |
| [ / ] | gear selector P R N D 2 1 |
| Q / E (held) | indicator stalk; the lamps flash at 90/min, as the car's flasher does |
| H J K L | lamp inputs: main beam, dipped beam, the "-D" lamp, sidelights |
| T / Y | clock hour / minute buttons |
| U | mph / km button |
| C | system check: "All systems go" or "Warning" |
| X | cancel message centre B (until reset) |
| R | trip reset |
| W | rear window heater |
| F1-F6 | oil pressure, water temp, oil temp, brake fluid, bulb failure, ice |
| F7-F9 | parking brake, seat belts, boot open |

The other warning switches are choke, low brake vacuum, fuel flap and low
wash level.  Set them, and the battery sense (ch7, warning below 100),
with `sensor`.  Message centre A shows the highest-priority warning.
When the car is moving (more than about 5 mph and 500 rpm), the speech
card announces it every ~5 s:

| Command | EPROM | Message |
|---------|-------|---------|
| 11h / 13h | A | low oil pressure / high water temperature |
| 21h / 23h | B | high oil temperature / low brake fluid |
| 31h / 33h | C | battery charge / exhaust temperature (listed as "bulb failure" in the manual) |
| 41h / 43h | D | low fuel / ice warning (said three times at most) |
| 51h / 53h | E | warning / all systems go |

The second half of EPROM B (low brake fluid) was dumped as all FFh, so
that message plays as a thump until the chip is read again.

### Inferred

These come from the program and the chip list.  No schematic was
available to confirm them:

- **CPU clock:** 4 MHz (Z80A), which makes one main-loop pass 18.8 ms.
  Override it with `-clock HZ`.  Nothing in the firmware depends on the
  clock except the loop rate: the warning-speech interval and how fast a
  held button repeats.
- **Speech latch:** the 74C42 takes the high nibble, and output 0 (written
  at power-on) means "no EPROM".  Bit 1 is A11.  Bit 0, set in every
  command, is treated as the play enable.  Messages run the whole 2 KB, as
  the 4040 counts them (no end markers in the data).
- **Speech output filter:** the op-amp filter's values aren't known.  Two
  poles at 1.8 kHz are used.
- **ADC clock:** the ADC0809 clock from the card's 555 is taken as
  640 kHz.  The firmware reads results long after conversions finish.
- **Port 20h:** a latch the program writes 00h to on every loop; what
  it drives isn't known.
- **Lamp names:** the lamp-input names follow the symbols the firmware draws.
- **Clock buttons:** they increment the RTC's BCD registers with a plain
  `INC`, so 09 becomes 0Ah, as on the car.
