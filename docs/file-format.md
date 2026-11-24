# The .sb file format (schemas 1 and 2)

A `.sb` file is a design. It is an ordinary zip file holding JSON text files, so
you can rename it to `.zip`, open it and read every step. The code that reads and
writes it is `sb_io` (`src/io/SketchBilletFile.cpp`).

The file holds the **recipe** (the steps and their numbers), not the finished
geometry. SketchBillet rebuilds the shape every time it opens a file, the same way it does
when you change a number.

A part (one design with steps) is saved as schema 1. An assembly (parts put
together) is saved as schema 2, with each of its parts inside it as a schema 1
part; see [Assemblies (schema 2)](#assemblies-schema-2).

## What's inside

```text
tag.sb
├── manifest.json                    what this file is and which SketchBillet wrote it
├── timeline.json                    the steps, in order
├── materials.json                   the design's own materials (only if it has any)
├── folders.json                     the feature tree's folders (only if it has any)
├── parameters.json                  named values like wall = 3 mm (only if it has any)
├── features/
│   ├── 926bde46-....json            one file per step
│   ├── f153a811-....json
│   └── acb900d7-....json
└── imports/
    └── 5d1e0a72-....step            an imported part's file, as it was (only if it has any)
```

An `Import` step's file is saved in `imports/`, exactly as it was read, named
after the step's id. That's how a design with an imported part opens on any
computer, even one without the file. On the clipboard (copied steps), the file
is in the step's `params` instead, since the clipboard is one piece of text: as
`content` for a STEP file (text), and as `content_base64` for STL and 3MF files,
which can be binary.

### manifest.json

```json
{
  "app": "0.0.1",
  "document": {
    "id": "26c5d391-a653-4100-ae82-ff04872a50fd",
    "kind": "part",
    "name": "Demo tag"
  },
  "files": [
    "timeline.json",
    "features/926bde46-9c5b-403a-bc25-c01e5cf11ce3.json"
  ],
  "format": "sketchbillet",
  "schema": 1,
  "units": { "angle": "rad", "length": "mm" }
}
```

| Key | Meaning |
| --- | --- |
| `format` | Always `"sketchbillet"`. A file without it is not a SketchBillet design. |
| `schema` | The format version, a whole number. A part is schema 1; an assembly is schema 2. |
| `app` | The SketchBillet version that saved the file. For information only. |
| `units` | Units of every number in the file: lengths in millimeters, angles in radians. |
| `document.id` | The design's UUID. It never changes, even when the file is renamed. |
| `document.kind` | `"part"` (or `"assembly"`, from Phase 4: see [Assemblies](design/assemblies.md#file-format)). |
| `document.name` | The design's name. |
| `document.lengthUnit` | Only when the student picked another unit: the unit the design *shows* lengths in, `"cm"`, `"m"`, `"in"` or `"ft"`. The numbers in the file are still in mm (see `units`). A unit SketchBillet doesn't know opens as mm. |
| `files` | Every other entry in the zip. For information only; readers go by `timeline.json`. |

### timeline.json

```json
{
  "features": [
    { "id": "926bde46-9c5b-403a-bc25-c01e5cf11ce3", "suppressed": false },
    { "id": "f153a811-ba41-4895-ab27-886a6ca3871e", "suppressed": false }
  ]
}
```

The steps in the order they are built. `suppressed: true` means the step is turned
off. Each `id` names a file `features/<id>.json`.

A design saved [rolled back](user-guide/timeline.md) also has `rollback`: the id of
the step the rollback marker is just after, or `"start"` for before the first step.
The steps after the marker aren't built. A design that isn't rolled back has no
`rollback`, and saves exactly as before.

```json
{
  "features": [
    { "id": "926bde46-9c5b-403a-bc25-c01e5cf11ce3", "suppressed": false },
    { "id": "f153a811-ba41-4895-ab27-886a6ca3871e", "suppressed": false }
  ],
  "rollback": "926bde46-9c5b-403a-bc25-c01e5cf11ce3"
}
```

### features/&lt;id&gt;.json

Every step has `id`, `type` and `name`. Steps that use other steps list them under
`inputs` by id. The numbers go under `params`.

| `type` | `inputs` | `params` |
| --- | --- | --- |
| `Box` | none | `corner` [x, y, z] mm, `size` [x, y, z] mm |
| `Cylinder` | none | `base` [x, y, z] mm, `axis` [x, y, z] direction, `radius` mm, `height` mm |
| `Boolean` | `a`, `b`: step ids | `operation`: `"union"`, `"cut"` or `"intersect"` |
| `Sketch` | none; `body`: step id for a sketch on a face; `on`: a Plane step's id for a sketch on a construction plane | `plane`: `"xy"`, `"xz"` or `"yz"`; `entities` and `constraints` (below); for a sketch on a flat face of a body, `face`: the face, as a [reference](#edge-references) with `"kind": "face"`. Its `plane` is then the base plane nearest the face, which an older SketchBillet draws it on |
| `Move` | `body`: step id | `offset` [x, y, z] mm, `center` [x, y, z] mm, `axis` [x, y, z] direction, `angle` radians. Turns by `angle` about the line through `center` along `axis`, then slides by `offset` |
| `Extrude` | `sketch`: a Sketch step's id; `body`: step id, for a join or cut | `distance` mm (the whole thickness), `side`: `"one"` (out of the sketch's front, the side its plane is seen from), `"flip"` (the other way) or `"both"` (half each way). Every closed shape of the sketch is swept; holes go through. `operation` (if there): `"join"` (added to `body`) or `"cut"` (taken out of `body`); without it, a new body |
| `Revolve` | `sketch`: a Sketch step's id; `body`: step id, for a join or cut | `angle` radians (the whole angle; 2 pi is a full turn), `axis`: `"x"` or `"y"` (the sketch's axes) or `{"line": <id>}` (a line entity of the sketch, from its start to its end), `side`: `"one"` (the right-hand way about the axis), `"flip"` (the other way) or `"both"` (half each way). Every closed shape of the sketch turns about the axis. `operation` as for Extrude |
| `Hole` | `body`: step id; `sketch`: a Sketch step's id, for holes at its points | `placement`: `"points"` (at every point of `sketch` that isn't a helper point) or `"edges"` (one hole, `distance_a` mm in from `edge_a` and `distance_b` mm in from `edge_b`, two [edge references](#edge-references)); `kind`: `"simple"`, `"counterbore"` or `"countersink"`; `diameter` mm; `through` true or false, and `depth` mm (used when not through); `counterbore_diameter` and `counterbore_depth` mm; `countersink_diameter` mm and `countersink_angle` radians; `flip` true or false. `size` (like `"M3"`) and `fit` (`"clearance"` or `"tap"`), if there, only say which screw the numbers came from: the numbers are what it drills |
| `Plane` | `body`: step id, for a plane from a face; `plane`: a Plane step's id, for a plane from another plane | A construction plane, not a body. `from`: `"xy"`, `"xz"` or `"yz"` (a base plane), `"face"` (with `face`, a [face reference](#edge-references) on `body`) or `"plane"`; `angle` radians, turned the right-hand way about `hinge`: `"x"` or `"y"` (the starting plane's directions through its origin) or `{"edge": <edge reference>}` (a straight edge of the face); then `offset` mm along the way it faces |
| `Shell` | `body`: step id | `thickness` mm; `faces`: a list of [face references](#edge-references) on `body`, the faces left open |
| `Import` | (none) | `format`: `"step"`, `"stl"` or `"3mf"`; `unit` (only for an STL file whose numbers aren't mm): `"cm"`, `"m"`, `"in"` or `"ft"`; `source`: where the file was imported from (the whole path, as the computer that imported it wrote it), for reading it again; `data`: where in the `.sb` the file is, `imports/<id>.step` (`.stl`, `.3mf`) |
| `Mirror` | `body`: step id; `steps`: a list of step ids, empty to mirror `body` itself | `plane`: `"yz"`, `"xz"` or `"xy"` (through 0, facing x, y or z) with `offset` mm along the way it faces, or `"face"` with `face`, a [face reference](#edge-references) on `body`. With `steps`, it mirrors their holes, joins and cuts on `body`; without, it joins `body` and its mirror image |
| `Pattern` | `body`: step id; `steps`: a list of step ids, empty for copies of `body` | `kind`: `"linear"` or `"circular"`; `count` (a whole number, the first one included); linear: `direction` [x, y, z], `spacing` mm, `count2`, `direction2` [x, y, z], `spacing2` mm; circular: `center` [x, y, z] mm, `axis` [x, y, z] direction, `angle` radians (2 pi spreads them evenly all the way round). With `steps`, it repeats their holes, joins and cuts on `body`; without, it copies `body` |
| `Fillet` | `body`: step id | `radius` mm, `edges`: the edges it rounds, as [edge references](#edge-references) |
| `Chamfer` | `body`: step id | `distance` mm (how far back from the edge, on both faces), `edges`: the edges it bevels, as [edge references](#edge-references) |

```json
{
  "id": "acb900d7-6be6-44e9-aace-a242499cd817",
  "inputs": {
    "a": "926bde46-9c5b-403a-bc25-c01e5cf11ce3",
    "b": "f153a811-ba41-4895-ab27-886a6ca3871e"
  },
  "name": "Tag",
  "params": { "operation": "cut" },
  "type": "Boolean"
}
```

A step's inputs must come before it in the timeline.

#### Edge references

A Fillet or Chamfer keeps each of its edges, and a sketch on a face its face, as
a **name** that says where it came from, never as a number (numbers change when earlier steps change).
See [Naming faces and edges](design/naming.md) for how names are built. Each
reference also keeps what the edge looked like when it was picked, so SketchBillet
can still find it if an edit renames it:

| Key | Meaning |
| --- | --- |
| `kind` | `"edge"` or `"face"` (`"vertex"` is kept for later steps) |
| `name` | Its name, like `"E[0b1c2d3e/end:4f5a6b7c|0b1c2d3e/side:9d8e7f6a]"`: the edge between the two faces named inside the brackets |
| `geom` | What it is: `"line"`, `"circle"`, `"ellipse"`, `"spline"` or `"other"` (for faces: `"plane"`, `"cylinder"`, `"cone"`, `"sphere"`, `"torus"`) |
| `center` | Its middle [x, y, z], mm |
| `direction` | A line's direction, or a circle's axis [x, y, z]; [0, 0, 0] for none |
| `size` | Its length, mm |
| `radius` | A circle's radius, mm; 0 otherwise |

```json
{
  "id": "6e0f0a52-2c3b-4f7e-9f44-3b8e2c7d1a90",
  "inputs": { "body": "0b1c2d3e-5f60-4a71-8b92-a3b4c5d6e7f8" },
  "name": "Fillet1",
  "params": {
    "edges": [
      {
        "center": [20.0, 0.0, 10.0],
        "direction": [1.0, 0.0, 0.0],
        "geom": "line",
        "kind": "edge",
        "name": "E[0b1c2d3e/end:4f5a6b7c|0b1c2d3e/side:9d8e7f6a]",
        "radius": 0.0,
        "size": 40.0
      }
    ],
    "radius": 3.0
  },
  "type": "Fillet"
}
```

A `Sketch` step keeps what was drawn, not the solved positions: SketchBillet solves it
again every time it opens or rebuilds the design.

- Each entity has `id`, `name` (like `"Line1"`), `type` and its numbers (mm,
  angles in radians), and `"construction": true` if it's helper geometry:
  `point` (`at` [x, y]), `line` (`start`, `end`), `circle` (`center`, `radius`),
  `arc` (`center`, `radius`, `start_angle`, `end_angle`, counterclockwise),
  `spline` (`poles`: a list of [x, y] control points; `degree`, 1 to 5;
  `weights`, one per pole, left out when they're all 1; `"closed": true` for a
  closed loop, left out when false; `source` only for one imported from a file
  of points: `file`, the file's path when it was imported, `through`, true if the
  curve goes through the points (false: they're its poles), and `unit`, the
  unit of the file's numbers, `"mm"`, `"cm"`, `"m"`, `"in"` or `"ft"`). A spline's knots aren't saved: they're spread
  evenly, clamped at the ends of an open spline and periodic for a closed one (see
  `sb::sketch::SplineShape`).
- Each constraint has `id`, `name` (like `"Length1"`), `kind` (`"horizontal"`,
  `"point_on_line"`, ... as `sketch.constrain` spells them), `points` (each an
  `entity` id and a `role`: `"start"`, `"end"`, `"center"`, `"pole"` with an
  `index` from 0 for a spline's control point, or none for a point),
  `entities` (ids), `value` for dimensions (mm, or radians for `angle`),
  `expression` for a dimension whose number comes from a formula (like
  `"wall * 2"`, exactly as typed; left out for a typed number), and `at` [x, y]
  for `fix`. A dimension with a formula still has its `value`: what the formula
  last worked out to, which is what the solver uses (and what an older SketchBillet,
  which skips `expression`, uses).

```json
{
  "id": "38e7e201-4f01-4d9a-b39e-be083cdc1d15",
  "name": "Sketch1",
  "params": {
    "constraints": [
      { "entities": ["bee5fd67-8e65-4251-954d-2d8741b05c01"], "id": "2e306830-39b8-49c1-8656-45f9405a1fc9",
        "kind": "length", "name": "Length1", "value": 40.0 }
    ],
    "entities": [
      { "end": [38.0, 2.0], "id": "bee5fd67-8e65-4251-954d-2d8741b05c01", "name": "Line1",
        "start": [0.0, 0.0], "type": "line" }
    ],
    "plane": "xy"
  },
  "type": "Sketch"
}
```

A step whose numbers come from [formulas](user-guide/parameters.md#using-parameters-in-steps)
has `"formulas"`: each number's key (see `feature.set_value`) and its formula as
typed, like `{"distance": "wall * 4"}`. `params` still has every number, as the
formula last worked out, which is what's built (and what an older SketchBillet, which
skips `formulas`, uses). A key this SketchBillet doesn't know is kept.

A step can also have `"material"`: the key of the material picked for its body,
either a built-in material's key (like `"pla"` or `"aluminum_6061"`, listed in
`src/model/Material.cpp`) or the `id` of one of the design's own materials. A step
with no material picked has no `"material"` key. A key SketchBillet doesn't know is kept
as it is, and the body counts as PLA.

A step can also have `"color"`: the color picked for its body, as `"#rrggbb"`
(like `"#d03030"`). A step with no color of its own has no `"color"` key, and its
body is drawn in its material's color. A color SketchBillet can't read is left out
when the design opens. Older versions of SketchBillet skip the key, so they open the
design with the material's colors.

### materials.json

Only a design that added materials of its own has this file:

```json
{
  "materials": [
    {
      "color": "#3366cc",
      "cost_per_kg": 25.0,
      "density": 1.27,
      "id": "5b0f3c1e-8d2a-4f6b-9c3d-2e1a0b9c8d7f",
      "name": "Blue PETG",
      "poissons_ratio": 0.0,
      "youngs_modulus": 0.0,
      "yield_strength": 0.0
    }
  ]
}
```

| Key | Meaning |
| --- | --- |
| `id` | The material's UUID. Steps refer to it by this, so renaming it later won't break them. |
| `name` | Its name. No two of the design's materials share a name, and none uses a built-in material's name. |
| `density` | g/cm³, from 0.01 to 25. |
| `color` | `#rrggbb`. |
| `youngs_modulus`, `poissons_ratio`, `yield_strength`, `cost_per_kg` | GPa, a ratio, MPa, and price per kg. 0 means unknown. |

Materials are added before the steps, through the same command a student uses
(`material.define`), so the same checks run.

### folders.json

Only a design with folders in its feature tree has this file:

```json
{
  "folders": [
    {
      "id": "3c1f7e2a-9b4d-4e8a-a1f2-6d5c4b3a2918",
      "name": "Tag parts",
      "steps": [
        "926bde46-9c5b-403a-bc25-c01e5cf11ce3",
        "f153a811-ba41-4895-ab27-886a6ca3871e"
      ]
    }
  ]
}
```

| Key | Meaning |
| --- | --- |
| `id` | The folder's UUID. |
| `name` | Its name. No two folders share a name. |
| `steps` | Its steps' ids, in design order. They are next to each other in `timeline.json`, and no step is in two folders. |

Folders only arrange the feature tree; they never change what's built. They are
added after the steps, through the same command a student uses (`folder.add`),
so the same checks run.

### parameters.json

Only a design with [parameters](user-guide/parameters.md) has this file:

```json
{
  "parameters": [
    {
      "expression": "3 mm",
      "id": "98104fd6-668c-4e5e-b457-bcd28e6e0511",
      "name": "wall",
      "note": "How thick the walls are"
    },
    {
      "expression": "wall * 4",
      "id": "d46c5462-e9c0-4a29-add0-0b294fabc63c",
      "name": "height"
    }
  ]
}
```

| Key | Meaning |
| --- | --- |
| `id` | The parameter's UUID. It stays the same when the parameter is renamed. |
| `name` | Its name: letters, digits and `_`, starting with a letter. No two names differ only in upper and lower case. |
| `expression` | Its formula, exactly as typed, with its units: `3 mm`, `1.5 in`, `wall * 4`. Not converted to mm, so it reads back the way the student wrote it. |
| `note` | What it's for (optional; left out when empty). |

Parameters are listed in the order they were added, not the order they're
worked out in. A formula is stored even if it has a problem (it uses a name
that isn't there, say), so the design still opens and the student can fix it.
Names must be ones a parameter can have, or the file counts as damaged.

## Same design, same bytes

Saving a design that hasn't changed gives exactly the same file, so Git shows no
change and a diff of two versions shows only the steps that changed. To make that
work:

- JSON keys are sorted and indented with two spaces, with a newline at the end.
- Every zip entry is dated 1980-01-01 00:00, written directly (not through the
  computer's time zone).
- The file has no "created" or "modified" dates and no hashes.

## Opening a file

SketchBillet reads `manifest.json`, then `parameters.json` and `materials.json` if they're there, then
`timeline.json`, then each step, then `folders.json` if it's there. Each step is
added through the normal add-step command, so the same checks run as when a student
builds the design. The opened design has no undo history and no unsaved changes.

If something is wrong, SketchBillet opens nothing and explains why:

| Code | When |
| --- | --- |
| `file.cant_read` | The file isn't there, or SketchBillet isn't allowed to open it. |
| `file.not_sketchbillet` | Not a zip, no `manifest.json`, or `format` isn't `"sketchbillet"`. |
| `file.too_new` | `schema` is higher than this SketchBillet reads. The message says to update SketchBillet. |
| `file.unknown_step` | A step's `type` is one this SketchBillet doesn't know (maybe from a plugin). |
| `file.damaged` | Anything else: bad JSON, a missing step file, a missing number, an input that doesn't exist, a material without its id, name, density or color, a folder whose steps aren't next to each other, a parameter without its id, name or expression, or with a name it can't have. |
| `file.cant_write` | (Saving) The folder is missing, read-only or full. |

Saving writes `<name>.saving` first and then renames it over the old file, so a
failed save never damages the last good copy.

## Assemblies (schema 2)

An assembly is a design made of parts put together, like a car made of a body
and four wheels. Each part it uses is saved inside it, so the assembly opens on
any computer, even one without the parts' own files. A **component** is one
copy of a part, placed somewhere: four wheels are four components of one Wheel
part, and the Wheel part is saved once.

```text
car.sb
├── manifest.json                    kind "assembly", schema 2
├── components.json                  each component: its part, where it is, grounded or free
├── joints.json                      the joints between components (only if it has any)
├── pressfits.json                   pairs of components meant to overlap (only if it has any)
├── parameters.json                  the assembly's own named values (only if it has any)
└── parts/
    ├── 017e13e9-....                an inside part: one folder per part, named after its id
    │   ├── manifest.json            laid out exactly like a part's .sb file (schema 1)
    │   ├── timeline.json
    │   └── features/...
    └── 1305cf0c-....                a linked part: its copy as last used, plus link.json
        ├── link.json                where its own .sb file is
        └── ...
```

The assembly's `manifest.json` is the same as a part's, with `"kind": "assembly"`
and `"schema": 2`. Its `files` lists `components.json`, `joints.json`,
`pressfits.json` and `parameters.json` if there are any, and each part's `parts/<id>/manifest.json`. The parts are listed
in the assembly's order (the order they were put in), and that is the order they
open in.

Each part folder holds what a part's `.sb` file holds, with the same names and a
schema 1 manifest. You can copy a folder out, zip it, and open it as a part.

### link.json

A **linked part** follows its own `.sb` file (see
[Linked and inside parts](user-guide/assemblies.md#linked-and-inside-parts)).
Its folder still holds its copy as last used, so the assembly opens without the
file, plus `link.json` saying where the file is. The manifest's `files` lists it
right after the part's `manifest.json`. An inside part has no `link.json`.

```json
{
  "documentId": "1305cf0c-6a3e-4c5b-9b1d-2f7e8a4d9c10",
  "path": "parts/wheel.sb",
  "sha256": "9f2c0d1e...",
  "updated": "2026-11-13T23:40:00Z"
}
```

| Key | Meaning |
| --- | --- |
| `documentId` | The part's design id: the same as its folder's name, and the id in the file's own manifest. |
| `path` | The file, relative to the assembly's folder, with `/` between folders on every computer. A file on another drive is written as a full path. |
| `sha256` | The file's fingerprint as last used: SHA-256 of its bytes, as 64 lowercase hex digits. A file with another fingerprint has changed. |
| `updated` | When the assembly last took its copy from the file, in UTC. |

The path is relative so that a folder holding an assembly and its parts can be
moved or copied as a whole. A SketchBillet that doesn't know about `link.json`
(the first one to read schema 2) opens the part as an inside part, from its
copy.

### components.json

```json
{
  "components": [
    {
      "grounded": true,
      "id": "0a40363b-e9b6-4fed-bdbc-89b401ada34f",
      "name": "Body 1",
      "part": "017e13e9-3138-437a-ac92-a865b700afd2",
      "placement": {
        "origin": [0.0, 0.0, 0.0],
        "xAxis": [1.0, 0.0, 0.0],
        "zAxis": [0.0, 0.0, 1.0]
      }
    }
  ]
}
```

| Key | Meaning |
| --- | --- |
| `id` | The component's UUID. It never changes, even when the component is renamed. |
| `name` | Its name in the assembly, like `Wheel 2`. No two components have the same name. |
| `part` | The id of the part it is a copy of: one of the folders in `parts/`. |
| `grounded` | `true` if it's fixed where it is, `false` if it's free to move. |
| `placement` | Where the part's origin goes (`origin`, in mm), and which way the part's own x and z axes point in the assembly (`xAxis`, `zAxis`, each one unit long and at right angles to each other). The y axis is z × x. |

Components are listed in the assembly's order, which is the order a list or the
feature tree shows them in.

Where a joint puts a component isn't saved: its `placement` is where it was put
by itself, and the joints place it again each time the assembly is built. Take
the joint out, and the component goes back there.

### joints.json

```json
{
  "joints": [
    {
      "a": {
        "at": "end",
        "body": "93a3c1a6-40d6-4ef9-86f7-89085b5e4361",
        "component": "0a40363b-e9b6-4fed-bdbc-89b401ada34f",
        "element": {"center": [20.0, 40.0, 10.0], "direction": [1.0, 0.0, 0.0], "geom": "line",
                    "kind": "edge", "name": "E[93a3c1a6/back|93a3c1a6/top]", "radius": 0.0, "size": 40.0}
      },
      "angle": 0.0,
      "b": {"component": "5c1d2e3f-4a5b-4c6d-8e7f-9a0b1c2d3e4f"},
      "flip": true,
      "highest": 0.0,
      "id": "e2b54d6a-0c1f-4f5e-9d3a-1b2c3d4e5f60",
      "kind": "revolute",
      "lowest": -1.9198621771937625,
      "name": "Joint1",
      "off": false,
      "offset": 0.0,
      "rest": 0.0,
      "value": -1.7453292519943295
    }
  ]
}
```

| Key | Meaning |
| --- | --- |
| `id`, `name` | The joint's UUID, which never changes, and its name, like `Joint1`. |
| `kind` | `rigid`, `revolute` (turns about the joint) or `slider` (slides along it). |
| `a`, `b` | Its two sides. `b` is placed against `a` (or `a` against `b`, when only `b` is placed already). |
| `component` | The side's component. |
| `body` | The id of the step whose body the side is on, in the component's part. Left out for the part's own origin. |
| `element` | The face, edge or vertex picked on that body: a reference like a fillet's edges, found again by name when the part changes (see [features](#featuresidjson)). Left out for the part's own origin. |
| `at` | `start` or `end` for a straight edge's end; left out for its middle. |
| `flip` | `true` if `b` is turned round so the two sides point the same way; otherwise they point at each other. |
| `angle`, `offset` | How far `b` is turned about the joint (radians) and the gap left along it (mm). |
| `value`, `rest` | How far a revolute joint is turned (radians) or a slider slid (mm), and where Reset Joints puts it back to. 0 for a rigid joint. |
| `lowest`, `highest` | The value's limits, in the same units. Left out when there's none. |
| `off` | `true` if it's turned off: it places nothing. |

Joints are listed in the order they were added: when two could place the same
component, the earlier one does.

### pressfits.json

The pairs of components meant to overlap, like an axle pushed into a hole a bit
smaller than it. Checking for overlaps leaves them out.

```json
{
  "press_fits": [
    {"id": "4f0c2d0e-...", "a": "9b1e44a2-...", "b": "c3d97f10-..."}
  ]
}
```

| Field | Meaning |
| --- | --- |
| `id` | The press fit's id. |
| `a`, `b` | The two components' ids, either way round. |

### Opening an assembly

SketchBillet reads the manifest, then `parameters.json`, then each part in the
order the manifest lists them (each one opened the same way as a part file), then
`components.json`, then `joints.json`, then `pressfits.json`. Each part,
component, joint and press fit is added through a command, so the same checks
run as when a student builds the assembly.

Then it looks for each linked part's file: at its `path`; then by its file name
in the assembly's folder; then by its `documentId` among the `.sb` files in the
assembly's folder and the folders in it, three deep. A file only counts if its
manifest has the same design id.

| What it finds | What happens |
| --- | --- |
| The file, with the same fingerprint | Nothing. |
| The file, with another fingerprint | The part is read from the file, in place of the copy, and its fingerprint and `updated` change. One undo step, and the assembly is changed (not saved). |
| The file, somewhere else | The link's `path` changes to where it is now, and the part is updated as above if it changed. |
| Nothing | The copy is used, and the part is marked as not found (that isn't saved). |

Each of these but the first leaves a note saying what happened.

An assembly is `file.damaged` if a part listed in the manifest has no folder, a
part's `link.json` has no `path` or `documentId` or is for another design, a
part in it is itself an assembly, a component is missing its id, name, part or
placement, a component's part isn't in `parts/`, or a joint is missing its id,
name, kind or sides, or joins components that aren't there. An assembly that says it is
schema 1 is damaged too. A SketchBillet that only knows schema 1 says the file is
`file.too_new` and to update SketchBillet.

## Changing the format

- Adding a step type, or an optional key that older SketchBillet can ignore, keeps schema 1.
  Materials were added this way (Oct 2026): an older SketchBillet opens a design with
  materials, shows every body as plain geometry, and drops the materials if it
  saves the file again. Folders were added the same way: an older SketchBillet shows the
  steps without folders, and drops the folders if it saves the file again.
  Splines, and the constraints only splines use (`point_on_spline`, `smooth`,
  Oct 2026), were added in schema 1 too, but an older SketchBillet can't skip them: it
  won't open a design that has them, and says the file is damaged. The same goes
  for `symmetric`. A spline's
  `source` is an optional key an older SketchBillet skips (and drops if it saves).
  Parameters were added like materials (Oct 2026): an older SketchBillet opens the
  design without them, and drops them if it saves the file again. The same
  goes for a dimension's `expression` and a step's `formulas`: an older SketchBillet
  keeps the numbers, not the formulas. `Fillet` and `Chamfer` steps (Nov 2026)
  are new step types in schema 1: an older SketchBillet won't open a design that
  has them, and says the design has a step of a kind it doesn't know. A sketch's
  `face` (Nov 2026) is new too: an older SketchBillet opens the design and draws
  the sketch on its `plane` instead. So is an Extrude's `operation`: an older
  SketchBillet makes a body of its own instead of joining or cutting. `Revolve`,
  `Hole`, `Pattern`, `Mirror`, `Plane` and `Shell` steps (Nov 2026) are new step types, like `Fillet`.
  A `Sketch` can have the input `on`, a Plane step's id: an older SketchBillet
  draws that sketch on its base `plane` instead. The timeline's `rollback` (Nov 2026)
  is a key an older SketchBillet skips: it opens the design rolled forward, with
  every step built, and drops the marker if it saves the file again. `Import`
  steps and the `imports/` folder (Nov 2026) are a new step type too: an older
  SketchBillet won't open a design that has one, and says the design has a step
  of a kind it doesn't know.
- Assemblies (Nov 2026) are schema 2, because an older SketchBillet would read an
  assembly as an empty part. A part is still saved as schema 1, so a part saved
  by a newer SketchBillet still opens in an older one. A part inside an assembly
  is saved as schema 1 too.
- Anything an older SketchBillet would read wrongly bumps `schema` (in
  `src/core/include/sb/core/Version.h`) and adds a migration from the old schema.
- `tests/files/` holds files saved with each schema. They are checked in and never
  regenerated; every version of SketchBillet must still open them. Add one when the schema
  changes.
- Format changes need a line-by-line human review (see CLAUDE.md).

## Not in the format yet

From the design document's file format plan, these come later:

- A `cache/` folder with built geometry for faster opening.
- Keeping keys this SketchBillet doesn't understand when it saves a file from a newer
  minor version. Today they are dropped.
