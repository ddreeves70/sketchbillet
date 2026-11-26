# sketchbillet_cli open

Opens a saved design, rebuilds it, and prints every step with its volume, and
each finished body's mass and material. It can
also export the finished shape for 3D printing or another CAD program, list a
sketch as the solver left it, or list a body's edges, faces and points. An
[assembly](#an-assembly) lists its components and parts instead.

```text
sketchbillet_cli open <file.sb> [--export <out.stl|out.3mf|out.step> [--step <name>]]
sketchbillet_cli open <file.sb> --sketch <name>
sketchbillet_cli open <file.sb> --edges <name>
sketchbillet_cli open <file.sb> --faces <name>
sketchbillet_cli open <file.sb> --points <name>
sketchbillet_cli open <file.sb> --components
sketchbillet_cli open <assembly.sb> --edges <component>
sketchbillet_cli open <assembly.sb> --export <out.3mf>
```

| Option | Meaning |
| --- | --- |
| `<file.sb>` | The design to open |
| `--export <out.stl>` | Write the finished shape as an STL mesh, for slicers and 3D printing |
| `--export <out.3mf>` | Write the finished shape as a 3MF file, for slicers and 3D printing: a mesh in millimeters, named after its step, with its material and color |
| `--export <out.step>` | Write the finished shape as a STEP file, for other CAD programs (`.stp` works too) |
| `--step <name>` | Export the shape of this step instead, even if later steps use it |
| `--sketch <name>` | Also list this sketch: each line, circle, arc, spline and point where the solver put it (in the design's unit, angles in degrees; a spline lists its degree and control points), each constraint (a dimension with a formula shows it after its number, like `40.00 mm = wall * 10`), then its closed shapes (with their areas) and any ends that aren't joined |
| `--edges <name>` | Also list the edges of this step's body, numbered, as [fillet and chamfer](commands.md#featureadd_fillet) take them (see below) |
| `--faces <name>` | Also list the faces of this step's body, numbered |
| `--points <name>` | Also list the points (corners) of this step's body, numbered |
| `--components` | For an [assembly](#an-assembly): also say which way each component faces |
| `--export <out.3mf>` | For an assembly: write it as a 3MF file, put together, each part once with a copy where each component is (see [below](#export-an-assembly)) |
| `--edges "Wheel 1"` | For an assembly: list a component's edges (or `--faces`, `--points`), numbered as [joints](commands.md#jointadd) take them (see [below](#a-components-edges-faces-and-points)) |

Each step shows one of these:

| Word | Meaning |
| --- | --- |
| `ok` | The step built. Its volume is shown; a finished body also shows its mass and [material](../user-guide/materials.md). A [sketch](../user-guide/sketches.md) shows what's in it and how free it still is |
| `failed` | The step couldn't be built. The line under it says why |
| `blocked` | The step uses another step that failed or is turned off |
| `off` | The step is turned off (suppressed) |
| `rolled back` | The step is after the [rollback marker](../user-guide/timeline.md), so it isn't built. A line, `-- rolled back to here: the steps below aren't built --`, shows where the marker is |

A step number that comes from a formula is listed under its step, like
`    distance = wall * 4  (12 mm)`. A design with
[parameters](../user-guide/parameters.md) lists them after its steps, each with its formula and, if that isn't just a number, what it works out
to. A parameter SketchBillet can't work out shows the problem under it.

## Examples

First make a design to open (see [`sketchbillet_cli demo`](demo.md)):

<!-- cli-example -->
```console
$ sketchbillet_cli demo tag.sb
wrote tag.sb
$ sketchbillet_cli open tag.sb
Demo tag: 3 steps
  Plate        ok  volume 7200.00 mm^3
  Hole         ok  volume 117.81 mm^3
  Tag          ok  volume 7121.46 mm^3  mass 8.83 g (PLA)
```

Export it for 3D printing:

<!-- cli-example -->
```console
$ sketchbillet_cli open tag.sb --export tag.stl
Demo tag: 3 steps
  Plate        ok  volume 7200.00 mm^3
  Hole         ok  volume 117.81 mm^3
  Tag          ok  volume 7121.46 mm^3  mass 8.83 g (PLA)
wrote tag.stl
```

Or as 3MF, which keeps the part's name, material and color:

<!-- cli-example -->
```console
$ sketchbillet_cli open tag.sb --export tag.3mf
Demo tag: 3 steps
  Plate        ok  volume 7200.00 mm^3
  Hole         ok  volume 117.81 mm^3
  Tag          ok  volume 7121.46 mm^3  mass 8.83 g (PLA)
wrote tag.3mf
```

Export just the hole's cylinder:

<!-- cli-example -->
```console
$ sketchbillet_cli open tag.sb --export hole.step --step Hole
Demo tag: 3 steps
  Plate        ok  volume 7200.00 mm^3
  Hole         ok  volume 117.81 mm^3
  Tag          ok  volume 7121.46 mm^3  mass 8.83 g (PLA)
wrote hole.step
```

## More than one body

`--export` writes one body. A design with two separate bodies needs `--step`, or
a [`feature.add_boolean`](commands.md#featureadd_boolean) to join them:

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli run --save two.sb feature.add_box size=10,10,10
two: 1 step
  Box1         ok  volume 1000.00 mm^3  mass 1.24 g (PLA)
wrote two.sb
$ sketchbillet_cli run --open two.sb --save two.sb feature.add_box corner=20,0,0 size=5,5,5
two: 2 steps
  Box1         ok  volume 1000.00 mm^3  mass 1.24 g (PLA)
  Box2         ok  volume 125.00 mm^3  mass 0.15 g (PLA)
wrote two.sb
$ sketchbillet_cli open two.sb --export two.stl
two: 2 steps
  Box1         ok  volume 1000.00 mm^3  mass 1.24 g (PLA)
  Box2         ok  volume 125.00 mm^3  mass 0.15 g (PLA)
error: this design has 2 finished bodies, and export writes one. Combine them with feature.add_boolean, or pick one with --step <name>.
```

## A sketch, solved

A sketch is a flat drawing that the solver makes exact (see
[Sketches](../user-guide/sketches.md)). `--sketch` lists it as solved:

<!-- cli-example -->
```console
$ sketchbillet_cli run --save disc.sb feature.add_sketch
disc: 1 step
  Sketch1      ok  empty
wrote disc.sb
$ sketchbillet_cli run --open disc.sb --save disc.sb sketch.add_circle sketch=Sketch1 center=1,2 radius=3
disc: 1 step
  Sketch1      ok  1 circle, 0 constraints, free to move 3 ways
wrote disc.sb
$ sketchbillet_cli run --open disc.sb --save disc.sb sketch.constrain sketch=Sketch1 kind=diameter on=Circle1 value=10
disc: 1 step
  Sketch1      ok  1 circle, 1 constraint, free to move 2 ways
wrote disc.sb
$ sketchbillet_cli open disc.sb --sketch Sketch1
disc: 1 step
  Sketch1      ok  1 circle, 1 constraint, free to move 2 ways
Sketch1 (XY plane): free to move 2 ways
  Circle1        circle  center 1.00, 2.00  radius 5.00
  Diameter1      diameter            Circle1  10.00 mm
closed shapes: 1
  shape 1  Circle1  area 78.54 mm^2
```

Last come the sketch's **closed shapes**, which [extrude](../user-guide/extrude.md) turns into solids:
each shape's lines, arcs and circles in order around it, its area, and any holes
in it. Ends of lines and arcs that nothing else meets are listed as
`open ends`; a shape with an open end isn't closed yet. A `warning` says when
three or more pieces meet at one point, or when shapes cross, since SketchBillet can't
tell which closed shape those belong to.

A sketch whose constraints conflict is listed as drawn, with `(conflicts)` after
the constraints that fight.

## A body's edges and faces

`--edges` lists the edges of a step's body, `--faces` its faces, and `--points`
its points, each numbered. A [fillet](commands.md#featureadd_fillet) or
[chamfer](commands.md#featureadd_chamfer) takes edges by these numbers, and
[`measure`](measure.md) takes all three. They're
listed bottom to top, then front to back, then left to right (by where each
middle is), each with what it is (a straight `line`, a `circle`; a `flat` face, a
`cylinder`), its length or area, where its middle is, and where it came from:

<!-- cli-example -->
```console
$ sketchbillet_cli open tag.sb --faces Tag
Demo tag: 3 steps
  Plate        ok  volume 7200.00 mm^3
  Hole         ok  volume 117.81 mm^3
  Tag          ok  volume 7121.46 mm^3  mass 8.83 g (PLA)
faces of Tag: 7
   1  flat       1780.37 mm^2  at 30.23,15.00,0.00  bottom of Plate
   2  flat        240.00 mm^2  at 30.00,0.00,2.00  front side of Plate
   3  flat        120.00 mm^2  at 0.00,15.00,2.00  left side of Plate
   4  cylinder     62.83 mm^2  at 9.00,15.00,2.00  side of Hole
   5  flat        120.00 mm^2  at 60.00,15.00,2.00  right side of Plate
   6  flat        240.00 mm^2  at 30.00,30.00,2.00  back side of Plate
   7  flat       1780.37 mm^2  at 30.23,15.00,4.00  top of Plate
```

An edge is named for the two faces it runs between:

<!-- cli-example -->
```console
$ sketchbillet_cli open tag.sb --edges Hole
Demo tag: 3 steps
  Plate        ok  volume 7200.00 mm^3
  Hole         ok  volume 117.81 mm^3
  Tag          ok  volume 7121.46 mm^3  mass 8.83 g (PLA)
edges of Hole: 3
   1  circle         15.71 mm  at 9.00,15.00,-1.00  between bottom of Hole and side of Hole
   2  line            6.00 mm  at 11.50,15.00,2.00  seam of side of Hole
   3  circle         15.71 mm  at 9.00,15.00,5.00  between side of Hole and top of Hole
```

The hole's side has a **seam**: an edge down its length where the round face meets
itself. A step with no body to list says so:

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli open tag.sb --edges Missing
Demo tag: 3 steps
  Plate        ok  volume 7200.00 mm^3
  Hole         ok  volume 117.81 mm^3
  Tag          ok  volume 7121.46 mm^3  mass 8.83 g (PLA)
error: this design has no step called Missing.
```

`--points` lists a body's points: its corners, and where a seam meets an edge.
Each is named for the faces that meet there:

<!-- cli-example -->
```console
$ sketchbillet_cli open tag.sb --points Tag
Demo tag: 3 steps
  Plate        ok  volume 7200.00 mm^3
  Hole         ok  volume 117.81 mm^3
  Tag          ok  volume 7121.46 mm^3  mass 8.83 g (PLA)
points of Tag: 10
   1  point                    at 0.00,0.00,0.00  corner of bottom of Plate, front side of Plate and left side of Plate
   2  point                    at 60.00,0.00,0.00  corner of bottom of Plate, front side of Plate and right side of Plate
   3  point                    at 11.50,15.00,0.00  corner of bottom of Plate and side of Hole
   4  point                    at 0.00,30.00,0.00  corner of back side of Plate, bottom of Plate and left side of Plate
   5  point                    at 60.00,30.00,0.00  corner of back side of Plate, bottom of Plate and right side of Plate
   6  point                    at 0.00,0.00,4.00  corner of front side of Plate, left side of Plate and top of Plate
   7  point                    at 60.00,0.00,4.00  corner of front side of Plate, right side of Plate and top of Plate
   8  point                    at 11.50,15.00,4.00  corner of side of Hole and top of Plate
   9  point                    at 0.00,30.00,4.00  corner of back side of Plate, left side of Plate and top of Plate
  10  point                    at 60.00,30.00,4.00  corner of back side of Plate, right side of Plate and top of Plate
```

## An assembly

An [assembly](commands.md#assemblynew) has no steps of its own, so `open` lists
its components instead: each one's part, whether it's grounded (fixed where it
is), joined (placed by a [joint](commands.md#jointadd)) or free, and where its
part's origin is. Then its joints, if it has any: what each one joins, its kind,
how far it's turned or slid, its limits, and anything wrong with it. Then its parts, each built once
however many components use it, with its steps, mass and material, and how many
are in the assembly, and the file each linked part is linked to. Last, the mass
of everything together, its weight on Earth in newtons (N), and its **balance
point** (its center of gravity): where in the assembly it would balance on a
pin, as x, y and z. A component whose part has no bodies yet isn't counted.

Opening an assembly first updates its [linked parts](../user-guide/assemblies.md#linked-and-inside-parts)
from their files, and says what changed, before the list:

```text
note [part.updated]: Wheel changed in Wheel.sb, so Car uses the new Wheel.
```

A linked part whose file can't be found says so, and the assembly uses the copy
it keeps. `open` doesn't save anything: to keep updated parts, save the assembly
with [`run --open Car.sb --save Car.sb`](run.md#save-an-assembly-with-its-parts-updated).

<!-- cli-example -->
```console
$ sketchbillet_cli run --save Wheel.sb feature.add_cylinder radius=12 height=6
...
$ sketchbillet_cli run --save Car.sb assembly.new
...
$ sketchbillet_cli run --open Car.sb --save Car.sb component.insert file=Wheel.sb count=4 step=25,0,0
...
$ sketchbillet_cli open Car.sb
Car: assembly, 4 components, 1 part
  Wheel 1      Wheel      grounded at 0, 0, 0 mm
  Wheel 2      Wheel      free     at 25, 0, 0 mm
  Wheel 3      Wheel      free     at 50, 0, 0 mm
  Wheel 4      Wheel      free     at 75, 0, 0 mm
Parts:
  Wheel        1 step   ok        mass 3.37 g (PLA), 4 in the assembly
    linked to Wheel.sb
Mass in all: 13.46 g (weighs 0.132 N)
Balance point: 37.5, 0, 3 mm
```

`--components` also says which way each component faces: where its part's x and
z directions point. Listing its parts' sketches comes in a later version:

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli open Car.sb --sketch Sketch1
error: Car is an assembly. List it with --components or --exploded, a component's edges, faces or points with --edges "Wheel 1", or export it with --export Car.3mf; listing its sketches comes in a later version.
```

### Export an assembly

`--export` with a `.3mf` name writes the assembly put together, as it looks in
the app. Each part's mesh is in the file once, and each component is a copy of
it, moved and turned to where the component is: four wheels share one mesh. A
slicer shows the parts by name, each in its material and color, so you can see
the whole thing, or print it in one go if it fits on the plate and its parts
don't touch.

<!-- cli-example -->
```console
$ sketchbillet_cli open Car.sb --export car.3mf
Car: assembly, 4 components, 1 part
  Wheel 1      Wheel      grounded at 0, 0, 0 mm
  Wheel 2      Wheel      free     at 25, 0, 0 mm
  Wheel 3      Wheel      free     at 50, 0, 0 mm
  Wheel 4      Wheel      free     at 75, 0, 0 mm
Parts:
  Wheel        1 step   ok        mass 3.37 g (PLA), 4 in the assembly
    linked to Wheel.sb
Mass in all: 13.46 g (weighs 0.132 N)
Balance point: 37.5, 0, 3 mm
wrote car.3mf
```

To print each part as many times as it's used, laid out side by side on the
plate instead, use [`parts --export`](parts.md#print-every-part).

An assembly exports only to 3MF for now. STL has no names or copies, and STEP
with each part and component in it comes in a later version:

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli open Car.sb --export car.stl
error: Car is an assembly, and for now it exports to 3MF only, with each part in its place. Use a name ending in .3mf, or export one part at a time from its own file.
```

### A component's edges, faces and points

`--edges`, `--faces` and `--points` with a component's name list its part's
edges, faces or points, numbered, to pick the sides of a
[joint](commands.md#jointadd) with. Where they are is in the part's own place,
as if the part's origin were at 0, 0, 0. A part with more than one body names
the body too: `--edges "Wheel 1:Hub"`.

<!-- cli-example -->
```console
$ sketchbillet_cli open Car.sb --faces "Wheel 2"
Car: assembly, 4 components, 1 part
  Wheel 1      Wheel      grounded at 0, 0, 0 mm
  Wheel 2      Wheel      free     at 25, 0, 0 mm
  Wheel 3      Wheel      free     at 50, 0, 0 mm
  Wheel 4      Wheel      free     at 75, 0, 0 mm
Parts:
  Wheel        1 step   ok        mass 3.37 g (PLA), 4 in the assembly
    linked to Wheel.sb
Mass in all: 13.46 g (weighs 0.132 N)
Balance point: 37.5, 0, 3 mm
faces of Cylinder1: 3
   1  flat        452.39 mm^2  at 0.00,0.00,0.00  bottom of Cylinder1
   2  cylinder    452.39 mm^2  at 0.00,0.00,3.00  side of Cylinder1
   3  flat        452.39 mm^2  at 0.00,0.00,6.00  top of Cylinder1
```

### An exploded view

`--exploded` with an exploded view's name (see
[`explode.add`](commands.md#explodeadd)) lists where it draws each component it
moves, and how far it moved it. The components themselves stay where they are.

<!-- cli-example -->
```console
$ sketchbillet_cli run --save Body.sb feature.add_box size=80,40,10
...
$ sketchbillet_cli run --save Wheel.sb feature.add_cylinder radius=12 height=6
...
$ sketchbillet_cli run --save Car.sb assembly.new
...
$ sketchbillet_cli run --open Car.sb --save Car.sb component.insert file=Body.sb
...
$ sketchbillet_cli run --open Car.sb --save Car.sb component.insert file=Wheel.sb count=2 at=10,-6,0 step=60,0,0
...
$ sketchbillet_cli run --open Car.sb --save Car.sb explode.add
...
$ sketchbillet_cli run --open Car.sb --save Car.sb explode.add_step view="Exploded 1" components="Wheel 1, Wheel 2" direction=0,-1,0 distance=20
...
$ sketchbillet_cli open Car.sb --exploded "Exploded 1"
Car: assembly, 3 components, 2 parts
  Body 1       Body       grounded at 0, 0, 0 mm
  Wheel 1      Wheel      free     at 10, -6, 0 mm
  Wheel 2      Wheel      free     at 70, -6, 0 mm
Exploded views:
  Exploded 1: 1 step
    1. Wheel 1, Wheel 2: 20 mm along 0, -1, 0
Parts:
  Body         1 step   ok        mass 39.68 g (PLA), 1 in the assembly
    linked to Body.sb
  Wheel        1 step   ok        mass 3.37 g (PLA), 2 in the assembly
    linked to Wheel.sb
Mass in all: 46.41 g (weighs 0.455 N)
Balance point: 40, 16.23, 4.71 mm
In Exploded 1:
  Wheel 1      at 10, -26, 0 mm (moved 0, -20, 0 mm)
  Wheel 2      at 70, -26, 0 mm (moved 0, -20, 0 mm)
```

## Files that can't be opened

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli open missing.sb
error [file.cant_read]: SketchBillet couldn't open missing.sb. Check that the file is still there and that you're allowed to open it. (missing.sb)
```

The [file format page](../file-format.md) lists every reason a file can be refused.
