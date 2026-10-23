Disk images are attached the same way as in QEMU: with `-drive`, or with QEMU's shorthand options. Machines that predate this (NES Famicom Disk System, Atari 810) keep their own `-fda`/`-floppy`.

# -drive
```
-drive [file=PATH][,if=ide|floppy|scsi|...][,bus=N][,unit=N][,index=N]
       [,media=disk|cdrom][,readonly=on|off][,snapshot=on|off][,format=raw][,id=NAME]
```
- `if=` defaults to the machine's default interface (IDE on PC-class machines).
- `index=N` picks a position on the interface: IDE has two units per bus, so index 0/1 are `ide0` master/slave and index 2/3 are `ide1`. Without `index`, `bus` or `unit`, a drive takes the next free unit.
- A CD-ROM or floppy drive can be given without `file=` to start empty; insert media later from the monitor.
- `media=cdrom` is always read-only. `readonly=on` makes writes fail as on write-protected media.
- `snapshot=on` keeps every write in memory and discards it on exit; `-snapshot` does this for all drives.
- Only raw images are supported (convert others with `qemu-img convert -O raw`). Write a comma inside a value as `,,`.
- QEMU keys with no meaning here (`cache=`, `aio=`, `discard=`, `werror=`, ...) are accepted and ignored.

# Shorthand
| Option | Same as |
|--------|---------|
| `-hda`..`-hdd FILE` | `-drive file=FILE,index=0..3,media=disk` |
| `-cdrom FILE` | `-drive file=FILE,index=2,media=cdrom` |
| `-fda`/`-fdb FILE` (also `-floppy`) | `-drive file=FILE,index=0..1,if=floppy` |
| `-snapshot` | `snapshot=on` on every drive |

A drive the machine has no device for is an error ("machine type does not support if=...,bus=...,unit=..."), as in QEMU.

# Monitor
Drives have QEMU's names - `ide0-hd0`, `ide1-cd0`, `floppy0`, ... or their `id=`:
```
(gemu) info block
floppy0: dos.img (raw)
    Removable device: not locked, tray closed
(gemu) change floppy0 disk2.img
(gemu) eject ide1-cd0
```

# Machines
| Machine | Drives |
|---------|--------|
| `ibm5150` | up to four diskette drives (`if=floppy`, 160K/180K/320K/360K images). The diskette adapter is installed when a floppy drive is given; POST then boots from A:. No IDE. |
| `i2000` | `ide0-hd0` ATA hard disk (`-hda`), `ide1-cd0` ATAPI CD-ROM (`-cdrom`). |
| `generic-ia64` | `ide0-hd0` hard disk, `ide1-cd0` CD-ROM. |
