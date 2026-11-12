# Reference parts

Phase 2's exit test: twenty real parts (brackets, enclosures, knobs and mounts) that
must model cleanly, and keep modeling cleanly when an early sketch dimension
changes. CTest runs each one as `reference.<name>` through
`tools/reference/check_parts.py`, which:

1. builds the part with `sketchbillet_cli run`, one command at a time, into `part.sb`;
2. fails on any step that isn't `ok`, and on any warning or error;
3. checks each volume in `expect` against its formula, worked out by hand;
4. makes the `change`, and checks every step and formula again with the changed numbers.

The formulas use the changed numbers, so a change that is ignored fails just like
one that breaks the part.

The gallery page, [docs/user-guide/reference-parts.md](../../docs/user-guide/reference-parts.md),
is made from these files; `reference.gallery` fails if it's out of date. Each part's
picture is a `"part"` recipe in `docs/pictures.json`.

## File format

```json
{
  "title": "L bracket",
  "group": "Brackets",              // Brackets, Enclosures, Knobs or Mounts
  "order": 1,                       // its place in the group on the gallery page
  "about": "What it is, for a student.",
  "uses": ["sketch", "extrude"],    // the features it shows
  "picture_alt": "Words for the picture",
  "numbers": {"W": 60, "t": 5},     // the sizes the formulas use, in mm
  "steps": ["feature.add_sketch plane=xy name=BaseSketch", "..."],
  "expect": {"Base": "W*D*t"},      // step name: its volume in mm^3
  "change": {
    "command": "sketch.set_value sketch=BaseSketch constraint=Depth value=55",
    "numbers": {"D": 55},           // the numbers that change
    "about": "What the change does, for the gallery's Try line."
  },
  "show": "Bracket"                 // the step pictured
}
```

(The real files are plain JSON, without the comments.)

Formulas are Python expressions using the part's numbers, `pi`, `sqrt`, `sin`, `cos`,
`tan`, `asin`, `acos`, `atan`, `radians`, and these areas (mm^2):

| Helper | The area of |
| --- | --- |
| `seg(r, h)` | a circle's segment `h` deep: what a straight cut takes off a circle of radius `r` |
| `lens(r1, r2, d)` | where two circles `d` apart overlap |
| `band(r, a)` | a circle of radius `r` between `y = -a` and `y = a` |
| `rounding(r)` | what a rounding of radius `r` takes off a square corner (or adds to an inside one) |
| `bevel(c)` | what a 45-degree bevel `c` wide takes off a square corner |

A rounding or bevel round a circle is its area times the distance its middle travels
(Pappus's rule), e.g. `2*pi*(R - c/3)*bevel(c)` for a bevel on a disc's edge.

## Adding or changing a part

Face and edge numbers belong to the body you sketch on or pick from, and change
after each step. To find them, build the part so far and list a step's faces and
edges:

```bash
python3 tools/reference/check_parts.py --sketchbillet-cli <path> my-part --list StepName
```

Then work every formula out by hand, never from SketchBillet's own output, run
`cmake --preset <preset>` so CTest picks up a new file, rewrite the gallery with
`python3 tools/reference/check_parts.py --gallery docs/user-guide/reference-parts.md --write`,
and make the picture with `tools/docs/make_images.py ... ref-my-part`.
