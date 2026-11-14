# SketchBillet user's guide

SketchBillet is a free, open-source CAD program for designing parts you can 3D print or
machine. This guide explains how SketchBillet works and everything it can do, with
pictures. It grows with SketchBillet: every new feature is added here when it's built.

![A desk organizer designed in SketchBillet](../tutorials/images/organizer-done.png)

## Where SketchBillet is today

SketchBillet is early. [Install it](install.md) on Windows, macOS or Linux, then
design parts in the [desktop app](desktop-app.md), with
menus, a 3D viewer, a feature tree and a Properties panel, or with `sketchbillet_cli`,
SketchBillet's command-line tool. Both run the same commands, so everything in this guide
works in either.

| You can do now | Coming next |
| --- | --- |
| Boxes, cylinders, union, cut, intersect | Pulling a face of a solid |
| Sketches with constraints and dimensions, drawn in the app or with `sketchbillet_cli` | |
| Extrude a sketch into a solid; revolve one around a line, for round parts | |
| Fillet and chamfer edges, picked in the 3D view; they stay on the same edges when earlier steps change | |
| Sketch on a flat face of a body; the sketch follows the face | |
| Extrude and revolve that join onto a body or cut into it (a peg, a pocket, a groove) | |
| Holes for screws: simple, counterbored, countersunk; at a sketch's points or in from two edges | Threads |
| Patterns: holes, joins, cuts or bodies, in a row, a grid or round a circle | |
| Mirror: holes, joins, cuts or a body, across a plane or a face | Mirrored copies as separate bodies |
| Construction planes: moved out and tilted from a base plane, a face or another plane, to sketch on | Planes through three points, or midway between two faces |
| Shell: hollow a body to thin walls, leaving faces open | Walls of different thicknesses |
| Name, rename and delete steps; undo and redo | |
| Move and turn bodies; copy, paste and duplicate steps; turn steps off; folders | |
| The timeline: roll back to see the part as it was, add steps in the middle, drag steps to reorder them | Folders shown in the timeline bar |
| The desktop app: 3D viewer with 1, 2 or 4 panes, feature tree, Properties, layouts | |
| Materials: mass, weight, material cost and color | |
| Parameters: named values like `wall = 3 mm`, with formulas and units, used by sketch dimensions and steps' numbers, with a Parameters panel in the app; formulas typed straight into Properties and the Sketch panel | Formulas for a pattern's count, typed in the app |
| Save and open `.sb` designs | |
| Export STL and 3MF (3D printing) and STEP (other CAD programs), from the app's File menu or `sketchbillet_cli` | |
| Check a part is ready to print: closed all round, no walls too thin | Overhang checks |
| Measure a face, edge or point, or between two: sizes, distances, angles, hole spacing, in the app (Tools > Measure) or with `sketchbillet_cli` | |
| Import parts from other CAD programs: STEP files, kept inside the design, with Reload from file | Import STL and 3MF |
| Add plugins | |

## Contents

1. [Installing SketchBillet](install.md): download it for Windows, macOS or Linux.
2. [Getting started](getting-started.md): get SketchBillet and make your first part.
3. [The desktop app](desktop-app.md): the window, the feature tree, Properties, layouts.
4. [Designs and steps](designs-and-steps.md): how a SketchBillet design works.
5. [Shapes](shapes.md): boxes and cylinders, and where they go.
6. [Combining bodies](combining.md): union, cut and intersect.
7. [Moving bodies](moving.md): slide and turn a body.
8. [Sketches](sketches.md): flat drawings with constraints and dimensions.
9. [Extrude](extrude.md): turn a sketch into a solid.
10. [Revolve](revolve.md): turn a sketch around a line, for round parts.
11. [Sketching on a face](sketch-on-face.md): draw on the top or side of a body.
12. [Fillet and chamfer](fillet-chamfer.md): round or bevel edges.
13. [Holes](holes.md): holes for screws, counterbored or countersunk.
14. [Patterns](patterns.md): repeat holes, pegs, slots or bodies in a row or round a circle.
15. [Mirror](mirror.md): model half a part, mirror it to make the whole.
16. [Construction planes](planes.md): planes to sketch on, moved out or tilted.
17. [Shell](shell.md): hollow a body out to thin walls, for boxes and enclosures.
18. [Materials](materials.md): what a body is made of, its mass, weight and cost.
19. [Parameters](parameters.md): named values like `wall = 3 mm`, and formulas that use them.
20. [The timeline](timeline.md): roll back, add steps in the middle, change the order.
21. [Files](files.md): saving, opening, and exporting for 3D printing.
22. [Importing parts](importing.md): parts from other CAD programs, from STEP files.
23. [Getting ready to print](printing.md): check for gaps and walls too thin to print.
24. [Measuring](measuring.md): sizes, distances and angles, like how far apart two holes are.
25. [Plugins](plugins.md): adding features to SketchBillet.
26. [When something goes wrong](troubleshooting.md): what SketchBillet's messages mean.
27. [Reference parts](reference-parts.md): twenty real parts to build, from brackets to knobs.

## Other guides

- [Tutorials](../tutorials/README.md): step-by-step projects.
- [sketchbillet_cli reference](../cli/README.md): every command, with examples.
- [Plugin developer guide](../plugin-guide/README.md): write your own plugins.

## Units

SketchBillet works in millimeters (mm). Volumes are in cubic millimeters (mm^3);
1000 mm^3 is 1 cm^3. A design can show its lengths in centimeters, meters,
inches or feet instead: see
[Document settings and units](desktop-app.md#document-settings-and-units).
Commands typed into `sketchbillet_cli` always take mm.
