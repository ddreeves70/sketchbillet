# sb.viewoptions: keeps every view option (the grid's, the axes', any
# plugin's), builds their View tab items through addOption(), and adds the
# View options panel. Qt for the panel, so it's built only with the desktop app.
sb_add_library(sb_plugin_viewoptions
  SOURCES
    ViewOptionsPlugin.cpp
  DEPS
    sb::ui
    sb::plugin
)
sb_embed_files(sb_plugin_viewoptions HEADER ViewOptionsIcons.h NAMESPACE sb::icons::viewoptions
  FILES icons/view_options.svg)
