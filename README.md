# GEMU (Generic EMUlator)

> [!WARNING]
> GEMU is currently unstable and should not be used in production environments.

<img align="left" src="gemu.png" alt="GEMU Icon" /> GEMU is a multi-machine emulator which seeks to emulate every device and machine in existence (yes, really) while having an extremely wide variety of features and the versatile command set of QEMU. 

The emulator seeks to offer:
- full, accurate, and fast low-level emulation of a wide variety of computers, machines and devices
- a simple, easy to use UI to easily set up any machine
- full, backwards compatibility with QEMU, including console commands, command line options, even featuring full QMP support.
- a wide variety of helpful debug features

GEMU currently emulates around 32 machines, and is currently in an **alpha** state.

# Build command
```
python configure && ninja -j$(nproc)
python configure --enable-gtk && ninja -j$(nproc) # for GTK support
```

# Targets & Documentation
See https://gemu.miraheze.org/

# Transparency
This entire program is vibe coded, though I have some experience with programming which helps make it not completely horrible. I mainly started this as a way to see how far AI agents have come - and now I'm curious to see how many different machines and devices I can make AI emulate with little reference (simply feeding it datasheets, manuals, expected behavior etc)