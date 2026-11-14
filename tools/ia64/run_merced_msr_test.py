#!/usr/bin/env python3
"""Build and run the Merced MSR model test using the configured GEMU toolchain.

Run ./configure with IA-64 enabled first.  No ROM or ISO is needed.
"""

from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def command_for(target):
    output = subprocess.check_output(
        ["ninja", "-t", "commands", target], cwd=ROOT, text=True
    )
    return shlex.split(output.strip().splitlines()[-1])


def main():
    subprocess.run(["ninja", "bin/gemu"], cwd=ROOT, check=True)
    link = command_for("bin/gemu")
    if not any(arg.endswith("/machine_i2000.o") for arg in link):
        raise SystemExit("Configure GEMU with IA-64 enabled before running this test.")
    main_object = next(arg for arg in link if arg.endswith("/src/main.o"))
    compile_command = command_for(main_object)

    with tempfile.TemporaryDirectory(prefix="merced-msr-", dir=ROOT / "build") as tmp:
        work = Path(tmp)
        test_object = str(work / "merced_msr.o")
        executable = str(work / "merced_msr")
        substitutions = {
            main_object: test_object,
            main_object + ".d": test_object + ".d",
            "src/main.c": "tests/merced_msr.c",
            "bin/gemu": executable,
        }
        compile_command = [substitutions.get(arg, arg) for arg in compile_command]
        link = [substitutions.get(arg, arg) for arg in link]
        subprocess.run(compile_command, cwd=ROOT, check=True)
        subprocess.run(link, cwd=ROOT, check=True)
        subprocess.run([executable, *sys.argv[1:]], cwd=work, check=True)


if __name__ == "__main__":
    main()
