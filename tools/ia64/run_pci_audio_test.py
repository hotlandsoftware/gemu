#!/usr/bin/env python3
"""Run PCI sound-controller and SDL stereo-output tests without ROMs.

Builds and runs the `pci_audio` and `audio_out` unit tests (see TESTS in
configure).  Same as `ninja check-pci_audio check-audio_out`, but prints the
tests' own output.  `ninja check` runs every configured test.

    python3 tools/ia64/run_pci_audio_test.py [--no-build] [--sanitize]

--sanitize instead compiles both tests directly with address/undefined
sanitizers (outside ninja, without the configured flags). SDL uses a dummy
host sink.
"""
from pathlib import Path
import argparse
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from run_tests import headless_env, run_named  # noqa: E402


def run_sanitized():
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    audio = "hardware/device/audio/"
    sdl = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    with tempfile.TemporaryDirectory(prefix="gemu-audio-", dir=ROOT / "build") as tmp:
        for name, sources, libs in [
            ("pci_audio", [audio + n + ".c" for n in ["pci_audio", "cs4281", "ich_ac97"]], ["-lm"]),
            ("audio_out", [], sdl),
        ]:
            exe = str(Path(tmp) / name)
            subprocess.run(cc + flags + ["-Ihardware/device", "-Icore/include",
                "tests/" + name + ".c"] + sources + libs + ["-o", exe], cwd=ROOT, check=True)
            subprocess.run([exe], cwd=tmp, env=headless_env(), check=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    if args.sanitize:
        return run_sanitized()
    rc = 0
    for name in ("pci_audio", "audio_out"):
        rc |= run_named(name, (), not args.no_build)
    return rc


if __name__ == "__main__":
    sys.exit(main())
