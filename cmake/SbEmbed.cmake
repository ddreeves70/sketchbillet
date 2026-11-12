# sb_embed_files(): builds small text files (like SVG icons) into a target.
#
#   sb_embed_files(sb_plugin_modeling
#     HEADER ModelingIcons.h NAMESPACE sb::icons::modeling
#     FILES icons/box.svg icons/cylinder.svg)
#
# makes a header, included as "ModelingIcons.h", with one constant per file:
#
#   namespace sb::icons::modeling {
#   inline constexpr std::string_view box_svg = R"khembed(<svg ...>)khembed";
#   }
#
# The name is the file name with "." and "-" turned into "_". Changing a file
# re-runs CMake, which rewrites the header only if something changed.
function(sb_embed_files target)
  cmake_parse_arguments(ARG "" "HEADER;NAMESPACE" "FILES" ${ARGN})
  if(NOT ARG_HEADER OR NOT ARG_NAMESPACE OR NOT ARG_FILES)
    message(FATAL_ERROR "sb_embed_files(${target}) needs HEADER, NAMESPACE and FILES")
  endif()
  set(dir "${CMAKE_CURRENT_BINARY_DIR}/sb_embedded/${target}")
  set(text "// Made by sb_embed_files() (cmake/SbEmbed.cmake). Don't edit: change the files instead.\n")
  string(APPEND text "#pragma once\n\n#include <string_view>\n\nnamespace ${ARG_NAMESPACE} {\n\n")
  foreach(file IN LISTS ARG_FILES)
    # Absolute, with any ../ worked out: ninja must see one spelling of each file.
    get_filename_component(path "${CMAKE_CURRENT_SOURCE_DIR}/${file}" ABSOLUTE)
    get_filename_component(name "${file}" NAME)
    string(MAKE_C_IDENTIFIER "${name}" id)
    file(READ "${path}" body)
    if(body MATCHES "\\)khembed\"")
      message(FATAL_ERROR "${file} contains )khembed\", which sb_embed_files can't embed")
    endif()
    string(APPEND text "/// ${file}\ninline constexpr std::string_view ${id} = R\"khembed(${body})khembed\";\n\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
  endforeach()
  string(APPEND text "}  // namespace ${ARG_NAMESPACE}\n")
  set(out "${dir}/${ARG_HEADER}")
  set(old "")
  if(EXISTS "${out}")
    file(READ "${out}" old)
  endif()
  if(NOT old STREQUAL text)
    file(WRITE "${out}" "${text}")
  endif()
  target_include_directories(${target} PRIVATE "${dir}")
  # So IDEs list them: only the target's own files. A file from another target's
  # folder (../modeling/icons/box.svg) is listed there, and ninja won't list it twice.
  set(own "")
  foreach(file IN LISTS ARG_FILES)
    if(NOT file MATCHES "^\\.\\./")
      list(APPEND own "${file}")
    endif()
  endforeach()
  if(own)
    target_sources(${target} PRIVATE ${own})
  endif()
endfunction()
