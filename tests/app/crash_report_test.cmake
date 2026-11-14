# The crash reports end to end (apps/sketchbillet/CrashReport.h): the app clicks
# Create > Box, crashes on purpose, then the next run finds the report.
#
#   cmake -DAPP=<sketchbillet> -DDIR=<empty folder> -DHOW=segfault -P crash_report_test.cmake
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
execute_process(
  COMMAND "${APP}" --settings "${DIR}/settings.ini" --crash-folder "${DIR}/crashes"
          --trigger sb.modeling.box --crash-for-test ${HOW}
  RESULT_VARIABLE crashed OUTPUT_QUIET ERROR_QUIET)
if(crashed EQUAL 0)
  message(FATAL_ERROR "sketchbillet --crash-for-test ${HOW} didn't crash")
endif()
execute_process(
  COMMAND "${APP}" --settings "${DIR}/settings.ini" --crash-folder "${DIR}/crashes" --crash-report
  RESULT_VARIABLE found OUTPUT_VARIABLE report ERROR_QUIET)
if(NOT found EQUAL 0)
  message(FATAL_ERROR "No crash report after crashing (${HOW}):\n${report}")
endif()
foreach(expected "**Last thing clicked:** `sb.modeling.box`" "${EXPECT}" "stack:"
                 "https://github.com/ddreeves70/sketchbillet/issues/new?title=Crash%20in%20SketchBillet")
  string(FIND "${report}" "${expected}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "The crash report (${HOW}) doesn't say \"${expected}\":\n${report}")
  endif()
endforeach()
message(STATUS "crash report (${HOW}) found, with the last thing clicked")
