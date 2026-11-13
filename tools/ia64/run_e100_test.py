#!/usr/bin/env python3
"""Build/run the Intel 82559 (e100) model and the PCI ROM helpers without a
machine or CPU.

Configure GEMU first so that build.ninja holds the compiler flags. No ROM or
disk image is needed; the model links against libc only, which catches
accidental CPU or machine dependencies.
"""
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
# Any configured object gives the compiler flags and include paths.
FLAGS_FROM = ("build/vga/multi/rage128.o", "vga/multi/rage128.c")
SOURCES = (
    "hardware/device/network/e100.c",
    "hardware/device/network/pci_rom.c",
    "tests/e100.c",
)


def command_for(target):
    output = subprocess.check_output(
        ["ninja", "-t", "commands", target], cwd=ROOT, text=True
    )
    return shlex.split(output.strip().splitlines()[-1])


def main():
    reference, reference_source = FLAGS_FROM
    base = command_for(reference)
    link = command_for("bin/gemu")
    with tempfile.TemporaryDirectory(prefix="e100-", dir=ROOT / "build") as tmp:
        work = Path(tmp)
        objects = []
        for source in SOURCES:
            obj = str(work / (source.replace("/", "_") + ".o"))
            substitutions = {
                reference: obj, reference + ".d": obj + ".d",
                reference_source: source,
            }
            subprocess.run([substitutions.get(arg, arg) for arg in base],
                           cwd=ROOT, check=True)
            objects.append(obj)
        # Keep the configured linker flags but none of the emulator objects.
        exe = str(work / "e100_test")
        link = [exe if arg == "bin/gemu" else arg
                for arg in link if not arg.endswith(".o")]
        link[link.index(exe) + 1:link.index(exe) + 1] = objects
        subprocess.run(link, cwd=ROOT, check=True)
        subprocess.run([exe], cwd=work, check=True)


if __name__ == "__main__":
    main()
