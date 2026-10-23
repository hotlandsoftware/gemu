The GEMU monitor is made to give commands to the GEMU emulator. It is designed to be as backwards compatible as possible with the QEMU monitor, with a few additional commands.

Every machine has the same monitor: the same commands, the same `help` output, and the same syntax. Commands that only make sense on certain machines (Game Genie codes, DIP switches, coin slots, ...) are still listed by `help`, together with the machines they work on. Using one on another machine prints which machines support it, rather than "unknown command".

# Selecting a monitor
The monitor is enabled on stdio by default when stdin is an interactive
terminal:

> ``-monitor stdio``

Uses stdin/stdout for the monitor prompt.

> ``-monitor telnet:127.0.0.1:4444,server,nowait``

Starts a telnet monitor server on the given host and port. One client can be
connected at a time.

> ``-monitor gmp:127.0.0.1:4444`` / ``-monitor gmp-stdio``

Speaks GMP, GEMU's QMP-compatible JSON protocol, over TCP or stdio. As in QEMU,
the server sends a greeting, the client must send `qmp_capabilities` first, and
every reply echoes the request's `id`. Requests may span lines or share one.

| Command | Description |
|---------|-------------|
| `qmp_capabilities` | finish capabilities negotiation |
| `query-commands` | list the supported commands |
| `query-status` | whether emulation is running or paused |
| `query-version` | protocol/emulator version |
| `human-monitor-command` | run any monitor command below (`"arguments": {"command-line": "..."}`) and return its output as a string |
| `stop`, `cont`, `system_reset`, `quit` | same as the monitor commands; the reply is sent once the command has taken effect |

```
-> {"execute": "qmp_capabilities"}
<- {"return": {}}
-> {"execute": "human-monitor-command", "arguments": {"command-line": "info cpu"}, "id": 1}
<- {"return": "PC=C000 A=00 X=16 Y=00 SP=FA P=24 [..-..I..] insn=44 cyc=117\n", "id": 1}
```

> ``-monitor none``

Disables the interactive monitor.

# Syntax
- Addresses and byte values are hexadecimal, with or without a `0x` or `$` prefix (`break c000`, `poke 0x200 a9 01`).
- Counts and sizes are decimal unless prefixed with `0x` (`peek 200 32`, `memsave 0 0x800 ram.bin`).
- File names can be quoted if they contain spaces.
- `help <command>` describes a single command, including which machines support it.

# Commands available on every machine

## General
> ``help`` / ``?`` ``[command]``

Lists all commands, or describes one.

> ``info <subcommand>``

| Subcommand | Shows |
|------------|-------|
| `info block` | media devices and the files inserted in them |
| `info breakpoints` / `info bp` | breakpoints and watchpoints |
| `info cpu` / `info registers` | CPU state (also `cpu`) |
| `info machine` | the emulated machine (`-M` name) |
| `info media` | media device status |
| `info roms` | loaded ROM images |
| `info status` | whether emulation is running or paused |

> ``quit`` / ``q``

Quits the emulator.

## Execution
> ``reset`` / ``system_reset``

Hard resets the machine (equivalent to pressing the reset button on a PC).

> ``reset keys``

Resets all input bindings to their defaults.

> ``stop`` / ``halt``

Halts emulation (does not quit).

> ``cont`` / ``c``

Resumes emulation if it was stopped or halted.

> ``step [count]`` / ``s``

Executes `count` instructions (default 1), then stops.

## Breakpoints
> ``break <addr>`` / ``b``

Sets an exec breakpoint.

> ``watch <addr>`` / ``awatch``, ``rwatch <addr>``, ``wwatch <addr>``

Sets a read+write, read or write watchpoint.

> ``delete [N]`` / ``del``

Deletes breakpoint N, or all breakpoints if N is omitted.

## Memory
These use the machine's memory view, which reads RAM and ROM directly and never disturbs hardware registers. Bytes that can't be read safely (I/O registers whose reads have side effects) show as `--`.

> ``x [/fmt] <addr>`` / ``xp [/fmt] <addr>``

Examines memory. `fmt` is `[count][format][size]`: format is `x` (hex, default), `d`, `u`, `o` or `c`; size is `b` (byte, default), `h` (2 bytes), `w` (4 bytes) or `g` (8 bytes), in the machine's byte order. For example, `x /16xb 0200` or `x /4xw 8000`. `xp` is the same as `x` on machines without an MMU.

> ``peek <addr> [len]``

Hex dumps `len` bytes (default 16).

> ``poke <addr> <byte>...``

Writes bytes to memory, for example `poke 0300 a9 00 60`.

> ``memsave <addr> <size> <file>`` / ``pmemsave``

Saves `size` bytes of memory starting at `addr` to a file.

## Media
> ``change <device> <file>``

Inserts or changes media attached to a device. Device names depend
on the active machine (see `info block`), for example ``change cartridge game.bin`` on Studio II
or ``change tape program.bin`` on COSMAC VIP.

> ``change <device> <addr>:<file>``

Passes an address-qualified file argument to devices that support it, for example the COSMAC VIP tape: ``change tape 0x0200:program.bin``.

> ``eject <device>``

Ejects media from a device.

> ``flip <device>``

Flips disk media to the next side, useful for e.g. Famicom floppies or dual-sided DVDs.

> ``change vnc password``

Changes/sets the VNC password, if the VNC server is enabled.

## Screen and input
> ``screendump <file>[.png]``

Saves a screenshot (PPM, or PNG if the name ends in `.png`).

# Commands that need machine support
These are available on every machine that implements them; `help` marks the ones the current machine doesn't support.

> ``savevm <file>`` / ``loadvm <file>``

Saves or restores the full machine state.

> ``sendkey <keys>``

Presses keys or buttons on the machine's input device. The key names depend on the machine (e.g. `sendkey start` on the NES, `sendkey HELLO enter` on the Apple I).

> ``hexeditor``

Toggles the hex editor window (GTK builds, or SDL on Windows).

# Machine-specific commands
Run `help` on any machine for the authoritative list; the machines each command supports are shown in brackets.

| Command | Machines | Description |
|---------|----------|-------------|
| `gamegenie add <code>` / `list` / `delete <code>` | nes, nespal, famicom, dendy | manage Game Genie codes |
| `ppudump`, `mapperdump` | NES family, VS. System | show PPU state / mapper registers |
| `dipswitch [list \| <switch> <value>]` / `dip` | vssmb, vsskatekids, 5clown, magicfly, 7mezzo, destroyer, altair2 | list or set DIP switches |
| `coin [slot]` | vssmb, vsskatekids, 5clown, magicfly, 7mezzo | insert a coin |
| `service` | vssmb, vsskatekids | press the service button |
| `mouse`, `mousemove`, `mouseclear`, `mousestate` | um6578, asobou, czdf1 (`mousemove`, `mousestate` also i2000) | drive a synthetic mouse |
| `key`, `trace`, `history`, `calls`, `dump`, `vhptstats`, `faultstats` | i2000, generic-ia64 | IA-64 debugging |
| `input`, `panel`, `mousekick`, `atapistate` | i2000 | i2000 debugging |
| `regpoke`, `vgareg`, `vgapeek` | generic-ia64 | generic IA-64 debugging |

# Adding commands (for developers)
GMP lives in `core/src/monitor_qmp.c` and uses the bundled [cJSON](https://github.com/DaveGamble/cJSON) (`core/third_party/cjson`, MIT) for all JSON parsing and generation; add QMP commands to its `qmp_cmds[]` table.

All commands are declared in the `mon_cmds[]` table in `core/src/monitor_cmds.c`; `help` is generated from it. Machines implement the ones the core can't with `gemu_monitor_bind()`, and provide a memory view with `gemu_monitor_set_memory()`. See the comment at the top of `monitor_cmds.c` and `core/include/gemu/monitor.h`.
