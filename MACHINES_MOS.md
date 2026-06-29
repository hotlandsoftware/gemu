# MOS 6502 machines emulated by GEMU

## CPUs
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| 2a03 | `Ricoh 2A03` | working |  |
| 6501 | `MOS Technology 6501` | working | Extremely rare early version of 6502,  pin-compatible with Motorola 6800 |
| 6502 | `MOS Technology 6502` | working | |

## VGA
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| 2c02 | `Ricoh RP2C02` | imperfect |

## Sound Cards
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| 2a03 | `Ricoh 2A03` | working | supports MIDI playback via `2a03,output=midi` |

## Devices
| Name   | Full Name     | Status | Notes |
|--------|---------------|---------|---------|
| fds | `Famicom Disk System` | working |  |
| famicom-keyboard | `Famicom 72-key Keyboard` | working |  |
| famicom-mic | `Famicom Microphone Controller` | working | no proper mic detection yet, can send mic sound by pressing M (default) |
| rob | `R.O.B. (Robotic Operating Buddy)` | working | opens in a second window, 3D simulation of ROB |
| rob-famicom | `Family Computer Robot (Famicom R.O.B.)` | working | |
| kim-keypad | `KIM-1 hex keypad` | working |  |
| nes-controller | `NES/Famicom Standard Controller` | working |  |
| vt100 | `DEC VT100 serial terminal` | working |  |
| wozmon | `Wozniak 'Woz' Monitor` | working |  |
| zapper | `NES Zapper (light gun)` | working |  |

# Machines

Simulated machines include:
## Implemented
| Name   | Full Name     | Year | Graphics  | Input | Sound | Status |
|--------|---------------|---------|---------|-------|-------|-------|
| apple1 | `Apple I` | 1976 | 🟢 | 🔴 | 🔴 | preliminary | 
| dendy | `Dendy Famiclone` | 1992 | 🟢 | 🟢 | 🟢 | imperfect | 
| famicom | `Nintendo Family Computer` | 1983  | 🟢 | 🟢 | 🟢 | imperfect | 
| generic | `Generic MOS-compatible machine` | N/A | 🔴 | 🔴 | 🔴 | preliminary | 
| kim1 | `MOS KIM-1` | 1976 | 🟢 | 🟢 | 🔴 | imperfect | 
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