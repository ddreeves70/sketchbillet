# sketchbillet_cli check

Checks a design before you 3D print it: that its surface is closed all round, so
a slicer can tell what's inside it, and that no wall is too thin to print. It
looks at the same shape [`open --export`](open.md) writes.

```text
sketchbillet_cli check <file.sb> [--step <name>] [--min-wall <mm>]
```

| Option | Meaning |
| --- | --- |
| `<file.sb>` | The design to check |
| `--step <name>` | Check this step's shape, instead of the design's one finished body |
| `--min-wall <mm>` | The thinnest wall that's OK, in mm. Default 0.8 mm: two lines from a 0.4 mm nozzle. Use less if your printer can print finer, like `--min-wall 0.5` for a 0.25 mm nozzle |

It exits with 0 if the part is ready to print, and 1 if it isn't, or the design
can't be opened. The [Getting ready to print](../user-guide/printing.md) page
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

## Errors

| Message | What to do |
| --- | --- |
| `this design has 2 finished bodies, and check looks at one` | Join them with `feature.add_boolean`, or pick one with `--step <name>` |
| `this design has no step called ...` | Check the name with [`sketchbillet_cli open`](open.md) |
| `--min-wall needs a thickness in mm bigger than 0` | Give a number, like `--min-wall 0.8` |
| `warning [print.thin]` | Some walls are too thin: see [Thin walls](../user-guide/printing.md#thin-walls) |
| `error [print.open]` | The surface has gaps: see [Gaps in the surface](../user-guide/printing.md#gaps-in-the-surface) |
