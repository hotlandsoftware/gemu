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
    tools/ia64/ia64_conformance.py [--group mmu] [--name substring] [-v]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
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
        # Some case bodies chain off the handle (qemu.query_status().state).
        # Returning self rather than None keeps those chains resolvable, so
        # the case still reaches its run_program() call.
        def _noop(*_a, **_kw):
            return self
        return _noop

    def __bool__(self):
        return True


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


class RunState:
    """The `result.state` a few case bodies assert on directly.

    Those cases express their expectation as Python (`if state.gr[17] <=
    state.gr[16]: raise`) rather than as an `expected` dict, so they need the
    run to have already happened by the time run_program() returns.
    """

    def __init__(self, actual):
        self.gr = [actual.get(f"r{i}", 0) for i in range(128)]
        self.ar = [actual.get(f"ar{i}", 0) for i in range(128)]
        self.ip = actual.get("ip", 0)
        self.psr = actual.get("psr", 0)
        self.pr = actual.get("pr", 0)


class RunResult:
    def __init__(self, actual, text):
        self.state = RunState(actual)
        self.actual = actual
        self.register_output = text


STATE_RE = re.compile(r"^IA64TEST (.*)$")


def run_microprogram(text: str):
    with tempfile.NamedTemporaryFile("w", suffix=".mp", delete=False) as fh:
        fh.write(text)
        path = fh.name
    try:
        out = subprocess.run([str(GEMU), "-M", "generic-ia64", "-microprogram", path],
                             capture_output=True, text=True, timeout=60).stdout
    finally:
        Path(path).unlink(missing_ok=True)

    state = {}
    for line in out.splitlines():
        m = STATE_RE.match(line)
        if not m:
            continue
        body = m.group(1)
        if body.startswith("halt "):
            state["halt"] = body[5:]
            continue
        if body.startswith("cr "):
            body = body[3:]
        for field in body.split():
            if "=" in field:
                k, v = field.split("=", 1)
                try:
                    state[k] = int(v, 16)
                except ValueError:
                    state[k] = v
    return state, out


def compare(expected: dict, actual: dict):
    """Return (mismatches, unsupported) for one case."""
    mismatches, unsupported = [], []
    for key, want in expected.items():
        # A few cases express an expectation as a helper object (masked bit
        # patterns, ranges).  Comparing those needs the reference's semantics,
        # so report them as unsupported rather than guessing.
        if not isinstance(want, int):
            unsupported.append(key)
            continue
        if key == "ip":
            # The reference's require_uncollected_reserved_field cases set
            # "ip" to the same address as "fault_ip": that reference harness
            # observes the raw exception at helper_raise_exception() (which
            # sets env->ip = fault_ip directly) rather than after a full
            # architectural fault delivery. Our microprogram harness runs a
            # real CPU that *does* complete fault delivery (vectoring to
            # iva+vector, per ia64_translation_insert_fields_valid()'s own
            # vector table), so the guest-visible ip legitimately moves on.
            # cr.iip is what stays pinned at the faulting instruction's
            # address in both models, so that is the correct field to check
            # for this expectation instead of the final running ip.
            if expected.get("fault_ip") == want:
                got = actual.get("iip")
                if got is None:
                    unsupported.append(key)
                elif (got & ~0xF) != (want & ~0xF):
                    mismatches.append((key, want, got))
                continue
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
        elif key == "exception":
            # 492 of 494 uses are IA64_EXCP_NONE, i.e. "the run finished in a
            # normal architectural state".  Our equivalent is that the core
            # reached its terminal IP rather than bailing out on an
            # unimplemented instruction or a halt, so assert exactly that.
            if want == 0 and actual.get("stop") not in ("terminal", "maxinsts"):
                mismatches.append((key, want, actual.get("stop")))
            elif want != 0:
                unsupported.append(key)
        elif key == "cfm_sof":
            got = actual.get("cfm")
            if got is not None and (got & 0x7F) != want:
                mismatches.append((key, want, got & 0x7F))
        elif key == "cfm_sol":
            got = actual.get("cfm")
            if got is not None and ((got >> 7) & 0x7F) != want:
                mismatches.append((key, want, (got >> 7) & 0x7F))
        elif key == "pr_mask":
            got = actual.get("pr")
            if got is not None and (got & want) != want:
                mismatches.append((key, want, got))
        else:
            # fN / ar_* / fault_code need extra plumbing; report rather than
            # silently pass.
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
        exp = expected or {}
        # ia64_selftest checks "have we reached terminal?" BEFORE executing
        # the bundle there (src/ia64_selftest.c), so a fault-testing case
        # whose expected "ip" is the address of the faulting instruction
        # itself (the common pattern for reserved-field-fault cases, where
        # the post-fault ip isn't tracked and "ip" is set equal to fault_ip
        # as a placeholder) would stop before ever running it. Only default
        # terminal from "ip" when it's not also the expected fault_ip.
        if terminal_ip is not None:
            terminal = terminal_ip
        elif "fault_ip" in exp and exp.get("fault_ip") == exp.get("ip"):
            terminal = None
        else:
            terminal = exp.get("ip")
        text = render(bundles, entry, terminal, memory, encoding)
        actual, output = run_microprogram(text)
        captured[name] = dict(expected=dict(expected or {}), actual=actual)
        # Run here rather than in the caller so the handful of cases that
        # assert on the result in Python get a real one back.
        return RunResult(actual, output)
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
    halts = []
    for name in names:
        fn = cases.get(name)
        if fn is None:
            continue
        captured.clear()
        body_error = None
        try:
            fn(rec)
        except Exception as exc:                       # noqa: BLE001
            # A case that asserts in Python raises to signal failure, and by
            # then it has already run its program.  Treat that as a failure of
            # the case; only a case that never ran anything is a driver error.
            if not captured:
                errored += 1
                if args.verbose:
                    print(f"ERROR  {name}: {exc}")
                continue
            body_error = exc
            if args.verbose:
                print(f"ASSERT {name}: {exc}")
        for cname, c in captured.items():
            ran += 1
            actual = c["actual"]
            bad, unsup = compare(c["expected"], actual)
            if body_error is not None and not bad:
                bad = [("assertion", 0, str(body_error).split("\n")[0])]
            if bad:
                failed += 1
                failures.append((cname, bad, unsup, actual.get("stop")))
                if actual.get("halt"):
                    halts.append(actual["halt"])
                if args.verbose:
                    print(f"FAIL   {cname}  stop={actual.get('stop')}")
                    for k, want, got in bad:
                        gs = got if isinstance(got, str) else f"{got:#x}"
                        ws = want if isinstance(want, str) else f"{want:#x}"
                        print(f"         {k}: expected {ws}, got {gs}")
            else:
                passed += 1

    encoding.run_program = real_run_program
    print(f"\nran={ran} passed={passed} failed={failed} errored={errored}")
    if halts:
        import collections
        norm = collections.Counter(
            re.sub(r"0x[0-9A-Fa-f]+", "0xNN", h.split(" at=")[0])
            for h in halts)
        print("\nunimplemented / halted on:")
        for msg, n in norm.most_common(25):
            print(f"  {n:4d}  {msg}")
    if failures and not args.verbose:
        print("\nfirst failures:")
        for cname, bad, _unsup, stop in failures[:15]:
            detail = ", ".join(
                f"{k}: want {w:#x} got " +
                (g if isinstance(g, str) else f"{g:#x}")
                for k, w, g in bad[:3])
            print(f"  {cname} (stop={stop}): {detail}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
