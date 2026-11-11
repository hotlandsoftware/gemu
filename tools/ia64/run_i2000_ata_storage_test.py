#!/usr/bin/env python3
"""Build/run ROM-free ATA disk read/write boundary regressions.

Configure IA-64 first. --no-build uses existing non-machine objects.
"""
from pathlib import Path
import argparse
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def command_for(target):
    output = subprocess.check_output(["ninja", "-t", "commands", target], cwd=ROOT, text=True)
    return shlex.split(output.strip().splitlines()[-1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    if not args.no_build:
        subprocess.run(["ninja", "bin/gemu"], cwd=ROOT, check=True)
    link = command_for("bin/gemu")
    machine = next((arg for arg in link if arg.endswith("/machine_i2000.o")), None)
    if machine is None:
        raise SystemExit("Configure GEMU with IA-64 enabled first.")
    main_object = next(arg for arg in link if arg.endswith("/src/main.o"))
    with tempfile.TemporaryDirectory(prefix="i2000-ata-storage-", dir=ROOT / "build") as tmp:
        work = Path(tmp)
        obj = str(work / "i2000_ata_storage.o")
        exe = str(work / "i2000_ata_storage")
        substitutions = {machine: obj, machine + ".d": obj + ".d",
                         "hardware/machine/ia64/machine_i2000.c": "tests/i2000_ata_storage.c",
                         "bin/gemu": exe}
        compile_command = [substitutions.get(arg, arg) for arg in command_for(machine)]
        link = [substitutions.get(arg, arg) for arg in link if arg != main_object]
        subprocess.run(compile_command, cwd=ROOT, check=True)
        subprocess.run(link, cwd=ROOT, check=True)
        (work / "snapshots").touch()
        subprocess.run([exe], cwd=work, check=True)


if __name__ == "__main__":
    main()
