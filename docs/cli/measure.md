# sketchbillet_cli measure

Measures a design: the size of one face, edge or point, or the distance between
two, and the angle when they're flat or straight. It's how you check a part is
the size you meant, like how far apart two holes are.

```text
sketchbillet_cli measure <file.sb> <name> face|edge|point <n> [<name> face|edge|point <n>]
```

| Option | Meaning |
| --- | --- |
| `<file.sb>` | The design to measure |
| `<name>` | The step whose body it's on, like `Tag` |
| `face`, `edge` or `point` | What to measure on it |
| `<n>` | Which one: its number in [`open --faces`, `--edges` or `--points`](open.md#a-bodys-edges-and-faces) |

Give one face, edge or point to get its size, or two to measure between them. The
two can be on different bodies. It prints, in the design's unit:

| Line | Meaning |
| --- | --- |
| `Tag, flat face: area 120.00 mm^2` | What each one is, and its size: a face's area, an edge's length, a point's place. A round face or edge also gives its radius and how far it is across |
| `Distance: 4 mm (x 0, y 0, z 4 mm)` | The shortest distance between the two, and how far that is along x, y and z. `0 mm (they touch)` if they meet |
| `Straight across: 30.27 mm` | For two parallel things (flat faces or straight edges), the distance square across them, when it's different from the shortest distance |
| `Angle: 90°` | The angle between two flat faces, two straight edges, or one of each. `0° (they're parallel)` when they're parallel |
| `Centers: 42 mm apart` | For two round edges that go all the way round (like the rims of two holes), how far apart their centers are |

It exits with 0 when it measured everything, 1 if the design can't be opened or
something can't be measured, and 2 for a mistake in how it was typed.

## Examples

The demo tag is a 60 × 30 × 4 mm plate with a hole. First list its faces, to
find their numbers:

<!-- cli-example -->
```console
$ sketchbillet_cli demo tag.sb
wrote tag.sb
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

How thick is the tag? Measure from the bottom to the top:

<!-- cli-example -->
```console
$ sketchbillet_cli measure tag.sb Tag face 1 Tag face 7
Tag, flat face: area 1780.37 mm^2
Tag, flat face: area 1780.37 mm^2
Distance: 4 mm (x 0, y 0, z 4 mm)
Angle: 0° (they're parallel)
```

How big is the hole? Measure its side:

<!-- cli-example -->
```console
$ sketchbillet_cli measure tag.sb Tag face 4
Tag, cylinder face: radius 2.5 mm (5 mm across), area 62.83 mm^2
```

And how much plastic is left between the hole and the end of the tag? Measure
from the hole's side to the left end:

<!-- cli-example -->
```console
$ sketchbillet_cli measure tag.sb Tag face 4 Tag face 3
Tag, cylinder face: radius 2.5 mm (5 mm across), area 62.83 mm^2
Tag, flat face: area 120.00 mm^2
Distance: 6.5 mm (x -6.5, y 0, z 0 mm)
```

The front and the left end meet square:

<!-- cli-example -->
```console
$ sketchbillet_cli measure tag.sb Tag face 2 Tag face 3
Tag, flat face: area 240.00 mm^2
Tag, flat face: area 120.00 mm^2
Distance: 0 mm (they touch)
Angle: 90°
```

Points are corners, listed by `open --points`. Two opposite corners:

<!-- cli-example -->
```console
$ sketchbillet_cli measure tag.sb Tag point 1 Tag point 10
Tag, point: at x 0, y 0, z 0 mm
Tag, point: at x 60, y 30, z 4 mm
Distance: 67.2 mm (x 60, y 30, z 4 mm)
```

## Errors

A number that isn't in the list says how many there are, and how to list them:

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli measure tag.sb Tag face 9
error: Tag has 7 faces, numbered 1 to 7, so there's no face 9. `sketchbillet_cli open tag.sb --faces Tag` lists them.
```

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli measure tag.sb Lid face 1
error: this design has no step called Lid.
```

<!-- cli-example exit=2 -->
```console
$ sketchbillet_cli measure tag.sb Tag side 2
error: say face, edge or point after Tag, not side.
```

| Message | What to do |
| --- | --- |
| `this design has no step called ...` | Check the step's name: `sketchbillet_cli open <file.sb>` lists them |
| `... has no body to measure` | The step is a sketch, or it didn't build. Measure a step that made a body, or fix the step first |
| `... there's no face 9` | Use a number from `open --faces`, `--edges` or `--points` |
| `say face, edge or point after ...` | Type `face`, `edge` or `point` between the step's name and the number |
