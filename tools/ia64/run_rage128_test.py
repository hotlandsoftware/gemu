#!/usr/bin/env python3
"""Build/run the Rage 128 and VGA core models without a machine or CPU.

Builds and runs the `rage128` unit test (see TESTS in configure).  Same as
`ninja check-rage128`, but prints the test's own output.  `ninja check` runs
every configured test.

    python3 tools/ia64/run_rage128_test.py [--no-build]
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_tests import run_named  # noqa: E402

if __name__ == "__main__":
    args = sys.argv[1:]
    build = "--no-build" not in args
    args = [a for a in args if a != "--no-build"]
    if args:
        raise SystemExit("usage: run_rage128_test.py [--no-build]")
    sys.exit(run_named("rage128", (), build))
