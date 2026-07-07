# GEMU (Generic EMUlator)

> [!WARNING]
> GEMU is currently unstable and should not be used in production environments.

GEMU is a generic multi-machine emulator designed to be backwards compatible with QEMU while emulating as many different machines and architectures (along with their hardware and peripherals) as possible. The UI (planned) is to be based on the simplistic UI of Virtual PC 2007 while still containing an advanced featureset.

GEMU is currently in an alpha state. It will likely never be "complete" as there will always be more machines and hardware for it to emulate. Currently the absolute latest (for my end) is set to 1985 (exceptions made for clone consoles), and will be increased as more machines are completed/added.

# Build command
```
python configure && ninja -j$(nproc)
python configure --enable-gtk && ninja -j$(nproc) # for GTK support
```

# Targets & Documentation
See https://gemu.mirhaeze.org/

# Transparency
This entire program is vibe coded - though I have some experience with programming which helps make it not completely horrible. I mainly started this as a way to see how far AI agents have come, and now I'm curious how many machines I can make AI emulate with little reference (manuals if necessary are allowed.)