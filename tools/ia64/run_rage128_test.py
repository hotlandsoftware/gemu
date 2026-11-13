#!/usr/bin/env python3
"""Build/run the Rage 128 and VGA core models without a machine or CPU.

Configure GEMU with IA-64 enabled first. No ROM or disk image is needed.
"""
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MODEL_OBJECTS = ("rage128.o", "rage128_gui.o", "rage128_regs.o", "vga.o",
                 "vgafont16.o")


def command_for(target):
    output = subprocess.check_output(
        ["ninja", "-t", "commands", target], cwd=ROOT, text=True
    )
    return shlex.split(output.strip().splitlines()[-1])


def main():
    # Regenerate configured source lists before looking up the new objects.
    subprocess.run(["ninja", "build.ninja"], cwd=ROOT, check=True)
    link = command_for("bin/gemu")
    model_objects = [arg for arg in link
                     if arg.endswith(".o") and "/vga/multi/" in arg and
                     arg.rsplit("/", 1)[1] in MODEL_OBJECTS]
    if len({arg.rsplit("/", 1)[1] for arg in model_objects}) != len(MODEL_OBJECTS):
        raise SystemExit("Configure GEMU with IA-64 enabled first.")
    subprocess.run(["ninja", *model_objects], cwd=ROOT, check=True)
    rage = next(arg for arg in model_objects if arg.endswith("/rage128.o"))
    with tempfile.TemporaryDirectory(prefix="rage128-", dir=ROOT / "build") as tmp:
        work = Path(tmp)
        obj, exe = str(work / "rage128_test.o"), str(work / "rage128_test")
        substitutions = {
            rage: obj, rage + ".d": obj + ".d",
            "vga/multi/rage128.c": "tests/rage128.c",
        }
        compile_command = [substitutions.get(arg, arg) for arg in command_for(rage)]
        # Keep configured linker flags but omit every emulator object except
        # the models'. This catches accidental CPU or machine dependencies.
        link = [arg for arg in link
                if not arg.endswith(".o") or arg in model_objects]
        link = [exe if arg == "bin/gemu" else arg for arg in link]
        link.insert(link.index(rage), obj)
        subprocess.run(compile_command, cwd=ROOT, check=True)
        subprocess.run(link, cwd=ROOT, check=True)
        subprocess.run([exe], cwd=work, check=True)


if __name__ == "__main__":
    main()
