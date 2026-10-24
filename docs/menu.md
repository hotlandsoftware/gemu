# Window menu bar

Every GEMU window has a menu bar along the top, for every machine. With
`-display sdl` GEMU draws it itself, in the style of DOSBox-X. With
`-display gtk` it is a normal GTK menu bar. It is part of the window only:
VNC clients (`-vnc`) and `screendump` see just the emulated screen.

| Menu    | Items |
|---------|-------|
| Machine | Pause, Reset, Screenshot (`gemu-YYYYMMDD-HHMMSS.png`), Quit |
| Blocks  | one submenu per drive (`floppy0: disk.img`): Change..., Eject, Flip disk |
| Scale   | window scale 1x-4x (you can also drag the window edges); starts at 1x unless `-scale N` |
| Input   | one entry per input device (`nes-controller`, `nes-controller (2)`, ...) to remap its keys and controller; Reset bindings to defaults. Hidden when the machine has no remappable input (SDL) |
| Debug   | Hex Editor (machines that have one) |
| View    | which monitor the window shows, on machines with several (e.g. an IBM 5150 with CGA + MDA); Ctrl+Alt+1..9. Detach ... to new window gives the current monitor its own window (closing it brings the monitor back) |
| Help    | About GEMU |

The menus run the matching monitor commands (`stop`/`cont`, `system_reset`,
`screendump`, `change`, `eject`). The Blocks menu therefore lists the same
devices as `info block`.

## Keyboard and controller

- **Ctrl+Shift+Tab** opens the first menu. It works on every machine.
- **Tab** (or the controller's Menu button) also works on machines without a
  keyboard. On machines with a keyboard (such as the IBM 5150), Tab goes to
  the guest.
- Arrow keys move between items, Enter selects, and Esc closes the menu.

The Change... item opens a file browser. Use Enter or double-click to open a
file or directory, choose `..` to go up, and type a letter to jump to
the files that start with it.
