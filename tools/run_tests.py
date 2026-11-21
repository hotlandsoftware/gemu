#!/usr/bin/env python3
"""Run GEMU's unit-test executables (built by `ninja tests`).

    python3 tools/run_tests.py [--work DIR] [--timeout S] [-j N] [-v] EXE... [-- ARGS...]

`ninja check` and `ninja check-NAME` call this; the test list lives in
`configure` (TESTS).  Each test runs in its own new directory under --work
(tests create scratch images there), headless, with a timeout.  One line is
printed per test; a failing test's output follows its line.  Arguments after
`--` go to every test (useful with a single test, e.g. merced_jit's
[programs] [seed]).  Exits non-zero if any test fails.
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def headless_env():
    env = dict(os.environ)
    for var in ("DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(var, None)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    return env


def run_one(exe, work_root, timeout, extra_args, env):
    name = Path(exe).stem
    work = Path(tempfile.mkdtemp(prefix=name + "-", dir=work_root))
    # Machines that autosave write into ./snapshots; a plain file in its place
    # makes that fail harmlessly instead of leaving state behind.
    (work / "snapshots").touch()
    start = time.monotonic()
    try:
        proc = subprocess.run([str(Path(exe).resolve()), *extra_args], cwd=work,
                              env=env, stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout)
        ok, output = proc.returncode == 0, proc.stdout
        status = "ok" if ok else f"FAILED (exit {proc.returncode})"
    except subprocess.TimeoutExpired as e:
        ok, output = False, e.stdout or b""
        status = f"FAILED (timed out after {timeout} s)"
    elapsed = time.monotonic() - start
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    else:
        status += f", files kept in {work}"
    return name, ok, status, elapsed, output.decode(errors="replace")


def run_named(name, extra_args=(), build=True):
    """Build test NAME with ninja (unless build is False) and run it verbosely.
    For the per-test wrapper scripts in tools/<family>/."""
    if build:
        subprocess.run(["ninja", f"test-{name}"], cwd=ROOT, check=True)
    query = subprocess.run(["ninja", "-t", "query", f"test-{name}"], cwd=ROOT,
                           text=True, capture_output=True)
    lines = query.stdout.splitlines()
    if query.returncode != 0 or "  input: phony" not in lines:
        raise SystemExit(f"test {name} is not configured; check ./configure "
                         f"--target-list (see TESTS in configure)")
    exe = lines[lines.index("  input: phony") + 1].strip()
    work_root = ROOT / "build" / "tests-run"
    work_root.mkdir(parents=True, exist_ok=True)
    _, ok, status, elapsed, output = run_one(ROOT / exe, work_root, None,
                                             list(extra_args), headless_env())
    sys.stdout.write(output)
    print(f"{name}: {status} ({elapsed:.1f} s)")
    return 0 if ok else 1


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--work", default=str(ROOT / "build" / "tests-run"),
                   help="directory for per-test working directories")
    p.add_argument("--timeout", type=float, default=600, help="seconds per test")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    p.add_argument("-v", "--verbose", action="store_true",
                   help="print every test's output, not only failures'")
    p.add_argument("exes", nargs="*")
    args = p.parse_args(argv)

    if not args.exes:
        print("check: no unit tests are configured for these families")
        return 0
    work_root = Path(args.work)
    work_root.mkdir(parents=True, exist_ok=True)
    env = headless_env()
    width = max(len(Path(e).stem) for e in args.exes)
    failed = []
    start = time.monotonic()
    jobs = max(1, min(args.jobs, len(args.exes)))
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        futures = [pool.submit(run_one, exe, work_root, args.timeout, extra, env)
                   for exe in args.exes]
        for fut in concurrent.futures.as_completed(futures):
            name, ok, status, elapsed, output = fut.result()
            print(f"  {name:<{width}}  {elapsed:6.1f} s  {status}", flush=True)
            if output and (args.verbose or not ok):
                tail = output.rstrip("\n").splitlines()
                if not args.verbose and len(tail) > 60:
                    tail = ["    ..."] + tail[-60:]
                print("\n".join("    | " + l for l in tail), flush=True)
            if not ok:
                failed.append(name)
    total = time.monotonic() - start
    n = len(args.exes)
    if failed:
        print(f"check: {len(failed)} of {n} failed ({', '.join(sorted(failed))}) "
              f"in {total:.1f} s")
        return 1
    print(f"check: all {n} passed in {total:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
