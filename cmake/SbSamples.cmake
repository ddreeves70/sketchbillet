# Sample designs, for the desktop app's File > Open Sample: each reference part
# (tests/reference/*.json) built with sketchbillet_cli into <build>/samples/<title>.sb,
# and <build>/samples/index.json listing them (file, title, group, order, about).
# The app finds the folder next to itself once installed (SbPackaging.cmake), or
# here in the build folder (SB_SAMPLES_DIR).
set(SB_SAMPLES_DIR "${CMAKE_BINARY_DIR}/samples" CACHE INTERNAL "Where the sample designs are built")

# Text for a JSON string: backslashes and quotes escaped.
function(sb_json_text out value)
  string(REPLACE "\\" "\\\\" value "${value}")
  string(REPLACE "\"" "\\\"" value "${value}")
  set(${out} "\"${value}\"" PARENT_SCOPE)
endfunction()

file(GLOB sb_sample_parts CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tests/reference/*.json")
set(sb_sample_files "")
set(sb_sample_index "[]")
set(sb_sample_n 0)
foreach(part IN LISTS sb_sample_parts)
  file(READ "${part}" json)
  get_filename_component(key "${part}" NAME_WE)
  string(JSON title GET "${json}" title)
  string(JSON group GET "${json}" group)
  string(JSON order GET "${json}" order)
  string(JSON about GET "${json}" about)
  set(out "${SB_SAMPLES_DIR}/${title}.sb")
  add_custom_command(OUTPUT "${out}"
    COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:sketchbillet_cli>" "-DPART=${part}" "-DOUT=${out}"
            -P "${PROJECT_SOURCE_DIR}/cmake/SbMakeSample.cmake"
    # Made again when the part changes, not every time sketchbillet_cli is rebuilt:
    # a sample is a saved design, and newer SketchBillets open older designs.
    DEPENDS "${part}" "${PROJECT_SOURCE_DIR}/cmake/SbMakeSample.cmake"
    COMMENT "Making the sample ${title}"
    VERBATIM)
  list(APPEND sb_sample_files "${out}")
  set(entry "{}")
  foreach(field key title group about)
    sb_json_text(text "${${field}}")
    string(JSON entry SET "${entry}" ${field} "${text}")
  endforeach()
  string(JSON entry SET "${entry}" file "\"${title}.sb\"")
  string(JSON entry SET "${entry}" order "${order}")
  string(JSON sb_sample_index SET "${sb_sample_index}" ${sb_sample_n} "${entry}")
  math(EXPR sb_sample_n "${sb_sample_n} + 1")
endforeach()
file(CONFIGURE OUTPUT "${SB_SAMPLES_DIR}/index.json" CONTENT "${sb_sample_index}\n" @ONLY)
add_custom_target(sb_samples ALL DEPENDS ${sb_sample_files})
add_dependencies(sb_samples sketchbillet_cli)
