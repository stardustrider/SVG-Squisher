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

file(WRITE "${test_root}/dashed.svg"
  "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\"><path d=\"M1 10H19\" fill=\"none\" stroke=\"black\" stroke-width=\"2\" stroke-dasharray=\"2 2\"/></svg>")
execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" dashed.svg filled-compatible.svg
    --conversion-policy filled-paths --report policy-report.json
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE filled_compatible_result
  OUTPUT_QUIET
  ERROR_VARIABLE filled_compatible_error
  TIMEOUT 10
)
if(NOT filled_compatible_result EQUAL 0 OR
   NOT filled_compatible_error MATCHES "live-stroke-retained")
  message(FATAL_ERROR
    "Compatible filled-path policy did not report its live-stroke fallback: ${filled_compatible_error}")
endif()
file(READ "${test_root}/policy-report.json" policy_report)
if(NOT policy_report MATCHES "\"conversionPolicy\":\"filled-paths\"")
  message(FATAL_ERROR "Report omitted the selected conversion policy: ${policy_report}")
endif()

execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" dashed.svg filled-strict.svg
    --conversion-policy filled-paths --strict
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE filled_strict_result
  OUTPUT_QUIET
  ERROR_VARIABLE filled_strict_error
  TIMEOUT 10
)
if(filled_strict_result EQUAL 0 OR EXISTS "${test_root}/filled-strict.svg" OR
   NOT filled_strict_error MATCHES "live-stroke-retained")
  message(FATAL_ERROR
    "Strict filled-path policy did not reject the live-stroke fallback: ${filled_strict_error}")
endif()

execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" input.svg invalid-policy.svg
    --conversion-policy unsupported
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE invalid_policy_result
  OUTPUT_QUIET
  ERROR_VARIABLE invalid_policy_error
  TIMEOUT 10
)
if(invalid_policy_result EQUAL 0 OR
   NOT invalid_policy_error MATCHES "preserve-appearance or filled-paths")
  message(FATAL_ERROR "Invalid conversion policy was not rejected clearly")
endif()

file(WRITE "${test_root}/malformed-list.svg"
  "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\"><rect width=\"10\" height=\"10\" transform=\",translate(5,,6),,\"/></svg>")
execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" malformed-list.svg malformed-list-output.svg --strict
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE malformed_list_result
  OUTPUT_QUIET
  ERROR_VARIABLE malformed_list_error
  TIMEOUT 10
)
if(malformed_list_result EQUAL 0 OR EXISTS "${test_root}/malformed-list-output.svg" OR
   NOT malformed_list_error MATCHES "invalid-numeric-value")
  message(FATAL_ERROR
    "Strict conversion accepted malformed transform comma-wsp: ${malformed_list_error}")
endif()

file(WRITE "${test_root}/malformed-path.svg"
  "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\"><path d=\"M,0,,0,L10 10,\"/></svg>")
execute_process(
  COMMAND "${SVG_SQUISHER_EXE}" malformed-path.svg malformed-path-output.svg --strict
  WORKING_DIRECTORY "${test_root}"
  RESULT_VARIABLE malformed_path_result
  OUTPUT_QUIET
  ERROR_VARIABLE malformed_path_error
  TIMEOUT 10
)
if(malformed_path_result EQUAL 0 OR EXISTS "${test_root}/malformed-path-output.svg" OR
   NOT malformed_path_error MATCHES "invalid-path-data")
  message(FATAL_ERROR
    "Strict conversion accepted malformed path comma-wsp: ${malformed_path_error}")
endif()

file(MAKE_DIRECTORY
  "${test_root}/linked-output-input/sub"
  "${test_root}/linked-output-root"
  "${test_root}/linked-output-outside")
file(WRITE "${test_root}/linked-output-input/sub/a.svg" "${original_input}")
file(WRITE "${test_root}/linked-output-outside/a.svg" "outside sentinel")
file(CREATE_LINK
  "${test_root}/linked-output-outside"
  "${test_root}/linked-output-root/sub"
  SYMBOLIC
  RESULT linked_output_result)
if(linked_output_result STREQUAL "0")
  execute_process(
    COMMAND "${SVG_SQUISHER_EXE}"
      "${test_root}/linked-output-input"
      "${test_root}/linked-output-root"
      --recursive
      --report "${test_root}/linked-output-report.json"
    RESULT_VARIABLE linked_output_cli_result
    OUTPUT_VARIABLE linked_output_cli_stdout
    ERROR_VARIABLE linked_output_cli_stderr
    TIMEOUT 10
  )
  if(linked_output_cli_result EQUAL 0 OR
     NOT linked_output_cli_stderr MATCHES "symbolic link or reparse point")
    message(FATAL_ERROR
      "Recursive CLI did not reject a linked output parent: ${linked_output_cli_stderr}")
  endif()
  file(READ "${test_root}/linked-output-outside/a.svg" linked_output_sentinel)
  if(NOT linked_output_sentinel STREQUAL "outside sentinel")
    message(FATAL_ERROR "Recursive CLI overwrote the outside sentinel")
  endif()
  file(READ "${test_root}/linked-output-report.json" linked_output_report)
  if(NOT linked_output_report MATCHES "\"status\":\"failed\"" OR
     NOT linked_output_report MATCHES "symbolic link or reparse point")
    message(FATAL_ERROR
      "Recursive CLI report omitted the linked-parent failure: ${linked_output_report}")
  endif()
endif()

file(REMOVE_RECURSE "${test_root}")
