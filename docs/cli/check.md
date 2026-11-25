# sketchbillet_cli check

Checks a design before you 3D print it: that its surface is closed all round, so
a slicer can tell what's inside it, and that no wall is too thin to print. It
looks at the same shape [`open --export`](open.md) writes.

For an assembly, it checks that no two components overlap: that no part
would have to be in the same place as another.

```text
sketchbillet_cli check <file.sb> [--step <name>] [--min-wall <mm>]
sketchbillet_cli check <assembly.sb> [--overlaps]
```

| Option | Meaning |
| --- | --- |
| `<file.sb>` | The design to check |
| `--step <name>` | Check this step's shape, instead of the design's one finished body |
| `--min-wall <mm>` | The thinnest wall that's OK, in mm. Default 0.8 mm: two lines from a 0.4 mm nozzle. Use less if your printer can print finer, like `--min-wall 0.5` for a 0.25 mm nozzle |
| `--overlaps` | Check an assembly for components that overlap. An assembly is always checked for that, so you can leave it out; it's an error on a part |

It exits with 0 if the part is ready to print (or no components overlap), and 1
if it isn't (or some do), or the design can't be opened. The [Getting ready to print](../user-guide/printing.md) page
explains what each problem means and how to fix it.

## Examples

The demo tag is ready to print:

<!-- cli-example -->
```console
$ sketchbillet_cli demo tag.sb
wrote tag.sb
$ sketchbillet_cli check tag.sb
Tag is ready to print: its surface is closed all round, and its thinnest wall is 4 mm (0.8 mm or more prints well).
```

A 0.5 mm shim is too thin for most printers:

<!-- cli-example -->
```console
$ sketchbillet_cli run --save shim.sb feature.add_box size=40,20,0.5 name=Shim
shim: 1 step
  Shim         ok  volume 400.00 mm^3  mass 0.50 g (PLA)
wrote shim.sb
```

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli check shim.sb
warning [print.thin]: Shim has walls thinner than 0.8 mm: the thinnest is 0.5 mm thick, near x 13.33, y 13.33, z 0 mm. Most printers can't print walls that thin well, so they may come out weak, with holes, or not at all. Make them at least 0.8 mm thick.
```

With `--min-wall 0.4`, for a printer that can print 0.4 mm walls, it's fine:

<!-- cli-example -->
```console
$ sketchbillet_cli check shim.sb --min-wall 0.4
Shim is ready to print: its surface is closed all round, and its thinnest wall is 0.5 mm (0.4 mm or more prints well).
```

### Overlaps in an assembly

A car with a body and four wheels in a row beside it. Nothing overlaps: only
pairs of components close enough to touch need checking, and these wheels are
1 mm apart and 8 mm from the body.

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
$ sketchbillet_cli run --open Car.sb --save Car.sb component.insert file=Wheel.sb count=4 at=0,-20,0 step=25,0,0
...
$ sketchbillet_cli check Car.sb
Car: no components overlap (5 components, 0 pairs close enough to check).
```

Move Wheel 2 so its middle is on the body's edge, and half of it is inside the
body. Wheel 3, put on top of the body's edge, only touches it, so that's fine:

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli run --open Car.sb --save Car.sb component.place "component=Wheel 2" origin=25,0,0
...
$ sketchbillet_cli run --open Car.sb --save Car.sb component.place "component=Wheel 3" origin=50,-12,10
...
$ sketchbillet_cli check Car.sb
warning [assembly.overlap]: Body 1 and Wheel 2 overlap by 1357.17 mm^3, around x 25, y 5.09, z 3 mm. Parts can't be in the same place, so they won't go together. Move one of them, or change one of their parts so they fit.
```

Half the wheel is inside: half of π × 12 mm × 12 mm × 6 mm is 1357.17 mm³.
Faces that only touch don't count, and nor does anything smaller than
0.001 mm³.

Two components meant to overlap, like an axle pushed into a hole a bit smaller
than it, are a **press fit**: mark them with
[`press_fit.add`](commands.md#press_fitadd), and `check` lists them apart, as
meant. If a press fit's components don't overlap at all, it says they'll be
loose.

## Errors

| Message | What to do |
| --- | --- |
| `this design has 2 finished bodies, and check looks at one` | Join them with `feature.add_boolean`, or pick one with `--step <name>` |
| `this design has no step called ...` | Check the name with [`sketchbillet_cli open`](open.md) |
| `--min-wall needs a thickness in mm bigger than 0` | Give a number, like `--min-wall 0.8` |
| `--overlaps checks an assembly, and ... is a part` | Leave out `--overlaps` to check a part for printing |
| `--step and --min-wall check a part for printing, and ... is an assembly` | Check each part's own `.sb` file for printing, or leave them out to look for overlaps |
| `warning [assembly.overlap]` | Two components overlap: move one, or change one of their parts so they fit. [Checking for overlaps](../user-guide/assemblies.md#checking-for-overlaps) explains more |
| `warning [assembly.press_fit_loose]` | Two components marked as a press fit don't overlap, so they'll be loose: make one a little bigger, or unmark it with [`press_fit.delete`](commands.md#press_fitdelete) |
| `warning [assembly.overlap_failed]` | The kernel couldn't compare two components' shapes: look at where they meet in the 3D view |
| `warning [print.thin]` | Some walls are too thin: see [Thin walls](../user-guide/printing.md#thin-walls) |
| `error [print.open]` | The surface has gaps: see [Gaps in the surface](../user-guide/printing.md#gaps-in-the-surface) |
