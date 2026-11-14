# When something goes wrong

SketchBillet explains every problem in plain words and says how to fix it. Each message
also has a **code** in brackets, like `[command.arg_missing]`. The code never
changes, so search for it here or quote it when you ask for help.

## A step failed

The step shows `failed`, with the reason under it:

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli run feature.add_box size=10,0,10
Untitled: 1 step
  Box1         failed
    error [kernel.box.size]: A box needs a width, depth and height greater than 0 mm.
```

Steps that use a failed step show `blocked`: fix the failed step first, and they
build again. A step is also `blocked` when it uses a step that's turned off
(`off`): turn that step back on (Edit > Turn on), or change the blocked step.

## A step has a warning

A warning means the step built, but probably not the way you meant:

| Code | Meaning | Fix |
| --- | --- | --- |
| `feature.used_twice` | The step uses a body that an earlier step already changed (like a cut from Box1 after Cut1 cut Box1), so it starts again from the body as it was, and makes a second body in the same place | Use the newest step instead, as the warning says (Cut1, not Box1): delete the step and make it again from the right one. See [Combining bodies](combining.md#chains-of-boolean-steps) |
| `extrude.cut_missed` | A cut didn't reach the body, so nothing was cut | Flip it, to go into the body, or make it longer |
| `dimension.formula` | A formula doesn't work any more (a parameter was deleted, say), so the number keeps its last value | Fix the formula, or the parameter it uses |

## A command was typed wrong

`sketchbillet_cli run` checks the settings first and changes nothing if one is wrong.

| What you see | Fix |
| --- | --- |
| `error [command.arg_missing]: Add box needs size: ...` | Add the setting it names |
| `error: size must be three numbers in mm, like 10,20,30` | Write x,y,z with commas and no spaces |
| `error: Add box has no setting called colour. Its settings are: ...` | Use one of the settings listed |
| `error [command.arg_feature]: ... points to a step that isn't in the design.` | Check the step's name with `sketchbillet_cli open` |
| `error: there is no command called ...` | Run `sketchbillet_cli help` to see the commands |

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli run feature.add_box
error [command.arg_missing]: Add box needs size: Width, depth and height in mm.
```

## A file won't open

| Code | Meaning | Fix |
| --- | --- | --- |
| `file.cant_read` | The file isn't there, or you aren't allowed to open it | Check the name and folder |
| `file.not_sketchbillet` | It isn't a SketchBillet design | Open `.sb` files only; export files (STL, 3MF, STEP) can't be opened as designs yet |
| `file.damaged` | Part of the file is missing or broken | Open an older copy |
| `file.too_new` | A newer SketchBillet saved it | Update SketchBillet |
| `file.unknown_step` | It uses a kind of step this SketchBillet doesn't know | Update SketchBillet, or add the plugin that makes that step |

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli open missing.sb
error [file.cant_read]: SketchBillet couldn't open missing.sb. Check that the file is still there and that you're allowed to open it. (missing.sb)
```

## Export didn't work

The desktop app shows these in the status bar after **File > Export...**:

| Code | Meaning | Fix |
| --- | --- | --- |
| `export.nothing` | There's no shape to export yet, or the picked step didn't build | Make a shape (Create > Box), or fix the step that failed |
| `export.which` | The design has two or more finished bodies, and Export writes one | Pick one step in the feature tree, or join the bodies with Combine > Union |
| `export.format` | The file name doesn't end with `.stl`, `.3mf`, `.step` or `.stp` | End the name with `.stl` or `.3mf` (3D printing) or `.step` (other CAD programs) |
| `export.stl.failed`, `export.step.failed`, `export.cant_write` | The file couldn't be written | Check the folder is still there, that you're allowed to save in it, and that the disk isn't full |

## Import didn't work

These show after **File > Import...** or `feature.import`, or on the Import step
when it builds:

| Code | Meaning | Fix |
| --- | --- | --- |
| `import.format` | The file isn't a STEP (`.step` or `.stp`), STL or 3MF file | Export the part from the other program as one of those |
| `import.no_file` | The file isn't where SketchBillet looked (it says where) | Check the file's name and folder. For Reload from file, put the file back where it was imported from |
| `import.empty`, `import.too_big` | The file is empty, or over 64 MB | Export the part again; for a big one, export just the piece you need |
| `import.step.unreadable` | The file ends in `.step` but isn't a STEP file SketchBillet can read | Export it again from the other program, as STEP AP203, AP214 or AP242 |
| `import.step.no_solid` | The file has only surfaces (skin), no solid with an inside | Export the part as a solid body |
| `import.stl.unreadable`, `import.3mf.unreadable` | The file ends in `.stl` or `.3mf` but isn't one SketchBillet can read | Export it again from the other program |
| `import.mesh.open` | The mesh has gaps or overlaps (it says how many edges), so it has no clear inside | Repair it first: most slicers have a Repair button, or use a mesh program like Meshmixer. Then export it again |
| `import.mesh.too_big` | The mesh has more than 100,000 triangles | Export it with fewer triangles (a coarser resolution), or simplify it in a mesh program |
| `import.mesh.failed`, `import.mesh.empty` | The mesh couldn't be made into a solid, or has no triangles | Repair it, or export it again |
| `import.unit` | `unit=` was given for a STEP or 3MF file, which say their own unit | Leave `unit=` out: it's only for STL files |

## The print check found a problem

**File > Check for Printing...** and `sketchbillet_cli check` show these. The
[Getting ready to print](printing.md) page has more on each.

| Code | What it means | How to fix it |
| --- | --- | --- |
| `print.thin` | Some walls are thinner than 0.8 mm (or the `--min-wall` you gave) | Make them thicker, or move a hole or pocket away from the edge |
| `print.open` | The surface has gaps, so a slicer can't tell what's inside | Make touching bodies overlap by 0.1 mm or more, and join them |
| `print.invalid` | The shape itself has problems | Look at the step that made it; try changing its sizes a little |
| `print.nothing`, `print.which` | There's no shape to check, or more than one and none is picked | Make a shape, or pick the step to check |

## Paste didn't work

The desktop app shows these in the status bar after **Edit > Paste**:

| Code | Meaning | Fix |
| --- | --- | --- |
| `paste.not_steps` | What's on the clipboard isn't steps copied from SketchBillet | Pick steps in the feature tree, Edit > Copy, then paste |
| `paste.too_new` | The steps were copied from a newer SketchBillet | Update SketchBillet, then paste again |
| `paste.damaged` | The copied text was changed or cut short | Copy the steps again |
| `paste.unknown_step` | A copied step is a kind this SketchBillet doesn't know | Update SketchBillet, or add the plugin that makes that kind of step |

## A parameter was refused

Each message says what's wrong and how to fix it. The codes:

| Code | Meaning | Fix |
| --- | --- | --- |
| `parameter.name` | The name can't be a parameter's | Use letters, digits and `_`, starting with a letter; not a unit like `mm` or a function like `sin` |
| `parameter.name_taken` | Another parameter has that name, maybe in other upper and lower case | Pick another name |
| `expression.unknown_name` | The formula uses a name that isn't a parameter | Check the spelling (case counts), or add that parameter first |
| `expression.units` | The units don't fit, like a length plus a plain number | Give both the same kind of unit: `wall + 2 mm` |
| `expression.unit` | A unit SketchBillet doesn't know, like `kg` | Use `mm`, `cm`, `m`, `in`, `ft`, `deg` or `rad` |
| `parameter.loop` | Parameters use each other in a loop | Change one of them to a number |
| `parameter.breaks` | The change would stop another parameter working | Fix that one first, as the message says |
| `parameter.in_use` | Another parameter, or a sketch dimension, uses the one you're deleting | Change or delete that one first |
| `slide.used` | The body you dragged (or slid) is used by a later step | Drag the finished body instead, the one the message names |
| `slide.sketch` | A sketch can't be slid like a body | Drag its lines with Select, or move the step made from it |
| `feature.number` | `feature.set_value` was given a number the step doesn't have, or no value or formula | Use one of the numbers the message lists, and give `value` or `formula` |
| `dimension.units` | A dimension's formula gives the wrong kind of value, like a plain number for a length | Give it a unit: `12 mm`, or `wall * 2` where `wall` is a length |
| `dimension.size` | A length, distance, radius, diameter or size would be 0 mm or less | Change the formula or the parameters it uses |
| `dimension.formula` | (A warning on rebuild.) A dimension's formula can't be worked out, maybe because it was copied from another design | Add the parameter it uses, or type a number for the dimension |

The [Parameters](parameters.md) page has what a formula can have.

## A plugin didn't load

Run [`sketchbillet_cli plugins`](../cli/plugins.md): it lists each plugin with the reason.
The plugin guide lists [every reason and its fix](../plugin-guide/manifest.md#when-sketchbillet-refuses-a-plugin).

## SketchBillet closed by itself

That's a crash: a bug in SketchBillet, not something you did wrong. Your design
is as it was when you last saved it, so open it again and carry on.

When SketchBillet crashes, it writes a short report on your computer. The next
time it starts, it asks whether you'd like to tell SketchBillet's makers:

- **Report It** opens a new issue on GitHub in your web browser, filled in with
  the report. Write what you were doing at the top (which buttons you clicked,
  what part you were making), then click **Create**. You need a free GitHub
  account. Crashes that are reported get fixed first.
- **Copy Report** copies the report, to paste into an email to your teacher or a
  message, if you don't have a GitHub account.
- **Don't Report** forgets it. Nothing is sent.

Click **Show Details** to read the whole report first. It has SketchBillet's
version, your computer's system (like Windows 11), when it crashed, what went
wrong, the last menu item or button you clicked, and where in SketchBillet's code
it happened. It never has your designs, file names, or anything you typed, and
nothing is sent unless you click Report It and then Create.

If it crashes again and again doing the same thing, try it a different way for
now (another tool, or smaller steps), and report it so it gets fixed. If it
crashes as soon as it starts, or the 3D view is blank, update your computer's
graphics driver ([Installing](install.md#tips)).

## Still stuck?

Ask in [GitHub Discussions](https://github.com/ddreeves70/khan/discussions).
Include the output of `sketchbillet_cli --version` and the message you got.
