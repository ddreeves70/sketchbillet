# sb.grid: the grid's settings, View tab items, status-bar text, drawing (as a
# view overlay) and snapping. Qt for its status-bar label and spacing dialog,
# so it's built only with the desktop app.
sb_add_library(sb_plugin_grid
  SOURCES
    GridPlugin.cpp
  DEPS
    sb::ui
    sb::plugin_viewoptions
    sb::plugin
    sb::kernel
)
sb_embed_files(sb_plugin_grid HEADER GridIcons.h NAMESPACE sb::icons::grid
  FILES icons/grid.svg icons/snap.svg)
