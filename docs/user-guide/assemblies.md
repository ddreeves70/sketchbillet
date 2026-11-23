# Assemblies

An **assembly** puts parts together. A toy car is a body and four wheels; a
project box is a box and a lid. In an assembly you say which parts it uses, how
many of each, and where each one sits.

Each copy of a part in an assembly is a **component**. Four wheels are four
components, Wheel 1 to Wheel 4, all made from one Wheel part. Change the Wheel
part and all four change, because they're the same part.

Start one in the desktop app with **File > New assembly**, or with
`sketchbillet_cli` (below). While an assembly is open, the **Assemble** tab takes
the Model tab's place:

![The toy car open in the desktop app: the Assemble tab, and the feature tree listing its five components and two parts](images/app-assembly-car.png)

In the app, the feature tree lists the assembly's **Components** (each with its
part, and whether it's grounded or free) and its **Parts** (each linked to its
file, or inside the assembly). Click a component, in the tree or in the viewer,
to pick it: Properties shows its part, its file, where it's placed and which way
it faces. Rename it there, or tick **Grounded** to fix it where it is.
Right-click it in the tree to rename it, ground it or delete it.

| Assemble tab | What it does |
| --- | --- |
| **Insert part** | Puts a part from a `.sb` file in, linked to the file |
| **Insert copy** | Puts a copy of a part from a `.sb` file in, as an inside part |
| **New part** | Adds a new, empty part, made inside the assembly |
| **Add another** | Adds another copy of the picked component's part, beside it, facing the same way |
| **Ground** | Fixes the picked component where it is, or lets it go |
| **Delete** | Takes the picked component out |
| **Update** | Uses the new part from each linked file that changed |
| **Find file** | Links the picked component's part to its file where it is now |
| **Make part inside**, **Save part as file** | In the Parts dropdown: switch the picked component's part between inside and linked |

**Moving a component:** pick a free one, and drag the arrows or the square that
appear on it, in steps of 1 mm, like [dragging a body](moving.md#dragging-a-body).
Its picture moves while you drag; when you let go it's placed there, as one step
of Undo. A grounded component has no arrows: it stays where it is. Press
**Escape** before you let go to put it back.

Joints, which hold parts together and say how they move (a wheel turning on its
axle), come next.

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
    linked to Body.sb
  Wheel        1 step   ok        mass 3.37 g (PLA), 2 in the assembly
    linked to Wheel.sb
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
    linked to Body.sb
  Wheel        1 step   ok        mass 3.37 g (PLA), 2 in the assembly
    linked to Wheel.sb
Mass in all: 46.41 g
```

## Grounded and free

The first component in an assembly is **grounded**: fixed where it is. The
others are **free**. When joints come, they'll move the free components to fit
the grounded ones, so a car usually has its body grounded and its wheels free.
Fix or free a component with
[`component.ground`](../cli/commands.md#componentground).

## Linked and inside parts

A part in an assembly is either **linked** to its own `.sb` file, or **inside**
the assembly.

- **Linked parts** follow their file. Put one in with `component.insert`: change
  `Wheel.sb` later, and the car uses the new wheel the next time it opens. This is
  how a team works: one student makes the wheel, another the car.
- **Inside parts** live only in the assembly. Make one with `component.new`, or
  insert a file with `link=false` to keep a copy that doesn't change.

Either way, the assembly keeps a copy of every part, so `Car.sb` opens even on
a computer that doesn't have `Wheel.sb`.

Here the wheel gets a hub. Opening the car says it changed, and uses the new
wheel; saving the car keeps it:

<!-- cli-example -->
```console
$ sketchbillet_cli run --open Wheel.sb --save Wheel.sb feature.add_cylinder radius=4 height=10
...
$ sketchbillet_cli run --open Car.sb --save Car.sb
note [part.updated]: Wheel changed in Wheel.sb, so Car uses the new Wheel.
...
```

**When SketchBillet can't find a part's file**, it looks for it next to the
assembly, then in the folders inside the assembly's folder, even under another
name. If it finds it there, it says so, and links to it there from now on:

```text
note [part.moved]: Car found Wheel.sb in the parts folder, and links to it there now.
```

If the file is gone, the assembly uses the copy it kept, and says what to do:

```text
warning [part.not_found]: Wheel.sb can't be found, so Car uses the copy of Wheel saved on Nov 20, 2026. Put Wheel.sb back, or link Wheel to it where it is now with part.relink part="Wheel" file=<where it is>.
```

So keep an assembly and its parts' files in one folder (and the folders in
it): you can move or copy the whole folder, and everything still links up.

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
| Update linked parts from their files | [`part.update`](../cli/commands.md#partupdate), or just open the assembly |
| Link a part to its file where it is now | [`part.relink`](../cli/commands.md#partrelink) |
| Stop following the file: make a part inside | [`part.make_inside`](../cli/commands.md#partmake_inside) |
| Save an inside part to its own file, and link it | [`part.save_as`](../cli/commands.md#partsave_as) |
| List the components and parts | [`open --components`](../cli/open.md#an-assembly) |

Undo works for all of them in a script, like any other command.

## Tips

- **Open and save to keep updates.** Opening an assembly updates its linked
  parts, but only saving it keeps them: `sketchbillet_cli run --open Car.sb --save Car.sb`.
- **A file only counts if it holds the same design.** A different `Wheel.sb`,
  made from scratch, isn't the wheel the car uses, even with the same name.
  Copies of the file are fine.
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
