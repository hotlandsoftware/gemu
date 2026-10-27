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
| `lagonda` | Javelina AUDIT, Aston Martin Lagonda | skeleton | ROMs identified (`-rom roms/lagonda`); hardware not emulated yet |

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
