A device is a chip or card: a PIC, a UART, a disk controller, a network card. A machine is the board that connects devices to a CPU. Keep them apart: **new hardware is written as a self-contained device, never as fields and code inside a machine.**

This is what lets a device be reused by another machine, tested on its own, and later moved onto a shared device framework without touching the machines that use it.

# Where devices go
| Kind | Directory |
|------|-----------|
| Interrupt, DMA, parallel I/O, timer chips | `hardware/device/controller/`, `hardware/device/timer/` |
| Disk, tape and flash controllers | `hardware/device/storage/` |
| Keyboards, mice, controllers | `hardware/device/input/` |
| Serial, network, audio | `hardware/device/serial/`, `network/`, `audio/` |
| Chipsets, buses, I²C/SMBus parts | `hardware/device/chipset/`, `hardware/device/i2c/` |
| Video chips and cards | `vga/multi/` |

Add the `.c` to each family's source list in `configure` (`FAMILY_SRC`) that uses it.

Before writing a new one, check whether it exists already: `i8259`, `i8253`, `i8237`, `i8255`, `ins8250`, `m6532`, `pia6821`, `upd765`, `ps2_keyboard` and others are there. Extend an existing device (for example, adding a mode it lacks) rather than writing a second copy inside a machine.

# Shape of a device
`hardware/device/network/e100.h` is a good model. A device `foo` has:

- **`Foo`**: a struct holding all of its state. Nothing about the device lives in the machine's state struct, and the device never reaches into the machine's.
- **`FooConfig`**: what varies between boards (model, sizes, identity, straps), given at init.
- **`FooWiring`**: how it talks to the outside, as callbacks plus an `opaque` pointer: its clock (`clock()` + `clock_hz`), interrupt and DMA request lines (`irq(opaque, level)`), bus-master memory access, and host-side I/O (drives, network, audio). The machine fills this in; the device calls nothing else.
- **Functions**: `foo_init(Foo *, const FooConfig *, const FooWiring *)`, `foo_reset(Foo *)`, register access (`foo_read`/`foo_write`, or `foo_io_access`/`foo_mem_access`/`foo_cfg_read` for PCI), and, if it does work over time, `foo_tick` or `foo_poll`.

Rules that follow from this:

- **Addresses are the device's own.** A register function takes the offset within the device (`reg 0-7`), not the machine's port or physical address. The machine decides where the device sits.
- **Interrupt numbers belong to the machine.** A device raises *its* line; which IRQ, GSI or vector that becomes is the machine's wiring.
- **Time comes from `clock()`.** Don't count guest instructions in a device, and don't read the CPU's state to pace it.
- **Workarounds stay with their owner.** If firmware or an OS needs a quirk, put it in the device that has the quirk (or in the chipset that routes it), with a comment saying who needs it, not in the machine's poll loop.
- **State is plain data.** Keep host pointers (buffers the machine owns, `GemuDrive *`, wiring) out of the part of the struct that would be saved, or group them at the end, so the state can be written to a snapshot and restored.
- **Monitor commands for a device** (`info`-style dumps) are written next to the device and registered by the machine, not written inside the machine.

# Tests
A device should have a unit test in `tests/` that drives it through its register functions with a stub `FooWiring`, without a machine. Follow `tests/e100.c` or `tests/rage128.c`.

Register the test in the `TESTS` table in `configure`: a `'device'` test links only the sources you list (so an accidental dependency on a CPU or machine fails to link), a `'program'` test links the whole emulator in place of `main.c` (and of a machine file it `#include`s). Then:

```
ninja check            # build and run every configured test
ninja check-foo        # just one
ninja tests            # build them without running
```

Each test runs headless in its own temporary directory, so it can create scratch files with relative paths. It should print a one-line summary and exit non-zero on failure. `ninja check` runs in CI.

# Existing machines
Some machines still have devices built into them (the i2000's PICs, IDE and keyboard controller; the NES mappers; the KIM-1's RRIOTs). These are being moved out step by step. When you change one of them, prefer moving it out first over adding to it.
