# sketchbillet_cli parts

Lists an assembly's **parts list** (a bill of materials): each part once, how
many components use it, what it's made of, and its mass each and in all. That's
what to print or make: one wheel, four times. With `--csv`, it also writes the
list as a spreadsheet file, and with `--export`, a 3MF file with every part laid
out ready to print.

```text
sketchbillet_cli parts <assembly.sb> [--csv <out.csv>] [--export <out.3mf>]
```

| Option | Meaning |
| --- | --- |
| `<assembly.sb>` | The assembly |
| `--csv <out.csv>` | Also write the list as CSV, for a spreadsheet like Excel, Google Sheets or LibreOffice Calc |
| `--export <out.3mf>` | Also write every part as many times as it's used, side by side, as a 3MF file for a slicer (see [below](#print-every-part)) |

| Column | Meaning |
| --- | --- |
| `#` | The part's number in the list |
| `Part` | Its name |
| `Count` | How many components use it: how many to print or make |
| `Material` | What its bodies are made of (`-` for a part with no body yet) |
| `Mass each` | The mass of one, in grams |
| `Mass in all` | The mass of all of them: mass each × count |
| `Kept` | `linked to` its own `.sb` file, or `inside` the assembly |

The CSV has the same columns, with the masses as plain numbers in grams, the
volume of one (in the design's unit, cubed), and the linked file's name in a
column of its own.

It exits with 0, 1 if the design can't be opened, isn't an assembly or a file
can't be written, or 2 for a mistake in how it was run.

## Examples

A car: a body, and four wheels of one part.

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
$ sketchbillet_cli parts Car.sb
Car: 2 parts, 5 components
  # Part  Count Material Mass each Mass in all Kept
  1 Body      1 PLA        39.68 g     39.68 g linked to Body.sb
  2 Wheel     4 PLA         3.37 g     13.46 g linked to Wheel.sb
Mass in all: 53.14 g
```

Print the body once and the wheel four times: 39.68 g + 4 × 3.37 g of PLA.

As a spreadsheet, too:

<!-- cli-example -->
```console
$ sketchbillet_cli parts Car.sb --csv car-parts.csv
Car: 2 parts, 5 components
  # Part  Count Material Mass each Mass in all Kept
  1 Body      1 PLA        39.68 g     39.68 g linked to Body.sb
  2 Wheel     4 PLA         3.37 g     13.46 g linked to Wheel.sb
Mass in all: 53.14 g
wrote car-parts.csv
```

`car-parts.csv` holds:

```text
Item,Part,Count,Material,Mass each (g),Mass in all (g),Volume each (mm^3),Kept,File
1,Body,1,PLA,39.68,39.68,32000.00,linked,Body.sb
2,Wheel,4,PLA,3.37,13.46,2714.34,linked,Wheel.sb
```

A part added with [`component.new`](commands.md#componentnew) has no body until
you add a shape to it, so it weighs nothing yet:

<!-- cli-example -->
```console
$ sketchbillet_cli run --open Car.sb --save Car.sb component.new name=Axle
...
$ sketchbillet_cli parts Car.sb
Car: 3 parts, 6 components
  # Part  Count Material Mass each Mass in all Kept
  1 Body      1 PLA        39.68 g     39.68 g linked to Body.sb
  2 Wheel     4 PLA         3.37 g     13.46 g linked to Wheel.sb
  3 Axle      1 -           0.00 g      0.00 g inside
Mass in all: 53.14 g
note: Axle has no body yet, so it weighs nothing. Add a shape to it.
```

### Print every part

`--export` writes a 3MF file to open in a slicer: each part as many times as
components use it, in rows on the build plate (one row for each part, 5 mm
apart), each standing the way it was modeled with its lowest point on the
plate. Each part's mesh is in the file once, so four wheels are four copies of
one wheel, each named and in its material and color. Turn or move them in the
slicer to suit your printer, and leave out any you make another way:

<!-- cli-example -->
```console
$ sketchbillet_cli parts Car.sb --export car-parts.3mf
Car: 3 parts, 6 components
  # Part  Count Material Mass each Mass in all Kept
  1 Body      1 PLA        39.68 g     39.68 g linked to Body.sb
  2 Wheel     4 PLA         3.37 g     13.46 g linked to Wheel.sb
  3 Axle      1 -           0.00 g      0.00 g inside
Mass in all: 53.14 g
note: Axle has no body yet, so it weighs nothing. Add a shape to it.
wrote car-parts.3mf
```

A part with no body yet isn't in the file. To export the assembly put together
instead, use [`open --export`](open.md#export-an-assembly).

## Errors

| Message | What to do |
| --- | --- |
| `... is a part, not an assembly, so it has no parts list` | Open an assembly. A part's mass is in [`open`](open.md) |
| `couldn't write ...` | Check the folder exists, and that you can save there |
| `parts --export writes a 3MF file` | Give `--export` a name ending in `.3mf` |
| `... has no parts with a shape to export yet` | Add a shape to a part first |
| `note: ... has a problem, so its mass may be wrong` | Open the part to see what went wrong with it |

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli parts Body.sb
error: Body is a part, not an assembly, so it has no parts list. Open an assembly, or check this part's mass with open.
```
