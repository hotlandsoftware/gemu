#!/usr/bin/env python3
"""Build and run the Merced JIT differential test using the configured GEMU toolchain.

Builds and runs the `merced_jit` unit test (see TESTS in configure).  Same as
`ninja check-merced_jit`, but prints the test's own output.  `ninja check` runs
every configured test.

    python3 tools/ia64/run_merced_jit_test.py [--no-build] [ARGS...]
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_tests import run_named  # noqa: E402

if __name__ == "__main__":
    args = sys.argv[1:]
    build = "--no-build" not in args
    args = [a for a in args if a != "--no-build"]
    sys.exit(run_named("merced_jit", args, build))
