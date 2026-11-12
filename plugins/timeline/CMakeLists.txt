# sb.timeline: the timeline bar (Qt), with the steps in order and the rollback
# marker. Built only with the desktop app.
sb_add_library(sb_plugin_timeline
  SOURCES
    TimelinePlugin.cpp
  DEPS
    sb::ui
    sb::plugin
    sb::model
)
# Each step's icon is its toolbar button's picture, from the plugins that add
# those buttons, so there's only one copy of each.
sb_embed_files(sb_plugin_timeline HEADER TimelineIcons.h NAMESPACE sb::icons::timeline
  FILES ../modeling/icons/box.svg ../modeling/icons/cylinder.svg ../modeling/icons/union.svg
        ../modeling/icons/cut.svg ../modeling/icons/intersect.svg ../modeling/icons/move.svg
        ../modeling/icons/extrude.svg ../modeling/icons/fillet.svg ../modeling/icons/chamfer.svg
        ../modeling/icons/revolve.svg ../modeling/icons/hole.svg ../modeling/icons/pattern-linear.svg
        ../modeling/icons/pattern-circular.svg ../modeling/icons/mirror.svg ../modeling/icons/shell.svg
        ../sketcher/icons/new_sketch.svg ../sketcher/icons/new_plane.svg)
