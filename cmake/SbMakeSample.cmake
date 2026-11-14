# Builds one sample design from a reference part (tests/reference/<part>.json):
# runs its steps with sketchbillet_cli, one at a time, and saves the design as OUT.
# The design is named after OUT's file name, so OUT is "<title>.sb".
#
#   cmake -DCLI=<sketchbillet_cli> -DPART=<part.json> -DOUT=<dir/Title.sb> -P SbMakeSample.cmake
#
# Run by the custom commands in SbSamples.cmake; fails the build if a step fails.
file(READ "${PART}" json)
string(JSON count LENGTH "${json}" steps)
get_filename_component(dir "${OUT}" DIRECTORY)
get_filename_component(name "${OUT}" NAME)
# Made in a folder of its own, then moved, so a failed build leaves no half-made sample.
string(MD5 tag "${OUT}")
set(work "${dir}/.making-${tag}")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
math(EXPR last "${count} - 1")
foreach(i RANGE ${last})
  string(JSON step GET "${json}" steps ${i})
  separate_arguments(args UNIX_COMMAND "${step}")
  set(open "")
  if(i GREATER 0)
    set(open --open "${name}")
  endif()
  execute_process(COMMAND "${CLI}" run ${open} --save "${name}" ${args}
    WORKING_DIRECTORY "${work}" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${PART}: '${step}' failed:\n${out}")
  endif()
endforeach()
file(RENAME "${work}/${name}" "${OUT}")
file(REMOVE_RECURSE "${work}")
