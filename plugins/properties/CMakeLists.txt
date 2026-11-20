# sb.properties: the Properties panel (Qt). Built only with the desktop app.
sb_add_library(sb_plugin_properties
  SOURCES
    PropertiesPlugin.cpp
  DEPS
    sb::ui
    sb::plugin
    sb::model
)
sb_embed_files(sb_plugin_properties HEADER PropertiesIcons.h NAMESPACE sb::icons::properties
  FILES icons/properties.svg)
