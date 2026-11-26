# GEMU's 86Box layer

86Box (v6.0) device drivers, unchanged, as GEMU core devices. 

```
compat/86box/include/86box/   the framework headers, shaped like v6.0's
compat/86box/*.c              their implementation on the core
compat/86box/gemu_86box.h     GEMU's side: a device_t as a GemuDeviceType
compat/86box/ports/           one file per ported driver
third_party/86box/v6.0/       the drivers' sources, unchanged (README.md there)
```

## What a driver gets

| 86Box | In GEMU |
|---|---|
| `device_t`, `device_config_t` | a core device type; an option per config entry |
| `device_get_config_int/_int_ex/_hex16/_hex20/_string` | the device's options |
| `io_sethandler`, `io_removehandler`, `io_handler` (+`_common`) | SHARED mappings in the machine's `io` space |
| `timer_add`, `timer_set_delay_u64`, `timer_advance_u64`, `timer_on_auto`, `timer_disable`, `timer_stop`, `tsc`, `TIMER_USEC` | core timers, exact to the picosecond; saved with the device |
| `picint`, `picintc`, `picintlevel`, `picintclevel` | the device's `irq0`..`irq15` lines |
| `pclog`, `pclog_ex`, `fatal` | stderr; `fatal` stops GEMU |
| `device_add`, `_inst`, `_params`, `_inst_params` (from an init) | a child core device of the `device_t`'s port, started at once |
| `sound_add_handler`, `music_add_handler`, `sound_pos_global`, `music_pos_global` | a core stream per kind (48000 / 49716 Hz, stereo) through the mixer |
| `fm_driver_get`, `fm_driver_get_ex` (`fm.c`) | the Nuked OPL2 (YM3812) as a sub-device; other chips stop GEMU |
| `cycles`, `isa_timing` (`"cpu.h"`) | a sink: GEMU's CPUs time their own bus cycles |
| `mca_add` | stops GEMU: no machine has an MCA bus |

The headers declare only what is implemented: a driver that uses anything
else - `mem_mapping_*`, DMA, CPU state, 86Box's global settings - doesn't
compile.  That's the point: each gap is filled when a
port needs it (E2-E5 in the plan), never by a silent stub.

Things to know:

- **`tsc`** counts clocks of the 86Box devices' clock: `gemu_device_clock()`
  of the device, normally its bus's (the 5150's ISA bus runs at the CPU's
  4.77 MHz).  All 86Box devices in a machine share it, as 86Box's `tsc` is
  one global.  It is brought up to date whenever GEMU calls into a driver
  (an I/O handler, a timer, init, reset, close).
- **Timers** must be made in the driver's init (`timer_add`): that is where
  core timers and save state are registered.
- **`picint()`** is a fresh edge each call, as each sets the PIC's request
  bit in 86Box; level requests are plain lines, wired-OR across devices.
- **Word I/O** to a port reaches the driver's `inw`/`outw` when the access
  is inside one `io_sethandler` range; otherwise it is made of bytes.
- **Sub-devices** (`device_add` in an init) are core children named after
  their type (`isa/adlib/ym3812_nuked_opl2`), so they have their own state
  and show in `info qtree`; the parent's handlers reach them through their
  priv as in 86Box.
- **Sound**: the handlers are called with the samples since the last call,
  in pieces of at most a buffer, whenever the device's stream is brought
  up to date (the mixer, a timer every half buffer, before a save).  On
  every entry into the device's code `*_pos_global` says how far the present
  is into the current piece, which is where a driver generates up to
  before a register change: changes land on the right sample.  Before a
  save the handlers are run up to that moment, so a driver that generates
  lazily (an OPL) is saved at it; after a load they start empty there.

## Porting a driver

1. Copy its files from the v6.0 tag into `third_party/86box/v6.0/` (same
   paths) and list them, with their license, in the README there.
2. Write `ports/<file>.c`: include the driver's .c (it is the same
   translation unit, so it sees the driver's private structure), then

   ```c
   static bool card_state(GemuDevice *d, void *priv) {
       card_t *dev = priv;                 /* the driver's own type */
       return GEMU_STATE_VAR(d, "reg", dev->reg) && ...;
   }
   GEMU_86BOX_PORT(gemu_86box_card_type, card_device, "card", card_state, NULL, false);
   ```

   Every device saves (question 7): register what the driver changes after
   init; a driver whose private data all comes from its options passes
   `NULL, NULL, true` (stateless).  Its timers register themselves.  A
   `post_load` hook redoes what follows from the state (ports moved by a
   register, say).  Upstream warnings are silenced around the `#include`
   with `#pragma GCC diagnostic`, not by editing the file.
3. Add `ports/<file>.c` to `COMPAT_86BOX_PORTS` in `configure`, and a
   `data/device/86box/<name>.xml` with the type's symbol and source.
4. A machine adds it like any device type and connects its IRQs
   (`gemu_86box_connect_irqs(dev, "board/pic:ir%u", 8)`).  The 5150 does
   it for any card whose XML names it (`<machine name="ibm5150"/>`):
   `-device <type>[,opts]` puts it under `isa/` and connects its
   `irq2`..`irq7` to the PIC.

## Ports

| Type | Driver | Machine |
|---|---|---|
| `novellkeycard` | `src/device/novell_cardkey.c` | (none yet) |
| `adlib` | `src/sound/snd_adlib.c` | the 5150: `-device adlib` |
| `ym3812_nuked_opl2` | `src/sound/snd_opl2_nuked.c` | the AdLib's chip |

The OPL2's port saves the chip whole and moves its pointers (all into the
chip) on load, and switches off the driver's CPU-cycle charging.

`tests/86box.c` is the reference: a driver written as 86Box's are, the
Novell key card and the AdLib from `third_party`, on a small machine.
