# GEMU (Generic EMUlator)

> [!WARNING]
> GEMU is currently unstable and should not be used in production environments.

<img align="left" src="gemu.png" alt="GEMU Icon" /> GEMU is a multi-machine emulator which seeks to emulate every device and machine in existence (yes, really) while having an extremely wide variety of features and the versatile command set of QEMU. 

The emulator seeks to offer:
- full, accurate, and fast low-level emulation of a wide variety of computers, machines and devices
- a simple, easy to use UI to easily set up any machine
- full, backwards compatibility with QEMU, including console commands, command line options, even featuring full QMP support.
- a wide variety of helpful debug features

GEMU currently emulates around 35 machines, and is currently in an **alpha** state.

# Targets & Documentation
See https://gemu.miraheze.org/

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

# Transparency
This entire program is vibe coded, though I have some experience with programming which helps make it not completely horrible. I mainly started this as a way to see how far AI agents have come - and now I'm curious to see how many different machines and devices I can make AI emulate with little reference (simply feeding it datasheets, manuals, expected behavior etc)