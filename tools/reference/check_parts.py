#!/usr/bin/env python3
"""Checks the reference parts in tests/reference/: Phase 2's exit test.

Each part is a JSON file with the commands that build it, a formula for each
volume worked out by hand, and a change to an early sketch dimension. This
builds the part with sketchbillet_cli, checks that every step built and that the
volumes match the formulas, makes the change, and checks again: the formulas
are worked out with the changed numbers, so the change must have flowed through
every step, not just failed to break them.

  check_parts.py --sketchbillet-cli <path> [--parts tests/reference] [names...]
  check_parts.py --gallery docs/user-guide/reference-parts.md [--write]

With --gallery it checks (or, with --write, rewrites) the gallery page made
from the same files. See tests/reference/README.md for the file format.
"""
import argparse
import json
import math
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

# What a formula may use, besides the part's own numbers.
def seg(r, h):
    """The area of a circle's segment h deep (mm^2): the part a straight cut takes off."""
    return r * r * math.acos((r - h) / r) - (r - h) * math.sqrt(2 * r * h - h * h)


def lens(r1, r2, d):
    """The area where two circles d apart overlap (mm^2)."""
    if d >= r1 + r2:
        return 0.0
    if d <= abs(r1 - r2):
        return math.pi * min(r1, r2) ** 2
    a1 = math.acos((d * d + r1 * r1 - r2 * r2) / (2 * d * r1))
    a2 = math.acos((d * d + r2 * r2 - r1 * r1) / (2 * d * r2))
    return r1 * r1 * (a1 - math.sin(2 * a1) / 2) + r2 * r2 * (a2 - math.sin(2 * a2) / 2)


def band(r, a):
    """The area of a circle between y = -a and y = a (mm^2)."""
    return 2 * (a * math.sqrt(r * r - a * a) + r * r * math.asin(a / r))


def rounding(r):
    """The area a rounding of radius r takes off a square corner (mm^2), or adds to an inside one."""
    return (1 - math.pi / 4) * r * r


def bevel(c):
    """The area a 45-degree bevel c wide takes off a square corner (mm^2)."""
    return c * c / 2


SAFE = {"pi": math.pi, "sqrt": math.sqrt, "sin": math.sin, "cos": math.cos, "tan": math.tan,
        "asin": math.asin, "acos": math.acos, "atan": math.atan, "radians": math.radians,
        "seg": seg, "lens": lens, "band": band, "rounding": rounding, "bevel": bevel}

STATUS = re.compile(r"^\s{2,4}(\S+)\s+(ok|failed|blocked|off|rolled back)\b(?:\s+volume ([\d.]+) mm\^3)?")


def worked_out(formula, numbers):
    return eval(formula, {"__builtins__": {}}, {**SAFE, **numbers})  # noqa: S307 (our own files)


def summary(text):
    """Each step's status and volume, from sketchbillet_cli's summary."""
    steps = {}
    for line in text.splitlines():
        m = STATUS.match(line)
        if m:
            steps[m.group(1)] = (m.group(2), float(m.group(3)) if m.group(3) else None)
    return steps


def load(path):
    part = json.loads(path.read_text(encoding="utf-8"))
    part["key"] = path.stem
    return part


def check_volumes(part, text, numbers, when, problems):
    # A clean part has no warnings either: a hole that missed, a formula that didn't work.
    for line in text.splitlines():
        if "warning [" in line or "error [" in line:
            problems.append(f"{when}: {line.strip()}")
    steps = summary(text)
    for step, (status, _) in steps.items():
        if status != "ok":
            problems.append(f"{when}: {step} is {status}")
    for step, formula in part["expect"].items():
        if step not in steps or steps[step][1] is None:
            problems.append(f"{when}: no volume for {step}")
            continue
        want = worked_out(formula, numbers)
        got = steps[step][1]
        # sketchbillet_cli prints 2 decimals.
        if abs(got - want) > 0.0051 + 1e-9 * abs(want):
            problems.append(f"{when}: {step} is {got:.2f} mm^3, the formula says {want:.2f} mm^3 ({formula})")


def check_part(cli, part, show=None):
    problems = []
    with tempfile.TemporaryDirectory(prefix="sb_reference_") as tmp:
        def run(command, first=False):
            argv = [cli, "run"] + ([] if first else ["--open", "part.sb"]) + ["--save", "part.sb"]
            argv += shlex.split(command)
            proc = subprocess.run(argv, cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            out = proc.stdout.decode("utf-8", "replace")
            if proc.returncode != 0:
                problems.append(f"'{command}' failed:\n{out}")
            return out

        for i, command in enumerate(part["steps"]):
            run(command, first=(i == 0))
            if problems:
                return problems
        if show is not None:  # while writing a part: its summary, and a step's faces and edges
            print(run_open(cli, tmp))
            for kind in ("--faces", "--edges"):
                if show:
                    proc = subprocess.run([cli, "open", "part.sb", kind, show], cwd=tmp, stdout=subprocess.PIPE)
                    lines = proc.stdout.decode().splitlines()
                    print("\n".join(l for l in lines if re.match(r"^\s+\d+\s", l) or l.startswith(("faces", "edges"))))
            return problems
        check_volumes(part, run_open(cli, tmp), part["numbers"], "built", problems)
        change = part["change"]
        run(change["command"])
        if problems:
            return problems
        check_volumes(part, run_open(cli, tmp), {**part["numbers"], **change["numbers"]},
                      f"after {change['command']}", problems)
    return problems


def run_open(cli, tmp):
    proc = subprocess.run([cli, "open", "part.sb"], cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.stdout.decode("utf-8", "replace")


GROUPS = ["Brackets", "Enclosures", "Knobs", "Mounts"]


def gallery(parts):
    """The gallery page, made from the parts' files."""
    out = ["<!-- Made by tools/reference/check_parts.py from tests/reference/. Don't edit: change the",
           "     parts' files, then run: python3 tools/reference/check_parts.py --gallery "
           "docs/user-guide/reference-parts.md --write -->",
           "",
           "# Reference parts",
           "",
           "Twenty real parts, modeled with SketchBillet: brackets, enclosures, knobs and mounts. Each",
           "one is built from sketches with dimensions, so you can change a dimension and the",
           "whole part follows. They're also SketchBillet's own test: every change to",
           "SketchBillet builds all twenty, checks every volume against a hand calculation,",
           "changes the dimension listed under each, and checks them all again.",
           "",
           "To look inside one, open it in the desktop app: **File > Open Sample**, then",
           "its group and its name. It opens as a new design, so try the change listed under",
           "it in Properties, or make it your own and save it wherever you like.",
           "",
           "To build one with `sketchbillet_cli`, run its commands one after the other, in a",
           "folder of your own. The first one makes `part.sb`; the others open it, change it",
           "and save it.",
           ""]
    for group in GROUPS:
        members = [p for p in parts if p["group"] == group]
        if not members:
            continue
        out += [f"## {group}", ""]
        for p in members:
            out += [f"### {p['title']}", "",
                    f"![{p['picture_alt']}](images/ref-{p['key']}.png)", "",
                    p["about"], "",
                    f"**Uses:** {', '.join(p['uses'])}. **Try:** {p['change']['about']}", "",
                    "```console"]
            for i, command in enumerate(p["steps"]):
                flags = "--save part.sb" if i == 0 else "--open part.sb --save part.sb"
                out.append(f"$ sketchbillet_cli run {flags} {command}")
            out += ["```", "", "Then, to make the change:", "", "```console",
                    f"$ sketchbillet_cli run --open part.sb --save part.sb {p['change']['command']}",
                    "```", ""]
    return "\n".join(out).rstrip() + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sketchbillet-cli")
    ap.add_argument("--parts", default=str(REPO / "tests/reference"))
    ap.add_argument("--gallery", help="the gallery page: check it's up to date (or --write it)")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--list", metavar="STEP", nargs="?", const="",
                    help="while writing a part: build it, and print its summary and STEP's faces and edges")
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()
    paths = sorted(Path(args.parts).glob("*.json"))
    parts = [load(p) for p in paths]
    parts.sort(key=lambda p: (GROUPS.index(p["group"]), p.get("order", 0), p["key"]))

    if args.gallery:
        page = Path(args.gallery)
        text = gallery(parts)
        if args.write:
            page.write_text(text, encoding="utf-8")
            print(f"wrote {page}")
            return 0
        if not page.exists() or page.read_text(encoding="utf-8") != text:
            print(f"{page} isn't up to date with tests/reference/. Run:\n"
                  f"  python3 tools/reference/check_parts.py --gallery {args.gallery} --write")
            return 1
        print(f"{page} is up to date ({len(parts)} parts)")
        return 0

    if not args.sketchbillet_cli:
        ap.error("--sketchbillet-cli is needed to build the parts")
    cli = str(Path(args.sketchbillet_cli).resolve())
    failed = 0
    for part in parts:
        if args.names and part["key"] not in args.names:
            continue
        problems = check_part(cli, part, args.list)
        if args.list is not None:
            print("\n".join(problems))
            continue
        if problems:
            failed += 1
            print(f"FAILED {part['key']}:")
            for p in problems:
                print("  " + p.replace("\n", "\n  "))
        else:
            print(f"ok {part['key']}: {', '.join(part['expect'])}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
