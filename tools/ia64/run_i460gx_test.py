#!/usr/bin/env python3
"""Build/run the 460GX register model without the i2000 machine or CPU.

Configure GEMU with IA-64 enabled first. No ROM or disk image is needed.
"""
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def command_for(target):
    output = subprocess.check_output(
        ["ninja", "-t", "commands", target], cwd=ROOT, text=True
    )
    return shlex.split(output.strip().splitlines()[-1])


def main():
    # Regenerate configured source lists before looking up the new object.
    subprocess.run(["ninja", "build.ninja"], cwd=ROOT, check=True)
    link = command_for("bin/gemu")
    chipset_objects = [arg for arg in link
                       if arg.endswith(".o") and
                       ("/hardware/device/chipset/i460gx" in arg or
                        "/hardware/device/i2c/" in arg or
                        "/hardware/device/storage/fwh82802" in arg)]
    if not chipset_objects:
        raise SystemExit("Configure GEMU with IA-64 enabled first.")
    chipset = next(arg for arg in chipset_objects if arg.endswith("/i460gx.o"))
    subprocess.run(["ninja", *chipset_objects], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="i460gx-", dir=ROOT / "build") as tmp:
        work = Path(tmp)
        obj, exe = str(work / "i460gx_test.o"), str(work / "i460gx_test")
        substitutions = {
            chipset: obj, chipset + ".d": obj + ".d",
            "hardware/device/chipset/i460gx.c": "tests/i460gx.c",
        }
        compile_command = [substitutions.get(arg, arg) for arg in command_for(chipset)]
        # Keep configured linker flags but omit every emulator object except
        # the chipset's. This catches accidental CPU or machine dependencies.
        link = [arg for arg in link
                if not arg.endswith(".o") or arg in chipset_objects]
        link = [exe if arg == "bin/gemu" else arg for arg in link]
        link.insert(link.index(chipset), obj)
        subprocess.run(compile_command, cwd=ROOT, check=True)
        subprocess.run(link, cwd=ROOT, check=True)
        subprocess.run([exe], cwd=work, check=True)


if __name__ == "__main__":
    main()
