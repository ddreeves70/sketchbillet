#!/usr/bin/env python3
"""Checks the reference assemblies in tests/reference/assemblies/: Phase 4's test.

Each assembly is a JSON file with the sketchbillet_cli commands that build its
parts and put them together, and what it must come to, worked out by hand:
where each component is and which way it faces, the mass in all, the parts that
overlap, and any warnings (a loop whose sides don't meet). This builds it, checks
all of that, makes its change (a joint turned, say), and checks again with the
changed numbers.

  check_assemblies.py --sketchbillet-cli <path> [--assemblies tests/reference/assemblies] [names...]

See tests/reference/README.md for the file format.
"""
import argparse
import importlib.util
import json
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

# The formulas use the same helpers as the reference parts (pi, sin, band, ...).
_spec = importlib.util.spec_from_file_location("check_parts", HERE / "check_parts.py")
_parts = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_parts)
worked_out = _parts.worked_out

# PLA, which every part is made of unless it says otherwise (g/mm^3).
SAFE_NUMBERS = {"rho": 1.24e-3}

COMPONENT = re.compile(r"^  (\S.*?)\s{2,}\S.*?\s+(grounded|joined|free)\s+at (\S+), (\S+), (\S+) mm$")
FACING = re.compile(r"^    facing x (\S+), (\S+), (\S+), z (\S+), (\S+), (\S+)$")
MASS = re.compile(r"^Mass in all: ([\d.]+) g")
WARNING = re.compile(r"^\s*warning \[([\w.]+)\]: (.*)$")
OVERLAP = re.compile(r"^\s*warning \[assembly\.overlap\]: (.+?) and (.+?) overlap by ([\d.]+) mm\^3")
APART = re.compile(r"its two sides are ([\d.]+) mm apart")

# sketchbillet_cli prints positions, masses, volumes and gaps with 2 decimals, and
# directions with 3 significant digits.
TOLERANCE = 0.0051


def load(path):
    assembly = json.loads(path.read_text(encoding="utf-8"))
    assembly["key"] = path.stem
    return assembly


def listing(text):
    """Each component's place and facing, the mass, and the warnings, from `open --components`."""
    components, warnings, mass, name = {}, [], None, None
    for line in text.splitlines():
        if m := COMPONENT.match(line):
            name = m.group(1)
            components[name] = {"at": [float(m.group(i)) for i in (3, 4, 5)]}
        elif (m := FACING.match(line)) and name:
            components[name]["x"] = [float(m.group(i)) for i in (1, 2, 3)]
            components[name]["z"] = [float(m.group(i)) for i in (4, 5, 6)]
        elif m := MASS.match(line):
            mass = float(m.group(1))
        elif m := WARNING.match(line):
            warnings.append((m.group(1), m.group(2)))
    return components, mass, warnings


def close(got, want):
    return abs(got - want) <= TOLERANCE + 1e-9 * abs(want)


def check(cli, assembly, expect, numbers, tmp, when, problems):
    file = assembly["assembly"]
    numbers = {**SAFE_NUMBERS, **numbers}

    def run(*argv):
        proc = subprocess.run([cli, *argv], cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return proc.returncode, proc.stdout.decode("utf-8", "replace")

    code, text = run("open", file, "--components")
    if code != 0:
        problems.append(f"{when}: open {file} failed:\n{text}")
        return
    components, mass, warnings = listing(text)

    for name, want in expect.get("components", {}).items():
        if name not in components:
            problems.append(f"{when}: no component {name}")
            continue
        for key in ("at", "x", "z"):
            if key not in want:
                continue
            target = [worked_out(f, numbers) for f in want[key]]
            got = components[name].get(key)
            if got is None or not all(close(g, t) for g, t in zip(got, target)):
                problems.append(f"{when}: {name} {key} is {got}, the formulas say "
                                f"{[round(t, 4) for t in target]} ({want[key]})")

    if "mass" in expect:
        target = worked_out(expect["mass"], numbers)
        if mass is None or not close(mass, target):
            problems.append(f"{when}: the mass in all is {mass} g, the formula says {target:.2f} g ({expect['mass']})")

    # Warnings: exactly the ones expected, each with its gap if it says one.
    want_warnings = expect.get("warnings", [])
    if sorted(c for c, _ in warnings) != sorted(w["code"] for w in want_warnings):
        problems.append(f"{when}: warnings are {warnings}, expected {[w['code'] for w in want_warnings]}")
    else:
        for w in want_warnings:
            if "apart" not in w:
                continue
            message = next(msg for c, msg in warnings if c == w["code"])
            m = APART.search(message)
            target = worked_out(w["apart"], numbers)
            if not m or not close(float(m.group(1)), target):
                problems.append(f"{when}: {w['code']} says '{message}', the formula says {target:.2f} mm apart "
                                f"({w['apart']})")

    # Overlaps: `check` lists each pair that overlaps, with how much.
    code, text = run("check", file)
    found = {}
    for line in text.splitlines():
        if m := OVERLAP.match(line):
            found[frozenset((m.group(1), m.group(2)))] = float(m.group(3))
    want_overlaps = {frozenset(o["between"]): o["volume"] for o in expect.get("overlaps", [])}
    if set(found) != set(want_overlaps):
        problems.append(f"{when}: overlapping pairs are {[sorted(p) for p in found]}, expected "
                        f"{[sorted(p) for p in want_overlaps]}")
    else:
        for pair, formula in want_overlaps.items():
            target = worked_out(formula, numbers)
            if not close(found[pair], target):
                problems.append(f"{when}: {' and '.join(sorted(pair))} overlap by {found[pair]} mm^3, the formula "
                                f"says {target:.2f} mm^3 ({formula})")
    if (code == 0) != (not want_overlaps):
        problems.append(f"{when}: check exited with {code}:\n{text}")


def check_assembly(cli, assembly):
    problems = []
    with tempfile.TemporaryDirectory(prefix="sb_reference_assembly_") as tmp:
        def run(command):
            proc = subprocess.run([cli, *shlex.split(command)], cwd=tmp, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
            out = proc.stdout.decode("utf-8", "replace")
            # Every command works, and every part builds: no step that failed, no error.
            if proc.returncode != 0 or "error [" in out or re.search(r"\s(failed|blocked)\s", out):
                problems.append(f"'{command}' failed:\n{out}")
                return False
            return True

        for command in assembly["commands"]:
            if not run(command):
                return problems
        check(cli, assembly, assembly["expect"], assembly["numbers"], tmp, "built", problems)
        change = assembly["change"]
        if not run(change["command"]):
            return problems
        check(cli, assembly, {**assembly["expect"], **change.get("expect", {})},
              {**assembly["numbers"], **change["numbers"]}, tmp, f"after {change['command']}", problems)
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sketchbillet-cli", required=True)
    parser.add_argument("--assemblies", default=str(REPO / "tests" / "reference" / "assemblies"))
    parser.add_argument("names", nargs="*", help="Just these assemblies (file names without .json).")
    args = parser.parse_args()
    cli = str(Path(args.sketchbillet_cli).resolve())  # the commands run in a folder of their own
    folder = Path(args.assemblies)
    paths = [folder / f"{n}.json" for n in args.names] if args.names else sorted(folder.glob("*.json"))
    failed = 0
    for path in paths:
        assembly = load(path)
        problems = check_assembly(cli, assembly)
        if problems:
            failed += 1
            print(f"FAIL {assembly['key']}: {assembly['title']}")
            for p in problems:
                print("  " + p.replace("\n", "\n    "))
        else:
            print(f"ok   {assembly['key']}: {assembly['title']}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
