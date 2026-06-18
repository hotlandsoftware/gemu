# gemu-mos
MOS 65xx microprocessor emulator. 

Currently emulated hardware for this machine:

## CPUs
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| 2a03 | `Ricoh 2A03` | working |  |
| 6501 | `MOS Technology 6501` | working | |
| 6502 | `MOS Technology 6502` | working | |

## VGA
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| 2c02 | `Ricoh RP2C02` | imperfect |

## Sound Cards
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| 2a03 | `Ricoh 2A03` | imperfect | supports MIDI playback via `2a03,outpu=midi` |


# Machines

Simulated machines include:
## Implemented
| Name   | Full Name     | Year | Graphics  | Input | Sound | Status |
|--------|---------------|---------|---------|-------|-------|-------|
| famicom | `Nintendo Family Computer` | 1983  | 🟢 | 🟢 | 🟢 | imperfect | 
| generic | `Generic MOS-compatible machine` | N/A | 🔴 | 🔴 | 🔴 | imperfect | 
| kim1 | `MOS KIM-1` | 1976 | 🟠 | 🔴 | 🔴 | imperfect | 
| nes | `Nintendo Entertainment System (NTSC)` | 1985 | 🟢 | 🟢 | 🟢 | imperfect | 
| nespal | `Nintendo Entertainment System (PAL)` | 1986 | 🟢 | 🟢 | 🟢 | imperfect | 

## Planned, not implemented yet
| Name   | Full Name     | Year |
|--------|---------------|---------------|
| atom | `Acorn Atom` | 1980 |
| electron | `Acorn Electron` | 1983 |
| atari400 | `Atari 400` | 1979 |
| atari130xe | `Atari 130XE` | 1983 |
| atari65xe | `Atari 65XE` | 1983 |
| atari800 | `Atari 800` | 1979 |
| atari800xl | `Atari 800XL` | 1983 |
| atari2600 | `Atari 2600/VCS` | 1977 |
| atari5200 | `Atari 5200` | 1982 |
| atari5200 | `Atari 7800` | 1986 |
| atarilynx | `Atari Lynx` | 1989 |
| apple1 | `Apple I` | 1976 |
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
| kim1 | `KIM-1` | 1976 |
| challenger | `Ohio Scientific Challenger 4P` | 1979 |
| orao | `Orao (Yugoslavia)` | 1984 |
| sfc | `Super Famicom` | 1991 |
| snes | `Super Nintendo Entertainment System (NTSC)` | 1991 |
| snespal | `Super Nintendo Entertainment System (PAL)` | 1992 |
| oric1 | `Tangerine Oric-1` | 1982 |
| oricatoms | `Tangerine Oric Atoms` | 1983 |
| tg16 | `TurboGrafx-16` | 1987 |