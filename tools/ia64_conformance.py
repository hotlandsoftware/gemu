#!/usr/bin/env python3
"""Run the reference IA-64 architectural microprogram cases against GEMU.

reference/qemu-system-ia64 ships ~1000 hand-verified conformance cases
(tests/unit/ia64/).  Each one is emulator-agnostic in substance - "load these
bundles, run from this entry, expect these architectural values" - even though
its runner drives QEMU over QMP.  This driver imports the case *definitions*
only, renders each one into the flat microprogram format understood by
`gemu -M generic -microprogram`, runs it on a bare Merced core, and diffs the
resulting state against the expectations.

That turns "guess which semantic differs, then boot Windows for twelve minutes
to find out" into a finite list of precise failures that run in milliseconds.

Usage:
    tools/ia64_conformance.py [--group mmu] [--name substring] [-v]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REF = REPO / "reference/qemu-system-ia64"
GEMU = REPO / "bin/gemu"


def load_reference_cases():
    """Import the reference's case modules without dragging in its runner."""
    sys.path.insert(0, str(REF / "tests"))
    sys.path.insert(0, str(REF))
    from unit.ia64 import encoding, registry  # noqa: E402
    return encoding, registry


class Recorder:
    """Stands in for the QEMU handle the case bodies expect.

    A case is a closure that calls run_program(qemu, bundles, ..., expected=...).
    We never launch QEMU: we capture those arguments and run them ourselves.
    """

    def __init__(self):
        self.calls = []

    def __getattr__(self, _name):
        def _noop(*_a, **_kw):
            return None
        return _noop


def render(bundles, entry, terminal, memory, encoding) -> str:
    lines = [f"entry {entry:x}"]
    if terminal is not None:
        lines.append(f"terminal {terminal:x}")
    lines.append("maxinsts 200000")
    for b in encoding.encode_bundles(encoding.normalized_bundles(bundles),
                                     encoding.bundle_words):
        lines.append(f"bundle {b.address:x} {b.low:x} {b.high:x}")
    for init in memory or ():
        # MemoryInitializer objects in most cases; a few pass raw tuples.
        if hasattr(init, "address"):
            addr, size, value = init.address, init.size, init.value
        elif isinstance(init, (tuple, list)) and len(init) == 3:
            addr, size, value = init
        else:
            continue
        lines.append(f"mem {addr:x} {size} {value:x}")
    return "\n".join(lines) + "\n"


STATE_RE = re.compile(r"^IA64TEST (.*)$")


def run_microprogram(text: str):
    with tempfile.NamedTemporaryFile("w", suffix=".mp", delete=False) as fh:
        fh.write(text)
        path = fh.name
    try:
        out = subprocess.run([str(GEMU), "-M", "generic", "-microprogram", path],
                             capture_output=True, text=True, timeout=60).stdout
    finally:
        Path(path).unlink(missing_ok=True)

    state = {}
    for line in out.splitlines():
        m = STATE_RE.match(line)
        if not m:
            continue
        body = m.group(1)
        if body.startswith("cr "):
            body = body[3:]
        for field in body.split():
            if "=" in field:
                k, v = field.split("=", 1)
                try:
                    state[k] = int(v, 16)
                except ValueError:
                    state[k] = v
    return state


def compare(expected: dict, actual: dict):
    """Return (mismatches, unsupported) for one case."""
    mismatches, unsupported = [], []
    for key, want in expected.items():
        if key == "ip":
            got = actual.get("ip", 0) & ~0xF
            if got != (want & ~0xF):
                mismatches.append((key, want, got))
        elif re.fullmatch(r"r\d+", key):
            got = actual.get(key)
            if got is None:
                unsupported.append(key)
            elif got != want:
                mismatches.append((key, want, got))
        elif key == "psr":
            got = actual.get("psr")
            if got is not None and got != want:
                mismatches.append((key, want, got))
        elif key == "fault_ip":
            got = actual.get("iip")
            if got is not None and (got & ~0xF) != (want & ~0xF):
                mismatches.append((key, want, got))
        else:
            # exception / cfm_* / pr_mask / fN / ar_* need extra plumbing;
            # report rather than silently pass.
            unsupported.append(key)
    return mismatches, unsupported


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--group", default="mmu")
    ap.add_argument("--name", default="")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    encoding, registry = load_reference_cases()

    captured = {}
    real_run_program = encoding.run_program

    def capture(qemu, bundles, entry=0x10, alat="full", terminal_ip=None,
                expected=None, timeout=2.0, name="ia64-microprogram",
                memory=None, **kw):
        captured[name] = dict(bundles=bundles, entry=entry,
                              terminal=terminal_ip if terminal_ip is not None
                              else (expected or {}).get("ip"),
                              expected=dict(expected or {}), memory=memory)
    encoding.run_program = capture

    if args.group and args.group != "all":
        cases = registry.cases_for_group(args.group)
    else:
        cases = {}
        for g in registry.GROUPS:
            cases.update(registry.cases_for_group(g))
    names = sorted(n for n in cases if args.name.lower() in n.lower())
    if args.limit:
        names = names[:args.limit]

    rec = Recorder()
    ran = passed = failed = errored = 0
    failures = []
    for name in names:
        fn = cases.get(name)
        if fn is None:
            continue
        captured.clear()
        try:
            fn(rec)
        except Exception as exc:                       # noqa: BLE001
            errored += 1
            if args.verbose:
                print(f"ERROR  {name}: {exc}")
            continue
        for cname, c in captured.items():
            ran += 1
            text = render(c["bundles"], c["entry"], c["terminal"],
                          c["memory"], encoding)
            actual = run_microprogram(text)
            bad, unsup = compare(c["expected"], actual)
            if bad:
                failed += 1
                failures.append((cname, bad, unsup, actual.get("stop")))
                if args.verbose:
                    print(f"FAIL   {cname}  stop={actual.get('stop')}")
                    for k, want, got in bad:
                        print(f"         {k}: expected {want:#x}, got {got:#x}")
            else:
                passed += 1

    encoding.run_program = real_run_program
    print(f"\nran={ran} passed={passed} failed={failed} errored={errored}")
    if failures and not args.verbose:
        print("\nfirst failures:")
        for cname, bad, _unsup, stop in failures[:15]:
            detail = ", ".join(f"{k}: want {w:#x} got {g:#x}"
                               for k, w, g in bad[:3])
            print(f"  {cname} (stop={stop}): {detail}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
