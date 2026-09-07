if(NOT DEFINED SVG_SQUISHER_SOURCE_DIR OR NOT DEFINED SVG_SQUISHER_BINARY_DIR)
  message(FATAL_ERROR "Source and binary directories are required")
endif()

set(test_root "${SVG_SQUISHER_BINARY_DIR}/installed-package-test")
set(install_root "${test_root}/prefix")
set(consumer_source "${test_root}/source")
set(consumer_build "${test_root}/build")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${consumer_source}")

set(config_args)
if(DEFINED SVG_SQUISHER_TEST_CONFIG AND NOT SVG_SQUISHER_TEST_CONFIG STREQUAL "")
  list(APPEND config_args --config "${SVG_SQUISHER_TEST_CONFIG}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${SVG_SQUISHER_BINARY_DIR}"
          --prefix "${install_root}" ${config_args}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "Install failed:\n${install_output}\n${install_error}")
endif()

file(WRITE "${consumer_source}/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.20)\n"
  "project(svg_squisher_consumer LANGUAGES CXX)\n"
  "find_package(SvgSquisher CONFIG REQUIRED)\n"
  "add_executable(consumer main.cpp)\n"
  "target_link_libraries(consumer PRIVATE SVG::Squisher)\n")
file(WRITE "${consumer_source}/main.cpp"
  "#include <svg_squisher.h>\n"
  "int main() {\n"
  "  const auto result = svg_squisher::SvgSquisher{}.convert_string(\n"
  "    \"<svg xmlns='http://www.w3.org/2000/svg'><rect width='1' height='1'/></svg>\");\n"
  "  return result.success && !result.svg.empty() ? 0 : 1;\n"
  "}\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${consumer_source}" -B "${consumer_build}"
          "-DCMAKE_PREFIX_PATH=${install_root}"
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "Consumer configure failed:\n${configure_output}\n${configure_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" ${config_args}
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "Consumer build failed:\n${build_output}\n${build_error}")
endif()

set(executable_suffix "")
if(CMAKE_HOST_WIN32)
  set(executable_suffix ".exe")
endif()
set(consumer_executable "${consumer_build}/consumer${executable_suffix}")
if(DEFINED SVG_SQUISHER_TEST_CONFIG AND
   EXISTS "${consumer_build}/${SVG_SQUISHER_TEST_CONFIG}/consumer${executable_suffix}")
  set(consumer_executable
      "${consumer_build}/${SVG_SQUISHER_TEST_CONFIG}/consumer${executable_suffix}")
endif()
execute_process(COMMAND "${consumer_executable}" RESULT_VARIABLE run_result)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "Installed-library consumer returned ${run_result}")
endif()
