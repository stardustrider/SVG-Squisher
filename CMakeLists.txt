cmake_minimum_required(VERSION 3.20)

project(svg_squisher VERSION 0.2.0 LANGUAGES C CXX)

include(CTest)
include(CMakePackageConfigHelpers)
include(FetchContent)
include(GNUInstallDirs)

option(SVG_SQUISHER_USE_SYSTEM_DEPS "Use installed pugixml, FreeType, and HarfBuzz packages" OFF)
option(SVG_SQUISHER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SVG_SQUISHER_BUILD_FUZZER "Build the libFuzzer SVG conversion target" OFF)
option(SVG_SQUISHER_WARNINGS_AS_ERRORS "Treat SVG Squisher compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(SVG_SQUISHER_USE_SYSTEM_DEPS)
  find_package(pugixml CONFIG REQUIRED)
  find_package(Freetype REQUIRED)
  find_package(harfbuzz CONFIG REQUIRED)
  set(SVG_SQUISHER_PUGIXML_TARGET pugixml::pugixml)
  set(SVG_SQUISHER_FREETYPE_TARGET Freetype::Freetype)
  set(SVG_SQUISHER_HARFBUZZ_TARGET harfbuzz::harfbuzz)
else()
  FetchContent_Declare(
    pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG ee86beb30e4973f5feffe3ce63bfa4fbadf72f38
    GIT_SHALLOW FALSE
  )

  set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)

  FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_TAG 42608f77f20749dd6ddc9e0536788eaad70ea4b5
    GIT_SHALLOW FALSE
  )

  set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
  set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
  set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
  set(HB_HAVE_CORETEXT OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    harfbuzz
    GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
    GIT_TAG b42511e071162fe76102f613a6ccc009726c99af
    GIT_SHALLOW FALSE
  )

  FetchContent_MakeAvailable(pugixml freetype harfbuzz)
  set(SVG_SQUISHER_PUGIXML_TARGET pugixml)
  set(SVG_SQUISHER_FREETYPE_TARGET freetype)
  set(SVG_SQUISHER_HARFBUZZ_TARGET harfbuzz)
endif()

find_package(Threads REQUIRED)

set(SVG_SQUISHER_CORE_SOURCES
  src/svg_computed_style.cpp
  src/svg_diagnostics.cpp
  src/svg_dom.cpp
  src/svg_entry.cpp
  src/svg_font_identity.cpp
  src/svg_geometry.cpp
  src/svg_output.cpp
  src/svg_paint.cpp
  src/svg_path.cpp
  src/svg_postprocess.cpp
  src/svg_report.cpp
  src/svg_shape.cpp
  src/svg_style.cpp
  src/svg_stroke.cpp
  src/svg_text.cpp
  src/svg_transform.cpp
  src/svg_traversal.cpp
  src/svg_util.cpp
  src/svg_squisher.cpp
)

add_library(svg_squisher_core STATIC ${SVG_SQUISHER_CORE_SOURCES})
add_library(SVG::Squisher ALIAS svg_squisher_core)
set_target_properties(svg_squisher_core PROPERTIES EXPORT_NAME Squisher)
target_include_directories(svg_squisher_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    $<$<NOT:$<BOOL:${SVG_SQUISHER_USE_SYSTEM_DEPS}>>:${freetype_SOURCE_DIR}/include>
)
target_link_libraries(svg_squisher_core
  PRIVATE
    ${SVG_SQUISHER_PUGIXML_TARGET}
    ${SVG_SQUISHER_FREETYPE_TARGET}
    ${SVG_SQUISHER_HARFBUZZ_TARGET}
    Threads::Threads
)
target_compile_features(svg_squisher_core PUBLIC cxx_std_17)
target_compile_definitions(svg_squisher_core PRIVATE SVG_SQUISHER_VERSION="${PROJECT_VERSION}")

add_executable(svg_squisher src/main.cpp)
target_link_libraries(svg_squisher PRIVATE SVG::Squisher)
target_compile_definitions(svg_squisher PRIVATE SVG_SQUISHER_VERSION="${PROJECT_VERSION}")

function(svg_squisher_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(SVG_SQUISHER_WARNINGS_AS_ERRORS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /WX)
    else()
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

svg_squisher_enable_warnings(svg_squisher_core)
svg_squisher_enable_warnings(svg_squisher)

if(SVG_SQUISHER_ENABLE_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "SVG_SQUISHER_ENABLE_SANITIZERS currently requires GCC or Clang")
  endif()
  target_compile_options(svg_squisher_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(svg_squisher_core PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/font_identity_tests.cpp")
    add_executable(font_identity_tests tests/font_identity_tests.cpp)
    target_link_libraries(font_identity_tests PRIVATE SVG::Squisher)
    svg_squisher_enable_warnings(font_identity_tests)
    add_test(NAME font_identity COMMAND font_identity_tests)
    set_tests_properties(font_identity PROPERTIES TIMEOUT 10)
  endif()

  add_executable(parser_stroke_tests tests/parser_stroke_tests.cpp)
  target_link_libraries(parser_stroke_tests PRIVATE SVG::Squisher ${SVG_SQUISHER_PUGIXML_TARGET})
  svg_squisher_enable_warnings(parser_stroke_tests)
  add_test(NAME parser_stroke COMMAND parser_stroke_tests)
  set_tests_properties(parser_stroke PROPERTIES TIMEOUT 10)

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/style_text_tests.cpp")
    add_executable(style_text_tests tests/style_text_tests.cpp)
    target_link_libraries(style_text_tests PRIVATE SVG::Squisher ${SVG_SQUISHER_PUGIXML_TARGET})
    svg_squisher_enable_warnings(style_text_tests)
    add_test(NAME style_text COMMAND style_text_tests)
    set_tests_properties(style_text PROPERTIES TIMEOUT 15)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/text_shaping_tests.cpp")
    add_executable(text_shaping_tests tests/text_shaping_tests.cpp)
    target_link_libraries(
      text_shaping_tests PRIVATE SVG::Squisher ${SVG_SQUISHER_PUGIXML_TARGET})
    svg_squisher_enable_warnings(text_shaping_tests)
    add_test(NAME text_shaping COMMAND text_shaping_tests)
    set_tests_properties(text_shaping PROPERTIES TIMEOUT 15)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/core_tests.cpp")
    add_executable(svg_squisher_tests tests/core_tests.cpp)
    target_link_libraries(
      svg_squisher_tests PRIVATE SVG::Squisher Threads::Threads ${SVG_SQUISHER_PUGIXML_TARGET})
    svg_squisher_enable_warnings(svg_squisher_tests)
    add_test(NAME svg_squisher_core COMMAND svg_squisher_tests)
    set_tests_properties(svg_squisher_core PROPERTIES TIMEOUT 15)
  endif()

  add_test(NAME cli_help COMMAND svg_squisher --help)
  add_test(NAME cli_version COMMAND svg_squisher --version)
  add_test(
    NAME cli_conversion
    COMMAND ${CMAKE_COMMAND}
      -DSVG_SQUISHER_EXE=$<TARGET_FILE:svg_squisher>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_tests.cmake
  )
  set_tests_properties(cli_conversion PROPERTIES TIMEOUT 20)

  add_test(
    NAME installed_package
    COMMAND ${CMAKE_COMMAND}
      -DSVG_SQUISHER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DSVG_SQUISHER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DSVG_SQUISHER_TEST_CONFIG=$<CONFIG>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/installed_package_test.cmake
  )
  set_tests_properties(installed_package PROPERTIES TIMEOUT 90)
endif()

if(SVG_SQUISHER_BUILD_FUZZER)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SVG_SQUISHER_BUILD_FUZZER requires Clang libFuzzer")
  endif()
  add_executable(svg_squisher_fuzz tests/fuzz_svg.cpp)
  target_link_libraries(svg_squisher_fuzz PRIVATE SVG::Squisher)
  target_compile_options(svg_squisher_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(svg_squisher_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

install(TARGETS svg_squisher
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(TARGETS svg_squisher_core
  EXPORT SvgSquisherTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES src/svg_squisher.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md")
  install(FILES THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/licenses")
  install(DIRECTORY third_party/licenses/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses)
endif()
configure_package_config_file(
  cmake/SvgSquisherConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/SvgSquisherConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SvgSquisher
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/SvgSquisherConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(EXPORT SvgSquisherTargets
  FILE SvgSquisherTargets.cmake
  NAMESPACE SVG::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SvgSquisher
)
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/SvgSquisherConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/SvgSquisherConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SvgSquisher
)
