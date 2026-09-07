if(NOT DEFINED SVG_SQUISHER_EXE)
  message(FATAL_ERROR "SVG_SQUISHER_EXE is required")
endif()

set(test_root "${CMAKE_CURRENT_BINARY_DIR}/cli-test")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")
file(WRITE "${test_root}/input.svg"
  "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\"><rect width=\"20\" height=\"20\"/></svg>")

execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" input.svg output.svg --report report.json
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE conversion_result
  OUTPUT_VARIABLE conversion_output
  ERROR_VARIABLE conversion_error
  TIMEOUT 10
)
if(NOT conversion_result EQUAL 0)
  message(FATAL_ERROR "Basename conversion failed: ${conversion_error}")
endif()
if(NOT EXISTS "${test_root}/output.svg" OR NOT EXISTS "${test_root}/report.json")
  message(FATAL_ERROR "Conversion did not create output and report files")
endif()

file(READ "${test_root}/report.json" report)
if(NOT report MATCHES "\"status\":\"converted\"")
  message(FATAL_ERROR "Report does not record a converted file")
endif()
if(NOT report MATCHES "\"generatorVersion\":[ ]*\"[0-9]+\\.[0-9]+\\.[0-9]+\"")
  message(FATAL_ERROR "Report does not record the converter version")
endif()

file(READ "${test_root}/input.svg" original_input)
execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" input.svg input.svg
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE same_path_result
  OUTPUT_QUIET
  ERROR_VARIABLE same_path_error
  TIMEOUT 10
)
if(same_path_result EQUAL 0 OR NOT same_path_error MATCHES "--in-place")
  message(FATAL_ERROR "Default same-path conversion was not rejected clearly: ${same_path_error}")
endif()
file(READ "${test_root}/input.svg" input_after_rejection)
if(NOT input_after_rejection STREQUAL original_input)
  message(FATAL_ERROR "Rejected same-path conversion changed the input")
endif()

file(WRITE "${test_root}/in-place.svg" "${original_input}")
execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" in-place.svg in-place.svg --in-place
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE in_place_result
  OUTPUT_VARIABLE in_place_output
  ERROR_VARIABLE in_place_error
  TIMEOUT 10
)
if(NOT in_place_result EQUAL 0)
  message(FATAL_ERROR "Explicit in-place conversion failed: ${in_place_error}")
endif()
file(READ "${test_root}/in-place.svg" in_place_svg)
if(NOT in_place_svg MATCHES "<path ")
  message(FATAL_ERROR "Explicit in-place conversion did not atomically replace the SVG")
endif()

file(WRITE "${test_root}/--source.svg" "${original_input}")
execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" -- --source.svg --output.svg
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE end_options_result
  OUTPUT_VARIABLE end_options_output
  ERROR_VARIABLE end_options_error
  TIMEOUT 10
)
if(NOT end_options_result EQUAL 0 OR NOT EXISTS "${test_root}/--output.svg")
  message(FATAL_ERROR "-- did not allow dash-prefixed paths: ${end_options_error}")
endif()

execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" input.svg report-collision.svg
    --report report-collision.svg
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE report_collision_result
  OUTPUT_QUIET
  ERROR_VARIABLE report_collision_error
  TIMEOUT 10
)
if(report_collision_result EQUAL 0 OR EXISTS "${test_root}/report-collision.svg")
  message(FATAL_ERROR "Report/output collision was not rejected before conversion")
endif()

file(MAKE_DIRECTORY "${test_root}/reports/session")
file(WRITE "${test_root}/naïve file.svg"
  "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\"><circle cx=\"10\" cy=\"10\" r=\"8\"/></svg>")
execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" "naïve file.svg" "résultat file.svg"
    --report reports/session/report.json
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE nested_report_result
  OUTPUT_VARIABLE nested_report_output
  ERROR_VARIABLE nested_report_error
  TIMEOUT 10
)
if(NOT nested_report_result EQUAL 0)
  message(FATAL_ERROR "Nested report conversion failed: ${nested_report_error}")
endif()
file(READ "${test_root}/reports/session/report.json" nested_report)
if(NOT nested_report MATCHES "\"pathBase\": \"page\"")
  message(FATAL_ERROR "Nested report does not declare page-relative paths")
endif()
if(NOT nested_report MATCHES "\"input\":\"\\.\\./\\.\\./naïve file\\.svg\"")
  message(FATAL_ERROR "Nested report input is not relative to the report page: ${nested_report}")
endif()
if(NOT nested_report MATCHES "\"output\":\"\\.\\./\\.\\./résultat file\\.svg\"")
  message(FATAL_ERROR "Nested report output is not relative to the report page: ${nested_report}")
endif()

execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" input.svg typo.svg --nonsense
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE typo_result
  OUTPUT_QUIET
  ERROR_QUIET
  TIMEOUT 10
)
if(typo_result EQUAL 0)
  message(FATAL_ERROR "Unknown option was accepted")
endif()

execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" input.svg second.svg extra-operand
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE operand_result
  OUTPUT_QUIET
  ERROR_QUIET
  TIMEOUT 10
)
if(operand_result EQUAL 0)
  message(FATAL_ERROR "Extra positional operand was accepted")
endif()

file(REMOVE_RECURSE "${test_root}")
