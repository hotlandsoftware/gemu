# GEMU (Generic EMUlator) README

> [!WARNING]
> GEMU is currently unstable and should not be used in production environments.

<img align="left" src="gemu.png" alt="GEMU Icon" /> GEMU is designed to be a fast, accurate multi-machine emulator for a variety of different systems. The overall goal is to emulate every device and machine in existence along with their peripherals, while maintaining as much backwards compatibility with QEMU as possible.

GEMU seeks to offer:
- full, accurate, low-level emulation of a wide variety of computers, machines, and devices
- a simple and easy to use UI to easily setup a machine
- full, backwards compatibility with QEMU, including monitor commands, command line options, and QMP support.
- a wide variety of debugging features for homebrew and other uses

GEMU offers emulation for over 40 different machines, and is currently in an **alpha** state.

# Supported Architectures
GEMU currently emulates the following architectures:

- [X] IA64 / Itanium
- [X] MOS
- [X] RCA
- [X] X86
- [X] Z80

# Targets & Documentation
see https://gemu.miraheze.org/

# Screenshots

| ![](imgs/nes.webp)   | ![](imgs/atari400.webp)  | ![](imgs/2600.webp)      |
|:--------------------:|:------------------------:|:------------------------:|
| NES/Famicom          | Atari 400                | Atari VCS/2600           |
| ![](imgs/chip8.webp) | ![](imgs/i2000.webp)     | ![](imgs/ibm5150.webp)   |
| CHIP-8               | HP i2000                 | IBM 5150                 |
| ![](imgs/lagonda.webp)| ![](imgs/pecom64.webp)  | ![](imgs/apple1.webp)    |
| Lagonda Car Dashboard | Pecom 64                | Apple I                  |


# Building
## Linux
Install the required dependencies first:

**Debian / Ubuntu**
```
sudo apt-get install gcc make ninja-build python3 libsdl2-dev libsdl2-ttf-dev libncurses-dev libssl-dev
```
Optional: `libzip-dev` (`-rom FILE.zip`), `libsdl2-image-dev` (window icon), `libgtk-3-dev libepoxy-dev` (`--enable-gtk`), `libcaca-dev` (`-display curses` fallback), `libasound2-dev` (ALSA MIDI out).

**Arch Linux**
```
sudo pacman -S --needed base-devel ninja python sdl2 sdl2_ttf ncurses openssl
```
Optional: `libzip`, `sdl2_image`, `gtk3 libepoxy`, `libcaca`, `alsa-lib`.

Then configure and build:
```
python configure && ninja -j$(nproc)
python configure --enable-gtk && ninja -j$(nproc) # for GTK support
```

## Windows

**MSYS2**
Install [MSYS2](https://www.msys2.org/), then from an **MSYS2 MinGW64** shell (not the plain MSYS2 shell):
```
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-ninja mingw-w64-x86_64-python
python configure && ninja
```
`mingw-w64-x86_64-SDL2_ttf`, `mingw-w64-x86_64-SDL2_image`, `mingw-w64-x86_64-libzip`, `mingw-w64-x86_64-gtk3` and `mingw-w64-x86_64-libepoxy` are optional, for VT100/window-icon/`-rom FILE.zip`/GTK support.

The MinGW64 toolchain (`gcc`, `python`, `ninja`, `pkg-config`) lives under `<msys64>\mingw64\bin` as ordinary Windows executables, so once installed you can also build and run from a plain PowerShell/cmd session by adding that directory to `PATH`:
```
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
python configure && ninja
```
`bin\gemu.exe` is dynamically linked against MSYS2's mingw64 DLLs (SDL2 etc.), so run it either from a shell that has `mingw64\bin` on `PATH`, or copy the DLLs `ldd` reports next to the executable - see the `build-windows` job in [.github/workflows/build.yml](.github/workflows/build.yml) for the exact list this project ships with releases.

**Cygwin**
TODO

**Visual Studio**
TODO

# Disclosure
This program is made pretty much entirely with AI (though I have enough programming experience to make it not completely unusable/horrible.) The emulator is mainly an experiment to see how many different machines AI agents can make an emulator with little reference (simply feeding it datasheets, manuals, expected behavior, etc.) If you are against AI developed software, you'll want to close the page now!