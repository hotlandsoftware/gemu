The GEMU monitor is made to give commands to the GEMU emulator. It is designed to be as backwards compatible as possible with the QEMU emulator, with a couple of additional commands per emulator.

# Selecting a monitor
The monitor is enabled on stdio by default when stdin is an interactive
terminal:

> ``-monitor stdio``

Uses stdin/stdout for the monitor prompt.

> ``-monitor telnet:127.0.0.1:4444,server,nowait``

Starts a telnet monitor server on the given host and port. One client can be
connected at a time.

> ``-monitor none``

Disables the interactive monitor.

# Commands
The following commands are available:

> ``help``

Shows the list of commands.

> ``quit`` / ``q``

Quits the emulator.

> ``reset`` / ``system_reset``

Hard resets the emulator (equivalent to pressing the reset button on a PC)

> ``stop`` / ``halt``

Halts the emulator (does not quit)

> ``cont``

Resumes the emulator if it was stopped or halted.

> ``reset keys``

Resets all input bindings to default

> ``step [count]`` / ``s``

Steps through (x) instructions, defaults to 1 if no count specified

> ``info block``

Lists block devices

> ``info breakpoints``

Lists active breakpoints

> ``info roms``

Lists loaded ROM images

> ``dipswitch list``

Lists machine-specific DIP switches (when supported by the active machine)

> ``dipswitch (name) (value)``

Sets a machine-specific DIP switch (when supported by the active machine)

> ``change (device) (file)``

Inserts or changes media attached to a registered device. Device names depend
on the active machine, for example ``change cartridge game.bin`` on Studio II
or ``change tape program.bin`` on COSMAC VIP.

> ``change (device) (ADDR):(file)``

Passes an address-qualified file argument to devices that support it. For example, the COSMAC VIP tape accepts this form, for example ``change tape 0x0200:program.bin``.

> ``change vnc password``
Change/set VNC password, if VNC server is enabled

> ``eject (device)``

Ejects media from a registered device. 

> ``flip (device)``
Flips disk media to the next side, useful for i.e. Famicom floppies or dual-sided DVDs

> ``screendump (image name)``

Dumps a screenshot of the emulator.

> ``gamegenie add (code)``

Inserts a Game Genie code. Only supported on: ``nes``, ``snes`` (future), ``gameboy`` (future), ``genesis`` (future), and ``gamegear`` (future)

> ``gamegenie list``

Lists active Game Genie codes. Only supported on: ``nes``, ``snes`` (future), ``gameboy`` (future), ``genesis`` (future), and ``gamegear`` (future)

> ``gamegenie delete (code)``

Deletes a Game Genie code. Only supported on: ``nes``, ``snes`` (future), ``gameboy`` (future), ``genesis`` (future), and ``gamegear`` (future)

> ``break / b <addr>``
Set exec breakpoint

> ``delete [N]``
Delete breakpoint N, or all if N is omitted

> ``watch <addr>``
Set read+write watchpoint

> ``wwatch <addr>``
Set write watchpoint