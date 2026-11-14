# Importing parts

**Import** a part made in another CAD program (Fusion, Onshape, SolidWorks,
FreeCAD, Tinkercad, ...) and build on it in SketchBillet: drill holes in it, sketch
on its faces, join your own parts to it, and 3D print the result. For now
SketchBillet imports **STEP** files (`.step` or `.stp`), the file almost every CAD
program can export. STL and 3MF files come next.

An imported part is one step, **Import**, with the part as a body. You can't
change how the other program made it, but every step after it works like on any
other body.

## In the desktop app

1. Pick **File > Import...** (Ctrl+I).
2. Pick the STEP file. The part appears as a new step, named after the file, and
   it's picked, so Properties shows it.
3. Build on it: pick it and another body and **Combine > Cut**, sketch on one of
   its faces with **Sketch > New sketch on a face**, or fillet its edges.

![The demo tag imported from tag.step: an Import step in the feature tree, with its file and Reload from file in Properties](images/import-app.png)

The design keeps **its own copy of the file**, saved inside the `.sb`. So the
design opens anywhere, like on a classmate's computer, even where the STEP file
isn't.

Changed the part in the other program? Export it again over the same file,
then click **Reload from file** in Properties. The steps built on it follow
the new part. **Edit > Undo** puts the old one back.

## An example

`sketchbillet_cli demo` can write the demo tag as a STEP file, as another
program would. Import it, then cut a second hole into the other end, for a strap:

<!-- cli-example -->
```console
$ sketchbillet_cli demo tag.step
volume: 7121.46 mm^3
wrote tag.step
$ sketchbillet_cli run --save strap.sb feature.import file=tag.step
strap: 1 step
  tag          ok  volume 7121.46 mm^3  mass 8.83 g (PLA)
wrote strap.sb
$ sketchbillet_cli run --open strap.sb --save strap.sb feature.add_cylinder base=51,15,-1 radius=2.5 height=6
strap: 2 steps
  tag          ok  volume 7121.46 mm^3  mass 8.83 g (PLA)
  Cylinder1    ok  volume 117.81 mm^3  mass 0.15 g (PLA)
wrote strap.sb
$ sketchbillet_cli run --open strap.sb --save strap.sb feature.add_boolean operation=cut a=tag b=Cylinder1 name=Strap
strap: 3 steps
  tag          ok  volume 7121.46 mm^3
  Cylinder1    ok  volume 117.81 mm^3
  Strap        ok  volume 7042.92 mm^3  mass 8.73 g (PLA)
wrote strap.sb
```

The tag came in at its real size: 7121.46 mm^3, the same as the one SketchBillet
made. The strap takes away another 117.81 mm^3, the part of the cylinder inside
the 4 mm thick tag.

See [`feature.import`](../cli/commands.md#featureimport) and
[`feature.reload_import`](../cli/commands.md#featurereload_import) for every setting.

## Tips

- **Export as a solid.** SketchBillet needs a solid part, with an inside. A
  surface model (just skin) can't be imported: export it from the other program
  as a solid body.
- **Units sort themselves out.** A STEP file says what unit it's in. A part made
  in inches comes in at its real size, shown in mm (or your design's unit).
- **One file, one step.** A STEP file with several separate parts comes in as one
  body made of all of them. To use one piece, cut or intersect it away.
- **The part is placed where the other program put it.** If it's far from the
  middle, add a **Move** step to bring it to where you want it.
- **Big files are slow.** Files up to 64 MB can be imported; a simpler export
  (just the part you need, without tiny details) builds faster.
- To send your part back to another program, [export](files.md#exporting) it as STEP.
