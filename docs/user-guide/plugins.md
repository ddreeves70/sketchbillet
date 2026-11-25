# Plugins

A **plugin** adds features to SketchBillet: new commands today, and later panels, tools
and file formats. SketchBillet's own modeling commands come from a built-in plugin,
`sb.modeling`.

## See what's loaded

<!-- cli-example -->
```console
$ sketchbillet_cli plugins
loaded   sb.modeling 0.1.0  Modeling (built in)
           tier    foundation
           command assembly.new
           command component.add
           command component.delete
           command component.ground
           command component.insert
           command component.new
           command component.place
           command component.rename
           command document.set_units
           command explode.add
           command explode.add_step
           command explode.delete
           command explode.delete_step
           command explode.rename
           command explode.set_step
           command feature.add_boolean
           command feature.add_box
           command feature.add_chamfer
           command feature.add_cylinder
           command feature.add_extrude
           command feature.add_fillet
           command feature.add_hole
           command feature.add_mirror
           command feature.add_move
           command feature.add_pattern
           command feature.add_plane
           command feature.add_revolve
           command feature.add_shell
           command feature.add_sketch
           command feature.delete
           command feature.duplicate
           command feature.import
           command feature.reload_import
           command feature.rename
           command feature.set_color
           command feature.set_edges
           command feature.set_material
           command feature.set_sketch_face
           command feature.set_value
           command feature.slide
           command feature.suppress
           command feature.unlink
           command folder.add
           command folder.delete
           command folder.rename
           command joint.add
           command joint.delete
           command joint.reset
           command joint.set
           command material.define
           command material.delete
           command parameter.delete
           command parameter.rename
           command parameter.set
           command part.make_inside
           command part.relink
           command part.save_as
           command part.update
           command press_fit.add
           command press_fit.delete
           command sketch.add_arc
           command sketch.add_circle
           command sketch.add_line
           command sketch.add_point
           command sketch.add_pole
           command sketch.add_rectangle
           command sketch.add_spline
           command sketch.constrain
           command sketch.delete
           command sketch.import_spline
           command sketch.reload_spline
           command sketch.remove_pole
           command sketch.round_corner
           command sketch.set_spline
           command sketch.set_value
           command sketch.set_weight
           command sketch.trim
           command timeline.move
           command timeline.rollback
           action  Model/Create > Box  (feature.add_box)
           action  Model/Create > Cylinder  (feature.add_cylinder)
           action  Model/Combine > Union  (feature.add_boolean)
           action  Model/Combine > Cut  (feature.add_boolean)
           action  Model/Combine > Intersect  (feature.add_boolean)
           action  Model/Modify > Move  (feature.add_move)
           action  Model/Modify > Linked Copy  (feature.add_move)
           action  Model/Modify > Unlink Copy  (feature.unlink)
           action  Model/Create > Extrude  (feature.add_extrude)
           action  Model/Create > Revolve  (feature.add_revolve)
           action  Model/Create > Hole  (feature.add_hole)
           action  Model/Create > Hole from two edges  (feature.add_hole)
           action  Model/Pattern > Linear pattern  (feature.add_pattern)
           action  Model/Pattern > Circular pattern  (feature.add_pattern)
           action  Model/Pattern > Mirror  (feature.add_mirror)
           action  Model/Modify > Shell  (feature.add_shell)
           action  Model/Modify > Fillet  (feature.add_fillet)
           action  Model/Modify > Chamfer  (feature.add_chamfer)
           action  Model/Select > Pick edges  (check mark)
           action  Model/Steps > Duplicate step  (feature.duplicate)
           action  Model/Steps > Turn off  (feature.suppress)
           action  Model/Steps > Turn on  (feature.suppress)
           action  Model/Steps > New folder  (folder.add)
           action  Model/Steps > Delete step  (feature.delete)
           action  Assemble/Insert > Insert part  (component.insert)
           action  Assemble/Insert > Insert copy  (component.insert)
           action  Assemble/Insert > New part  (component.new)
           action  Assemble/Insert > Add another  (runs plugin code)
           action  Assemble/Component > Ground  (check mark)
           action  Assemble/Component > Delete component  (component.delete)
           action  Assemble/Edit > Edit part  (runs plugin code)
           action  Assemble/Edit > Open part  (opens a file)
           action  Assemble/Joints > Joint  (check mark)
           action  Assemble/Joints > Reset joints  (joint.reset)
           action  Assemble/Parts > Update linked parts  (part.update)
           action  Assemble/Parts > Find linked file  (part.relink)
           action  Assemble/Parts > Make part inside  (part.make_inside)
           action  Assemble/Parts > Save part as file  (part.save_as)
           action  Assemble/Explode > Explode  (check mark)
           action  Assemble/Explode > Add explode step  (runs plugin code)
```

## Use plugins from a folder

A plugin is a folder with a `plugin.json` file and a program file. Put plugin
folders inside one folder, and give that folder to `sketchbillet_cli` with `--plugins`.
The plugins built with SketchBillet (like the example Hello plugin) are in
`build/dev/sketchbillet-plugins`:

<!-- cli-example -->
```console
$ sketchbillet_cli run --plugins build/dev/sketchbillet-plugins --save cube.sb example.hello.add_cube size=25
cube: 1 step
  Cube1        ok  volume 15625.00 mm^3  mass 19.37 g (PLA)
[example.tape] The ruler shows lengths in in.
[example.tape] A 25.4 mm box is 1.00 in long.
[example.hello] Hello saw a change: Add Cube1
wrote cube.sb
```

Lines in `[brackets]` are messages from plugins.

## Safety

A plugin runs inside SketchBillet with the same access to your computer as SketchBillet itself.
Only use plugins from people you trust, the same as any program you install.

## A plugin that doesn't load

SketchBillet still starts, and [`sketchbillet_cli plugins`](../cli/plugins.md) shows the plugin as
`failed` with a message saying why and how to fix it. The most common reason is a
plugin made for a different version of SketchBillet.

Lines starting with `warning` are for the plugin's author: the plugin works, but
something about how it's put together is probably a mistake. You can keep using
it, and tell its author.

## Make your own

The [plugin developer guide](../plugin-guide/README.md) walks you through
writing one, starting with the Hello plugin.
