# sketchbillet_cli run

Runs one modeling command, such as "add a box" or "cut a hole", on a new or saved
design. Then it rebuilds the design, prints every step, and saves it.

```text
sketchbillet_cli run [--plugins <folder>]... [--open <in.sb>] [--save <out.sb>] [<command> [name=value]...]
```

| Option | Meaning |
| --- | --- |
| `<command>` | The modeling command to run, e.g. `feature.add_box`. See [Modeling commands](commands.md), or run [`sketchbillet_cli help`](help.md). Leave it out to open a design and save it again (with `--open` and `--save`) |
| `name=value` | One setting for the command, e.g. `size=40,30,10`. Settings that aren't required can be left out |
| `--open <in.sb>` | Start from this saved design. Without it, `run` starts a new, empty design |
| `--save <out.sb>` | Save the result here. It may be the same file as `--open`. A new design is named after this file |
| `--plugins <folder>` | Load the plugins in this folder, so their commands can run. Can be given more than once |

Without `--save`, nothing is saved: useful to try a command first.

## Build a part one command at a time

Each `run` adds one step. Save to the same file each time to build up a design:

<!-- cli-example -->
```console
$ sketchbillet_cli run --save block.sb feature.add_box size=40,30,10
block: 1 step
  Box1         ok  volume 12000.00 mm^3  mass 14.88 g (PLA)
wrote block.sb
$ sketchbillet_cli run --open block.sb --save block.sb feature.add_cylinder base=20,15,-1 radius=6 height=12
block: 2 steps
  Box1         ok  volume 12000.00 mm^3  mass 14.88 g (PLA)
  Cylinder1    ok  volume 1357.17 mm^3  mass 1.68 g (PLA)
wrote block.sb
$ sketchbillet_cli run --open block.sb --save block.sb feature.add_boolean operation=cut a=Box1 b=Cylinder1
block: 3 steps
  Box1         ok  volume 12000.00 mm^3
  Cylinder1    ok  volume 1357.17 mm^3
  Cut1         ok  volume 10869.03 mm^3  mass 13.48 g (PLA)
wrote block.sb
```

![The block with a hole cut through it](images/run-block.png)

Steps that you don't name get names like `Box1` and `Cut1`. Later commands use
those names, like `a=Box1`.

## Build an assembly

On an [assembly](../user-guide/assemblies.md), `run` prints its components and
parts instead of steps:

<!-- cli-example -->
```console
$ sketchbillet_cli run --save Wheel.sb feature.add_cylinder radius=12 height=6
...
$ sketchbillet_cli run --save Cart.sb assembly.new
Cart: assembly, 0 components, 0 parts
wrote Cart.sb
$ sketchbillet_cli run --open Cart.sb --save Cart.sb component.insert file=Wheel.sb count=2 step=30,0,0
Cart: assembly, 2 components, 1 part
  Wheel 1      Wheel      grounded at 0, 0, 0 mm
  Wheel 2      Wheel      free     at 30, 0, 0 mm
Parts:
  Wheel        1 step   ok        mass 3.37 g (PLA), 2 in the assembly
    linked to Wheel.sb
Mass in all: 6.73 g
wrote Cart.sb
```

## Save an assembly with its parts updated

Opening an assembly updates its [linked parts](../user-guide/assemblies.md#linked-and-inside-parts)
from their files. With no command, `run` opens a design and saves it again, so
the updates are kept:

<!-- cli-example -->
```console
$ sketchbillet_cli run --open Wheel.sb --save Wheel.sb feature.add_cylinder radius=4 height=10
...
$ sketchbillet_cli run --open Cart.sb --save Cart.sb
note [part.updated]: Wheel changed in Wheel.sb, so Cart uses the new Wheel.
...
```

## Try a command without saving

<!-- cli-example -->
```console
$ sketchbillet_cli run feature.add_box size=10,10,10
Untitled: 1 step
  Box1         ok  volume 1000.00 mm^3  mass 1.24 g (PLA)
```

## Run a plugin's command

<!-- cli-example -->
```console
$ sketchbillet_cli run --plugins build/dev/sketchbillet-plugins example.hello.add_cube size=25
Untitled: 1 step
  Cube1        ok  volume 15625.00 mm^3  mass 19.37 g (PLA)
[example.tape] The ruler shows lengths in in.
[example.tape] A 25.4 mm box is 1.00 in long.
[example.hello] Hello saw a change: Add Cube1
```

Lines in `[brackets]` are messages from plugins.

## When something is wrong

`run` checks the settings before changing anything, says what's wrong, and exits
with code 1. The design is not changed or saved.

A setting that's missing:

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli run feature.add_box
error [command.arg_missing]: Add box needs size: Width, depth and height in mm.
```

A value of the wrong kind:

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli run feature.add_box size=10,20
error: size must be three numbers in mm, like 10,20,30
```

A setting the command doesn't have:

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli run feature.add_box size=10,20,30 colour=red
error: Add box has no setting called colour. Its settings are: size corner name
```

A step that is added but doesn't build, such as a box with no depth. The step
shows `failed` with the reason, and `run` exits with code 1. With `--save`, the
design is still saved, so you can fix the step later:

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli run feature.add_box size=10,0,10
Untitled: 1 step
  Box1         failed
    error [kernel.box.size]: A box needs a width, depth and height greater than 0 mm.
```
