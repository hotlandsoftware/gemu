#!/usr/bin/env python3
"""Run PCI sound-controller and SDL stereo-output tests without ROMs.

--sanitize enables address/undefined sanitizers. SDL uses a dummy host sink.
"""
from pathlib import Path
import argparse
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-g"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    audio = "hardware/device/audio/"
    sdl = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    with tempfile.TemporaryDirectory(prefix="gemu-audio-") as tmp:
        for name, sources, libs in [
            ("pci_audio", [audio + n + ".c" for n in ["pci_audio", "cs4281", "ich_ac97"]], ["-lm"]),
            ("audio_out", [], sdl),
        ]:
            exe = str(Path(tmp) / name)
            subprocess.run(cc + flags + ["-Ihardware/device", "-Icore/include",
                "tests/" + name + ".c"] + sources + libs + ["-o", exe], cwd=ROOT, check=True)
            subprocess.run([exe], cwd=ROOT, check=True)

if __name__ == "__main__":
    main()
