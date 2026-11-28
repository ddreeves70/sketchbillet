#!/usr/bin/env python3
"""Tests check_assemblies.py's reading of sketchbillet_cli's output, without building anything."""
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_assemblies", HERE / "check_assemblies.py")
ca = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ca)

LISTING = """Case: assembly, 2 components, 2 parts
  Tray 1       Tray       grounded at 0, 0, 0 mm
    facing x 1, 0, 0, z 0, 0, 1
  Cover 1      Cover      joined   at 0, 46.95, 49.39 mm
    facing x 1, 0, 0, z 0, 0.94, -0.342
Joints:
  Joint1       revolute  Cover 1 on Tray 1, at -100°, from -110° to 0°
    warning [joint.loop]: Joint5 makes a loop (Base 1, Arm 1, Link 1, Base 1), and its two sides are 3.20 mm apart. Turn Joint5 off.
Parts:
  Tray         1 step   ok        mass 19.84 g (PLA), 1 in the assembly
Mass in all: 27.78 g (weighs 0.272 N)
"""

CHECK = ("warning [assembly.overlap]: Base 1 and Lid 1 overlap by 532.95 mm^3, around x 30, y 17.05, z 16.31 mm. "
         "Parts can't be in the same place.")


def main():
    components, mass, warnings = ca.listing(LISTING)
    assert components["Tray 1"] == {"at": [0, 0, 0], "x": [1, 0, 0], "z": [0, 0, 1]}, components
    assert components["Cover 1"]["at"] == [0, 46.95, 49.39], components
    assert components["Cover 1"]["z"] == [0, 0.94, -0.342], components
    assert mass == 27.78, mass
    assert [c for c, _ in warnings] == ["joint.loop"], warnings
    assert ca.APART.search(warnings[0][1]).group(1) == "3.20"
    m = ca.OVERLAP.match(CHECK)
    assert m and (m.group(1), m.group(2), m.group(3)) == ("Base 1", "Lid 1", "532.95"), m
    assert ca.close(1.004, 1.0) and not ca.close(1.006, 1.0)
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
