# sb.measure: Tools > Measure. Click a face, edge or point in the view to see its
# size, or two to see the distance and angle between them, in a panel, with a
# line drawn between the nearest points. In an assembly: Check for Overlaps,
# Balance Point and the Parts List. Built only with the desktop app.
sb_add_library(sb_plugin_measure
  SOURCES
    MeasurePlugin.cpp
  DEPS
    sb::ui
    sb::plugin
    sb::model
    sb::io
)
sb_embed_files(sb_plugin_measure HEADER MeasureIcons.h NAMESPACE sb::icons::measure
  FILES icons/measure.svg icons/overlaps.svg icons/balance.svg icons/press_fit.svg icons/parts_list.svg)
