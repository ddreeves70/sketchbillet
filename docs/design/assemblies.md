# Assemblies

> **Draft, for review.** Phase 4's first item (see the [roadmap](roadmap.md)).
> Nothing on this page is built yet. Two choices were made on Nov 13, 2026: an
> assembly's parts can live **inside** the assembly file or be **linked** to
> other files, from the start; and parts move along **chains** of joints first,
> with loops (like a four-bar linkage) later. Changes to `sb_model`, naming and
> the file format need a line-by-line human review, as always.

Students rarely make one part. A toy car is a body, four wheels and two axles. A
project box has a lid on a hinge, and a phone stand has a phone resting on it.
An **assembly** puts parts together: where each one sits, how it can move
against the others, whether any of them overlap, and how many of each to print.

This page covers what the student gets, in the order it gets built:

| Part | What the student gets | Status |
| --- | --- | --- |
| 1. Where parts live | Parts made inside the assembly, or linked to their own `.sb` files | Planned |
| 2. Components | Parts placed in the assembly, the same part more than once, one part fixed in place | Planned |
| 3. Joints | Rigid, hinge (revolute) and slider joints that place parts and say how they move | Planned |
| 4. Placing parts along chains | Exact, instant placement from joint to joint, smooth enough to drag | Planned |
| 5. In the app | The assembly tree, inserting parts, joining, dragging, editing a part | Planned |
| 6. Checks | Parts that overlap, the whole assembly's mass and balance point, measuring between parts | Planned |
| 7. Exploded views | Parts pulled apart to show how they fit, for pictures and drawings | Planned |
| 8. Parts list | How many of each part, with material and mass, for printing and drawings | Planned |
| 9. Parts from bodies | A part with several bodies turned into an assembly (copy-and-group's "groups that move together") | Planned |

## Words

These words mean one thing each, in the app, the guides and the code:

- A **part** is a design made of steps, like every `.sb` file today.
- An **assembly** is a design made of parts placed together. It's a `.sb` file
  too, of the kind `assembly`.
- A **component** is one part placed in an assembly. Four wheels are four
  components of one part, **Wheel**, named Wheel 1 to Wheel 4.
- A **joint** connects two components: it places one against the other and says
  how it can still move (not at all, turning, or sliding).
- A component is **grounded** when it's fixed where it is. Other components are
  placed from it, through joints.

## 1. Where parts live

An assembly's parts can be **inside** it or **linked** to their own files, and a
student can switch a part from one to the other.

**Inside parts** are made in the assembly. They're saved in the assembly's
`.sb` file, so it opens anywhere, like a design with an imported part. This is
the easy path for a class: one file to hand in, nothing to lose.

**Linked parts** come from another `.sb` file: **Assemble > Insert Part from
File** picks `wheel.sb`. Changing `wheel.sb` changes every assembly that uses it,
which is what a team needs when one student owns the wheel and another the
car. Links are where files get lost, so a linked part always keeps:

- **The file's path**, relative to the assembly (like `parts/wheel.sb`), so a
  folder that's moved or copied as a whole keeps working.
- **The file's design id** (its `document.id` UUID), which never changes, even
  when the file is renamed.
- **A copy of the part as last used**, inside the assembly. That's what makes an
  assembly open even when the linked file can't be found: emailed on its own,
  or opened on another computer.
- **A fingerprint** of the file as last used (SHA-256 of its bytes), to tell
  when it has changed.

When an assembly opens, SketchBillet looks for each linked file: at its path,
then by name next to the assembly, then by its design id among the `.sb` files
in the assembly's folder and the folders inside it. Then one of these happens:

| What it finds | What happens |
| --- | --- |
| The file, unchanged | Nothing to do. |
| The file, changed | The part updates from it, joints are found again (part 3), and the status bar says so: "Wheel changed in wheel.sb, so Car uses the new Wheel." Undo goes back to the old Wheel. |
| The file, found somewhere else | As above, and the link remembers the new place: "Car found wheel.sb in the parts folder and links to it there now." |
| Nothing | The saved copy is used, and Wheel is marked in the tree: "wheel.sb can't be found, so Car uses the copy saved on Nov 13, 2026. Pick Wheel, then Assemble > Find Linked File." |

While the assembly is open, saving `wheel.sb` in SketchBillet updates the
assembly at once. A change made by another program, or on another computer, is
noticed the next time the assembly opens, or with **Assemble > Update Linked
Parts**. Watching the files for changes can come later.

Switching:

- **Make Part Inside** turns a linked part into an inside part. The link is
  gone; the part is the copy, and nothing else changes.
- **Save Part as File** writes an inside part to its own `.sb` file and links to
  it.

**Editing a part.** An inside part is edited in the assembly: **Edit Part**
shows that part's steps in the feature tree, fades the other components, and
the student works as in any part. **Done** goes back to the assembly. A linked
part is edited in its own file: **Open Part** opens `wheel.sb` beside the
assembly. That needs the desktop app to keep several designs open at once,
which is the first thing to build (see Building it).

**Undo.** Each file has one undo history. An assembly's history includes its
inside parts, so undoing a change to an inside part works from the assembly. A
linked part's changes are undone in its own file.

**No loops of files.** A part can't link to an assembly that already uses it,
directly or through other files: "Car can't use car.sb as a part, because
car.sb is Car itself." Assemblies inside assemblies (sub-assemblies) come
after v0.2: inserting an assembly file says so, plainly, for now.

## 2. Components

A component is:

| Field | What it is |
| --- | --- |
| `id` | Its UUID. |
| `name` | Like "Wheel 1": the part's name and the next free number. Students can rename it. |
| `part` | Which part: an inside part's id or a linked part's id (the design id of the file). |
| `placement` | Where it sits: an origin (mm) and its x and z directions, like a sketch plane. |
| `grounded` | True if it's fixed where it is. |

A component places the **whole part**: every finished body in it, as they are
at the end of the part's steps. A part with a body and a separate pin places
both.

The first component put into an assembly is grounded, and any can be grounded
or let go later (**Assemble > Ground**). A component that's neither grounded nor
joined is **free**: it stays where it was put, and the student moves it with the
move handles. A joint takes over its placement (part 3).

Components of the same part share it: Wheel is built once, and the viewer draws
the same shape four times, each with its own placement. That keeps an assembly
of many copies fast to rebuild and to draw (the "instancing" in the
[SolidWorks-class scope](solidworks-scope.md#decide-now-cheap-changes-to-phases-0-2)).

## 3. Joints

SketchBillet uses joints the way Fusion does, not SolidWorks-style mates (see
[Architecture](architecture.md#assemblies-joints-and-motion)): one joint between
two components says both where the second one goes and how it can still move.
It's easier to learn: a hinge is one joint, not three mates.

**Joint origins.** Each side of a joint is a point and a direction on a
component, picked on its geometry:

| Picked | Point | Direction (the joint's z) |
| --- | --- | --- |
| A round edge (a hole's rim, an axle's end) | The circle's center | Along the circle's axis |
| A flat face | The face's center, or a point picked on it | Straight out of the face |
| A straight edge | The edge's middle, or either end | Along the edge |
| A corner (vertex) | The corner | The component's z |
| Nothing (the part's own origin) | The part's origin | The part's z |

A joint origin is saved as the component, the body, a `GeometryRef` to the
picked face or edge (the same references fillets use, see
[Naming faces and edges](naming.md)), what was picked from it (center, middle,
end), and an optional offset in mm. When the part changes, the reference is
found again; if it's ambiguous or missing, the joint is marked and never
guessed: "Joint2 can't find the hole it sits in on Wheel: a change to Wheel
took it away. Pick Joint2, then Assemble > Change Joint Origins."

**Joining** moves the second component so its origin meets the first's, with
their z directions pointing at each other, like a peg dropped into a hole.
**Flip** turns it the other way round, **Angle** turns it about z, and
**Offset** leaves a gap along z (mm).

**Kinds** in v0.2:

| Kind | Still moves | Its value | Limits |
| --- | --- | --- | --- |
| Rigid | Not at all | None | None |
| Revolute (a hinge) | Turns about z | An angle | Optional lowest and highest angle |
| Slider | Slides along z | A distance (mm) | Optional lowest and highest distance |

Cylindrical, pin-slot, planar and ball joints come later, in that order
([Architecture](architecture.md#assemblies-joints-and-motion) has the full list).

A revolute or slider joint's value is a number like any other: it can be a
parameter or a formula (`lid_angle = 90 deg`). Turning or sliding a part sets
it. Its **rest** value is where it goes back to with **Assemble > Reset
Joints**, which is also where drawings show it.

## 4. Placing parts along chains

Components and joints make a graph: components are the dots, joints the lines.
Each grounded component is a starting point. SketchBillet walks out from the
starting points along the joints, placing each component from the one before
it:

```text
placement(child) = placement(parent) × origin(parent side) × motion(joint value) × flip × origin(child side)⁻¹
```

Every step is exact (no guessing, no iterating), so a chain of 100 components
is placed in well under a millisecond, and dragging a hinged lid follows the
mouse smoothly: the drag only changes one joint's value, and the chain is
walked again.

That works as long as each component is reached along **one** path of joints,
like a chain or a tree: car body, then axles, then wheels; base, then arm, then
gripper. When a joint connects two components that are both placed already, it
makes a **loop**, like a four-bar linkage, a lid with two hinges, or two
grounded components joined. Placing parts in a loop needs a numerical solver,
which comes later (the 3D solver in
[SolidWorks-class scope](solidworks-scope.md)). Until then, the joint that
closes the loop is checked but doesn't move anything:

- If it already lines up (its two origins within 0.01 mm and their directions
  within 0.01°), it's fine, and says nothing.
- If not, it's marked with a warning: "Joint5 makes a loop (Base, Arm, Link,
  Base), and its two sides are 3.20 mm apart. SketchBillet can't move parts in
  loops yet, so Joint5 doesn't move anything. Turn Joint5 off, or change a
  value in the loop so its sides meet."

A lid with two hinges is the common case. The first hinge places the lid. The
second closes a loop, but both hinges share one axis, so its sides always meet:
it's accepted and says nothing. Turning the lid changes the first hinge's
value, and the second follows. So the rule is: the first joint (in the order
they were added) places a component, and a later joint that agrees with it is
accepted as it is.

A component no joint reaches keeps its own placement. A grounded component
never moves.

## 5. In the app

**The tree.** In an assembly, the feature tree shows **Components** (each with
its part's name, a link mark for linked parts and a pin for grounded ones) and
**Joints** (each with its kind and value), plus the assembly's own parameters.
Properties shows a component's part, file and placement, and a joint's kind,
value, limits and rest value.

**The Assemble menu:**

| Item | What it does |
| --- | --- |
| New Part | Adds an empty inside part as a new component, and starts editing it. |
| Insert Part from File... | Links a part from a `.sb` file and places it where the student clicks. |
| Insert Copy of Part... | The same, as an inside part (no link). |
| Joint | Pick a joint origin on one component, then on another; the second one moves. Then pick Rigid, Revolute or Slider in Properties. |
| Ground | Fixes the picked component where it is, or lets it go. |
| Edit Part / Open Part | Edits an inside part here, or opens a linked part's file. |
| Make Part Inside / Save Part as File | Switches a part between inside and linked. |
| Update Linked Parts | Checks every linked file now. |
| Find Linked File... | Points a link at a file that moved. |
| Check for Overlaps | Part 6. |
| Explode | Part 7. |

**Dragging.** A free component moves with the move handles, as bodies do today.
A component on a revolute or slider joint turns or slides when dragged, along
its joint only and within its limits; the joint's value follows. A component
on a rigid joint, or grounded, doesn't drag; the status bar says why.

**The viewer** draws each component's shapes with its placement, sharing shapes
between components of the same part. Picking a face in an assembly picks it on
one component, so measuring and joint origins know which wheel was clicked.

**The command line** gets the same commands, so every guide example is tested:

```text
sketchbillet_cli run --save car.sb assembly.new
sketchbillet_cli run --open car.sb --save car.sb component.insert file=wheel.sb count=4
sketchbillet_cli run --open car.sb --save car.sb joint.add kind=revolute a=Axle1:edge:3 b=Wheel1:edge:1
sketchbillet_cli run --open car.sb --save car.sb joint.set joint=Joint1 value=30deg
sketchbillet_cli open car.sb --components
sketchbillet_cli check car.sb --overlaps
sketchbillet_cli parts car.sb
```

Edges and faces are named by component and number, as `open --edges` lists
them today.

## 6. Checks

- **Overlaps.** **Assemble > Check for Overlaps** (and `check --overlaps`) tests
  every pair of components whose boxes meet, with the kernel's Boolean common.
  Each pair that shares more than 0.001 mm³ is listed and shown in red: "Wheel 1
  and Axle 1 overlap by 12.57 mm³. Make the hole in Wheel bigger, or the axle
  thinner." Faces that only touch don't count. A pair can be marked as meant to
  overlap (a press fit).
- **Mass and balance.** The whole assembly's mass and weight, and its center of
  gravity, from each part's material (as parts have today), shown as a dot in
  the viewer.
- **Measuring** works between faces and edges of different components.

## 7. Exploded views

An exploded view pulls the components apart to show how they go together, for
pictures, instructions and drawings. It's saved with the assembly by name
("Exploded 1") and never changes the joints. Each step of it slides one or more
components along a direction by a distance (mm), with a thin line back to where
they came from. Phase 4's drawings use it for an exploded drawing view.

## 8. Parts list

**Assemble > Parts List** (and `sketchbillet_cli parts car.sb`) lists each part
once: how many components use it, its material, its mass each and in all, and
whether it's inside or linked (and its file). It exports as CSV, prints and
exports parts once with their count (the slicer gets one wheel and "print 4"),
and becomes the parts list (BOM) table in drawings.

## 9. Parts from bodies

[Copying and grouping](copy-and-group.md#5-groups-that-move-together-phase-4)
promised "groups of bodies that move together". That's an assembly: **Make
Parts from Bodies** takes a part with several bodies, makes an assembly with one
inside part per picked body (with the steps that make it, copied as Copy and
Paste does), places each where it was, and grounds the first. Then the student
joins them.

## File format

An assembly is a `.sb` file whose `manifest.json` has `"kind": "assembly"` and
`"schema": 2`. Parts are still written as schema 1, so an older SketchBillet
still opens every part; it refuses an assembly with its usual message, that the
design was made with a newer SketchBillet.

```text
car.sb
├── manifest.json            kind "assembly", schema 2
├── components.json          the components, in the order they were added
├── joints.json              the joints, in the order they were added
├── explodes.json            exploded views (only if it has any)
├── parameters.json          the assembly's own parameters (only if it has any)
└── parts/
    ├── 0b6f…/               an inside part: the same files as a part's .sb
    │   ├── manifest.json
    │   ├── timeline.json
    │   └── features/…
    └── 7e21…/               a linked part: its copy as last used, plus link.json
        ├── link.json
        ├── manifest.json
        └── …
```

Each folder in `parts/` is named after its part's design id. A linked part's
`link.json`:

```json
{
  "path": "parts/wheel.sb",
  "documentId": "7e21c0aa-5d3f-4c8e-9a0f-0d2b6c1e4f11",
  "sha256": "9f2c…",
  "updated": "2026-11-13T23:40:00Z"
}
```

`path` is relative to the assembly, with forward slashes on every system.
`components.json`:

```json
{
  "components": [
    {
      "id": "c1f0…", "name": "Body 1", "part": "0b6f…", "grounded": true,
      "placement": { "origin": [0, 0, 0], "xAxis": [1, 0, 0], "zAxis": [0, 0, 1] }
    },
    {
      "id": "c2a4…", "name": "Wheel 1", "part": "7e21…", "grounded": false,
      "placement": { "origin": [40, -32, 12], "xAxis": [1, 0, 0], "zAxis": [0, -1, 0] }
    }
  ]
}
```

A jointed component's `placement` is saved as last placed, so the file opens
right even before anything is rebuilt; the joints place it again on every
rebuild. `joints.json`:

```json
{
  "joints": [
    {
      "id": "j1…", "name": "Joint1", "kind": "revolute", "suppressed": false,
      "a": { "component": "c3…", "body": "f1…", "ref": { "kind": "edge", "name": "…", "signature": {} }, "at": "center" },
      "b": { "component": "c2a4…", "body": "f9…", "ref": { "kind": "edge", "name": "…", "signature": {} }, "at": "center" },
      "flip": false, "angle": 0, "offset": 0,
      "value": "30 deg", "rest": "0 deg", "min": null, "max": null
    }
  ]
}
```

`value`, `rest`, `min` and `max` are formulas, like steps' numbers, so they can
use the assembly's parameters. `docs/file-format.md` gets all of this when it's
built, with a checked-in schema 2 file in `tests/files/`.

## How it's built

| Library | What it gets |
| --- | --- |
| `sb_kernel` | A `Placement` value type (origin and axes), with composing and inverting, and `Motion` to and from it: plain math, no OCCT. A Boolean common (for overlaps) if `IKernel` doesn't have one yet, with a corpus case. |
| `sb_model` | `Component`, `Joint`, `JointOrigin` and `PartSource` (inside, or linked with its path, id and fingerprint), held by a `Document` of kind `Assembly` with its parts as `Document`s of kind `Part`. Commands for every change (`AddComponentCommand`, `AddJointCommand`, `SetJointValueCommand`, `GroundCommand`, `UpdateLinkedPartCommand`, ...), so undo and scripting work. `placeComponents()`: the chain walk of part 4, with loops reported. Regenerating an assembly builds each part once. |
| `sb_io` | Reading and writing the layout above, schema 2, `link.json`, and finding linked files (part 1) through a callback, so tests use a folder of files. |
| `sb_view` | Shapes drawn with a placement, shared between components (OCCT's connected interactive objects), and picks that say which component. |
| `sb.assembly` (new plugin) | The Assemble menu, the joint tool, dragging along joints, the tree's Components and Joints. |
| `sketchbillet_cli` | The commands in part 5, `open --components`, `check --overlaps` and `parts`. |

## Building it

Each line is one pull request, in this order:

1. **Several designs open at once** in the desktop app (tabs), needed to open a
   linked part beside its assembly. Useful on its own: a part and its drawing,
   two parts to copy between.
2. **Assembly documents** with inside parts and free or grounded components:
   `sb_model`, `sb_io` (schema 2), `assembly.new`, `component.new`,
   `component.insert`, `open --components`.
3. **Linked parts:** links, finding files, fingerprints, updating, the saved
   copy, Find Linked File, switching between inside and linked.
4. **Assemblies in the app:** the tree, inserting parts, moving components,
   shared shapes in the viewer, Edit Part.
5. **Joints:** rigid, revolute and slider; placing along chains; loops checked;
   limits; dragging along joints; the joint tool; `joint.add`, `joint.set`.
6. **Checks:** overlaps, the assembly's mass and center of gravity, measuring
   between components.
7. **Exploded views.**
8. **Parts list,** and exporting each part once with its count; STEP export
   with the assembly's structure (parts, components and placements), and 3MF
   with every part.
9. **Make Parts from Bodies.**
10. **Guides:** user's guide pages for each piece as it's built, and two
    tutorials, a toy car whose wheels turn and a box with a hinged lid.

## Tested by

- **Placing:** property tests that, after placing, every joint's two origins
  meet (within 0.000001 mm) with their directions opposite; that a revolute
  joint's component keeps its origin where it was as its value changes, and
  turns by exactly that angle; that a slider moves by exactly its distance; that
  limits hold; and that loops are found and reported, never placed wrongly.
- **Linked parts:** change `wheel.sb` and open the car (updated); move it to
  another folder (found by id); delete it (the saved copy is used, and marked);
  a file loop (refused).
- **Files:** saving and opening every field, a checked-in schema 2 file, and
  an older reader refusing it.
- **Reference assemblies,** like Phase 2's reference parts: a hinged box, the
  toy car, a three-joint arm, and a four-bar linkage that shows the loop
  warning, each built on every change and checked against values worked out by
  hand (placements, overlaps, mass).

## Choices

- **Joints, not mates,** as the [Architecture](architecture.md) page decided:
  one joint per connection is easier for a student than three mates.
- **Inside and linked parts from the start** (decided Nov 13, 2026). Linked
  parts always keep a copy inside the assembly, so a missing file never stops
  an assembly opening: the same reason imported files are kept inside designs.
- **Chains first, loops later** (decided Nov 13, 2026). Exact placement covers
  most class projects and drags smoothly; loops are found and explained, not
  placed wrongly.
- **A linked part that changed updates by itself on open,** with a message and
  undo, rather than asking. Asking every time teaches students to click Yes
  without reading.
- **A component places the whole part,** all its bodies. Placing one body of a
  part is what Make Parts from Bodies is for.
- **One undo history per file.**

## Not yet

- Loops of joints (a numerical 3D solver), and Stage 4's mates.
- Cylindrical, pin-slot, planar and ball joints; motion links (gears, rack and
  pinion); contact; motion studies and animations.
- Sub-assemblies (an assembly inside an assembly).
- Editing a part with references to another component (in-context, top-down
  design), and parameters of the assembly driving parts' numbers (overrides, then
  configurations), from [SolidWorks-class scope](solidworks-scope.md).
- Watching linked files for changes while the assembly is open in another
  program.
- Very large assemblies (thousands of components): lightweight loading and
  levels of detail.

## Questions for review

- Is "updates by itself on open, with undo" right for linked parts, or should a
  teacher be able to lock an assembly to the copies it has (for grading)?
- Should a pair of components joined by a rigid joint be skipped by Check for
  Overlaps automatically, since they're often meant to touch or press-fit?
- Names: "Insert Part from File" and "Insert Copy of Part", or one Insert item
  with a "Link to the file" checkbox?
