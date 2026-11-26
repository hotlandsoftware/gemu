# 86Box v6.0 sources

Files from [86Box](https://github.com/86Box/86Box), copied unchanged from the
release tag **v6.0** (commit `4fef696a4eead1d55a28d6ac0e5bd2864e5454da`,
30 May 2026), in upstream's tree layout. They compile against GEMU's
86Box layer, `compat/86box/` (see `compat/86box/README.md`), which implements
the framework headers (`86box/device.h`, `io.h`, `timer.h`, `pic.h`...) on
GEMU's core.

## License

86Box is released under the GNU General Public License, version 2 **or
later** (its README; `COPYING` here is its copy of the GPLv2). GEMU is
GPLv3, which the "or later" permits. Each file below was checked: either its
header says "or any later version", or it carries no license text of its own
and falls under the project's statement. A file from another project (an OPL
core, say) brings its own license, which must be listed here and be
GPLv3-compatible before it is vendored.

## Files

| File | Upstream license note | Local changes |
|---|---|---|
| `src/device/novell_cardkey.c` | none in file; project GPLv2+ | none |
| `src/include/86box/novell_cardkey.h` | none in file; project GPLv2+ | none |
| `src/sound/snd_adlib.c` | none in file; project GPLv2+ | none |
| `src/include/86box/snd_adlib.h` | none in file; project GPLv2+ | none |
| `src/include/86box/snd_opl.h` | none in file; project GPLv2+ | none |
| `src/sound/snd_opl2_nuked.c` | Nuked OPL2 Lite (nukeykt): GPLv2 or later, in the file | none |
| `src/include/86box/snd_opl2_nuked.h` | Nuked OPL2 Lite: GPLv2 or later, in the file | none |
| `COPYING` | GPLv2 text | none |

**Local changes are not allowed** without a row here saying what and why.
To check, compare a file with the tag:

```
git -C <86box clone> show v6.0:src/device/novell_cardkey.c | cmp - third_party/86box/v6.0/src/device/novell_cardkey.c
```

Not vendored on purpose: `src/sound/snd_opl.c` (86Box's FM core chooser,
which names every core, the C++ ymfm ones among them) - GEMU's layer has
its own `fm_driver_get()` (`compat/86box/fm.c`).

## Adding a file

1. Copy it from the v6.0 tag to the same path under `src/`.
2. Add a row above, after checking its license header.
3. Write its port in `compat/86box/ports/` (see `compat/86box/README.md`).
