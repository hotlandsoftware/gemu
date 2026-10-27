# MOS 6502 machines emulated by GEMU

## CPUs
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| 2a03 | `Ricoh 2A03` | working |  |
| 2a07 | `Ricoh 2A07` | working |  |
| 6501 | `MOS Technology 6501` | working | Extremely rare early version of 6502,  pin-compatible with Motorola 6800 |
| 6502 | `MOS Technology 6502` | working | |
| 6507 | `MOS Technology 6507` | working | 28-pin 6502: 13 address pins (8 KB, mirrored), no IRQ/NMI pins; usable on any MOS machine |

## VGA
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| rp2c02 | `Ricoh RP2C02` | working |
| rp2c02-0004 | `Ricoh RP2C02-0004 (VS. System RGB PPU)` | working |
| sh6578 | `UM6578/SH6578/NT6578 Famiclone PPU` | working |

## Sound Cards
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| 2a03 | `Ricoh 2A03` | working | supports MIDI playback via `2a03,output=midi` |
| ay8910 | `General Instrument AY-3-8910 PSG` | preliminary |  |
| msm6295 | `OKI MSM6295 ADPCM` | working |  |
| none | `None (disable all sound output)` | working |  |
| pcspk | `PC Speaker` | working |  |
| tia | `Atari TIA two-channel sound` | working | Atari 2600 |

## Devices
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| a1ci | `Apple I Cassette Interface` | working |  |
| cx10 | `Atari CX10 joystick` | working | the 1977 joystick; electrically identical to the CX40 |
| cx30 | `Atari CX30 paddle controllers` | working | a pair per jack; the mouse turns player 1's paddle, Left/Right turn either |
| cx40 | `Atari CX40 joystick` | working | default Atari 2600 controller |
| fds | `Famicom Disk System` | working |  |
| famicom-keyboard | `Famicom 72-key Keyboard` | working |  |
| famicom-mic | `Famicom Microphone Controller` | working | no proper mic detection yet, can send mic sound by pressing M (default) |
| rob | `R.O.B. (Robotic Operating Buddy)` | working | opens in a second window, 3D simulation of ROB |
| keyboard | `Generic QWERTY Keyboard` | working |  |
| kim-keypad | `KIM-1 hex keypad` | working |  |
| mouse | `Generic PS/2 mouse` | working |  |
| nes-controller | `NES/Famicom Standard Controller` | working |  |
| none | `None (disable all default input devices)` | working |  |
| rob-famicom | `Family Computer Robot (Famicom R.O.B.)` | working | |
| subor-mouse | `Subor Mouse` | working | |
| vt100 | `DEC VT100 serial terminal` | working |  |
| wozmon | `Wozniak 'Woz' Monitor` | working |  |
| zapper | `NES Zapper (light gun)` | working |  |

# Machines

Simulated machines include:
## Implemented
| Name   | Full Name     | Year | Graphics  | Input | Sound | Status |
|--------|---------------|---------|---------|-------|-------|-------|
| apple1 | `Apple I` | 1976 | 🟢 | 🟢 | 🔴 | working | 
| atari2600 | `Atari 2600 Video Computer System (NTSC)` | 1977 | 🟢 | 🟢 | 🟢 | working | 
| atari2600pal | `Atari 2600 Video Computer System (PAL)` | 1978 | 🟢 | 🟢 | 🟢 | working | 
| atari2600secam | `Atari 2600 Video Computer System (SECAM)` | 1983 | 🟢 | 🟢 | 🟢 | working | 
| atari400 | `Atari 400` | 1979 | 🟢 | 🟢 | 🟢 | imperfect | 
| dendy | `Dendy Famiclone` | 1992 | 🟢 | 🟢 | 🟢 | imperfect | 
| famicom | `Nintendo Family Computer` | 1983  | 🟢 | 🟢 | 🟢 | imperfect | 
| generic | `Generic MOS-compatible machine` | N/A | 🔴 | 🔴 | 🔴 | preliminary | 
| kim1 | `MOS KIM-1` | 1976 | 🟢 | 🟢 | 🔴 | imperfect | 
| nes | `Nintendo Entertainment System (NTSC)` | 1985 | 🟢 | 🟢 | 🟢 | imperfect | 
| nespal | `Nintendo Entertainment System (PAL)` | 1986 | 🟢 | 🟢 | 🟢 | imperfect | 
| um6578 | `UM6578 Enhanced Famiclone` | 199? | 🟢 | 🟢 | 🟢 | imperfect | 
| vssmb | `VS. Super Mario Bros.` | 1986 | 🟢 | 🟢 | 🟢 | working | 

### Atari 2600

```sh
./bin/gemu -M atari2600 -cartridge game.bin                      # CX40 joystick
./bin/gemu -M atari2600 -cartridge game.bin -device cx40 -device cx40   # two players
./bin/gemu -M atari2600 -cartridge breakout.bin -device cx30     # paddles
```

No BIOS is needed. The TIA is emulated one color clock at a time, synced to
the exact CPU cycle of every bus access (WSYNC, HMOVE and its HBLANK "comb",
per-register write delays, NUSIZ changes mid-draw and the other quirks the
library relies on), with two-channel TIA sound.

- **Cartridges**: 2K, 4K, F8/F6/F4 (+Superchip), EF/DF/BF (+SC), FE (Activision),
  E0 (Parker Bros), 3F/3E (Tigervision), E7 (M Network), FA (CBS RAM Plus),
  F0 (Megaboy), UA, CV (CommaVid), 0840, SB, DPC (Pitfall II) and AR
  (Starpath Supercharger tape images, with a built-in BIOS). The scheme is
  detected automatically; a file extension such as `.f8` or `.e0` forces one.
- **TV standard**: `atari2600` switches to the PAL palette by itself when a
  program draws PAL-length (312-line) frames; `atari2600pal` and
  `atari2600secam` force PAL timing/palette or the SECAM palette.
- **Keys** (Input menu to change): joystick on the arrows + Space, a second
  CX40 on W/A/S/D + Left Shift. Console: F1 Game Select, F2 Game Reset,
  F3 Color/B&W, F5/F6 left/right difficulty (toggles).
- **Monitor**: `sendkey up-fire`, `sendkey reset`, `sendkey p2left` (buttons:
  up down left right fire reset select, p2up... p2fire);
  `dipswitch color bw`, `dipswitch left a`, `dipswitch list` for the toggle
  switches; `mapperdump` shows the bank-switching state; `savevm`/`loadvm`.
- The machine menu's Reset power-cycles the console; the Game Reset switch
  is the F2 key / `sendkey reset`.

#### Debugging homebrew

```sh
dasm game.asm -f3 -ogame.bin -sgame.sym
./bin/gemu -M atari2600 -cartridge game.bin -symbols game.sym -monitor stdio
(gemu) autoreload on        # rebuild in another terminal: the game restarts with it
(gemu) break Kernel         # labels work anywhere an address does
(gemu) disas                # STA WSYNC, LDA INTIM... register names by direction
(gemu) tia                  # object x positions, NUSIZ, HMxx, PF bits, collisions
(gemu) scanline 10          # step the beam; breakline 100 stops when it gets there
(gemu) frameinfo watch on   # complain when a frame isn't 262 lines
```

- **Full-frame view** (View > "Full frame (debug)", or `view full`): every
  color clock of every line - HBLANK, VBLANK (dimmed, objects still drawn),
  VSYNC (magenta), the HMOVE comb, the TV window's edges - and, while
  stopped, the beam's position. It can also be detached into its own window.
- **Debug > Debug Colors** / `debugcolors on`: each object in a fixed colour
  (P0 red, M0 dark red, P1 yellow, M1 orange, BL blue, PF green);
  `hide p0 pf` takes objects out of the picture (they still collide).
- **Raster control**: `frame [n]`, `scanline [n]`, `breakline <n>`, plus
  `break`/`watch`, which match through the 6507's address mirrors (`break F000`
  also stops at `$1000`; with `vcs.h` labels in the symbol file, `wwatch WSYNC`
  works as written).
- `trace FILE` logs each instruction with frame, scanline and cycle-in-line;
  `riot` shows the timer and when it expires; `mapperdump` the bank state.

## Planned, not implemented yet
| Name   | Full Name     | Year |
|--------|---------------|---------------|
| atom | `Acorn Atom` | 1980 |
| electron | `Acorn Electron` | 1983 |
| atari130xe | `Atari 130XE` | 1983 |
| atari65xe | `Atari 65XE` | 1983 |
| atari800 | `Atari 800` | 1979 |
| atari800xl | `Atari 800XL` | 1983 |
| atari5200 | `Atari 5200` | 1982 |
| atari5200 | `Atari 7800` | 1986 |
| atarilynx | `Atari Lynx` | 1989 |
| apple2 | `Apple II` | 1977 |
| apple2plus | `Apple II Plus` | 1979 |
| apple2e | `Apple IIe` | 1983 |
| baby | `Baby! 1` | 1976 |
| master | `BBC Master` | 1986 |
| micro | `BBC Micro` | 1981 |
| c64 | `Commodore 64` | 1982 |
| c128 | `Commodore 128` | 1985 |
| pet | `Commodore PET` | 1977 |
| vic20 | `Commodore VIC-20` | 1980 |
| challenger | `Ohio Scientific Challenger 4P` | 1979 |
| orao | `Orao (Yugoslavia)` | 1984 |
| sfc | `Super Famicom` | 1991 |
| snes | `Super Nintendo Entertainment System (NTSC)` | 1991 |
| snespal | `Super Nintendo Entertainment System (PAL)` | 1992 |
| oric1 | `Tangerine Oric-1` | 1982 |
| oricatoms | `Tangerine Oric Atoms` | 1983 |
| tg16 | `TurboGrafx-16` | 1987 |

## Maybe in the future?

### Computers
| Name   | Full Name     | Year | Notes |
|--------|---------------|---------------|---------------|
| aone | `A-ONE` | 2006 | Apple 1 Replica | 
| beneater | `Ben Eater 6502 Computer` | ? | |
| beta | `Beta Single-Board Computer` | 1984 | |
| bem | `Brutech B.E.M.` | 1984 | |
| cepac65 | `CEPAC-65` | 1984 | |
| datahandler | `Data Handler` | 1975 | Built in 1975 by Western Data Systems Corporation, one of the first 6502 computers |
| datac1000 | `DATAC 1000` | 1976 | |
| dg6501 | `Digital Group 6501 CPU Board` | 1975 | |
| ec65 | `Elektor EC65` | 1981(?) | |
| elektor-clock | `Elektor 6502 Clock` | 1981(?) | |
| elektor-junior | `Elektor Junior` | 1980 | |
| rc6502 | `RC6502` | ? | Apple 1 Replica |
| replica1-se | `Replica 1 SE` | 2006 | Apple 1 Replica |
| replica1-te | `Replica 1 TE` | 2008 | Apple 1 Replica |
| replica1-ten | `Replica 1 TEN` | 2014 | Apple 1 Replica |
| mac3 | `Mac III Microcomputer` | 1990 |  |
| mega6502 | `Mega6502` | ? | Apple 1 Replica |
| mini-master | `Mini Master` | ? | Apple 1 Replica |


### Other
| Name   | Full Name     | Year | Notes |
|--------|---------------|---------------|---------------|
| asteroids | `Asteroids Deluxe` | 1980 |  | 
| hollywoodheat | `Hollywood Heat` | 1986 | Pinball game | 
| mtrap | `Mouse Trap` | 1981 | | 
| rackemup | `Rack 'em Up! Pinball` | 1983 | Pinball game | 