# sketchbillet_cli parts

Lists an assembly's **parts list** (a bill of materials): each part once, how
many components use it, what it's made of, and its mass each and in all. That's
what to print or make: one wheel, four times. With `--csv`, it also writes the
list as a spreadsheet file.

```text
sketchbillet_cli parts <assembly.sb> [--csv <out.csv>]
```

| Option | Meaning |
| --- | --- |
| `<assembly.sb>` | The assembly |
| `--csv <out.csv>` | Also write the list as CSV, for a spreadsheet like Excel, Google Sheets or LibreOffice Calc |

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

It exits with 0, or 1 if the design can't be opened or isn't an assembly.

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

## Errors

| Message | What to do |
| --- | --- |
| `... is a part, not an assembly, so it has no parts list` | Open an assembly. A part's mass is in [`open`](open.md) |
| `couldn't write ...` | Check the folder exists, and that you can save there |
| `note: ... has a problem, so its mass may be wrong` | Open the part to see what went wrong with it |

<!-- cli-example exit=1 -->
```console
$ sketchbillet_cli parts Body.sb
error: Body is a part, not an assembly, so it has no parts list. Open an assembly, or check this part's mass with open.
```
