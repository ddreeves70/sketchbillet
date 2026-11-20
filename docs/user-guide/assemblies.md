# Assemblies

An **assembly** puts parts together. A toy car is a body and four wheels; a
project box is a box and a lid. In an assembly you say which parts it uses, how
many of each, and where each one sits.

Each copy of a part in an assembly is a **component**. Four wheels are four
components, Wheel 1 to Wheel 4, all made from one Wheel part. Change the Wheel
part and all four change, because they're the same part.

Today you build assemblies with `sketchbillet_cli`. The desktop app shows them
in a later version; until then it says to list them with
`sketchbillet_cli open car.sb --components`. Joints, which hold parts together
and say how they move (a wheel turning on its axle), come after that.

## An example: a toy car

First make the parts, each in its own file: a body 80 mm long, 40 mm wide and
10 mm tall, and a wheel 24 mm across and 6 mm thick.

<!-- cli-example -->
```console
$ sketchbillet_cli run --save Body.sb feature.add_box size=80,40,10
...
$ sketchbillet_cli run --save Wheel.sb feature.add_cylinder radius=12 height=6
...
```

Then make an empty assembly, and put the parts in it. `count=2` puts in two
wheels, and `step=60,0,0` puts the second one 60 mm along from the first.

<!-- cli-example -->
```console
$ sketchbillet_cli run --save Car.sb assembly.new
...
$ sketchbillet_cli run --open Car.sb --save Car.sb component.insert file=Body.sb
...
$ sketchbillet_cli run --open Car.sb --save Car.sb component.insert file=Wheel.sb count=2 at=10,-6,0 step=60,0,0
Car: assembly, 3 components, 2 parts
  Body 1       Body       grounded at 0, 0, 0 mm
  Wheel 1      Wheel      free     at 10, -6, 0 mm
  Wheel 2      Wheel      free     at 70, -6, 0 mm
Parts:
  Body         1 step   ok        mass 39.68 g (PLA), 1 in the assembly
  Wheel        1 step   ok        mass 3.37 g (PLA), 2 in the assembly
Mass in all: 46.41 g
wrote Car.sb
```

Each component is placed by its part's **origin** (the part's 0, 0, 0 point):
Wheel 1's origin is at x 10 mm, y -6 mm, z 0 mm in the car. The list ends with
each part's mass and the whole car's.

Wheels stand on their edge, so turn them: `z=0,-1,0` makes the wheel's own z
axis (the way its cylinder points) face along -y, out to the side of the car.
`open --components` shows which way each component faces.

<!-- cli-example -->
```console
$ sketchbillet_cli run --open Car.sb --save Car.sb component.place component="Wheel 1" origin=10,-6,12 z=0,-1,0
...
$ sketchbillet_cli run --open Car.sb --save Car.sb component.place component="Wheel 2" origin=70,-6,12 z=0,-1,0
...
$ sketchbillet_cli open Car.sb --components
Car: assembly, 3 components, 2 parts
  Body 1       Body       grounded at 0, 0, 0 mm
    facing x 1, 0, 0, z 0, 0, 1
  Wheel 1      Wheel      free     at 10, -6, 12 mm
    facing x 1, 0, 0, z 0, -1, 0
  Wheel 2      Wheel      free     at 70, -6, 12 mm
    facing x 1, 0, 0, z 0, -1, 0
Parts:
  Body         1 step   ok        mass 39.68 g (PLA), 1 in the assembly
  Wheel        1 step   ok        mass 3.37 g (PLA), 2 in the assembly
Mass in all: 46.41 g
```

## Grounded and free

The first component in an assembly is **grounded**: fixed where it is. The
others are **free**. When joints come, they'll move the free components to fit
the grounded ones, so a car usually has its body grounded and its wheels free.
Fix or free a component with
[`component.ground`](../cli/commands.md#componentground).

## What you can do

| To | Use |
| --- | --- |
| Make a new design an assembly | [`assembly.new`](../cli/commands.md#assemblynew) |
| Put in a part from a `.sb` file | [`component.insert`](../cli/commands.md#componentinsert) |
| Put in more of a part it has already | [`component.add`](../cli/commands.md#componentadd) |
| Add an empty part, to make later | [`component.new`](../cli/commands.md#componentnew) |
| Move or turn a component | [`component.place`](../cli/commands.md#componentplace) |
| Fix a component or let it go | [`component.ground`](../cli/commands.md#componentground) |
| Rename a component | [`component.rename`](../cli/commands.md#componentrename) |
| Take a component out | [`component.delete`](../cli/commands.md#componentdelete) |
| List the components and parts | [`open --components`](../cli/open.md#an-assembly) |

Undo works for all of them in a script, like any other command.

## Tips

- **An assembly keeps its own copy of each part.** Change `Wheel.sb` after
  inserting it and the car still has the old wheel. Linking parts to their
  files, so the car picks up changes, comes in a later version.
- **Inserting the same file again adds more of the same part**, not a second
  Wheel part: you get Wheel 3, Wheel 4, ...
- **Deleting the last component of a part takes the part out too.**
- **An assembly has no steps of its own.** Boxes, sketches and fillets go in its
  parts. Typing a step command for an assembly says so.
- **Assemblies can't go inside assemblies yet.** Insert the parts one at a time.
- **Exporting an assembly** to STL, 3MF or STEP comes in a later version. For
  now, export each part from its own file.
- `.sb` files with assemblies need this version of SketchBillet or newer. An
  older one says to update. The [file format](../file-format.md#assemblies-schema-2)
  page shows what's inside.
